# FIX_MEDIA - BLE HID / Chat / Audio

Worktree: `orbit-wt-fix-media`, branch `review/fix-media`, base `ec5a601`
(ORBIT OS 2.0.2). Riferimento: `docs/integration/review/03-media.md`
(worktree `orbit-wt-review-qa`).

Ambito toccato: `src/modules/blehid/*`, `src/modules/chat/Chat.cpp`,
`src/modules/audio/AudioMenu.cpp`. Non toccati `AudioOut`, `partitions.csv`,
`flasher/`, IR, Radio, Music.

Nessun hardware collegato: build, dimensione e verifica statica. I test su
Cardputer reale restano da eseguire (sezione 4).

---

## 1. Modifiche

### M1 - Autorizzazione BLE HID legata al peer mostrato (TOCTOU)

File: `src/modules/blehid/BleHidKeyboard.{h,cpp}`,
`src/modules/blehid/BleHidMenu.cpp`.

- Nuovo `hid::PeerIdentity { uint16_t handle; std::string address; }` e
  `hid::peerIdentity(PeerIdentity&)`: legge handle e indirizzo **insieme**
  sotto `g_peerMux`; la `std::string` viene costruita fuori dalla sezione
  critica.
- `hid::authorizePeer()` e' sostituito da
  `authorizePeer(expectedHandle, expectedAddress)`: sotto `g_peerMux`
  confronta handle e indirizzo con `g_peerHandle`/`g_peerAddress` e verifica
  `g_connected && g_secure`. Se il peer e' cambiato non imposta
  `g_authorized` e ritorna `false`.
- `waitHost()` cattura l'identita' **prima** di `ui::confirm` e mostra
  `peer.address`. Se la conferma va a buon fine ma il peer e' cambiato,
  ridisegna il prompt sul nuovo indirizzo (`lastDraw = 0; continue`) invece
  di autorizzare. ESC continua a rifiutare.

### M2 - Tastiera live: nessuna sospensione durante la digitazione

File: `src/modules/blehid/BleHidMenu.cpp`.

- `runLive()` tiene un `powertimer::Hold` per tutta la sessione (stesso
  schema di scanner e player). Rimosso `powertimer::tick()` dal loop: con
  l'hold non ha effetto. La sessione termina con ESC, perdita del link o
  autorizzazione, quindi l'hold non resta attivo fuori dalla schermata.

### M3 - Chat: callback di scan con vita pari al programma

File: `src/modules/chat/Chat.cpp`.

- Un solo `ChatScanCallbacks g_chatScanCallbacks` a livello di namespace.
  `checkNicknameAvailable()` e `runChat()` usano `&g_chatScanCallbacks`
  invece di un oggetto di stack. `scan->stop()` puo' ancora consegnare
  `onResult()` al task host: la vtable resta valida.
- Il distacco del puntatore dopo `stop()` resta, ma non e' piu' usato come
  barriera di lifetime. Le due scansioni restano serializzate dal menu.

### M4 - Bad BLE: flow control reale e stop dichiarato

File: `src/modules/blehid/BleHidKeyboard.{h,cpp}`,
`src/modules/blehid/BleHidScript.{h,cpp}`,
`src/modules/blehid/BleHidMenu.cpp`.

- `sendReport()` non usa piu' `NimBLECharacteristic::notify()` (void, scarta
  in silenzio). Costruisce l'`os_mbuf` e chiama
  `ble_gatts_notify_custom()`; `rc != 0` o mbuf esaurito contano un drop in
  `g_reportDrops` (`hid::reportDrops()`).
- Ritmo minimo `kReportGapMs = 15` ms dopo ogni report accodato (prima
  press+release a 9 ms, ~222 report/s). `kKeyGapMs` rimosso: il gap e' ora
  dentro `sendReport`.
- `tapUsage()` incrementa `g_sentKeys` solo se **pressione e rilascio** sono
  stati accodati. Un report scartato non viene contato come tasto inviato.
- `ScriptResult::flowError` e `RunContext::flow`: `pollAbort()` confronta
  `hid::reportDrops()` con il valore iniziale e interrompe lo script. Alla
  fine `completed` e' false anche se il drop avviene sull'ultima riga.
  `runBadScript()` mostra "!Report BLE scartati / Host troppo lento o coda
  piena / Script NON completato" invece di "Script completato". Il ramo
  "annullato" non cattura piu' il caso flow.

### S1 - Audio: coda della registrazione scritta allo stop

File: `src/modules/audio/AudioMenu.cpp`.

- Estratto `recorderEmit(r, idx)` (warm-up ES8311, `writeAll`, contatore
  `r.bytes`, livello). Usato da `recorderWrite` e da `recorderFinish`.
- `recorderFinish()` drena la coda (`isRecording() == 0`, max 250 ms) e, se
  non c'e' errore e il drain e' completo, scrive **entrambi** i buffer in
  ordine FIFO: `r.next` (piu' vecchio) e `r.next ^ 1`. Prima di `endMic()` e
  del patch header. Ogni registrazione recupera fino a ~128 ms di coda.
- `M5.Mic.isRecording() != 0` dopo il timeout non scrive buffer parziali:
  comportamento invariato (file valido ma coda persa), come prima.

---

## 2. Build e dimensione

Comando:

```powershell
python -m platformio run -e cardputer-adv-offensive
```

Baseline misurata nello stesso worktree con `git stash` su `ec5a601`, poi
ripristinata:

| Voce | Base `ec5a601` | Con il fix | Delta |
|---|---:|---:|---:|
| Flash PlatformIO (`text+data`) | 2.697.341 B (95,7%) | 2.698.565 B (95,8%) | +1.224 B |
| `firmware.bin` | 2.697.712 B | **2.698.928 B** | **+1.216 B** |
| RAM statica | 147.392 B (45,0%) | **147.408 B (45,0%)** | +16 B |
| Slot app `0x2B0000` | 2.818.048 B | 2.818.048 B | - |
| Libero nello slot | 120.336 B | **119.120 B** | -1.216 B |

`tools/check_size.ps1 -SkipBuild`: OK, 119.120 B liberi nello slot.
Margine al tetto di budget (2.751.776 B): 52.848 B. Attenzione: la base era
gia' a 54.064 B dal tetto, quindi la "riserva 64 KiB" del documento BUDGET
era gia' intaccata prima di questo fix; il fix aggiunge solo 1.216 B.

Nessun warning del compilatore nei file modificati.

---

## 3. Verifiche eseguite

- Build `cardputer-adv-offensive`: **SUCCESS** (36 s incrementale, 580 s
  cold con download dipendenze).
- `git diff --check`: nessun errore di whitespace.
- Ispezione statica dei chiamanti:
  - `authorizePeer` ha un solo chiamante (`BleHidMenu::waitHost`);
  - `ChatScanCallbacks` non ha piu' istanze di stack;
  - `kKeyGapMs` non ha altri riferimenti;
  - `reportDrops()` usato da `BleHidScript` e dalla UI.
- Controllo dell'invariante audio sui due slot M5Unified: dopo ogni
  `recorderWrite` la coda contiene `[next, next^1]` (FIFO); al drain
  entrambi sono completi e non ancora scritti, quindi l'ordine
  `next`/`next^1` e' cronologico e non duplica buffer gia' scritti.
- Nessun test automatico hosts-side nuovo: le parti modificate dipendono da
  NimBLE, M5Unified e dal task host; non c'e' un harness nel repo per
  simularli.

---

## 4. Test hardware da eseguire

1. **M1 TOCTOU**: telefono A associato, modale "Autorizza host" aperto;
   far cadere A, collegare B e completare il pairing; Enter deve
   riproporre il prompt con l'indirizzo di B, non autorizzare B al primo
   Enter. Ripetere con ESC.
2. **M2 live**: timeout sospensione a 30 s, digitare per >2 minuti senza
   pause; lo schermo non deve attenuarsi ne' sospendere. Verificare che ESC
   esca e che il timer riparta.
3. **M3 chat**: con molti advertiser in aria (beacon/telefono), entrare e
   uscire dalla chat e dalla verifica nickname decine di volte; nessun
   reset. Ripetere con una seconda Cardputer che risponde alla query.
4. **M4 Bad BLE**: script con `STRING` di 300 caratteri su Windows,
   Android e Linux; ogni carattere deve arrivare, `Tasti:` deve
   corrispondere ai tasti realmente accettati. Forzare il drop (host lento
   o disconnessione a meta') e verificare il messaggio "Report BLE
   scartati" e `Script NON completato`. Misurare la velocita': ~25-27
   tasti/s (15 ms per report + 6 ms hold).
5. **S1 audio**: registrare 10 s, fermare subito dopo una parola; con
   `ffprobe` la durata deve essere entro +128 ms dal previsto e l'ultima
   parola non deve essere tagliata. Ripetere fermando durante il warm-up
   (<1,2 s) e a SD lenta.

---

## 5. Rischi residui

- **M4**: `ble_gatts_notify_custom()` con `rc == 0` significa "accodato
  nell'host", non "trasmesso e confermato dall'host". Non e' stato
  registrato `onStatus`/`BLE_GAP_EVENT_NOTIFY_TX`: un drop tardivo del
  controller resta invisibile. Il ritmo a 15 ms lo rende improbabile, non
  impossibile. Il costo e' script ~2x piu' lenti (39 ms/tasto contro 12).
- **M1**: il confronto e' handle + stringa indirizzo. Se lo stesso central
  si riconnette e il controller riusa lo stesso `conn_handle` mentre il
  modale e' aperto, la sessione non e' distinguibile. La finestra e' di
  pochi secondi e richiede lo stesso indirizzo; rischio accettato.
- **M2**: la sessione live non dorme mai finche' e' aperta (Hold). E'
  voluto, ma se l'utente lascia la schermata con un host connesso la
  batteria continua a consumare. Il ritmo a 15 ms/report vale anche per la
  tastiera live (~27 tasti/s): sopra la velocita' umana, ma una raffica
  molto rapida puo' essere campionata con un giro di ritardo.
- **M3**: un solo callback statico presuppone scansioni serializzate. Oggi
  lo sono (menu); una scan concorrente futura richiederebbe un oggetto heap
  con attesa di fine scan.
- **S1**: scrivendo il buffer in cattura dopo il drain, il file puo'
  estendersi fino a ~128 ms oltre il tasto di stop. Preferito alla perdita
  della coda; se serve un taglio esatto al millisecondo va registrato il
  timestamp di stop e troncato il buffer.
- **Flash**: margine al tetto 52.912 B, sotto la riserva indicata di 64 KiB
  (gia' cosi' nella base). Questo fix non sposta l'ordine di grandezza, ma
  il prossimo modulo deve misurarsi con `tools/check_size.ps1`.
- Nessuna validazione su hardware: pairing Windows/Android/Linux, qualita'
  I2S, latenza SD e comportamento del controller sotto carico restano da
  provare.
