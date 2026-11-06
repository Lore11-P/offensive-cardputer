# CardputerFirmware v1.0

Firmware modulare per **M5Stack Cardputer ADV** (ESP32-S3FN8, 8 MB flash, **senza PSRAM**), scritto in C++17 con PlatformIO/Arduino. Interfaccia a menu con icone, navigazione da tastiera e moduli attivabili/disattivabili a runtime.

> **Avviso legale**
> Il firmware include moduli radio e IR. Le funzioni offensive (BLE spam, deauth Wi-Fi, evil twin, evil portal, jammer IR) sono **disabilitate di default** (`ENABLE_OFFENSIVE=0`) e nella build pubblica non vengono nemmeno compilate. L'uso di trasmissioni radio non autorizzate, il disturbo di reti altrui e la raccolta non consensuale di credenziali sono reati in Italia e in molti altri Paesi. Usa questo progetto **solo su dispositivi e reti tuoi**, oppure in un ambiente RF isolato con autorizzazione scritta. L'autore non si assume alcuna responsabilità per usi impropri.

## Stato dei moduli

| Componente | Stato | Note |
|---|---|---|
| Menu principale / UI / navigazione | ✅ | Carosello orizzontale a icone con animazione, griglia, breadcrumb e liste scrollabili |
| Impostazioni | ✅ | Luminosità, rotazione, **spegnimento automatico**, info dispositivo, **rete WiFi** (scansione + password), moduli on/off, export/import JSON, reset |
| Giochi | ✅ | Tetris, Breakout, Doom Lite, Snake, Pong, 2048, Flappy, Simon, Minesweeper e Connect 4 con record su SD |
| Infrarossi | ✅ | Parser `.ir` Flipper, ricerca per nome, preferiti, ripresa ultimo telecomando, tasti rapidi, scanner Power, TV-B-Gone, jammer opzionale, learn disabilitato |
| BLE | ✅ | Scanner passivo/attivo, spam Random/Samsung/Google/Windows opzionale |
| Chat BLE | ✅ | Messaggi cifrati AES e filtrati per destinatario, nickname unico e cronologia locale |
| Wi-Fi | ✅ | Scanner reti, deauth/evil twin/evil portal opzionali |
| Radar Wi-Fi/BLE | ✅ | Finder BLE hot/cold con HUD stile contatore Geiger, radar PPI, vista banda 2.4 GHz con TDM Wi-Fi/BLE, presenza CSI sperimentale |
| Web Radio | ✅ | 5 stazioni HTTP MP3 con decoder ESP32-audioI2S, volume, stato; usa il bus I2S dello speaker |
| microSD | ✅ | File browser, info spazio, screenshot BMP, backup config e punteggi |
| Database IR | 🟡 | 307 file .ir, 38 cartelle: 32 marche TV complete, 5 parziali, 3 coperte da Universale; verifica i codici sui tuoi apparecchi |
| Asset icone 32×32 | ✅ | BMP 24 bit generati, con fallback se assenti |
| Test automatici host | ⬜ | Predisposta la cartella `test/`, nessun test incluso |
| Verifica su hardware reale | ⬜ | Compilazione e filesystem verificati; test su device da fare |

## Funzionalità

- **Menu gerarchico** con carosello orizzontale animato, card centrale e anteprime laterali; le viste con molti elementi usano griglie o liste con auto-repeat.
- **Display** 240×135 in landscape, framebuffer 16 bpp (nessun flicker), tema neubrutalista e rotazione configurabile.
- **Batteria**: indicatore nella barra superiore con percentuale, colore per livello e stato di carica.
- **Storage** LittleFS + microSD opzionale (`/sd`), con fallback automatico e file browser.
- **Configurazione** in NVS, export/import JSON (`/config.json`) senza dati sensibili; backup e screenshot su SD.
- **Giochi interrompibili** con `` ` `` (ESC fisico), record persistenti su SD e ripristino della rotazione all'uscita.
- **IR**: NEC/NECext, Samsung32, LG, Sony/SIRC, RC5/RC5X, RC6, JVC, Kaseikyo (Panasonic), RCA e comandi raw; preferiti in NVS, scanner Power per marca e anteprima dei telecomandi.
- **IR UX**: Premi `F` in un telecomando per aggiungere/togliere il preferito. Lo scanner invia il tasto Power di ogni file e permette di salvarlo con `F`, ispirato a Ultimate-Remote.
- **IR ricerca**: la voce **Cerca** interroga tutto il database per marca o modello (sottostringa, senza distinzione di maiuscole) e mostra i telecomandi trovati, senza navigare cartella per cartella.
- **IR tasti rapidi**: dentro un telecomando `P` = Power, `M` = Mute, `=`/`+` = Volume +, `-` = Volume −, `C` = Canale +, `X` = Canale −. Il tasto sposta la selezione sul comando, lo invia e `ENTER` lo ripete.
- **IR ripresa**: l'ultimo telecomando aperto compare come prima voce ("Riprendi marca/modello") ed è salvato in NVS.
- **BLE**: scansione con nome, MAC e RSSI; spam con MAC casuale e modalità Random, Apple, Samsung, Google Fast Pair e Windows SwiftPair.
- **Chat BLE**: messaggi brevi (max 15 caratteri) scambiati via advertising BLE legacy, senza pairing, con payload cifrato AES-128-CBC. Ogni messaggio è indirizzato a un nickname: il destinatario lo decritta e lo mostra, gli altri lo ignorano. `ENTER` scrive, `` ` `` esce; il menu Chat permette di cambiare nickname (max 6 caratteri, con verifica di unicità via BLE) e destinatario predefinito, e mostra i dispositivi vicini. La cronologia locale conserva gli ultimi 8 messaggi. All'ingresso forza l'indirizzo pubblico, così il filtro dei propri pacchetti resta corretto anche dopo BLE spam.
- **Wi-Fi**: scanner con dettagli; deauth con limite di 60 s; evil twin/portal con credenziali **solo in RAM** e cancellate all'uscita.
- **Radar**: modulo passivo (nessuna trasmissione). *Find BLE* insegue un singolo beacon/tag con media mobile dell'RSSI, distanza stimata dal modello log-distanza, gauge, barra a 16 segmenti con Peak Hold e indicatori `WARMER`/`COLDER`/`HOTSPOT`, con **bussola** (gyro BMI270) e freccia verso la direzione stimata. La voce *Sensibilità* (livelli 1–5, salvata in NVS) regola quanto rapidamente il tic accelera con la distanza: 5 = reattivo già a media distanza, 1 = solo da molto vicino; la schermata mostra l'anteprima in ms a −70/−55/−40 dBm. *Radar BLE* mostra tutti i dispositivi come blip su un PPI con sweep continuo, ancorati al mondo tramite la bussola. *Banda 2.4 GHz* misura i canali Wi-Fi 1–13 con lo sniffer promiscuo in modo continuo (cornice che scorre sui canali, fase BLE, picchi) e sovrappone i tre canali advertising BLE 37/38/39 sulla stessa scala di frequenza, dichiarando il riparto TDM del tempo radio. *Presenza CSI* usa il motore Channel State Information dell'ESP32-S3 con calibrazione su ambiente vuoto.
- **Spegnimento automatico**: dopo un tempo configurabile di inattività (30 s, 1, 2, 5, 10, 15 o 30 min, oppure disattivato) il dispositivo **abbassa la luminosità** (12/255) e, se non arriva un tasto entro 10 secondi, si spegne. Qualsiasi tasto annulla e ripristina la luminosità. Nelle schermate di sola osservazione (radar, banda, CSI, radio) il timer è sospeso.
- **Web Radio**: stazioni pubbliche in MP3 su HTTP (`SomaFM Groove Salad/Drone Zone/Lush`, `Radio Paradise`, `FIP`). `;`/`.` regolano il volume, `ENTER` ferma, `` ` `` esce. Il decoder prende il bus I2S dello speaker: durante l'ascolto `M5.Speaker` è chiuso (quindi niente beep del radar) e viene riaperto all'uscita. I pin I2S si leggono a runtime da `M5.Speaker.config()`.
- **Rete WiFi**: configurazione in **Impostazioni → Rete WiFi**. Scansiona le reti vicine (SSID, RSSI, canale, cifratura, `*` = aperta), fa scegliere la rete dall'elenco, chiede solo la password e la salva; in alternativa permette l'inserimento manuale dell'SSID (reti nascoste), il test di connessione con IP/gateway/RSSI e la cancellazione delle credenziali.

## Hardware

| Voce | Valore |
|---|---|
| MCU | ESP32-S3FN8, 240 MHz, 320 KB SRAM |
| Flash | 8 MB (partizione app 4 MB + LittleFS 3,9 MB) |
| Display | 1.14" 240×135 |
| Tastiera | 56 tasti (frecce: `;` `.` `,` `/`) |
| IR TX | GPIO 44 |
| microSD | CS 12, MOSI 14, SCK 40, MISO 39 |
| USB | USB-C nativo (CDC) |

## Build da sorgente

Requisiti: Python 3.9+ e PlatformIO. Su Windows:

```powershell
python -m pip install --upgrade platformio
cd "cardputer firmware"
python -m platformio pkg install
python -m platformio run                 # firmware
python -m platformio run -t buildfs      # immagine LittleFS (icone + database IR)
```

Output:

- `.pio/build/cardputer-adv/firmware.bin`
- `.pio/build/cardputer-adv/littlefs.bin`
- `.pio/build/cardputer-adv/bootloader.bin`
- `.pio/build/cardputer-adv/partitions.bin`

### Build con funzioni offensive

Solo per test autorizzati:

```powershell
python -m platformio run -e cardputer-adv-offensive
python -m platformio run -e cardputer-adv-offensive -t buildfs
```

La variante usa la stessa partizione ma include i moduli offensivi. Se la pubblichi,
mantieni gli avvisi legali e limita l'uso a dispositivi e ambienti autorizzati.
Per aggiornare anche il flasher con questa build: `flasher\build.ps1 -Offensive`
(sostituisce i binari del flasher; `flasher\build.ps1` senza flag ripristina la pubblica).

### Flag dei moduli

Ogni modulo si esclude dalla build con il proprio flag (in `platformio.ini` o in `build_flags`):

| Flag | Default | Effetto quando vale 0 |
|---|---|---|
| `ENABLE_MODULE_CHAT` | 1 | esclude `src/modules/chat/`; richiede comunque `ENABLE_MODULE_BLE=1` |
| `ENABLE_MODULE_RADAR` | 1 | esclude `src/modules/radar/` (Finder BLE, PPI, banda, CSI) |
| `ENABLE_MODULE_RADIO` | 1 | esclude `src/modules/radio/` (web radio) |
| `ENABLE_RADAR_CSI` | 1 | compila il radar ma toglie la sola voce "Presenza CSI" |

### Upload

```powershell
python -m platformio run -t upload
python -m platformio run -t uploadfs
```

Se la porta non viene rilevata, specificarla:

```powershell
python -m platformio run -t upload --upload-port COM5
```

> **Dopo ogni upload serve un ciclo di alimentazione.** La USB della Cardputer è quella *nativa* dell'ESP32-S3: qualunque reset comandato da esptool (o via registro RTC) fa ripartire il chip in **download mode**, quindi a fine flash lo schermo resta nero. Staccare e riattaccare il cavo USB avvia il firmware. Per controllare che la flash contenga davvero la build appena compilata:
>
> ```powershell
> python -m platformio run -t upload --upload-port COM5   # scrive
> python -m platformio run -t buildfs                     # solo se servono icone/IR
> ```
> e, per una verifica byte-per-byte senza riscrivere:
> ```powershell
> python -m esptool --port COM5 --chip esp32s3 verify_flash 0x10000 .pio\build\cardputer-adv-offensive\firmware.bin
> ```

In caso di problemi, tenere premuto il tasto di boot (G0) e reinserire il cavo USB.

## Flash pronto all'uso (senza toolchain)

La cartella `flasher/` contiene:

- `flash.ps1` / `flash.bat`: installa `esptool` se manca e scrive i binari precompilati.
- `bin/`: bootloader, partizioni, firmware e LittleFS pronti.
- `web/`: flasher browser **ESP Web Tools** (Chrome/Edge desktop).

La confezione attuale include la build **offensiva** (`ENABLE_OFFENSIVE=1`).
Esegui `flasher\build.ps1` per rigenerare e pubblicare la variante pubblica.

Uso rapido:

```powershell
cd flasher
.\flash.bat              # rileva la porta automaticamente
.\flash.bat -Port COM5    # porta esplicita
.\flash.bat -Erase        # cancella tutta la flash prima di scrivere
```

Per il flasher web: apri un server locale nella cartella `flasher/web` e visita `http://localhost:8000`.

```powershell
cd flasher\web
python -m http.server 8000
```

Poi collega la Cardputer via USB e premi **Installa firmware**. Dettagli in `flasher/README.md`.

## Comandi da tastiera

| Tasto | Funzione |
|---|---|
| `;` / `.` | Su / giù (nei menu a carosello scorrono anche le card) |
| `,` / `/` | Sinistra / destra (carosello, griglia, giochi e FPS) |
| ENTER | Seleziona / conferma / spara / fai volare Flappy |
| `` ` `` (ESC fisico) | Indietro / esci dal gioco |
| SPAZIO | Drop Tetris, lancio Breakout, sparo Doom, volo Flappy |
| `F` | Aggiungi/rimuovi il telecomando dai preferiti (menu comandi IR) |
| `P` `M` `=`/`+` `-` `C` `X` | Dentro un telecomando: Power, Mute, Volume +, Volume −, Canale +, Canale − |
| Frecce | Ripeti la sequenza in Simon |
| `w` `a` `s` `d` | Movimento alternativo in Doom Lite |

## Struttura del progetto

```
cardputer firmware/
├── platformio.ini
├── partitions.csv
├── include/                  # header core
├── src/
│   ├── main.cpp
│   ├── core/                 # display, menu, navigazione, UI
│   ├── utils/                # storage, configurazione, rete, input testuale
│   └── modules/
│       ├── ble/              # scanner + spam (payload separati)
│       ├── chat/             # chat BLE broadcast
│       ├── wifi/             # scanner + test sicurezza
│       ├── radar/            # finder BLE, PPI, banda 2.4 GHz, presenza CSI
│       ├── radio/            # web radio MP3 via ESP32-audioI2S
│       ├── ir/               # telecomandi, preferiti, scanner, TV-B-Gone, jammer
│       ├── games/            # Tetris, Breakout, Doom, Snake, Pong, 2048, Flappy, Simon, Minesweeper, Connect 4
│       ├── sd/               # file browser, screenshot, backup
│       └── settings/         # impostazioni e info
├── data/
│   ├── icons/                # icone BMP 32x32
│   └── ir/<marca>/           # 307 file .ir, 38 cartelle
├── flasher/                  # script + binari + flasher web
└── test/                     # predisposto per test host
```

## Configurazione

Le impostazioni sono salvate in NVS nel namespace `cardputer-fw`:

- luminosità (10–255), rotazione (0–3);
- maschera moduli attivi (runtime);
- nickname Chat BLE (`chat_nick`, default `CPxxxx` dal MAC efuse, max 6 caratteri) e destinatario predefinito (`chat_to`, max 6 caratteri);
- durata spam BLE, intervallo advertising, ripetizioni IR, ritardo TV-B-Gone;
- spegnimento automatico in secondi (`sleep_timeout_sec`: 0 = disattivato, 30, 60, 120, 300, 600, 900, 1800), default 120.

Le credenziali Wi-Fi non stanno in `config.json`: sono salvate nel namespace NVS `cpfw-net` dal solo `utils/WifiSetup.cpp`, e si gestiscono da **Impostazioni → Rete WiFi**. Si cancellano con "Dimentica rete". Restano in chiaro nella NVS: è l'unico modo per riconnettersi senza reinserire la password a ogni avvio.

Export/import: **Impostazioni → Esporta/Importa config** (`/config.json`). Il JSON non contiene credenziali di rete.

## Formato database IR

I file `.ir` seguono il formato Flipper Zero:

```
Filetype: IR signals file
Version: 1
#
name: Power
type: parsed
protocol: Samsung32
address: 07 00 00 00
command: 02 00 00 00
```

Protocolli parsed supportati: `NEC`, `NECext`, `Samsung32`, `LG`, `Sony`/`SIRC12/15/20`, `RC5`, `RC5X`, `RC6`, `JVC`, `Kaseikyo` (Panasonic) e `RCA`. I comandi `type: raw` con `frequency` e `data` in microsecondi sono inviati con `sendRaw`. I file sono letti da LittleFS e, se assenti, da `/sd/ir/...`. I comandi con protocolli non riconosciuti sono marcati `[n/d]`; dettagli e provenienza sono in `data/ir/SOURCES.txt`. LittleFS è dimensionata a 3,9 MB e i nomi dei file devono restare entro 32 caratteri.

## Limiti noti

- I payload BLE sono esempi: Apple e Fast Pair cambiano identificativi nel tempo; aggiorna `src/modules/ble/BlePayloads.h` per i dispositivi più recenti.
- Gli ID Fast Pair/Samsung inclusi sono dimostrativi e possono non generare popup su tutti i telefoni.
- Il database IR non contiene codici TV dedicati per Skyworth, OnePlus e Realme: usa la cartella `Universale`. ASUS, Huawei, Majestic, Trevi e Inno-Hit hanno solo codici parziali o per altri dispositivi.
- Il modulo IR dispone solo del trasmettitore: `ENABLE_IR_LEARN=1` richiede un ricevitore esterno e codice dedicato.
- Deauth è limitato a 60 secondi per build e va usato solo su reti proprie.
- Evil Portal non salva nulla su disco; le credenziali restano in RAM, sono mostrate solo come conteggio e vengono cancellate all'uscita.
- Doom Lite è un raycaster senza texture, con 3 mappe e nemici semplici.
- Nessuna PSRAM: il framebuffer e le risorse sono dimensionati per la SRAM disponibile.
- Radar: la radio è una sola, quindi Wi-Fi e BLE non possono ascoltare insieme. La vista banda alterna le due finestre (TDM) e mostra il riparto del tempo; durante la finestra BLE il ricevitore promiscuo Wi-Fi è spento.
- Radar: NimBLE non espone il canale di ricezione di un advertising, quindi l'attività dei canali BLE 37/38/39 è **stimata** dal rate aggregato (ogni advertiser ripete lo stesso PDU sui tre canali); i canali Wi-Fi sono invece misurati davvero, uno per uno.
- Radar: il "single target" del progetto originale ricava anche un bearing con IMU e schermatura del corpo. Qui non c'è bussola: `WARMER`/`COLDER` indicano solo la variazione di distanza stimata, non la direzione.
- Radar CSI (`ENABLE_RADAR_CSI`): le API `esp_wifi_set_csi_rx_cb/_config/_set_csi` esistono su ESP32-S3 in arduino-esp32 2.0.16 e `CONFIG_ESP32_WIFI_CSI_ENABLED=1` nella sdkconfig precompilata, ma senza un access point che generi traffico arrivano solo i beacon degli AP vicini (10–30 frame/s). La sensibilità è quindi molto inferiore al progetto di riferimento (290–320 pkt/s iniettati). Va verificata su hardware e va ricalibrata a ogni cambio canale.
- Web Radio: solo stream **HTTP in MP3/AAC** (niente HTTPS: il decoder non ha un client TLS e la RAM è contata) e una stazione alla volta. Durante l'ascolto il bus I2S è del decoder, quindi i beep del radar non suonano; il decoder consuma ~40 KB di heap, quindi conviene non ascoltare con radar/BLE attivi insieme. Se una stazione cambia URL va aggiornata la tabella `kStations` in `src/modules/radio/RadioMenu.cpp`.
- Chat BLE: i messaggi sono offuscati con AES-128-CBC ma la chiave deriva dai nickname pubblici, quindi non è una cifratura end-to-end robusta: chi conosce mittente e destinatario può decifrare. La verifica di unicità del nickname funziona solo con gli altri dispositivi che hanno la Chat aperta. Funziona solo tra dispositivi compatibili con il protocollo v2 e non garantisce la consegna se il destinatario è fuori portata. L'ingresso in Chat forza l'indirizzo pubblico, quindi non eredita il MAC random del BLE spam.
- Il progetto è compilato e verificato a livello di build; il collaudo su hardware reale è a carico dell'utente.

## Licenza

Il codice del firmware è distribuito con licenza **MIT** (vedi `LICENSE`).
Il database IR proviene da Flipper-IRDB ed è rilasciato sotto **CC0 1.0**;
l'attribuzione è riportata in `data/ir/SOURCES.txt`. Le librerie di terze
parti mantengono le rispettive licenze.

## Crediti

M5Stack M5Cardputer/M5Unified/M5GFX, h2zero NimBLE-Arduino, crankyoldgit IRremoteESP8266, ESPAsyncWebServer/AsyncTCP (esphome), bblanchon ArduinoJson, Espressif ESP-IDF/Arduino core. Database IR: Flipper-IRDB di Lucaslhm, licenza CC0 1.0; dettagli in `data/ir/SOURCES.txt`. Ispirazione per le funzioni IR (ricerca per nome, tasti rapidi, preferiti): [Ultimate-Remote di geo-tp](https://github.com/geo-tp/Ultimate-Remote), progetto separato per Cardputer; qui il codice è indipendente. Rispetta le rispettive licenze.

Uso previsto: studio, prototipazione e test di sicurezza su dispositivi propri.
