# OrbitOS 3.0 — cosa non è stato possibile fare

Data: 2026-11-27. Base: ESP-IDF 5.4, C++17, Cardputer ADV (ESP32-S3FN8, 8 MB,
senza PSRAM). Questa è la lista onesta dei limiti e delle funzioni 2.x non
portate. Il resto è in `docs/3.0-STATO.md` e nei README dei componenti.

## 1. Linguaggio e framework

- Il firmware 2.x è Arduino/PlatformIO C++17; il 3.0 è **ESP-IDF 5.4 C++17**,
  senza Arduino. Non è un rewrite in un linguaggio diverso (es. Rust/C puro):
  la UI e i giochi sono classi C++ e una riscrittura totale avrebbe invalidato
  il lavoro già fatto. La differenza reale è il framework e le API native IDF.

## 2. Funzioni 2.x non portate

| Funzione 2.x | Stato 3.0 | Motivo |
|---|---|---|
| Web Radio (5 stazioni MP3 HTTP) | **non portata** | il player locale MP3/WAV c'è; manca lo streaming HTTP con metadati ICY |
| BLE HID / BadUSB | **non portato** | richiede TinyUSB/HID e un altro profilo NimBLE; non era nel primo blocco |
| BLE+ (AirTag spoof, Flipper finder, GATT, skimmer, wardriving, cattura BLE) | **non portato** | modulo 2.x molto grande, dipende da BLE HID/GATT |
| WiFi Lab (PMKID, client scan, spettro) e WPA-SEC | **non portati** | il WiFi 3.0 copre scanner, deauth, beacon spam, evil twin/portal, canali |
| WiFi+ (Responder, Karma, Auto-Portal) | **parziale** | c'è l'evil twin + captive portal; mancano Responder/Karma |
| Capture/PCAP (browser, hc22000, EAPOL) | **non portato** | richiede sniffer promiscuo, parser pcap e conversione hashcat |
| Radar (Finder BLE, PPI, banda 2.4 GHz, CSI) | **non portato** | richiede CSI/promiscuo e UI dedicata |
| Sensori (livella, goniometro, pedometro, torcia) | **non portati** | manca il driver BMI270 su IDF |
| ESP-NOW (file/clipboard/comandi) | **non portato** | modulo 2.x da 90 KB, protocollo e UI non riscritti |
| USB file transfer (CDC) | **non portato** | conflitto con la console USB-Serial/JTAG usata per i test |
| OTA firmware firmato | **non portato** | la voce Impostazioni > Aggiornamenti è un segnaposto |
| Store canale nativo / pacchetti `.oap` / registry | **non portati** | lo Store 3.0 installa solo giochi Wren firmati ECDSA + SHA-256 |
| Sleep timer, toggle moduli, export/import config, reset | **non portati** | Impostazioni 3.0: WiFi, tema, luminosità, rotazione, nickname, storage, info, avviso legale |
| Screenshot BMP | **portato** | verificato solo a livello di codice, non su SD |
| Punteggi | **portato** | NVS `orbit-scores`; migrazione automatica da `/scores.txt` non fatta |
| Zorbo | **portato** | punti solo dopo `legalWarning`, a fine azione |

## 3. Limiti dei moduli portati

- **Musica**: MP3 e WAV PCM16; FLAC/M4A/AAC/OGG/Opus sono elencati ma danno
  "formato non supportato". Niente seek/shuffle/repeat, durata MP3 stimata CBR.
- **Giochi Wren**: il framebuffer 16bpp resta allocato anche durante la VM
  (niente disegno diretto, niente calo di colori all'uscita), creato una sola
  volta nella rotazione salvata. Per far spazio a Wren si ferma il WiFi e si
  usa uno stack task da 24 KB; gate 136 KB, 2048/breakout/flappy partono con
  ~164 KB liberi. I giochi Wren forzano il landscape se la rotazione utente è
  portrait. Non è stato testato l'uscita fisica con ESC né la resa a schermo
  di ogni gioco.
- **Deauth**: IDF 5.4 blocca i frame deauth con un controllo chiuso in
  `libnet80211.a`. Il binario consegnato è **patchato** con
  `tools/patch_raw_tx.py` (prologo della funzione → `movi.n a2,0; retw.n`) e
  la trasmissione è stata verificata via seriale (`deauth-tx=OK`). Un
  `idf.py build` normale rigenera un binario **senza** patch: usare
  `tools/build_offensive.ps1` o rifare la patch.
- La patch è stata verificata con un solo frame broadcast; non è stato testato
  un deauth reale contro un client per 60 secondi, né il comportamento del
  driver a lungo termine.
- **BLE spam**: usa l'indirizzo del controller, nessuna rotazione MAC; i popup
  su telefoni moderni non sono stati provati. Lo scan BLE è verificato.
- **Evil twin/portal**: non provato con un telefono reale; le credenziali
  restano in RAM e non vengono scritte.
- **IR**: nessun RX/learn, niente scanner per marca/TV-B-Gone; carrier fisso
  38 kHz; file `.ir` oltre 16 KB rifiutati (15 dei 307 campioni).
- **WiFi**: country forzato IT/1–13; max 24 AP; niente whitelist/NetGuard.
- **Strumenti**: TOTP e note in chiaro in NVS; fuso fisso italiano; QR fino a
  78 byte; niente impostazione manuale dell'ora.

## 4. Test non fatti

- Nessun test visivo/manuale dei menu sul dispositivo: i test sono via seriale.
- Nessun test di trasmissione RF prolungato, nessun test su reti/persone terze
  (non autorizzato), nessun test con telefoni per BLE spam/portal.
- Nessun test con elettrodomestici reali per i codici IR.
- Store/HTTP: la connessione WiFi è stata verificata; il download di un gioco
  dallo Store non è stato rieseguito in questa sessione.

## 5. Dati e partizioni

- NVS, LittleFS e `/games` sulla SD non sono stati cancellati. La LittleFS non
  viene più formattata automaticamente (`format_if_mount_failed=false`).
- Le impostazioni 2.x (luminosità, tema, rotazione, nickname) vengono importate
  dal namespace `cardputer-fw` al primo avvio.
- Un `flash` completo con la tabella partizioni nuova (M3, con `orbit_nap`) non
  è stato fatto: la tabella resta quella 2.x, quindi lo Store nativo e lo
  scratch per le app native non ci sono.

## 6. Spegnimento PC

Lo spegnimento del PC è stato richiesto e viene eseguito a fine sessione con un
ritardo di sicurezza (annullabile con `shutdown /a`). Non è una funzione del
firmware.
