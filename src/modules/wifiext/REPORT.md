# 09 — WiFi+ (Responder, Karma, Auto-Portal)

Modulo `src/modules/wifiext/` per M5Stack Cardputer ADV (ESP32-S3FN8, 320 KB SRAM,
2.5 MB slot OTA). Tutte le funzioni offensive sono compilate solo con
`ENABLE_OFFENSIVE=1` (env `cardputer-adv-offensive`); con flag 0 il menu mostra le
voci ma non esegue alcuna trasmissione. Ogni avvio passa da `ui::legalWarning` e
ricorda "solo reti proprie o autorizzate".

## Cosa è stato implementato

### Responder (`Responder.cpp`)
- Ascolto e risposta su:
  - **LLMNR** UDP 5355 (gruppo multicast 224.0.0.252), risposta A con il nome
    ripetuto (LLMNR non usa compressione).
  - **NBT-NS** UDP 137 (broadcast/unicast), risposta NB con il nome NetBIOS
    originale e l'IP del device.
  - **mDNS** UDP 5353 (gruppo 224.0.0.251), solo query A; risposta multicast,
    unicast se la query ha il bit QU.
- **SMB2 minimal su TCP 445**: risponde a NEGOTIATE (dialect 2.1), a
  SESSION_SETUP NTLM Type1 con un Type2 (challenge casuale + target info
  `ORBIT`/`WORKGROUP`) e legge il Type3. Il token SPNEGO viene gestito cercando
  la firma `NTLMSSP` nel security buffer.
- Hash **NetNTLMv2** (hashcat `-m 5600`) accodato in `/wifiext/ntlm.txt`:
  `utente::dominio:challenge:proof:blob` in esadecimale minuscolo, una riga per
  hash; prima riga di commento `#` alla creazione del file.
- Contatori UI: query LLMNR/NBT/mDNS, autenticazioni SMB, hash catturati/scritti,
  scartati (NTLMv1 o malformati), spazio SD. Se la SD manca il responder resta
  attivo ma segnala che gli hash non vengono salvati.
- Il Responder usa la rete in modalità STA (`wifisetup::ensureConnected`), perché
  le query arrivano dai client della LAN.

### Karma (`Karma.cpp`)
- AP aperto (`WiFi.softAP`, max 4 client) sul canale della prima rete della
  lista (fallback canale 6) e modalità promiscua con filtro management.
- Risposta ai **probe request** con frame 802.11 raw
  (`esp_wifi_80211_tx(WIFI_IF_AP, ...)`), SSID solo se presente in
  `/wifiext/karma.txt`; probe wildcard → SSID dell'AP. Nessun deauth.
- Lista: una riga per SSID (`#` commenti), max 20 voci, 32 caratteri, la prima
  riga è l'SSID dell'AP. Se il file manca si usa la scansione delle reti vicine
  (SSID noti); in ultima istanza quattro nomi generici.
- Contatori UI: client associati, probe ricevuti, risposte, errori TX, SSID in
  lista, spazio SD. Anello di 4 probe in RAM, nessuna allocazione nel callback.

### Auto-Portal (`AutoPortal.cpp`)
- AP aperto `WiFi-Portal` canale 6 + **DNS captive** (`DNSServer`, wildcard verso
  l'IP dell'AP) + HTTP su porta 80 (`ESPAsyncWebServer`).
- Pagina da `/wifiext/portal.html` (solo SD, max 16 KB) con fallback interno;
  qualsiasi GET non riconosciuto mostra la pagina (trigger captive portal).
- `POST /login` accetta `username|user|email` e `password|pass|pwd`, accoda la
  credenziale e risponde con una pagina di conferma. Body oltre 1024 byte
  rifiutato con HTTP 413 prima di accodare.
- Credenziali scritte **solo su SD** in `/wifiext/portal.csv`; senza SD il
  portale non parte. La coda usa 6 slot (5 elementi reali, il sesto indica
  pieno) ed è svuotata dal loop principale, mai dal task async del server; il
  file si cancella da menu. Le credenziali perse per coda piena sono mostrate
  in UI.
- Server HTTP e pagina sono **statici e mai distrutti** tra una sessione e
  l'altra: `AsyncWebServer::end()` non attende i callback già in esecuzione.
  Allo stop si aspetta un grace period limitato (250 ms) e si svuota di nuovo
  la coda, evitando use-after-free su ESC con richieste in volo.
- Contatori UI: client associati, richieste HTTP, credenziali salvate, coda,
  errori di scrittura, spazio SD.

### Menu (`WifiExtMenu.cpp`)
- Voci: Responder, Karma AP, Auto-Portal, Stato e spazio SD, Cancella ntlm.txt,
  Cancella portal.csv. Con `ENABLE_OFFENSIVE=0` le tre funzioni mostrano
  "Funzione non inclusa in questa build".
- Stato: spazio SD, dimensione/presenza di `ntlm.txt`, `portal.csv`,
  `karma.txt`, `portal.html`.

## Comandi / tasti
- Menu: `;` `.` per navigare, ENTER per aprire, ESC (`` ` ``) per tornare.
- Durante Responder/Karma/Portal: **ESC** ferma lo strumento.
- Il pulsante ENTER non ha altre funzioni durante le sessioni.

## Formati file (SD, `/wifiext/`)
| File | Formato |
|------|---------|
| `ntlm.txt` | NetNTLMv2 hashcat 5600: `utente::dominio:challenge:proof:blob` |
| `portal.csv` | `tempo_s;ip;utente;password`, campi tra virgolette, header iniziale |
| `karma.txt` | un SSID per riga, `#` commenti, max 20 righe da 32 char |
| `portal.html` | HTML del portale, max 16 KB (solo SD) |

`ntlm.txt` e `portal.csv` sono cancellabili dal menu; nessun dato viene inviato
in rete e nessuna credenziale viene scritta nel log seriale.

## Limiti noti
- **NTLMv1 non supportato**: le risposte NTLMv1 da 24 byte (e qualsiasi Type3
  con NT response < 44 byte, minimo NetNTLMv2) vengono contate come "scartati"
  e non finiscono in `ntlm.txt`. Il file è solo 5600.
- **Solo SMB2** (Windows 7+/10/11). SMB1 e HTTP NTLM/proxy non sono implementati:
  un client che non apre la 445 non produce hash. Il target info non include il
  timestamp; alcuni criteri di gruppo molto restrittivi potrebbero rifiutare
  l'autenticazione.
- **Responder in STA**: serve una rete a cui connettersi; il traffico multicast
  LLMNR/mDNS deve essere inoltrato dall'AP della rete (client isolation off).
  Senza multicast resta NBT-NS broadcast.
- **Karma senza channel hopping**: l'AP sta su un solo canale, quindi risponde
  solo ai probe su quel canale; il canale è quello della prima rete della lista
  se visibile, altrimenti 6. Non c'è deauth (richiesto). Un solo BSSID è
  associabile (l'SSID dell'AP): gli altri SSID della lista ricevono la probe
  response ma non hanno un'istanza AP dedicata.
- **Auto-Portal**: SSID e canale fissi (`WiFi-Portal`, 6); i campi del form
  personalizzato devono usare i nomi elencati sopra; il timestamp del CSV è
  l'uptime in secondi (nessun RTC/NTP). Le password sono in chiaro sul file: la
  SD va trattata come materiale sensibile. Il body HTTP è limitato a 1024 byte
  (413), ma `ESPAsyncWebServer` parsa il form prima dell'handler: il limite
  impedisce l'accodamento, non l'allocazione iniziale della libreria.
- **Scritture SD**: controllo dello spazio libero prima di ogni append; se la
  write è parziale viene aggiunto un newline per isolare la riga troncata, che
  resta comunque incompleta (nessuna riscrittura dell'intero file). La
  cancellazione dal menu è logica: su FAT i blocchi possono restare recuperabili.
- Testi SSID/HTML arbitrari: il parser accetta solo UTF-8/ASCII; nomi lunghi
  vengono troncati (SSID a 32 byte).
- Buffer piccoli e statici (2 KB per SMB, 640 B per UDP, anello probe 4 voci):
  niente allocazioni nel path di ricezione; la pagina del portale è una stringa
  statica riusata tra le sessioni (max 16 KB).

## Correzioni post bug-test
Il bug test statico sul codice integrato ha prodotto finding P0/P1. In questa
worktree sono stati corretti:

- **P0 — SMB2 SESSION_SETUP**: `SecurityBufferOffset`/`Length` sono letti a
  `message + 76`/`+78` (layout MS-SMB2 2.2.5: StructureSize a 64, Flags a 66,
  SecurityMode a 67, Capabilities a 68, Channel a 72, PreviousSessionId a 80,
  buffer da 88). La guardia richiede `offset >= 88` e
  `offset + length <= len`; prima si leggeva il campo Channel (0) e il Type2
  non veniva mai inviato.
- **P0 — overflow LLMNR/mDNS**: `skipDnsName()` rifiuta nomi oltre 255 byte;
  le risposte verificano `2*nameLen + 30 <= sizeof(out)` (LLMNR) e
  `nameLen + 32 <= sizeof(out)` (mDNS) prima di scrivere. I nomi troncati o
  con label invalide vengono ignorati.
- **P1 — Type3**: offset validati con `size_t` e regola
  `off <= len && n <= len - off` per NT response, dominio e utente; NT response
  minima 44 byte (NetNTLMv2); le risposte NTLMv1 da 24 byte incrementano
  `g_skipped` e non producono righe 5600.
- **P1 — Auto-Portal use-after-free**: `AsyncWebServer` e `g_html` sono statici
  e non vengono mai distrutti all'uscita; dopo `end()` si attende un grace
  period limitato (250 ms) e si svuota di nuovo la coda. ESC con GET/POST in
  volo non libera più memoria usata dai callback.
- **P2 — cleanup e dati**: il Responder chiama `wifisetup::disconnect()` se la
  connessione fallisce o viene annullata; il CSV conserva `;` dentro le
  virgolette e antepone l'apostrofo alle formule anche dopo spazi iniziali; il
  body HTTP oltre 1024 byte riceve 413; le credenziali perse per coda piena
  sono mostrate in UI; l'append su SD controlla lo spazio libero e isola con un
  newline un'eventuale riga troncata; mDNS risponde in unicast alle query con
  bit QU.

Restano da eseguire in laboratorio i test dinamici: fuzz LLMNR/mDNS con scapy
(nomi 255/305/624 byte), SMB2 Type1→Type3 con hashcat 5600, ESC durante GET e
POST (10 cicli), SD piena/rimossa e verifica CSV con `;`, virgolette e newline.

## Build
- Env: `cardputer-adv-offensive` (`ENABLE_OFFENSIVE=1`), comando
  `python -m platformio run -e cardputer-adv-offensive`.
- Esito: **OK** (build completa 17m58s, con librerie da zero; rebuild
  incrementali dopo le correzioni, ultimo 26s).
- `firmware.bin`: **2.391.072 byte** (ELF flash 2.390.701 su 2.621.440,
  91,2%: restano ~230 KB).
- RAM statica: **118.992 byte su 327.680** (36,3%; include framebuffer e
  driver). Heap libero a runtime non misurato in questa sede.
- Nota di budget: il binario di questa worktree e' gia' al 91% dello slot; le
  altre 9 feature devono restare entro i ~230 KB residui, altrimenti serve
  escludere moduli dalla build o ridurre i font/dipendenze.
- Nessuna modifica a `main.cpp`, `platformio.ini`, `ModuleFlags.h`, `Config.cpp`,
  `WifiMenu.cpp`.

## Test manuali suggeriti (laboratorio, reti proprie)
1. **Responder**: collegare Cardputer e un PC Windows alla stessa rete di test
   (senza DNS o con nome inesistente). Dal PC: `nslookup nomeinesistente` →
   il contatore LLMNR sale. Aprire `\\nomeinesistente` in Explorer → il
   contatore "SMB auth" sale e `/wifiext/ntlm.txt` contiene una riga 5600.
   Verificare con `hashcat --identify` e `hashcat -m 5600 ntlm.txt wordlist.txt`
   su un hash fittizio. Fuzz: query LLMNR/mDNS con nomi da 255, 305 e 624 byte
   (scapy) non devono causare reset né righe spurie.
2. **Karma**: creare `/wifiext/karma.txt` con l'SSID di una rete di test;
   avviare Karma; da un telefono cercare reti e connettersi all'SSID dell'AP;
   verificare "Client" a 1 e "Probe/Risposte" in aumento.
3. **Auto-Portal**: avviare con SD inserita; collegare un telefono a
   `WiFi-Portal`; il browser deve aprire il portale; inserire credenziali
   fittizie (`test` / `test`) e verificare la pagina di conferma e la riga in
   `/wifiext/portal.csv`; dal menu "Cancella portal.csv" verificare la rimozione.
   Ripetere l'uscita con ESC durante un GET/POST (10 cicli) e con un POST oltre
   1024 byte (deve rispondere 413). Provare una password con `;` e virgolette
   e verificare che il CSV la conservi.
4. **Stop pulito**: uscire con ESC da ogni funzione e controllare che la radio
   sia spenta (menù WiFi mostra WiFi disattivato) e che l'AP non sia più visibile.
5. **Flag 0**: compilare l'env `cardputer-adv` e verificare che le voci non
   trasmettano e mostrino il messaggio di build.

## Rischi
- Uso solo su reti/dispositivi propri o con autorizzazione scritta; la raccolta
  di credenziali è vietata su terzi. Il firmware non esfiltra nulla, ma i file
  su SD contengono dati sensibili in chiaro.
- Un AP aperto può attirare client reali: fermare sempre le sessioni a fine test.
- `esp_wifi_80211_tx` e la modalità promiscua possono interferire con altre
  sessioni radio; non usare in contemporanea con sniffer/deauth di altri moduli.
