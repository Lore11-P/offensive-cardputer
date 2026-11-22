# FIX_NET — OTA firmato, Evil Portal senza UAF, whitelist condivisa, hc22000 live, Karma fail-closed

Worktree: `orbit-wt-fix-net`, branch `review/fix-net`, base `ec5a601`
(ORBIT OS 2.0.2). Diagnosi: `docs/integration/review/02-net.md`.
Hardware di riferimento: M5Stack Cardputer ADV, ESP32-S3FN8, 320 KB SRAM,
nessuna PSRAM, microSD su SPI.

Nessun test su hardware in questo ambiente: build, firma e script sono
verificati; comportamenti radio/TLS reali no.

---

## 1. Modifiche

### 1.1 OTA autenticato (Must #1)

Prima: `WiFiClientSecure::setInsecure()` era l'unica difesa; nessun hash,
nessuna firma, nessun controllo di versione. `main` può servire una release
vecchia (oggi `origin/main` = 1.21.0, LittleFS a 0x510000).

Ora `src/utils/OtaUpdate.cpp`:

1. **Orologio** — `configTime()` + attesa NTP (max 10 s). Serve perché con la
   CA pinnata mbedtls verifica `notBefore/notAfter`: senza orologio il TLS
   fallirebbe sempre.
2. **TLS autenticato** — `client.setCACert(kRootCaPem)` con ISRG Root X1
   (Let's Encrypt). Nessun `setInsecure()`.
3. **Host allowlist** — redirect manuali (max 3) solo verso
   `raw.githubusercontent.com` e `objects.githubusercontent.com`, solo HTTPS.
4. **Manifest** — scarica `flasher/bin/firmware.json`:
   ```
   version=2.0.3
   size=2708784
   sha256=6985...d8f2
   ```
   Rifiuta manifest assente, malformato o con `version` **inferiore** a
   `fw::kVersion`. La stessa versione è ammessa (reinstallazione); un
   downgrade no. Questo blocca il caso "main serve 1.21.0".
5. **Download in streaming** — `Update.write()` sulla partizione inattiva,
   SHA-256 calcolato sullo stesso flusso (`mbedtls_sha256`). La dimensione
   HTTP deve coincidere con `size` del manifest.
6. **Verifica prima del boot** — dopo il download:
   - SHA-256 del binario == `sha256` del manifest;
   - `firmware.sig` (64 byte grezzi `r||s`) verificata con ECDSA P-256 /
     SHA-256 (`mbedtls_ecdsa_verify`) e chiave pubblica embedded in
     `include/OtaPublicKey.h`;
   - solo allora `Update.end(true)` sposta il boot. In ogni errore:
     `Update.abort()`, boot invariato.
7. Resta il controllo strutturale `esp_ota_get_partition_description()` dopo
   `Update.end`, con ripristino della partizione di running se fallisce.

La firma è la difesa primaria: la CA può cambiare (Fastly/GitHub), la chiave
ECDSA no. La CA è comunque pinnata per non lasciare TLS senza autenticazione.

#### Chiavi e pubblicazione (obbligatorio)

La chiave privata **non entra nel repo**:

```
~/.orbit-ota/ota_ec_private.pem      # fuori dal repository
include/OtaPublicKey.h               # solo chiave pubblica, versionata
```

`*.pem`/`*.key` sono già in `.gitignore`. Generazione (una volta):

```powershell
python tools/sign_ota.py keygen
```

Ad ogni release, sul ramo/tag che si vuole pubblicare:

```powershell
# 0. aggiornare fw::kVersion in include/ModuleFlags.h (es. 2.0.3) e ricompilare
# 1. compilare e copiare il binario in flasher/bin/firmware.bin
python tools/sign_ota.py sign     flasher/bin/firmware.bin
python tools/sign_ota.py manifest flasher/bin/firmware.bin --version 2.0.3
# 2. pubblicare insieme: firmware.bin, firmware.sig, firmware.json
```

`firmware.json` deve descrivere **lo stesso** `firmware.bin` pubblicato:
versione, dimensione e SHA-256 vengono dal file. Se il manifest manca, l'OTA
si blocca con un messaggio esplicito (fail-closed): non esiste un percorso che
installi un binario non firmato. Se la chiave privata viene persa, serve un
flash USB con una nuova `OtaPublicKey.h`.

Verifica locale:

```powershell
python tools/sign_ota.py verify flasher/bin/firmware.bin flasher/bin/firmware.sig
python tools/sign_ota.py selftest
```

### 1.2 Evil Portal: niente use-after-free (Must #2)

`src/modules/wifi/WifiMenu.cpp`:

- `AsyncWebServer` non è più sullo stack: un solo `g_portalServer` allocato
  con `new (std::nothrow)` e **mai distrutto**, come `AutoPortal`. Handler
  registrati una volta sola; `g_portalMode`/`g_portalRedirect` scelgono la
  pagina a runtime.
- All'uscita: `server.end()`, `g_portalMode = false` (i POST successivi non
  salvano nulla), **grace period 250 ms** per i callback AsyncTCP in volo, poi
  `g_credentials.clear()` sotto mutex. Il server resta allocato: nessun UAF
  su richieste aperte.
- Il mutex viene creato prima di `server.begin()`, non più in modo lazy dal
  task HTTP.
- Limite `contentLength() > 1024` mantenuto.

### 1.3 Whitelist unica (Should #3)

Nuovo `include/NetGuard.h` + `src/utils/NetGuard.cpp`:

- un solo parser di `/wifilab/whitelist.txt` (formato `BSSID|SSID`, commenti
  `#`, max 24 voci / 4 KB, fail-closed su file assente, malformato o overflow);
- `netguard::isProtected(bssid, ssid, len)` e `isProtectedBssid()`.

`WifiLabMenu` non ha più il parser: usa `netguard` (stessa UI, stesse voci).
`WifiMenu` ora applica la whitelist a:

- **Deauth target**: target protetto → bloccato;
- **Deauth Flood**: reti protette escluse dal flood; la UI mostra
  `WL: N`; se restano zero target, non parte nulla;
- **Evil Twin / Evil Portal**: controllo esplicito in `runEvilTwin()` e
  `runEvilPortal()` prima di `legalWarning`, più il gate centrale in
  `runFakeAccessPoint()` (difesa per futuri chiamanti): SSID/BSSID protetto →
  messaggio "Rete in whitelist" e nessun AP/portale avviato.

Policy quando il file manca: **fail-closed** in tutti i punti (messaggio che
rimanda a `WiFi Lab > Whitelist`). Non esiste più il percorso "attacco senza
whitelist".

### 1.4 hc22000 live (Should #4)

`src/utils/PcapTools.cpp`:

- tabella SSID del collector live da 8 a **48 voci** (`kMaxLiveSsids`), con
  sostituzione **LRU** (`seen` aggiornato ad ogni beacon/probe response). Non
  tocca il ring REC dello sniffer.
- Un **PMKID senza ESSID non viene più scritto**, né dal collector live né dal
  convertitore offline: viene contato in `skippedNoEssid`, come già avveniva
  per gli M2. La conversione offline mostra già "Senza SSID: N" in UI.
- `WifiMenu` mostra a fine cattura quanti hash sono stati esclusi senza SSID
  (oltre al log seriale già presente).

Costo RAM: la tabella passa da ~320 B a ~2,1 KB, nel collector allocato una
volta (`LiveCollector::Impl`, heap).

### 1.5 Karma fail-closed (Should #5)

`src/modules/wifiext/Karma.cpp`:

- rimosso `loadFallback()`: niente scansione automatica delle reti vicine,
  niente default `FreeWiFi/WiFi/Guest/Home`.
- senza `/wifiext/karma.txt` (o con file vuoto) Karma non parte e spiega
  perché. L'utente deve scrivere esplicitamente gli SSID da impersonare.

---

## 2. File toccati

| File | Modifica |
|---|---|
| `src/utils/OtaUpdate.cpp` | NTP, CA pinnata, allowlist host, manifest, downgrade, SHA-256 streaming, ECDSA, verifica prima di `Update.end(true)` |
| `include/OtaUpdate.h` | commento del flusso OTA |
| `include/OtaPublicKey.h` | chiave pubblica ECDSA P-256 generata (65 B) |
| `tools/sign_ota.py` | `keygen`, `sign`, `manifest`, `verify`, `selftest` |
| `src/modules/wifi/WifiMenu.cpp` | server portale persistente + grace + clear post-grace; whitelist su deauth/flood/evil twin; UI skippedNoEssid; gate netguard |
| `src/modules/wifilab/WifiLabMenu.cpp` | parser whitelist rimosso, usa `netguard` |
| `include/NetGuard.h`, `src/utils/NetGuard.cpp` | helper whitelist condiviso |
| `src/utils/PcapTools.cpp` | SSID live 48 con LRU; PMKID senza ESSID saltato |
| `src/modules/wifiext/Karma.cpp` | fail-closed senza `karma.txt` |
| `docs/integration/FIX_NET.md` | questo documento |

Non toccati: `partitions.csv`, `flasher/*`, `Storage`, `Config`, ring REC.

---

## 3. Build e dimensioni (misurate)

Comando: `python -m platformio run -e cardputer-adv-offensive` (timeout
900.000 ms). Esito: **SUCCESS**, 335 s.

| Voce | Valore |
|---|---:|
| `firmware.bin` | **2.709.184 B** |
| Flash `text+data` (PlatformIO) | 2.708.813 B (96,1%) |
| Slot app OTA (`0x2B0000`) | 2.818.048 B |
| Libero nello slot | **108.864 B** (106,3 KiB) |
| Budget `check_size.ps1` | 2.751.776 B |
| Margine al budget | **42.592 B** |
| RAM statica `.data+.bss` | **147.416 B** (45,0%) |

Rispetto ai ~145 KiB liberi indicati dal supervisore, il fix OTA + manifest +
NetGuard + Pcap consuma ~38 KiB; resta sopra la riserva minima di 64 KiB.
Nessuna nuova allocazione statica grande: la tabella SSID live (~2,1 KB) è
heap, allocata una volta.

---

## 4. Test

### 4.1 Eseguiti

- Build `cardputer-adv-offensive`: `SUCCESS` (2.709.184 B).
- `tools/check_size.ps1 -SkipBuild`: budget rispettato, 108.864 B liberi.
- `python tools/sign_ota.py selftest`: OK (firma e rifiuto di un digest
  diverso).
- Firma del `firmware.bin` appena compilato + `verify` con la chiave pubblica
  dell'header: **OK**.
- Firma con un bit alterato: **FIRMA NON VALIDA** (exit 1).
- `manifest` genera `version/size/sha256` coerenti con il binario.
- Compilazione senza errori dei file modificati; nessun errore di link.
### 4.2 Da eseguire sul dispositivo (non eseguiti)

1. OTA con manifest/firma validi: aggiorna e riavvia; con un byte alterato:
   "Firma non valida", boot invariato.
2. OTA con `firmware.json` assente: blocco esplicito, nessun download.
3. OTA con manifest `version=1.21.0` su firmware 2.0.2: "Downgrade bloccato".
4. OTA su rete senza NTP: messaggio di errore, nessuna scrittura.
5. Evil Portal: inviare un POST e premere ESC nello stesso istante, ripetere
   10 volte; nessun crash/reboot; credenziali a zero dopo l'uscita.
6. Deauth target/flood/evil twin con AP in whitelist: bloccato/escluso;
   senza `whitelist.txt`: bloccato con messaggio.
7. Sniffer in ambiente con 9+ AP: `.hc22000` accanto al pcap contiene gli
   handshake; i PMKID senza ESSID sono assenti e contati in UI.
8. Karma senza `/wifiext/karma.txt`: non parte; con file: solo gli SSID
   elencati.
9. Convertire offline un pcap con PMKID/M1 ma senza beacon dell'AP: nessuna
   riga PMKID scritta e contatore "Senza SSID" incrementato.

---

## 5. Rischi e limiti

| Rischio | Gravità | Stato / mitigazione |
|---|---|---|
| CA pinnata ISRG Root X1: GitHub/Fastly può cambiare CA | Media | OTA fallisce chiuso con errore TLS; aggiornare `kRootCaPem` via USB/OTA precedente. La firma ECDSA resta la difesa primaria |
| NTP bloccato dalla rete | Media | OTA non parte (fail-closed). Documentato; nessun fallback insicuro |
| Manifest non pubblicato o non aggiornato | Alta (disponibilità OTA) | OTA bloccato con messaggio; il publishing deve pubblicare `firmware.bin` + `.sig` + `.json` insieme. `origin/main` 1.21.0 viene rifiutato dal gate versione |
| Chiave privata persa/compromessa | Alta | chiave fuori repo, `*.pem` ignorato; se persa serve flash USB con nuova pubblica. Se compromessa, ruotare chiave e pubblicare via USB |
| Versione nel manifest non verificabile nel binario | Bassa | `esp_app_desc_t` del build Arduino riporta la versione IDF, non `fw::kVersion`; il manifest è autenticato dal TLS pinnato e la sua SHA-256 lega il binario. Un manifest sbagliato è un errore di publishing, non un attacco di rete |
| TLS con clock errato | Bassa | `syncClock()` attende NTP; senza orologio niente OTA |
| Grace 250 ms non copre un callback bloccato oltre | Molto bassa | server mai distrutto (nessun UAF); `g_portalMode=false` impedisce nuovi salvataggi; clear sotto mutex |
| Whitelist fail-closed blocca tutti gli attacchi senza SD/file | Media (usabilità) | voluto: il progetto dichiara la whitelist come controllo; il file si crea da WiFi Lab > Whitelist |
| Tabella SSID live 48 voci: oltre 48 BSSID nuovi l'AP dell'handshake può essere espulso se non beaconna più | Bassa | LRU: i BSSID attivi restano; i PMKID/M2 senza ESSID vengono scartati e contati, non scritti inutilizzabili |
| NetGuard `add()` non fonde voci esistenti | Molto bassa | comportamento precedente di WiFi Lab; file limitato a 24 voci/4 KB |
| Nessun test hardware/TLS reale | Media | dichiarato: build e script verificati, comportamento radio e handshake TLS no |

---

## 6. Conclusione

L'OTA non è più "solo TLS": manifest con versione/dimensione/SHA-256, gate
anti-downgrade, CA pinnata, host allowlist e firma ECDSA P-256 verificata
prima di `Update.end(true)`. Evil Portal non distrugge più il server con
richieste in volo. La whitelist è una sola e copre deauth target, flood, evil
twin/portal. Il collector live scrive solo hc22000 verificabili, e Karma non
impersona più reti senza una lista esplicita.

Il firmware cresce a 2.709.184 B (96,1% dello slot, 108.864 B liberi): entra
nel budget con 42.592 B di margine. Restano da fare i test su hardware e il
publishing del primo `firmware.json` firmato.
