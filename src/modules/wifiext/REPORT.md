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
  - **mDNS** UDP 5353 (gruppo 224.0.0.251), solo query A, risposta multicast.
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
  credenziale e risponde con una pagina di conferma.
- Credenziali scritte **solo su SD** in `/wifiext/portal.csv`; senza SD il
  portale non parte. La coda (6 voci) è svuotata dal loop principale, mai dal
  task async del server; il file si cancella da menu.
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
- **NTLMv1 non supportato**: le risposte da 24 byte vengono contate come
  "scartati" (hashcat userebbe `-m 5500`). Il file è solo 5600.
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
  SD va trattata come materiale sensibile.
- Testi SSID/HTML arbitrari: il parser accetta solo UTF-8/ASCII; nomi lunghi
  vengono troncati (SSID a 32 byte).
- Buffer piccoli e statici (2 KB per SMB, 640 B per UDP, anello probe 4 voci):
  niente allocazioni nel path di ricezione; il portale alloca la pagina su heap
  solo mentre è aperto.

## Build
- Env: `cardputer-adv-offensive` (`ENABLE_OFFENSIVE=1`), comando
  `python -m platformio run -e cardputer-adv-offensive`.
- Esito: **OK** (build completa 17m58s, con librerie da zero; rebuild
  incrementali dopo le ultime correzioni 2m44s e 1m33s).
- `firmware.bin`: **2.390.544 byte** (ELF flash 2.390.181 su 2.621.440,
  91,2%: restano ~226 KB).
- RAM statica: **118.992 byte su 327.680** (36,3%; include framebuffer e
  driver). Heap libero a runtime non misurato in questa sede.
- Nota di budget: il binario di questa worktree e' gia' al 91% dello slot; le
  altre 9 feature devono restare entro i ~226 KB residui, altrimenti serve
  escludere moduli dalla build o ridurre i font/dipendenze.
- Nessuna modifica a `main.cpp`, `platformio.ini`, `ModuleFlags.h`, `Config.cpp`,
  `WifiMenu.cpp`.

## Test manuali suggeriti (laboratorio, reti proprie)
1. **Responder**: collegare Cardputer e un PC Windows alla stessa rete di test
   (senza DNS o con nome inesistente). Dal PC: `nslookup nomeinesistente` →
   il contatore LLMNR sale. Aprire `\\nomeinesistente` in Explorer → il
   contatore "SMB auth" sale e `/wifiext/ntlm.txt` contiene una riga 5600.
   Verificare con `hashcat -m 5600 ntlm.txt wordlist.txt` su un hash di test.
2. **Karma**: creare `/wifiext/karma.txt` con l'SSID di una rete di test;
   avviare Karma; da un telefono cercare reti e connettersi all'SSID dell'AP;
   verificare "Client" a 1 e "Probe/Risposte" in aumento.
3. **Auto-Portal**: avviare con SD inserita; collegare un telefono a
   `WiFi-Portal`; il browser deve aprire il portale; inserire credenziali
   fittizie (`test` / `test`) e verificare la pagina di conferma e la riga in
   `/wifiext/portal.csv`; dal menu "Cancella portal.csv" verificare la rimozione.
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
