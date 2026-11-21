# Test 04 — BLE HID / Bad BLE (codice finale integrato)

- **Target:** `C:\Users\utente\Desktop\opencode\offensive-cardputer` (trunk di integrazione, build `cardputer-adv-offensive`)
- **Data:** 2026-11-21
- **Metodo:** sola analisi statica del codice finale e dell'artefatto di build presente in `.pio/`. Nessuna modifica, nessuna compilazione, nessun flash, nessun commit.
- **Artefatto ispezionato:** `.pio/build/cardputer-adv-offensive/firmware.bin`, 2.618.256 byte, 21/11/2026 15:43 (sorgenti `src/modules/blehid/*` 15:40). Slot reale `app0/app1` = `0x2B0000` = 2.818.048 byte (`partitions.csv:15-16`).
- **Esito sintetico:** **2 P0, 6 P1, 16 P2**. La feature non è pronta per il merge finché restano aperti i P0 e i P1 indicati. Il Report Map e il layout dichiarato sono corretti; i problemi gravi sono nel modello di sicurezza del link, nella gestione delle due caratteristiche input e nel ciclo vita NimBLE.

## Top 5

| # | ID | Gravità | Sintesi |
|---|----|---------|---------|
| 1 | P0-01 | **P0** | La **Boot Keyboard Input Report 0x2A22** non ha requisiti di sicurezza: qualsiasi central può connettersi senza pairing (o con un Just Works non confermato), sottoscriverla e ricevere tutti i tasti (e gli script Bad BLE). `subscribed()` la considera sufficiente per partire. |
| 2 | P0-02 | **P0** | Nessuna autorizzazione dell'host: pairing **Just Works** (`mitm=false`), nessun controllo di `sec_state`, fino a 3 connessioni, report inviati a tutti. La tastiera e Bad BLE fidano di qualunque central; la conferma Bad BLE non mostra l'identità del peer. |
| 3 | P1-03 | **P1** | Se l'host sottoscrive `0x2A4D` senza aver cifrato il link, `subscribed()` dice "pronto" ma NimBLE **scarta le notifiche** (`READ_ENC`), e il firmware non chiama `startSecurity()`: la UI conta i tasti, lo script dice "completato", ma non arriva nulla. |
| 4 | P1-04 | **P1** | `sendReport()` invia **sia** il report `0x2A4D` **sia** il boot report `0x2A22`, senza controllare la sottoscrizione per caratteristica: gli host che sottoscrivono entrambe ricevono ogni carattere due volte, con traffico BLE raddoppiato. |
| 5 | P1-05 | **P1** | `g_connected` è un solo booleano mentre NimBLE accetta fino a 3 connessioni: la disconnessione di un peer qualsiasi azzera lo stato, interrompe la sessione e deinizializza anche l'host legittimo. Attacco DoS banale; con due peer entrambi ricevono i tasti. |

---

## Findings

### P0-01 — Boot Keyboard Input Report senza requisiti di sicurezza: keylogger passivo senza pairing

**File:** `src/modules/blehid/BleHidKeyboard.cpp:119-122` (notify boot input), `:333-334` (`g_hid->bootInput()`), `:397-404` (`subscribed()` include il boot), `src/modules/blehid/BleHidMenu.cpp:86-90` (la sessione parte quando `subscribed()` è vero).
**Evidenza NimBLE:** `NimBLEHIDDevice::bootInput()` crea la caratteristica `0x2A22` con la sola proprietà `NOTIFY` (`NimBLE-Arduino/src/NimBLEHIDDevice.cpp:188-189`); `NimBLECharacteristic::notify()` applica il controllo `READ_ENC` solo a `m_properties` (`NimBLE-Arduino/src/NimBLECharacteristic.cpp:461-480`), quindi per il boot report non c'è alcun requisito di cifratura.

**Impatto:** un central qualsiasi nei paraggi può connettersi **senza pairing** (o, se il CCCD del boot richiede cifratura, con un pairing Just Works non confermato sulla Cardputer), sottoscrivere `0x2A22` e ricevere:
- ogni tasto premuto nella modalità "Tastiera BLE";
- ogni carattere e ogni scorciatoia di uno script Bad BLE.

In più `subscribed()` restituisce vero per la sottoscrizione al boot, quindi la UI passa a "Collegato" e `waitHost()` fa partire digitazione/esecuzione. È una fuga di keystroke P0, indipendente dal fatto che `0x2A4D` sia protetta: la notifica del boot non ha alcun requisito di sicurezza nel codice NimBLE, e `sendReport()` la invia sempre.

**Fix:** non notificare `0x2A22` se il link non è cifrato **e** autenticato. Opzioni: creare il boot input con `READ_ENC`/`WRITE_ENC` (o verificare `ble_gap_conn_find()->sec_state` prima di ogni notify), escludere la sottoscrizione boot da `subscribed()`, e notificare il boot report solo quando l'host ha selezionato il Boot Protocol. In ogni caso la sessione non deve considerarsi pronta se l'unica sottoscrizione è quella boot in chiaro.

---

### P0-02 — Nessuna autorizzazione dell'host: Just Works, multi-connessione, Bad BLE contro il peer sbagliato

**File:** `src/modules/blehid/BleHidKeyboard.cpp:309-310` (`setSecurityAuth(true,false,true)` + `NoInputNoOutput`), `:75-78` (`onConnect` imposta solo un flag), `:79-83` (`onDisconnect`), `:114-123` (notify a tutti i sottoscrittori), `:448-449` (`tapUsage` controlla solo `g_connected`), `src/modules/blehid/BleHidMenu.cpp:86-90` e `:314-319` (conferma Bad BLE dopo il pairing, senza identità del peer).

**Impatto:**
- Il pairing è **Just Works** (`mitm=false`, IOCap `NoInputNoOutput`): un central può associarsi senza alcuna conferma sulla Cardputer. `READ_ENC` richiede solo cifratura, non autenticazione MITM; un attaccante che fa pairing Just Works riceve i tasti.
- NimBLE è configurato con `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=3` di default (`NimBLE-Arduino/src/nimconfig.h:181-182`): il server accetta più central. `sendReport()` notifica a tutti i sottoscrittori, quindi due host connessi ricevono la stessa digitazione.
- Bad BLE mostra solo "Host connesso" e non l'indirizzo/nome del peer: se un attaccante si connette per primo, l'utente conferma e lo script viene digitato sul dispositivo dell'attaccante.

**Fix:** usare pairing autenticato (SC + MITM con passkey/DisplayOnly e conferma a schermo), rifiutare i peer non autenticati, mostrare l'indirizzo del peer nella schermata di attesa e nella conferma Bad BLE, accettare una sola connessione autorizzata e disconnettere le altre. `tapUsage()`/`sendReport()` devono verificare `sec_state.encrypted && sec_state.authenticated` e il peer selezionato, non solo `g_connected`.

---

### P1-03 — Nessuna richiesta di pairing: sottoscrizione senza cifratura, notifiche `0x2A4D` scartate in silenzio

**File:** `src/modules/blehid/BleHidMenu.cpp:86-90` (`subscribed()` fa partire la sessione), `src/modules/blehid/BleHidKeyboard.cpp:397-404` (`getSubscribedCount()` non guarda la sicurezza), `:309-310` (nessun `startSecurity`), `:448-467` (tap solo su `g_connected`).
**Evidenza NimBLE:** `NimBLECharacteristic::notify()` salta i subscriber se la caratteristica ha `READ_ENC` e `desc.sec_state.encrypted` è falso (`NimBLE-Arduino/src/NimBLECharacteristic.cpp:461-480`). La sottoscrizione al CCCD non è la lettura del valore `0x2A4D`: non è detto che faccia partire il pairing.

**Impatto:** su host che si connettono e sottoscrivono senza avviare la cifratura (possibile su BlueZ/alcuni Android, o con un central che non fa pairing), la UI mostra "Collegato", `runLive()` incrementa "Tasti inviati", Bad BLE arriva a "Script completato", ma **nessun carattere arriva**. Il commento in `REPORT.md:15-17` ("l'host avvia la cifratura quando sottoscrive il report") non è garantito dal codice.

**Fix:** in `ServerCallbacks::onConnect` chiamare `NimBLEDevice::startSecurity(conn_handle)` (o `ble_gap_security_initiate`) e non considerare pronta la sessione finché `ble_gap_conn_find()->sec_state.encrypted` non è vero. `subscribed()` deve restituire vero solo per un link cifrato/autenticato; la UI deve mostrare "in attesa di pairing" finché non lo è. Aggiungere un timeout con messaggio.

---

### P1-04 — Doppia notifica input `0x2A4D` + `0x2A22`, senza controllo di sottoscrizione

**File:** `src/modules/blehid/BleHidKeyboard.cpp:114-123`.

**Impatto:** `sendReport()` fa `setValue()` + `notify()` su `g_input` **e** su `g_bootInput` per ogni pressione e ogni rilascio. Se l'host sottoscrive entrambe le caratteristiche (alcuni stack HID lo fanno, e un central personalizzato può farlo), ogni carattere viene consegnato due volte; anche il traffico BLE raddoppia e aumenta la probabilità di perdita su `STRING` lunghe. Con un solo carattere sottoscritto il `notify` verso l'altro è scartato da NimBLE, ma il codice non lo verifica.

**Fix:** notificare ogni caratteristica solo se `getSubscribedCount() > 0`; usare il boot input solo quando il Protocol Mode è Boot (`0x2A4E`), altrimenti solo `0x2A4D`. Non chiamare `setValue()` e `notify()` insieme se `notify()` già aggiorna il valore.

---

### P1-05 — `g_connected` unico con più connessioni: DoS e broadcast a peer multipli

**File:** `src/modules/blehid/BleHidKeyboard.cpp:62` (`volatile bool g_connected`), `:75-83` (callback), `src/modules/blehid/BleHidMenu.cpp:162-164` (`runLive` esce appena `connected()` è falso).

**Impatto:** con `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=3`, `onDisconnect` di **qualsiasi** peer mette `g_connected=false`, anche se l'host legittimo è ancora connesso. `runLive()` esce, `HidSession` chiama `end()`, `deinit(true)` chiude anche l'host buono. Un attaccante può connettersi e disconnettersi per espellere l'utente a ripetizione (DoS). Se due peer restano connessi, `sendReport()` invia i tasti a entrambi.

**Fix:** tenere un set di `conn_handle` autorizzati e aggiornare `connected()` da `NimBLEDevice::getServer()->getConnectedCount()` o dal set; su disconnect rimuovere solo l'handle e chiudere la sessione solo se non resta nessun peer autorizzato. Accettare un solo peer e disconnettere gli extra.

---

### P1-06 — Deinit con host connesso / stack di altri moduli: rischio di blocco e distruzione dello stack

**File:** `src/modules/blehid/BleHidKeyboard.cpp:294-302` (se lo stack è già inizializzato: stop adv/scan e `deinit(true)`), `:303` (`NimBLEDevice::init`), `:374-387` (`end()` ferma solo l'advertising, poi `cleanup()` + `deinit(true)`). `NimBLEDevice::init` attende `m_synced` in busy-loop senza timeout (`NimBLE-Arduino/src/NimBLEDevice.cpp:909-912`).

**Impatto:** `end()` non disconnette il peer prima di `deinit(true)`. `nimble_port_stop()` con una connessione attiva può bloccarsi o richiedere tempo; non c'è timeout né watchdog feed esplicito, quindi ESC con host connesso può congelare la UI o far scattare il watchdog. Lo stesso rischio c'è entrando in BLE HID dopo Chat/BLE Spam/Radar: `begin()` distrugge lo stack esistente senza disconnettere e senza verificare l'esito. I test C04-C10 e P07 restano bloccanti.

**Fix:** prima del deinit disconnettere esplicitamente tutti i peer (`NimBLEDevice::getServer()->disconnect(handle)`), fermare advertising/scan, attendere un timeout breve e verificare `getInitialized()`; se `nimble_port_stop()` non completa, non forzare. Valutare di non fare deinit quando un host è connesso (o di farlo solo dopo disconnect confermata) e documentare la strategia. Aggiungere un timeout anche all'init.

---

### P1-07 — Nessun gate di heap prima di inizializzare NimBLE HID

**File:** `src/modules/blehid/BleHidKeyboard.cpp:286-307` (begin), `:320` (`new NimBLEHIDDevice`), `:328-343` (servizi/caratteristiche). Il budget del progetto richiede almeno **32 KB** di heap libero per BLE HID (`docs/integration/BUDGET.md:88`).

**Impatto:** dopo WiFi/TLS/audio, con framebuffer e menu sul heap, `NimBLEDevice::init()` può fallire o abortire (`ESP_ERROR_CHECK` nel controller); `new` e le allocazioni dei servizi non hanno `nothrow`. Su 320 KB senza PSRAM il rischio è concreto; il modulo non controlla `ESP.getFreeHeap()` prima di partire e non mostra un errore all'utente.

**Fix:** controllare `ESP.getFreeHeap()` (es. ≥ 32 KB, meglio 40 KB) prima di `begin()`, mostrare un messaggio e non inizializzare; usare `new (std::nothrow)` e verificare i puntatori; loggare heap prima/dopo. Aggiungere il test con heap basso (E02/C09).

---

### P1-08 — Ducky: i chord con lettera maiuscola aggiungono Shift (`CTRL C` → Ctrl+Shift+C, `GUI R` → Win+Shift+R)

**File:** `src/modules/blehid/BleHidScript.cpp:409-425` (token singolo passato così com'è) e `src/modules/blehid/BleHidKeyboard.cpp:499-506` (`tapScriptKey` → `mapCodepoint`), con `mapAscii()` che per `A-Z` aggiunge `kShift` (`BleHidKeyboard.cpp:132-136`).

**Impatto:** in DuckyScript i nomi dei tasti sono case-insensitive: `CTRL C`, `CTRL V`, `GUI R`, `ALT F4` sono la forma comune. Con questo parser la lettera maiuscola aggiunge Shift, quindi lo script invia una combinazione diversa (spesso innocua, a volte pericolosa: `Win+Shift+R` non è "Esegui"). È un errore silenzioso, non un rifiuto della riga.

**Fix:** per un token di chord composto da una lettera, usare il tasto fisico non shiftato (`tolower`), e aggiungere Shift solo se richiesto esplicitamente (`SHIFT C`). Per i simboli mantenere la mappa del layout. Aggiungere `CTRL C`, `GUI R`, `ALT F4` alla suite D04 e confrontare con BadUSB.

---

### P2-09 — `/badusb`: nessun filtro `.txt`, nessun limite a 100, fallback LittleFS bloccato da file non-script

**File:** `src/modules/blehid/BleHidScript.cpp:473-484` (`listScripts` usa `storage::list("/badusb", false, ...)`), `src/modules/blehid/BleHidMenu.cpp:363-378`. Confronto: BadUSB filtra `.txt` e limita a 100 (`src/modules/badusb/BadUsbMenu.cpp:41-44,52,60-61`).

**Impatto:** qualsiasi file in `/badusb` (binari, immagini, backup) viene mostrato ed eseguibile; un file non `.txt` sulla SD rende la lista non vuota e impedisce il fallback ai veri script LittleFS. D01/D10 falliscono; con centinaia di file la menu list cresce senza limite (heap).

**Fix:** riusare `storage::listEntries` + il predicato `isTxtFile()` e il cap 100 di BadUSB, oppure filtrare in `listScripts`. Preferire la SD solo se contiene almeno un `.txt`.

---

### P2-10 — Limite 5000 righe e righe >256 byte: script troncato ma UI dice "completato"

**File:** `src/modules/blehid/BleHidScript.cpp:497-516` (`capped` locale), `:542-547` (riga troncata saltata), `:593-597` (solo log), `src/modules/blehid/BleHidMenu.cpp:338-344` (messaggio "Script completato").

**Impatto:** un file con più di 5000 righe viene eseguito solo in parte, e una riga >256 byte viene ignorata; l'utente vede "Script completato" e non sa che il resto non è stato eseguito. D05 fallisce.

**Fix:** aggiungere `capped` e `skippedLines` a `ScriptResult`; mostrare un avviso esplicito ("limite 5000 righe", "N righe ignorate") e non usare "completato" senza qualifica.

---

### P2-11 — Caratteri non rappresentabili saltati in silenzio; deviazione dal parser USB

**File:** `src/modules/blehid/BleHidScript.cpp:279-281` (`tapCodepoint` falso → nessun conteggio), `src/modules/badusb/BadUsbScript.cpp:330-361` (USB conta e logga i caratteri non ASCII).

**Impatto:** `STRING città` su layout US produce `citt` senza alcun avviso; L05/L07 e D07 falliscono. Il REPORT dichiara la limitazione ma il codice non la rende visibile.

**Fix:** contare i caratteri saltati e mostrarli/loggarli a fine script; documentare la differenza UTF-8 IT vs USB.

---

### P2-12 — File vuoto o sparito: messaggio generico, nessun fallback LittleFS in apertura

**File:** `src/modules/blehid/BleHidScript.cpp:490-494` (open fallita), `:515-517` (total 0), `:519-522` (seconda open), `src/modules/blehid/BleHidMenu.cpp:346-350` (messaggio generico).

**Impatto:** un file vuoto, un file cancellato dopo la lista o una SD rimossa mostrano "Esecuzione non riuscita" con `Riga 0 / 0`, non "impossibile aprire" né "file vuoto". D09/D05 non hanno un esito chiaro. Se `preferSd` era vero, `storage::openRead` non ripiega su LittleFS.

**Fix:** restituire uno stato distinto (`OpenFailed`, `Empty`, `ReadError`) e mostrare il messaggio giusto; se `preferSd` e la SD fallisce, tentare LittleFS prima di dichiarare l'errore.

---

### P2-13 — `delay(700)` fisso dopo la sottoscrizione: UI bloccata e nessuna garanzia di cifratura

**File:** `src/modules/blehid/BleHidMenu.cpp:86-90` (`delay(700)`).

**Impatto:** 700 ms di blocco non interrompibile con ESC (S09: niente delay >100 ms nei loop interattivi). Inoltre il delay non verifica lo stato di sicurezza: serve solo a "sperare" che l'host abbia completato encryption/discovery.

**Fix:** sostituire con un loop `millis()` che polla `sec_state.encrypted`/`subscribed()` e ESC, con timeout breve e messaggio; non usare `delay(700)`.

---

### P2-14 — L'abort Bad BLE scatta anche con Backspace; backtick non digitabile in live

**File:** `src/modules/blehid/BleHidScript.cpp:228-231` (`navigation::poll()` → `Back`), `src/core/Navigation.cpp:120` (`back = ... || state.del`), `src/modules/blehid/BleHidMenu.cpp:165-167` (ESC = backtick in live).

**Impatto:** la schermata Bad BLE dice "ESC = annulla", ma anche il tasto Backspace/DEL annulla lo script. In live il backtick è riservato all'uscita e non è inviabile (A07 chiede di digitarlo); il REPORT lo documenta.

**Fix:** per l'abort dello script usare solo il tasto fisico ESC (`M5Cardputer.Keyboard.isKeyPressed('`')` con fronto), non `navigation::poll()` che include `state.del`; oppure documentare esplicitamente che Backspace annulla. Per il backtick prevedere una combinazione di escape o dichiarare la limitazione in A07.

---

### P2-15 — Battery Service senza livello: l'host può mostrare 0%

**File:** `src/modules/blehid/BleHidKeyboard.cpp:325-343` (creazione HID, nessun `setBatteryLevel`), `NimBLEHIDDevice` crea la caratteristica `0x2A19` nel costruttore.

**Impatto:** R06: l'host può mostrare batteria 0% o valore non valido invece di "assente". Non è sicurezza, ma è un dato falso.

**Fix:** impostare un livello plausible (`g_hid->setBatteryLevel(...)`) o rimuovere/ non avviare il Battery Service; completare Device Information (firmware/model) se lo si vuole esporre.

---

### P2-16 — Race minori: `getSubscribedCount()` dal task UI e `g_leds` non atomico

**File:** `src/modules/blehid/BleHidKeyboard.cpp:69` (`uint8_t g_leds`), `:397-404` (`getSubscribedCount()` chiamato dal menu), `:86-95` (callback NimBLE che scrive `g_leds`).

**Impatto:** `m_subscribedVec` è modificato nel task NimBLE durante subscribe/disconnect e letto dal task UI ogni 40 ms; `g_leds` è scritto nel task NimBLE e letto dal task UI. Sono data race benigne nella pratica, ma possono dare stato LED stale o, in teoria, letture inconsistenti del vettore. C11.

**Fix:** leggere lo stato in una callback e salvarlo in atomici; almeno `volatile`/`std::atomic<uint8_t>` per `g_leds` e un contatore aggiornato dalle callback invece di interrogare il vettore da un altro task.

---

### P2-17 — BLE HID non è governato da `Config::Module`: non disattivabile e sempre nel menu

**File:** `src/main.cpp:111` (`menu.addItem("BLE HID", ...)` senza `guardedModule`), `include/Config.h:15-30` (nessun `Module::BleHid`).

**Impatto:** l'utente non può disattivare BLE HID da Impostazioni; il modulo resta raggiungibile anche se il modulo BLE è disattivato. La review 04 lo segnala come decisione aperta; richiede il supervisore perché `Config.cpp/SettingsMenu.cpp` sono vietati al modulo.

**Fix:** aggiungere il bit `BleHid` e il guard (modifica autorizzata dai file condivisi), oppure accettare e documentare la deviazione. Nel frattempo usare `guardedModule` con `Module::Ble` è una scelta da valutare (non blocca BLE HID se BLE è spento, perché NimBLE è indipendente).

---

### P2-18 — REPORT.md non corrisponde all'artefatto finale

**File:** `src/modules/blehid/REPORT.md:86-87` (2.393.344 byte; RAM 115.616), `docs/integration/MERGE_B.md:31,35` (2.393.344 allo step 1; 2.586.768 finale), artefatto locale 2.618.256 byte.

**Impatto:** tre dimensioni diverse. Con il criterio "≤ 2,5 MiB" della review S01 (2.621.440) restano solo **3.184 byte**; con lo slot reale da 2.818.048 restano 199.792 byte. La RAM statica va rimisurata. I numeri di accettazione e BUDGET non sono affidabili.

**Fix:** ricompilare e aggiornare `REPORT.md` con `firmware.bin`, flash ELF, RAM statica e heap misurati; usare `partitions.csv` come fonte dello slot.

---

### P2-19 — Nessuna gestione dei bond: associazioni orfane e nome vecchio

**File:** `src/modules/blehid/BleHidKeyboard.cpp:309` (bonding attivo), `REPORT.md:135-136` (nessuna UI per cancellare).

**Impatto:** dopo un cambio nickname o la perdita di un host, il bond resta in NVS e l'host può continuare a mostrare/riconnettersi col nome vecchio; non c'è modo di cancellare il bond dalla Cardputer. S06.

**Fix:** aggiungere in Info/Layout una voce "Dimentica associazioni" con `NimBLEDevice::deleteAllBonds()` o `deleteBond(address)` e conferma; documentare il comportamento.

---

### P2-20 — Log seriale del contenuto delle righe di script non riconosciute

**File:** `src/modules/blehid/BleHidScript.cpp:551-554` (`logger::warn(... text.c_str())`).

**Impatto:** una riga non riconosciuta può contenere un comando o un dato sensibile (password, token) e finisce sul seriale. La review S05 chiede di non loggare il contenuto degli script.

**Fix:** loggare solo numero di riga e lunghezza, oppure una versione troncata/anonimizzata. Lo stesso vale per l'anteprima a schermo se il requisito di privacy è stringente.

---

### P2-21 — `parseU32` può accettare un overflow di `uint64`

**File:** `src/modules/blehid/BleHidScript.cpp:153-170`.

**Impatto:** un numero di 20+ cifre può overfloware `uint64_t` e, se il valore wrappato resta ≤ `0xFFFFFFFF`, essere accettato come delay/REPEAT piccolo. È un caso limite, ma il parser dichiara di rifiutare il testo residuo e i valori fuori range.

**Fix:** controllare l'overflow prima della moltiplicazione (`value > (0xFFFFFFFFULL - digit) / 10`) e rifiutare oltre 10 cifre.

---

### P2-22 — Layout IT: `~` e backtick non mappati; accenti solo IT

**File:** `src/modules/blehid/BleHidKeyboard.cpp:185-219` (switch IT senza `~`/`` ` ``), `:222-238` (accenti solo IT), `REPORT.md:124-126` (limiti dichiarati).

**Impatto:** il corpus L02/L07 con `~` e backtick su host IT non può passare; su host US le accentate italiane vengono saltate. È documentato, ma va escluso esplicitamente dal criterio di accettazione o implementato.

**Fix:** verificare e aggiungere le combinazioni AltGr italiane per `~` e `` ` `` (se esistono sulla tastiera host), oppure dichiarare il sottoinsieme supportato e far fallire/segnalare i caratteri non rappresentabili (vedi P2-11). I simboli `# @ [ ] { } \ | € £ ° § ç` verificati a tavolino risultano corretti.

---

### P2-23 — `ui::legalWarning`: la terza riga extra viene troncata

**File:** `src/core/Ui.cpp:146-150` (max 8 righe), `src/modules/blehid/BleHidMenu.cpp:265-270` e `:292-298` (3 righe extra).

**Impatto:** con 6 righe fisse + 3 extra, l'ultima extra (`"Nessun mouse..."` o `"Solo su dispositivi tuoi."`) viene sostituita dal disclaimer README. Il gate legale resta valido, ma il testo specifico del modulo non è completo.

**Fix:** aumentare il limite a 9 righe o accorpare le extra; non è un blocco di sicurezza.

---

### P2-24 — Live: cambiare Shift/Ctrl mentre un tasto è premuto invia un secondo carattere

**File:** `src/modules/blehid/BleHidMenu.cpp:187-199` (confronto `state.word` con `lastWord`), `:175-185` (confronto `state.hid_keys` con `lastHid`), `:234-238` (aggiornamento degli ultimi stati). La libreria cambia il carattere restituito in `word` quando Shift/Ctrl è premuto (`M5Cardputer/src/utility/Keyboard/Keyboard.cpp:200-208`), e aggiunge/toglie il bit `0x80` in `hid_keys` (`Keyboard_def.h:11`).

**Impatto:** premere un tasto, poi Shift (o Ctrl) mentre il tasto è ancora premuto cambia `word`/`hid_keys`: il confronto lo vede come un nuovo tasto e invia un secondo carattere. Esempio: tenere `a`, premere Shift → l'host riceve `aA`; rilasciare Shift → riceve un'altra `a`. Lo stesso con Ctrl nel ramo `hid_keys`. È un difetto di digitazione live, non del layout.

**Fix:** tracciare l'edge dei **tasti fisici** (posizione/usage senza bit Shift) e generare il carattere una sola volta alla pressione; ignorare i cambi di `word`/`hid_keys` dovuti solo ai modificatori. In alternativa, confrontare i caratteri in forma minuscola e il `hid_keys` mascherato con `0x7F`.

---

## Verifiche senza finding (o con esito positivo)

### Report Map / HID report
Il descrittore (`BleHidKeyboard.cpp:25-59`) è valido: Report ID 1, 8 bit modificatori, byte riservato, 6 tasti (6KRO), output LED 1 byte. `inputReport(1)`/`outputReport(1)` e i Report Reference `0x2908` sono coerenti; il boot report è 8 byte. **Il descrittore in sé non ha P0/P1.** I problemi sono la notifica del boot input in chiaro (P0-01) e la doppia notifica (P1-04).

### Layout IT/US
Verificati a tavolino i tasti fisici italiani: `! " £ $ % & / ( ) =`, `' ? ì ^`, `è é + * ò ç à ° ù §`, `\ |`, `< >`, `, ; . : - _`, `# @ [ ] { }`, `€`. Le posizioni corrispondono al layout italiano; il layout US è completo per ASCII 0x20–0x7E. Restano i limiti dichiarati (`~`/backtick IT, accenti US) in P2-22.

### ESC e `; . , /`
La modalità live legge `KeysState.word`/`hid_keys` grezzi (`BleHidMenu.cpp:142-245`), non `navigation::poll()`: quindi `; . , /`, spazio, Invio, Tab, Backspace e Ctrl/Alt/AltGr sono inviabili. ESC (backtick) esce e non viene inviato. **PASS** per A07, con le cautele P2-14 (backtick non digitabile) e P2-13.

### Legal gate
`runKeyboard()` chiama `ui::legalWarning` prima di `HidSession`/`hid::begin` (`BleHidMenu.cpp:265-270`); `runBadScript()` lo chiama prima di inizializzare il BLE (`:292-298`) e chiede una seconda conferma dopo la connessione (`:314-319`). La build pubblica non mostra Bad BLE (`#if ENABLE_OFFENSIVE`, `:414-416`). **PASS** per S01/S02/S11; la troncatura del testo è P2-23.

### Abort
ESC durante `DELAY`/`STRING` è pollato ogni 10 ms (`BleHidScript.cpp:219-245`); `releaseAll()` è chiamato in uscita (`:588`); la tastiera live rilascia (`BleHidMenu.cpp:247`). A01/A02/A04 passano a livello di codice; A03 (attesa host) usa `navigation::poll()` ogni 40 ms. Resta P2-14 (Backspace abortisce).

### Conflitti BLE
`begin()` ferma advertising e scan, poi deinit/reinit (`BleHidKeyboard.cpp:294-302`), imposta `BLE_OWN_ADDR_PUBLIC` (`:308`), resetta l'advertising (`:355`), imposta nome/appearance/UUID HID (`:356-358`). Questo copre C12-C14 (nome `ORBIT-<nick>`, indirizzo public, adv pulito) e il ritorno a Chat/BleSpam/Radar tramite i loro `ensureBle()`. **PASS statico**, ma il deinit con host connesso è P1-06.

### Parser Ducky
Comandi e limiti principali ci sono: `REM`, `#`, `//`, metadati, `DEFAULT_DELAY`, `DELAY`, `STRING`, `REPEAT` (cap 1000, `REPEAT 0` no-op), tasti nominati, chord, riga singolo carattere, riga sconosciuta ignorata, 256 byte/riga, 5000 righe. **PASS** per D04/D05 a livello strutturale, con i finding P1-08 (maiuscole), P2-10 (troncamento silenzioso), P2-11 (caratteri saltati) e P2-21 (overflow).

---

## Test hardware consigliati (in ordine di gravità)

1. **P0-01:** con `bleak`/nRF Connect, connettersi **senza pairing**, sottoscrivere `0x2A22`, digitare sulla Cardputer: verificare se i tasti arrivano. Ripetere con uno script Bad BLE.
2. **P0-02:** connettere un central non autorizzato e poi l'host reale; verificare chi riceve i tasti; provare `just works` da un telefono e controllare se la Cardputer chiede/conferma qualcosa.
3. **P1-03:** host BlueZ/Android che sottoscrive `0x2A4D` senza pairing: la UI dice "Collegato"? I caratteri arrivano? Parte il pairing?
4. **P1-04:** central che sottoscrive entrambe `0x2A4D` e `0x2A22`; contare i caratteri (atteso 1, osservato 2 se il bug è presente).
5. **P1-05:** due central connessi; disconnettere uno e verificare che la sessione dell'altro resti attiva; provare connect/disconnect ripetuti.
6. **P1-06:** ESC con host connesso e cronometrare l'uscita; entrare/uscire dopo Chat, BLE Spam e Radar; misurare heap e watchdog.
7. **P1-08:** script `GUI R`, `CTRL C`, `ALT F4` e confronto con BadUSB USB.
8. **P2:** `.txt`/non-`.txt` in `/badusb`, SD rimossa, file vuoto, 5001 righe, riga da 300 byte, `STRING città`, `STRING ~`/backtick su IT, heap basso.

---

## Nota sui limiti di questa verifica

Non sono stati eseguiti build, flash o test su hardware. Le conclusioni P0/P1 sono statiche, basate sul codice finale, sul comportamento di NimBLE-Arduino 1.4.3 presente in `.pio/libdeps/cardputer-adv-offensive/NimBLE-Arduino` e sull'artefatto `firmware.bin` già presente. Le build `cardputer-adv` (S02) e `cardputer-adv-hid` (S03) non sono state verificate; la loro assenza non prova un errore, ma S02/S03 restano gate aperti.
