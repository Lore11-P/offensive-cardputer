# 04 - BLE HID + Bad BLE

## Cosa è stato implementato

- **Tastiera BLE HID over GATT** con NimBLE 1.4.3:
  - nome dispositivo `ORBIT-<nick>` (nick da `config::chatNickname()`, ripulito;
    default `ORBIT-CPxxxx`);
  - servizio HID `0x1812` con Device Information e Battery Service;
  - Report Map tastiera boot da 8 byte (modificatori, riservato, 6 tasti),
    Report ID 1 + report caratteristica `0x2A4D` (input/output) e Report
    Reference `0x2908`;
  - Boot Keyboard Input/Output Report (`0x2A22` / `0x2A32`), così il
    dispositivo funziona anche in boot protocol;
  - LED host (Caps/Num/Scroll) letti dal report output e mostrati a schermo;
  - pairing Just Works con bonding (`setSecurityAuth(true,false,true)`,
    IOCap NoInputNoOutput); l'host avvia la cifratura quando sottoscrive il
    report (la caratteristica input è `READ_ENC`);
  - advertising con aspetto `0x03C1` (HID keyboard) e UUID `0x1812`.
- **Layout host IT / US** selezionabile e persistito in NVS
  (`orbit_blehid/layout`). La traduzione è carattere -> tasto fisico del layout
  scelto: lettere, cifre, punteggiatura, AltGr per `@ # [ ] { } €` su IT,
  lettere accentate italiane da script (`à è é ì ò ù ç ° § £`).
- **Tastiera dal vivo**: i tasti premuti sulla Cardputer vengono inviati
  subito all'host (caratteri, Invio, Tab, Backspace con auto-repeat,
  CTRL/ALT/ALTGR + tasto). ESC esce dal modulo e non viene inviato.
- **Bad BLE**: elenco script in `/badusb` (prima microSD, fallback LittleFS),
  scelta, avviso legale, attesa connessione + pairing, conferma esplicita,
  esecuzione con avanzamento (riga/totale, barra, anteprima, tasti inviati),
  abort con ESC e arresto se l'host si disconnette.
- **Deinit NimBLE all'uscita** (`NimBLEDevice::deinit(true)` dopo stop
  advertising), stato heap nel log seriale. Se Chat/BLE/RADAR hanno lasciato lo
  stack acceso, il modulo ferma advertising e scan e ricrea lo stack con il
  proprio nome: nessun crash e nessun modulo BLE in esecuzione in parallelo.
- UI italiana con `display::header` / `display::footer`, menu
  `Tastiera BLE`, `Bad BLE`, `Layout`, `Info`.

## Parser Ducky

`BadUsbScript.cpp` è compilato solo con `ENABLE_MODULE_BADUSB=1`, mentre
`cardputer-adv-offensive` usa `ENABLE_MODULE_BADUSB=0` (niente TinyUSB). Per
non toccare il modulo USB e non trascinare dipendenze USB nel modulo BLE, Bad
BLE usa un parser Ducky minimo in `BleHidScript.cpp`, con gli stessi comandi e
la stessa semantica di `BadUsbScript`:

- `REM`, `#`, `//`, metadati (`ID:`, `NAME:`, `AUTHOR:`, `VERSION:`, `DESCRIPTION:`);
- `STRING`, `DELAY`, `DEFAULT_DELAY`/`DEFAULTDELAY`, `REPEAT`;
- tasti nominati (ENTER, TAB, ESC, BACKSPACE, DELETE, frecce, F1-F12, ecc.);
- combinazioni `CTRL`/`SHIFT`/`ALT`/`GUI` + tasto;
- righe singolo carattere;
- sequenze `\n`, `\t`, `\\` e UTF-8 italiano in `STRING`.

Limiti: 256 byte per riga (righe più lunghe ignorate), 5000 righe, `REPEAT`
massimo 1000. Il modulo BadUSB USB non è stato modificato.

## Comandi / tasti

Menu `BLE HID`:

| Voce | Funzione |
|------|----------|
| Tastiera BLE | avvia HID, attende pairing, poi digitazione dal vivo |
| Bad BLE | sceglie ed esegue uno script da `/badusb` (solo con `ENABLE_OFFENSIVE=1`) |
| Layout | Italiano / US, salvato in NVS |
| Info | nome, layout, limiti |

- `ENTER` conferma, `ESC` (`backtick`) torna indietro.
- In digitazione: caratteri e simboli come stampati; `DEL` = Backspace;
  `ENTER` = Invio; `TAB` = Tab; `CTRL`/`ALT`/`OPT` come modificatori;
  `ESC` esce senza inviare nulla.
- In Bad BLE: `ESC` annulla lo script in qualsiasi momento.

## File

- `src/modules/blehid/BleHidKeyboard.h/.cpp` - stack NimBLE, HID, layout, invio.
- `src/modules/blehid/BleHidScript.h/.cpp` - parser Ducky e runner Bad BLE.
- `src/modules/blehid/BleHidMenu.cpp` - menu e UI.
- `src/modules/blehid/REPORT.md` - questo file.

Nessuna modifica a `main.cpp`, `platformio.ini`, `ModuleFlags.h`, `Config.cpp`
o al modulo BadUSB.

## Dimensione firmware

Build `cardputer-adv-offensive` (PlatformIO, `-Os`):

- `firmware.bin`: **2.393.344 byte** (2,28 MB).
- Memoria statica: **115.616 / 327.680 byte RAM (35,3%)**;
  flash ELF **2.392.981 / 2.621.440 byte (91,3%)**. Restano circa 228 KB
  nello slot applicativo da 2,5 MB.
- Heap libero a runtime: stampato nel log seriale all'inizio e alla fine della
  sessione (`BLE HID: pronto ... heap=...`, `BLE HID: stack rilasciato ...`).

## Test manuali suggeriti

1. Build: `python -m platformio run -e cardputer-adv-offensive`.
2. Tastiera BLE:
   - aprire `BLE HID > Tastiera BLE`, accettare l'avviso legale;
   - associare `ORBIT-CPxxxx` da Windows/Android/Linux;
   - aprire un editor di testo sul PC e digitare `ciao 123 !?`, Invio,
     Backspace, Tab, una combinazione CTRL+A/CTRL+C;
   - verificare Caps Lock: il LED `CAPS` deve comparire sulla Cardputer;
   - ESC: il modulo deve uscire e lo stack BLE essere rilasciato.
3. Layout: selezionare `US` e verificare che gli stessi caratteri arrivino
   corretti con host in layout US; tornare a `IT`.
4. Bad BLE:
   - creare `/badusb/hello.txt` sulla SD:
     ```
     REM test
     DELAY 500
     STRING hello
     ENTER
     ```
   - eseguire da `Bad BLE`, confermare l'avviso e la conferma dopo il pairing;
   - verificare l'avanzamento e l'abort con ESC;
   - provare lo stesso file su LittleFS (senza SD) per il fallback.
5. Rientri: aprire/chiudere il modulo 5 volte di seguito, alternando
   `Tastiera BLE` e `Bad BLE`, verificando heap stabile e nessun crash.
6. Coesistenza: aprire Chat o BLE Spam, uscire, poi BLE HID; e viceversa.

## Rischi e limiti noti

- Il layout deve corrispondere a quello configurato nel sistema operativo
  dell'host. Con `Italiano` e host US (o viceversa) i simboli non coincidono.
- Su layout IT non esistono tasti diretti per `` ` `` e `~`: non vengono
  inviati; `[ ] { } @ #` usano AltGr. Su layout US le lettere accentate
  italiane non sono inviabili.
- Caratteri Unicode fuori dalla tabella (emoji, altre lingue) vengono ignorati.
- M5Cardputer 1.1.1 non espone il layer Fn nella `KeysState`: in digitazione
  dal vivo non è possibile inviare frecce/F1-F12 (gli script sì). Fn non viene
  propagato come modificatore.
- Il pairing dipende dall'host: alcuni richiedono di confermare il dispositivo;
  il modulo attende la sottoscrizione dei report prima di abilitare l'invio.
- La build pubblica (`cardputer-adv`) ha `ENABLE_OFFENSIVE=0`: Bad BLE è un
  no-op e la voce non compare. La tastiera BLE resta disponibile.
- Le associazioni restano nel NVS del controller e non c'è una UI per
  cancellarle: si gestiscono dalle impostazioni Bluetooth dell'host.
- Nessun mouse, nessun HID USB: solo tastiera BLE.
