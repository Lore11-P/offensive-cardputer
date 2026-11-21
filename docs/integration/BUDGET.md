# Budget flash e RAM — integrazione 10 moduli

Misura del 21/11/2026 sul worktree `orbit-wt-budget`, branch `feat/budget`,
ambiente `cardputer-adv-offensive`.
Toolchain: PlatformIO Core 6.2.0, `espressif32@6.7.0`, GCC xtensa 8.4.0.
Hardware: M5Stack Cardputer ADV, ESP32-S3FN8, 8 MB flash, 320 KB SRAM, no PSRAM.

## 1. Baseline e stato integrazione

Comando:

```powershell
python -m platformio run -e cardputer-adv-offensive
```

| Voce | Valore | Note |
|---|---:|---|
| Flash PlatformIO | 2.358.193 B (83,7%) | `text + data` stimati sul file ELF |
| `firmware.bin` | **2.358.560 B** | dimensione reale del binario |
| Slot app OTA | 2.818.048 B (2,6875 MiB) | `0x2B0000` in `partitions.csv` (Piano B) |
| Libero nello slot | **459.488 B = 448,7 KiB** | 16,3% dello slot |
| RAM statica | 115.520 / 327.680 B (35,3%) | `.data + .bss` |
| RAM statica libera | 212.160 B | 207,2 KiB, prima di stack e heap |

> La baseline attesa di 2.330.085 B non è più valida: la build scaffold misura
> 2.358.560 B (+28.475 B). Gli 8 stub spiegano solo ~5 KB; il resto era già
> nella build corrente. I conti seguono il valore misurato.

Stato del merge (riferito dal supervisore, non misurato in questo worktree):

- 6 feature integrate: circa **2,52 MB**.
- Mancano WiFi Lab (budget 28 KiB), Sensori (10 KiB) ed ESP-NOW (18 KiB):
  **56 KiB** di budget residuo.
- Stima del margine finale: **115–235 KiB**, a seconda che i 2,52 MB siano
  decimali o MiB. Il valore va rimisurato dopo l'ultimo merge.

Gli stub dei 10 moduli sono già compresi nella baseline: i budget sotto sono
**delta** rispetto allo stub, cioè il codice che l'integrazione può aggiungere.

## 2. Budget per modulo e budget totale

| # | Modulo | Flash (KiB) | RAM statica (KiB) | Heap di picco (KiB) | Nota |
|---|---|---:|---:|---:|---|
| 1 | IR (DB completo + clima) | 8 | 1 | 4 | DB su SD; in firmware solo il merge SD/LittleFS |
| 2 | Radio (playlist, ICY, REC) | 16 | 2 | 8 | ring buffer piccolo per il WAV |
| 3 | Audio (FFT, registratore, memo) | 16 | 10 | 8 | FFT 512 punti + buffer statici ~8 KB |
| 4 | BLE HID + Bad BLE | 20 | 2 | 16 | NimBLE HID; deinit obbligatorio |
| 5 | Tools (clock, note, calcolatrice, QR, TOTP) | 24 | 4 | 8 | QR e HMAC-SHA1 |
| 6 | WiFi Lab (PMKID, monitor, waterfall) | 28 | 4 | 16 | modulo nuovo più incerto |
| 7 | WPA-SEC (upload/potfile) | 14 | 1 | 48 | TLS: richiede heap contiguo |
| 8 | Sensori (livella, goniometro, passi, LED) | 10 | 1 | 2 | solo IMU e LED |
| 9 | WiFi+ (responder, karma, portal) | 24 | 2 | 12 | riusa AsyncWebServer già linkato |
| 10 | ESP-NOW (file, comandi, clipboard) | 18 | 4 | 8 | chunk da ~200 B |
| | **Totale moduli** | **178** | **31** | **48 max** | gli heap non si sommano: i menu sono seriali |
| | Contingenza condivisa | 14 | 1 | — | solo per sforamenti motivati |
| | **Tetto moduli** | **192** | **32** | — | |
| | Margine Piano B | 192 | — | — | +192 KiB per slot rispetto al layout 2,5 MB |
| | **Tetto integrazione** | **384** | **32** | — | |
| | Riserva intoccabile | 64 | — | — | 65.536 B di flash |

Aritmetica del budget flash:

```text
baseline            2.358.560 B
tetto integrazione    393.216 B  (384 KiB)
---------------------------------------------------------------
tetto binario       2.751.776 B
slot app            2.818.048 B
margine finale         66.272 B  (64,7 KiB, >= 64 KiB richiesti)
```

Regole:

- Nessun modulo può sforare il proprio budget usando il margine di un altro
  senza una voce esplicita in questo file.
- Dopo ogni merge si misura con `.\tools\check_size.ps1`; il delta reale è
  `bin nuovo - bin precedente`, non la somma dei budget.
- Se un modulo sfora, deve liberare l'equivalente (feature rinviata o taglio
  nel piano sotto) prima del merge.
- Il budget RAM statica lascia ~176 KiB liberi per stack, framebuffer (~65 KB)
  e heap WiFi/BLE/TLS. Il vincolo vero a runtime è l'heap, non `.bss`.

Gate heap consigliati prima di aprire un modulo:

| Modulo | Heap libero minimo | Note |
|---|---:|---|
| WPA-SEC | 64 KB, di cui 48 KB contigui | TLS |
| BLE HID | 32 KB | NimBLE HID |
| WiFi Lab / WiFi+ | 32 KB | raw WiFi, server |
| Audio / Radio | 24 KB | buffer e decoder |
| Altri | 16 KB | |

## 3. Flag di compilazione

Verificati in `.pio/build/cardputer-adv-offensive/idedata.json` e nel map file:

| Flag | Stato | Evidenza |
|---|---|---|
| `-Os` | **attivo** | presente in `cc_flags` e `cxx_flags` |
| `-ffunction-sections` | **attivo** | presente in `cc_flags` e `cxx_flags` |
| `-fdata-sections` | **attivo** | presente in `cc_flags` e `cxx_flags` |
| `-Wl,--gc-sections` | **attivo** | il map contiene `Discarded input sections` |
| `-flto` | **non attivo** | assente dai flag |

Conclusioni:

- La configurazione sicura è già quella in uso: non c'è un flag ovvio da
  aggiungere. `--gc-sections` sta già eliminando il codice non referenziato.
- **Non attivare `-flto` durante l'integrazione.** Su GCC 8.4 xtensa con
  Arduino core, NimBLE e decoder audio ci sono rischi noti su sezioni IRAM,
  ISR, callback weak e ordine di inizializzazione. Il guadagno atteso (3–8%,
  indicativamente 70–190 KB) non è verificato su questa scheda e non va contato
  nel budget. Se serve, provarlo su un branch dedicato con test hardware
  completo (WiFi, BLE, audio, IR, SD, OTA) e solo dopo l'integrazione.
- `-fno-exceptions` non è proposto: le librerie C++ del core possono usare
  eccezioni e il risparmio non vale il rischio.
- Il decoder FLAC di ESP32-audioI2S è linkato (~8,4 KB) ma non ha uno switch
  supportato nella versione usata; non è un taglio sicuro senza patch della
  libreria.

## 4. Partizioni e LittleFS

Layout attuale (`partitions.csv`, 8 MiB totali):

| Nome | Offset | Size | Dimensione |
|---|---:|---:|---:|
| `nvs` | `0x009000` | `0x005000` | 20 KiB |
| `otadata` | `0x00E000` | `0x002000` | 8 KiB |
| `app0` | `0x010000` | `0x2B0000` | 2,6875 MiB |
| `app1` | `0x2C0000` | `0x2B0000` | 2,6875 MiB |
| `spiffs` (LittleFS) | `0x570000` | `0x280000` | 2,5 MiB |
| `coredump` | `0x7F0000` | `0x010000` | 64 KiB |

Verifica: `0x10000 + 0x2B0000 = 0x2C0000`, `0x2C0000 + 0x2B0000 = 0x570000`,
`0x570000 + 0x280000 = 0x7F0000`, `0x7F0000 + 0x10000 = 0x800000` (8 MiB).
Offset e dimensioni degli slot sono multipli di `0x10000` (64 KiB).

Perché non 2,75/2,375 MB: la mappa richiesta è aritmeticamente valida, ma con
il `data/` attuale **LittleFS a 2,375 MB si riempie**. `buildfs` fallisce con
`lfs_write error(-28): File system is full` prima di Toshiba, Trevi, United,
Universale e Xiaomi. Il payload è 1.474.017 B, ma l'allocazione LittleFS
(323 file, 41 directory, blocchi da 4 KiB) supera 2,375 MB. A 2,5 MB `buildfs`
completa senza errori. Il costo è 64 KiB per slot: margine 448,7 KiB invece
di 512,7 KiB.

LittleFS:

- Payload in `data/`: **1.474.017 B** su 2.621.440 B (56,2% del payload, ma
  l'allocazione è quasi satura).
- Di cui IR: **1.435.981 B in 309 file** (97,4% del payload); icone 37.512 B
  (12 file); script BadUSB 524 B.
- `data/` **non entra in `firmware.bin`**: è un'immagine separata. Spostare il
  DB IR su SD non libera flash applicativa, libera solo LittleFS.
- La LittleFS è quasi piena: non aggiungere altri file in `data/`. Il database
  IR completo deve stare su SD.

Cosa mettere su SD (e cosa no):

- **SD, sempre**: nuove categorie IR e crescita del DB (in LittleFS resta il
  fallback da 309 file richiesto dalla spec IR); registrazioni `/audio/rec`,
  memo, catture `.hc22000`/`.pcap`, potfile, playlist radio, whitelist e PMKID,
  inbox/outbox ESP-NOW, note/calendario/TOTP, log e cache. Non duplicare questi
  file in LittleFS.
- **LittleFS**: solo il fallback IR attuale e le icone dei menu (servono prima
  del mount SD). Nessun dato utente o file che cresce nel tempo.
- Il filesystem **non è la pressione attuale**: il rischio è la flash
  applicativa, ma la LittleFS è ora vicina al limite.

Piano C (futuro, non applicato): se il merge finale sposta tutto il DB IR su
SD e riduce `data/ir` sotto ~600 KB, si può tornare a slot da 2,75 MiB con
LittleFS da 2,375 MiB, guadagnando altri 64 KiB per slot. Richiede una modifica
alla specifica IR (fallback assente o ridotto) e un altro flash USB una tantum.

### Migrazione al nuovo layout

La tabella partizioni **non si aggiorna via OTA**: `Update.h` scrive solo lo
slot applicativo. Il passaggio richiede un flash USB una tantum:

1. Aggiorna il flasher con tutti e quattro gli artefatti (bootloader,
   `partitions.bin`, `firmware.bin`, `littlefs.bin`). Non mischiare la vecchia
   `littlefs.bin` da 3.014.656 B: non entra nella nuova partizione.
2. Collega la Cardputer via USB e lancia `flasher\flash.ps1` (o il web
   flasher). Scrive tabella a `0x8000`, firmware a `0x10000`, LittleFS a
   `0x570000`. La NVS a `0x9000` non viene toccata; `-Erase` la cancella.
3. Il vecchio LittleFS a `0x510000` resta fisicamente in flash ma non viene
   più letto. Il nuovo filesystem parte da `0x570000`.
4. Metti i file IR sulla microSD (`/ir`); in LittleFS resta il fallback.
5. Da questo momento l'OTA funziona di nuovo, su entrambi gli slot.

> **Stato del pacchetto in questo branch:** `flasher/bin|web/firmware.bin` resta
> la release 1.21.0 (2.323.600 B), compatibile con lo slot più grande;
> `partitions.bin` e `littlefs.bin` sono già quelli nuovi. Al merge finale
> eseguire `flasher\build.ps1` per rigenerare tutti e quattro gli artefatti
> insieme.

## 5. Piano di riduzione se si sfora

Soglie, misurate come spazio libero nello slot (`firmware.bin`):

| Stato | Libero | Azione |
|---|---:|---|
| Verde | > 160 KiB | sviluppo normale |
| Giallo | 96–160 KiB | niente nuove feature; preparare i tagli |
| Rosso | < 96 KiB | applicare il piano; `check_size.ps1` fallisce sotto 66.272 B liberi |

Ordine di taglio (1 = primo a cadere). I giochi e le feature esistenti si
sacrificano prima dei 10 moduli nuovi.

| Ordine | Cosa | Risparmio flash | Risparmio RAM | Come |
|---:|---|---:|---:|---|
| 1 | Modulo Giochi intero | 94,3 KiB | 6,3 KiB | `-DENABLE_MODULE_GAMES=0` (solo supervisore) |
| 2 | Giochi singoli | 2,6–12,4 KiB ciascuno | 0–2,6 KiB | modifica `GamesMenu.cpp`, solo se serve un taglio parziale |
| 3 | BLEEXT intero | 53,5 KiB | **42,1 KiB** | `-DENABLE_MODULE_BLEEXT=0`; è il maggiore guadagno RAM |
| 4 | Musica locale | 15,0 KiB | — | `-DENABLE_MODULE_MUSIC=0`; la radio copre lo streaming |
| 5 | Radar | 28,7 KiB | 1,4 KiB | `-DENABLE_MODULE_RADAR=0`, solo se i precedenti non bastano |
| 6 | Sensori | 10 KiB | 1 KiB | nuovo modulo meno critico |
| 7 | Delta IR | 8 KiB | 1 KiB | tenere il DB su SD, fallback invariato |
| 8 | WPA-SEC | 14 KiB | 1 KiB | richiede rete e chiave esterna |
| 9 | ESP-NOW | 18 KiB | 4 KiB | nicchia, richiede due device |
| 10 | BLE HID | 20 KiB | 2 KiB | tastiera BLE |
| 11 | Tools (o solo QR/TOTP) | 24 KiB | 4 KiB | tagliabile a metà feature |
| 12 | WiFi+ | 24 KiB | 2 KiB | responder/karma/portal |
| 13 | WiFi Lab | 28 KiB | 4 KiB | il nuovo modulo più costoso |
| 14 | Radio | 16 KiB | 2 KiB | **ultimo**: è il modulo richiesto |
| 15 | Audio | 16 KiB | 10 KiB | **ultimo**: è il modulo richiesto |

Dettaglio giochi (dal map, dal più grande): Scacchi 12,4 KiB, Doom Lite 9,0 KiB,
Minecraft 7,4 KiB, Pixel Jump 6,7 KiB, Lander 5,8 KiB, Pac-Man 5,6 KiB; gli
altri 11 giochi sono 2,7–4,5 KiB ciascuno.

Dettaglio BLEEXT (RAM in KiB): GattEnum 12,5 + 6,8, BleCapture 8,7 + 4,5,
Wardriving 7,1 + 4,8, FlipperFinder 6,7 + 4,8, AirTag 6,2 + 4,8,
SkimmerDetect 4,4 + 4,8.

Da non tagliare mai: `core/` (Display, MenuManager, Navigation, UI), `utils/`
(Storage, Config), WiFi base, BLE base, IR base e la infrastruttura OTA.

## 6. Procedura di verifica

```powershell
# compila e controlla il tetto di budget
.\tools\check_size.ps1

# solo lettura del binario già compilato
.\tools\check_size.ps1 -SkipBuild

# soglia diversa (es. prova locale)
.\tools\check_size.ps1 -SkipBuild -MaxBytes 2800000
```

Exit code: `0` ok, `2` budget superato, `3` firmware oltre lo slot app
(non flashabile via OTA), `1` errore di build o file mancante.
Lo script legge lo slot da `partitions.csv`, quindi segue automaticamente un
eventuale cambio di layout.

## 7. Rischi principali

1. **Migrazione one-time.** La tabella non si aggiorna via OTA: senza flash USB
   il device resta sul vecchio layout. Se si scrive una `littlefs.bin` vecchia
   da 3.014.656 B nella partizione da 2,5 MB si sfora: `flash.ps1` ora rifiuta
   l'immagine troppo grande, ma il pacchetto web va aggiornato con tutti gli
   artefatti nuovi.
2. **Margine reale più stretto della mappa richiesta.** La LittleFS da 2,375 MB
   non regge il fallback IR attuale; il layout effettivo è 2,6875 MB per slot,
   con 448,7 KiB liberi e 384 KiB di budget integrazione. Con 6 feature a
   ~2,52 MB e le 3 mancanti (56 KiB) il margine stimato scende a 115–235 KiB.
3. **RAM a runtime, non `.bss`.** Framebuffer (~65 KB), TLS WPA-SEC (48 KB),
   NimBLE HID (16 KB), stack e heap WiFi: un deinit mancante o la
   frammentazione causano crash anche con flash abbondante. I gate heap e il
   deinit all'uscita non sono opzionali.
