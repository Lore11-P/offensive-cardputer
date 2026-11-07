# Changelog

Tutte le modifiche rilevanti del progetto. Formato basato su Keep a Changelog.

## [Unreleased]

### Aggiunto

- **Scacchi** (`src/modules/games/Chess.cpp`, `Chess.h` e `ChessLogic.h`): partita contro il bot con tre difficolta' selezionabili a inizio partita (Facile, Medio, Difficile; alpha-beta con limite di nodi per la piu' alta). Regole complete: arrocco (con controllo delle case di transito), en passant, promozione automatica a donna, scacco, scacco matto, stallo, materiale insufficiente e regola delle 50 mosse. Comandi: frecce per muovere il cursore, INVIO per selezionare/muovere, `U` per annullare l'ultima coppia di mosse (giocatore + risposta del bot), `H` per lo storico navigabile con notazione semplificata (es. `e2-e4`, `O-O`, `a7xb8Q`). Vittorie persistenti su SD (`chess`). Icona vettoriale di fallback dedicata.
- **Pac-Man** (`src/modules/games/Pacman.cpp` + `Pacman.h`): labirinto 19×11 a tile da 10 px, 4 fantasmi con inseguimento greedy e modalità spaventata (200/400/800/1600 punti), pellet (10) e power pellet (50), 3 vite, livelli progressivi con fantasmi sempre più veloci e record su SD. Controllo con frecce o `w`/`a`/`s`/`d`; icona vettoriale di fallback dedicata.
- Breakout e Doom Lite ora salvano il **record su SD** e lo mostrano nell'HUD, come gli altri giochi: `scores::submit()` a fine partita, alla vittoria e all'uscita.
- Doom Lite più realistico: pareti texturizzate a mattoni con corsi sfalsati e giunti, variazione cromatica per tessera, illuminazione/fog per distanza e lato, pavimento in prospettiva, sfondo con gradiente più profondo, nemici animati (passo di camminata, occhi, bocca, braccia) con lampo bianco quando colpiti, arma con rinculo/bob, indicatore di colpo sul mirino, suoni per sparo/colpo/danno e punteggio (100 per nemico, 500 per settore) con record su SD.
- **Chat BLE** (`src/modules/chat/`, `ENABLE_MODULE_CHAT`): messaggi broadcast tra Cardputer vicini tramite advertising BLE legacy, senza pairing, con nickname configurabile (max 6 caratteri), destinatario predefinito, payload cifrato AES-128-CBC, filtro per nickname, verifica di unicità del nickname, cronologia degli ultimi 8 messaggi e conteggio dei dispositivi attivi. All'ingresso forza l'indirizzo pubblico per non ereditare il MAC random del BLE spam.
- Indicatore batteria nella barra superiore: percentuale, colore per livello e stato di carica, visibile in tutte le schermate con header.
- Modulo **Radar** (`src/modules/radar/`, `ENABLE_MODULE_RADAR`): modulo passivo che ricostruisce per comportamento osservato le funzioni del "Wi-Fi & BLE Radar ADV" closed source.
  - *Find BLE*: inseguimento di un singolo beacon/tag con media mobile esponenziale dell'RSSI, distanza stimata con modello log-distanza, gauge circolare, barra a 16 segmenti con Peak Hold, indicatori `WARMER`/`COLDER`/`HOTSPOT`, tic audio in stile contatore Geiger e timeout di link.
  - *Radar BLE*: vista PPI con sweep e blip stabili per MAC, posizionati per intensità di segnale, con elenco dei segnali più forti.
  - *Banda 2.4 GHz*: misura reale dei canali Wi-Fi 1–13 con sniffer promiscuo (frame/s per canale, peak hold), canali advertising BLE 37/38/39 sulla stessa scala di frequenza, riparto TDM del tempo radio e nota di coesistenza.
  - *Presenza CSI*: cattura Channel State Information con calibrazione su ambiente vuoto, spike rejector per gli aggiustamenti di AGC e indicatore di movimento (`ENABLE_RADAR_CSI`).
- Credenziali Wi-Fi in namespace NVS dedicato `cpfw-net`, cancellabili da menu.
- **Aggiornamento OTA via WiFi** (`include/OtaUpdate.h` + `src/utils/OtaUpdate.cpp`, voce **Impostazioni → Aggiorna firmware**): connette la rete salvata, scarica `flasher/bin/firmware.bin` dal ramo `main` del repository GitHub via HTTPS e lo scrive nella partizione OTA inattiva con `Update.h`. Mostra percentuale e MB scaricati, `` ` `` annulla senza toccare il firmware in esecuzione, al termine riavvia. URL fisso, firmware non firmato: vedi i limiti noti.
- `partitions.csv`: layout dual-OTA con `app0`/`app1` da 2 MB e `otadata`, LittleFS invariata a `0x410000`. Consente gli update OTA senza rischiare il firmware attivo.
- Nuova voce **Impostazioni > Rete WiFi**: scanner delle reti (SSID, RSSI, canale, cifratura), scelta dall'elenco, inserimento della sola password, salvataggio in NVS, test di connessione (IP/gateway/RSSI), inserimento manuale per reti nascoste e cancellazione delle credenziali.
- `utils/WifiSetup` (`include/WifiSetup.h` + `src/utils/WifiSetup.cpp`): unico punto che salva la password Wi-Fi (namespace `cpfw-net`), usato da Impostazioni > Rete WiFi.
- `utils/TextInput` (`include/TextInput.h` + `src/utils/TextInput.cpp`): editor di testo a schermo condiviso (campi normali, password mascherate, valori numerici), usato dalla configurazione rete e dalla ricerca IR.
- IR, ispirato a [Ultimate-Remote](https://github.com/geo-tp/Ultimate-Remote): voce **Cerca** che filtra per marca o modello su tutto il database `.ir` (sottostringa case-insensitive, massimo 40 risultati, `*` sui preferiti).
- IR: **tasti rapidi** nel telecomando — `P` Power, `M` Mute, `=`/`+` Volume +, `-` Volume −, `C` Canale +, `X` Canale −; il tasto seleziona il comando e lo invia, `ENTER` ripete.
- IR: **Riprendi** ultimo telecomando aperto come prima voce del menu, salvato in NVS (`ir-fav`/`last`).
- `MenuManager::selectIndex()` per spostare la selezione da codice (usato dalle scorciatoie IR).
- **Spegnimento automatico** per inattività (`include/PowerTimer.h` + `src/core/PowerTimer.cpp`): timeout configurabile da Impostazioni (30 s, 1, 2, 5, 10, 15, 30 min o disattivato), preavviso di 10 s con conto alla rovescia annullabile con qualsiasi tasto, poi `M5.Power.powerOff()` con fallback a deep sleep. Il timer è agganciato a `Navigation::poll()` (unico punto), si azzera a ogni tasto e riallinea lo stato della tastiera dopo il preavviso per non generare eventi fantasma. `powertimer::Hold` sospende il timer nelle schermate di monitoraggio (radar, banda, CSI, voli).
- Menu principale, menu Giochi e menu IR a carosello orizzontale animato: card centrale grande, anteprime laterali, indicatori di pagina e navigazione circolare con `,`/`/` o `;`/`.`.
- Quattro nuovi giochi: Flappy (fisica, tubi, record), Simon (sequenze con quattro pad colorati), Minesweeper (9x7, 10 mine, bandiere, flood fill, vittorie persistenti) e Connect 4 (7x6, IA con mosse vincenti/blocco, vittorie persistenti).
- Nuove icone vettoriali di fallback per Flappy, Simon, Minesweeper, Connect 4 e Preferiti.
- IR: preferiti persistenti in NVS (`ir-fav`, massimo 20) con `F` nel menu comandi e nella schermata scanner.
- IR: scanner Power per marca, ispirato a Ultimate-Remote: carica ogni telecomando, invia il comando Power/standby, permette di ripetere, cambiare file e salvarlo nei preferiti.
- IR: voce Preferiti nel menu principale, scanner per marca e indicatore `*` sui telecomandi preferiti.
- `MenuManager::setTextHandler` per gestire tasti stampabili senza uscire dal menu.
- **Modulo Radio** (`src/modules/radio/`, `ENABLE_MODULE_RADIO`): web radio MP3/AAC su HTTP con decoder `esphome/ESP32-audioI2S`. Cinque stazioni (SomaFM Groove Salad / Drone Zone / Lush, Radio Paradise, FIP), volume `;`/`.`, `ENTER` ferma. Il decoder prende il bus I2S: `M5.Speaker.end()` durante l'ascolto e `begin()` all'uscita; i pin I2S si leggono a runtime da `M5.Speaker.config()` (`pin_bck`, `pin_ws`, `pin_data_out`). Timer di spegnimento sospeso durante l'ascolto.
- Bussola a heading relativo (`include/Compass.h` + `src/core/Compass.cpp`): integrazione del giroscopio BMI270 con bias misurato a fermo e deadband.
- Radar: **freccia direzionale** nel finder BLE (24 settori da 15° che accumulano il picco di RSSI per heading, `LOCKED` entro ±20°, copertura in percentuale) e **tic Geiger** con intervallo e tono legati alla distanza.
- Radar: vista **Banda 2.4 GHz** riscritta come macchina a stati non bloccante: un canale per finestra con cornice che scorre, fase BLE, picchi e disegno a ~25 fps (prima misurava tutto e poi aspettava, sembrando in scansione continua).
- Radar: animazione fluida anche nel PPI, grazie al disegno durante la finestra di scansione (`scanWindow` con callback a 40 ms).
- Radar: **Sensibilità** configurabile (Radar > Sensibilità, livelli 1–5, persistita in NVS come `radar_s`) che regola la pendenza della curva distanza→frequenza del tic: con 5 il bip accelera già a media distanza, con 1 solo da molto vicino. La schermata mostra l'anteprima del tic a −70/−55/−40 dBm, e il finder indica il livello nel footer (`S3`). Nuovo helper `beepIntervalMs()` condiviso fra finder e anteprima.

### Modificato

- `partitions.csv`: da un solo slot app da 4 MB a due slot da 2 MB (`app0`/`app1`) per abilitare l'OTA. I device già flashati devono fare un flash USB una tantum del nuovo layout; LittleFS resta a `0x410000`, quindi i dati non si spostano.
- `SettingsMenu`: elenco dei moduli attivi scrollabile (6 voci su 5 righe visibili) con indicatore di posizione.
- Il menu principale e il menu Giochi non usano piu' la griglia 3x2: il carosello mostra una voce alla volta con le adiacenti in anteprima.
- Il menu Telecomandi usa una griglia 3 colonne; marche e file restano liste/griglie per una consultazione piu' rapida.
- Chat BLE: protocollo v2 con messaggi da 15 caratteri, chiave derivata da mittente/destinatario e verifica del nickname prima del salvataggio.
- README e CHANGELOG aggiornati con i nuovi comandi, i giochi, le funzioni IR, il modulo Radar, i flag e i limiti noti.

### Corretto

- **Crash entrando in Infrarossi e Giochi** (schermata nera e riavvio): era stack overflow del task dei menu, non un bug dei moduli. Il panic lo confermava (`Stack canary watchpoint triggered (loopTask)`). `display::drawIcon` teneva 4352 byte di buffer locali e i menu a carosello con molte icone **senza BMP** (Infrarossi 5 su 6, Giochi 8 su 10) passano dal fallback vettoriale, superando gli 8 KB di stack. Correzioni: buffer di `drawIcon` resi statici (−4 KB di stack per icona) e `ARDUINO_LOOP_STACK_SIZE=16384` (verificato nell'ELF: literal `0x4000`). *Info dispositivo* mostra ora lo **stack residuo minimo** per accorgersi in anticipo di un margine troppo stretto.

## [1.0.0] - 2026-10-31

### Aggiunto

- Struttura PlatformIO per M5Stack Cardputer ADV (ESP32-S3FN8, 8 MB, no PSRAM).
- Menu gerarchico con icone BMP 32x32, fallback vettoriale, breadcrumb e scrollbar.
- Framebuffer 16 bpp con rotazione configurabile e gestione display senza flicker.
- Storage LittleFS + microSD con fallback automatico e logger seriale.
- Configurazione persistente NVS, maschera moduli runtime, export/import JSON.
- Modulo Impostazioni: luminosità, rotazione, info chip/batteria/heap, moduli, reset.
- Giochi: Tetris (7 pezzi, ghost, livelli, hard drop), Breakout (3 vite, mattoni, rimbalzo angolare), Doom Lite (raycaster con 3 mappe, nemici, arma, HUD), Snake, Pong e 2048 con record persistenti su microSD.
- Menu principale a griglia di icone con navigazione a due assi e pagine.
- Modulo microSD: file browser con anteprima testuale, info spazio, screenshot BMP, backup di configurazione e punteggi, visualizzazione record.
- Modulo IR: parser Flipper `.ir`, NEC/NECext, Samsung32, LG, Sony/SIRC, RC5/RC5X, RC6, JVC, Kaseikyo (Panasonic), RCA, raw, TV-B-Gone, jammer opzionale, learn disabilitato di default.
- Modulo BLE: scanner con deduplica e dettagli; spam con MAC casuale e modalita' Random, Apple, Samsung, Google Fast Pair e Windows SwiftPair, durata/intervallo da configurazione.
- Modulo Wi-Fi: scanner asincrono con dettagli; deauth raw 802.11 (max 60 s), evil twin, evil portal con DNS captive e credenziali solo in RAM.
- Database IR con 307 file `.ir` e 38 cartelle: 32 marche TV complete, 5 parziali e 3 coperte dalla cartella Universale; include Kaseikyo Panasonic e fallback raw Pronto.
- Flasher PowerShell con installazione automatica di esptool e flasher web ESP Web Tools.
- Ambiente PlatformIO `cardputer-adv-offensive` per build di test con `ENABLE_OFFENSIVE=1`, pubblicabile nel flasher con `build.ps1 -Offensive`.
- README con build, flash, comandi, struttura, limiti e avviso legale.
- Build pubblica e offensiva verificate: 1.519.433 B e 1.598.569 B di flash usata; LittleFS da 3,9 MB con 307 file IR.

### Corretto

- LittleFS portata a 3,9 MB e rinominati due file oltre 32 caratteri: l'immagine include ora tutti i 307 file IR senza errori di spazio o di nome. Richiede il reflash di `partitions.bin` e `littlefs.bin` (il flasher lo fa automaticamente).
- I comandi parsed sono marcati come supportati solo se il protocollo è riconosciuto; prima venivano tutti mostrati come `[n/d]`.
- Indirizzi e comandi Flipper a 4 byte vengono letti in little-endian: NECext, Kaseikyo, Samsung32 e gli altri protocolli usano ora il valore corretto anche per address/command a 16 bit.
- Corretto il puntatore al font in 2048 che impediva la compilazione con M5GFX.

### Licenza

- Aggiunta licenza MIT per il firmware; database IR CC0 1.0 con attribuzione in `data/ir/SOURCES.txt`.

### Sicurezza

- `ENABLE_OFFENSIVE=0` di default: le funzioni offensive non sono compilate nella build pubblica.
- Avvisi legali obbligatori prima di ogni trasmissione offensiva.
- Credenziali del portale mai persistite, mostrate solo come conteggio, cancellate all'uscita.
- Nessun dato sensibile viene scritto su NVS, LittleFS o SD.

### Limitato / noto

- Payload BLE di esempio: gli identificativi dei dispositivi cambiano nel tempo.
- IR: nessun codice TV dedicato per Skyworth, OnePlus e Realme (usare Universale); ASUS, Huawei, Majestic, Trevi e Inno-Hit hanno copertura parziale o non-TV.
- IR learn richiede un ricevitore esterno (non incluso).
- Doom Lite senza texture e con IA nemica semplice.
- I record dei giochi vengono salvati su microSD; senza SD non sono persistenti.
- Non eseguito su hardware reale in questa fase: verificare pin SD, IR e comportamento radio sul dispositivo.
