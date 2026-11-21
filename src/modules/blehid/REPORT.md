# 04 - BLE HID + Bad BLE

## Correzioni di sicurezza P0/P1

- **P0 boot report `0x2A22`**: notificato solo con link cifrato e peer
  autorizzato, solo se sottoscritto e solo in Boot Protocol. Non è conteggiato
  in `subscribed()` e non viene mai inviato insieme a `0x2A4D`.
- **P0 autorizzazione host**: un solo central per volta (gli altri vengono
  disconnessi), indirizzo del peer mostrato sulla Cardputer, conferma esplicita
  `ENTER/ESC` prima della tastiera e prima di Bad BLE. La sessione parte solo
  con `sec_state.encrypted`; con `kRequireAuthenticated=true` richiede anche
  `sec_state.authenticated`.
- **P1 sicurezza/notifiche**: `startSecurity()` in `onConnect`; lo stato UI
  distingue attesa, pairing, autorizzazione e report pronti; nessun doppio
  invio sulle due caratteristiche; LED accettati solo da peer autorizzato.
- **P1 handle/limiti**: `conn_handle` tracciato, disconnessioni estranee
  ignorate, chiusura della sessione solo a server senza peer; gate di ~32 KB
  di heap prima di NimBLE e rifiuto se un altro modulo ha un host connesso.
- **P1 parser**: `CTRL C` e `GUI R` non aggiungono più Shift; self-check del
  parser all'avvio di ogni script.

## Cosa è stato implementato

- **Tastiera BLE HID over GATT** con NimBLE 1.4.3:
  - nome dispositivo `ORBIT-<nick>` (nick da `config::chatNickname()`, ripulito;
    default `ORBIT-CPxxxx`);
  - servizio HID `0x1812` con Device Information e Battery Service;
  - Report Map tastiera boot da 8 byte (modificatori, riservato, 6 tasti),
    Report ID 1 + report caratteristica `0x2A4D` (input/output) e Report
    Reference `0x2908`;
  - Boot Keyboard Input/Output Report (`0x2A22` / `0x2A32`); il boot input
    viene notificato **solo** in Boot Protocol, su link cifrato e con peer
    autorizzato;
  - LED host (Caps/Num/Scroll) letti dal report output e mostrati a schermo;
  - pairing con bonding (`setSecurityAuth(true,false,true)`, IOCap
    NoInputNoOutput) e `startSecurity()` in `onConnect`; l'host deve completare
    la cifratura, poi l'utente autorizza esplicitamente il peer sulla Cardputer;
  - advertising con aspetto `0x03C1` (HID keyboard) e UUID `0x1812`.
- **Sicurezza / autorizzazione host**:
  - un solo central alla volta: il primo che si connette viene tracciato per
    `conn_handle`, gli altri vengono disconnessi subito;
  - la UI mostra l'indirizzo del peer e non dice mai "Collegato" prima di
    `sec_state.encrypted`;
  - prima di abilitare la tastiera (e prima di Bad BLE) serve una conferma
    esplicita `ENTER = autorizza / ESC = rifiuta` sulla Cardputer;
  - nessun report viene inviato a link non cifrato o a peer non autorizzato;
  - `subscribed()` considera solo il report input `0x2A4D`; `0x2A22` non rende
    pronta la sessione;
  - una disconnessione di un handle diverso da quello autorizzato non tocca
    l'host legittimo.
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
  advertising), stato heap nel log seriale. Prima di inizializzare:
  - gate di ~32 KB di heap libero (`StartError::LowHeap`);
  - se un altro modulo (Chat/BLE+/Radar) ha ancora un host connesso, il modulo
    non fa deinit e si ferma con `StartError::PeerConnected`;
  - se lo stack è attivo ma senza host, ferma advertising e scan, poi lo
    ricrea in modo esclusivo con il proprio nome.
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
- combinazioni `CTRL`/`SHIFT`/`ALT`/`GUI` + tasto; le lettere maiuscole
  nel comando non aggiungono Shift (`CTRL C`, `GUI R`), come in DuckyScript;
- righe singolo carattere;
- sequenze `\n`, `\t`, `\\` e UTF-8 italiano in `STRING`.

Il parser esegue un self-check all'avvio di ogni script (`CTRL C`, `GUI R`,
`STRING hello`) e rifiuta l'esecuzione se il controllo fallisce.

Limiti: 256 byte per riga (righe più lunghe ignorate), 5000 righe, `REPEAT`
massimo 1000. Il modulo BadUSB USB non è stato modificato.

## Comandi / tasti

Menu `BLE HID`:

| Voce | Funzione |
|------|----------|
| Tastiera BLE | avvia HID, attende pairing e autorizzazione, poi digitazione dal vivo |
| Bad BLE | sceglie ed esegue uno script da `/badusb` (solo con `ENABLE_OFFENSIVE=1`) |
| Layout | Italiano / US, salvato in NVS |
| Info | nome, layout, modello di sicurezza |

- `ENTER` conferma, `ESC` (`backtick`) torna indietro.
- Dopo il pairing compare `Autorizza host` con l'indirizzo del peer:
  `ENTER` abilita la sessione, `ESC` rifiuta e chiude.
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

- `firmware.bin`: **2.397.568 byte** (2,29 MB).
- Memoria statica: **115.648 / 327.680 byte RAM (35,3%)**;
  flash ELF **2.397.201 / 2.621.440 byte (91,4%)**. Restano circa 224 KB
  nello slot applicativo da 2,5 MB.
- Heap libero a runtime: stampato nel log seriale all'inizio e alla fine della
  sessione (`BLE HID: pronto ... heap=...`, `BLE HID: stack rilasciato ...`).

## Test manuali suggeriti

1. Build: `python -m platformio run -e cardputer-adv-offensive`.
2. Tastiera BLE con pairing:
   - aprire `BLE HID > Tastiera BLE`, accettare l'avviso legale;
   - associare `ORBIT-CPxxxx` da Windows/Android/Linux;
   - finché il pairing non è completo la UI deve dire "attendo pairing",
     mai "Collegato";
   - verificare l'indirizzo del peer nella schermata `Autorizza host`;
   - `ESC` = rifiuta: nessun tasto deve arrivare all'host;
   - riprovare, `ENTER` = autorizza;
   - digitare `ciao 123 !?`, Invio, Backspace, Tab, CTRL+A/CTRL+C;
   - verificare Caps Lock: il LED `CAPS` deve comparire sulla Cardputer;
   - ESC: il modulo esce e lo stack BLE viene rilasciato.
3. P0 boot report:
   - con un central che rifiuta il pairing, verificare che non arrivino
     notifiche su `0x2A4D` né su `0x2A22`;
   - con nRF Connect, attivare il Boot Protocol: le notifiche devono passare
     solo su `0x2A22`, mai su entrambe le caratteristiche.
4. P0 secondo host:
   - collegare un secondo telefono mentre il PC autorizzato è in sessione:
     il secondo deve essere disconnesso e il PC deve continuare a digitare;
   - disconnettere il secondo: la sessione del PC non deve chiudersi.
5. Layout: selezionare `US` e verificare che gli stessi caratteri arrivino
   corretti con host in layout US; tornare a `IT`.
6. Bad BLE:
   - creare `/badusb/hello.txt` sulla SD:
     ```
     REM test
     DELAY 500
     STRING hello
     ENTER
     ```
   - eseguire da `Bad BLE`, confermare avviso legale, autorizzazione host e
     conferma finale; verificare che ENTER/ESC non vengano inviati all'host;
   - provare `/badusb/ctrl.txt` con `CTRL C` e `GUI R`: nessun carattere
     maiuscolo inatteso (niente Shift aggiunto);
   - verificare l'avanzamento e l'abort con ESC;
   - provare lo stesso file su LittleFS (senza SD) per il fallback.
7. Rientri: aprire/chiudere il modulo 5 volte di seguito, alternando
   `Tastiera BLE` e `Bad BLE`, verificando heap stabile e nessun crash.
8. Coesistenza: aprire Chat o BLE Spam, uscire, poi BLE HID; e viceversa.
   Con un host ancora connesso a Chat/Radar/BLE+ il modulo deve rifiutare
   l'avvio (`BLE già in uso`) senza fare deinit.

## Rischi e limiti noti

- Il pairing è Just Works (non autenticato a livello BLE): l'autorizzazione
  vera è la conferma esplicita sull'indirizzo del peer mostrato sulla
  Cardputer. Non esiste una lettura del nome del central via GATT; si mostra
  l'indirizzo di identità.
- La sessione richiede la sottoscrizione di `0x2A4D`: un host che usa solo il
  boot input `0x2A22` non viene considerato pronto (per scelta di sicurezza).
- Il pairing dipende dall'host: se non completa la cifratura la UI resta su
  "attendo pairing" e non invia nulla; l'utente può uscire con ESC.
- Un solo peer alla volta: le connessioni successive vengono disconnesse.
- Se un altro modulo BLE ha un host connesso, BLE HID rifiuta l'avvio invece di
  deinitializzare lo stack; chiudere prima quella sessione.
- Con heap libero inferiore a ~32 KB l'avvio viene rifiutato.
- Il layout deve corrispondere a quello configurato nel sistema operativo
  dell'host. Con `Italiano` e host US (o viceversa) i simboli non coincidono.
- Su layout IT non esistono tasti diretti per `` ` `` e `~`: non vengono
  inviati; `[ ] { } @ #` usano AltGr. Su layout US le lettere accentate
  italiane non sono inviabili.
- Caratteri Unicode fuori dalla tabella (emoji, altre lingue) vengono ignorati.
- M5Cardputer 1.1.1 non espone il layer Fn nella `KeysState`: in digitazione
  dal vivo non è possibile inviare frecce/F1-F12 (gli script sì). Fn non viene
  propagato come modificatore.
- La build pubblica (`cardputer-adv`) ha `ENABLE_OFFENSIVE=0`: Bad BLE è un
  no-op e la voce non compare. La tastiera BLE resta disponibile.
- Le associazioni restano nel NVS del controller e non c'è una UI per
  cancellarle: si gestiscono dalle impostazioni Bluetooth dell'host.
- Nessun mouse, nessun HID USB: solo tastiera BLE.
