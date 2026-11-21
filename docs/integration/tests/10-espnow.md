# Test 10 — ESP-NOW: file, comandi, clipboard mesh

- Data: 2026-11-21
- Oggetto: codice **finale integrato** in `offensive-cardputer`, HEAD `f19b6d0`
  (modulo introdotto da `5eb78d6`); file identico al worktree `orbit-wt-10-espnow`
  a meno dei line ending.
- File esaminati: `docs/integration/BRIEF.md`, `docs/integration/specs/10-espnow.md`,
  `orbit-wt-qa/docs/integration/reviews/10-espnow.md` (pre-implementazione, stub),
  `src/modules/espnow/REPORT.md`, `src/modules/espnow/EspNowMenu.cpp` (2371 righe),
  `src/modules/espnow/EspNowProtocol.h`, `src/modules/espnow/EspNowMenu.h`,
  utility condivise (`Storage.cpp`, `Ui.cpp`, `MenuManager.cpp`, `TextInput.cpp`),
  `main.cpp`, `platformio.ini`, `partitions.csv`.
- Modo: **sola lettura sul codice**. Nessuna build, nessuna modifica ai sorgenti,
  nessun commit. È stata scritta solo questa nota di test.

> La review in `orbit-wt-qa/.../reviews/10-espnow.md` è del giro precedente
> (modulo assente) e non è evidenza di test: i suoi T01–T10 restano il piano da
> eseguire su hardware. Questo documento è una verifica statica del codice finale
> e dei suoi artefatti, non un test su due Cardputer.

## Esito

**NON PRONTO per l'accettazione.** L'implementazione esiste e copre quasi tutti i
requisiti della spec, ma:

- non esiste alcuna evidenza di test su due dispositivi (il `REPORT.md` del
  modulo lo dichiara esplicitamente) e la review QA precedente è stata scritta
  sullo stub;
- è presente un **P0** di concorrenza sull'accesso alla microSD (task Wi-Fi vs
  task UI) che colpisce direttamente T07 (trasferimenti simultanei) e può
  corrompere la FAT;
- ci sono **5 P1** (MAC non vincolato, race di uscita/finalize, whitelist più
  permissiva del test T05, scritture NVS della clipboard, storico comandi rotto
  oltre 4 KB) e vari P2.

I punti “verdi” statici: codec header/CRC32 con self-test, chunk da 200 B,
limite 512 KB su invio e ricezione, `.part` + rename solo dopo CRC, path
traversal sanificato, nessuna `system()`/`popen()`/`exec*`/eval, NVS namespace
`orbit_espnow` con chiavi corte, legal warning prima della radio, `deinit`
ESP-NOW e ripristino della modalità WiFi nel percorso normale.

## Sintesi findings

| ID | Sev | Area | Sintesi |
|---|---|---|---|
| F01 | **P0** | streaming/file | I/O SD dal callback Wi-Fi in parallelo all'I/O SD del task UI, senza lock |
| F02 | P1 | ACK/sicurezza | I frame dati/ACK non sono vincolati al MAC del META/peer |
| F03 | P1 | deinit | `finalizeReceive` può sopravvivere al wait di 5 s; mutex cancellato mentre il callback è vivo |
| F04 | P1 | comandi | Whitelist case/whitespace-insensitive: `BEEP`, ` beep`, `beep\n` eseguono (T05) |
| F05 | P1 | clipboard NVS | `g_clipDirty` impostato a ogni frame, anche se il testo è identico; dirty azzerato prima della `put` |
| F06 | P1 | comandi/storico | Oltre 4 KB lo storico mostra “Nessun comando” e le righe recenti vengono perse |
| F07 | P2 | header/CRC | Il CRC copre solo il payload: type/flags/session/seq non sono protetti |
| F08 | P2 | comandi | `msg` remoto apre una modale bloccante; coda di 4, nessun rate limit |
| F09 | P2 | file | File da 0 B rifiutati (T09) |
| F10 | P2 | file/inbox | Nessun controllo dello spazio libero SD prima della ricezione (T09) |
| F11 | P2 | peer/RSSI | Manca il canale nella schermata Peer; peer fantasma mai espulsi; nessun hint per canale errato |
| F12 | P2 | deinit WiFi | `WiFi.setSleep(false)` e filtro promiscuo non ripristinati; connessione STA precedente persa |
| F13 | P2 | comandi | `serviceMesh` solo nel menu principale/scan: nei sottomenu i comandi non vengono serviti e la coda (4) scarta |
| F14 | P2 | ACK finale | Dopo un ACK finale perso, il blocco duplicato non fa ritrasmettere l'ACK finale; 80 ms di `g_finalizing` in più |
| F15 | P2 | peer | `ensurePeer` può aggiungere fino a 20 peer ESP-NOW con MAC arbitrari e non li rimuove: tabella piena = niente ACK |
| F16 | P3 | report | `REPORT.md` non dichiara clipboard max 160 / nome 16; tmp path diverso; taglia firmware del worktree, non quella finale |
| F17 | P3 | inbox | Un file ricevuto chiamato `x.crc` viene nascosto dall'inbox; `.crc.tmp`/`.bak` possono apparire in elenco |
| F18 | P3 | comandi | Il LED remoto resta acceso dopo l'uscita; `M5.Led` sulla ADV è da verificare |
| F19 | P3 | legal | `legalWarning` tronca le righe extra: sparisce la nota sul canale default |
| F20 | P3 | clipboard | `sendClipboardText` aggiorna la clipboard locale prima dell'invio: se il broadcast fallisce, il locale è già cambiato |
| F21 | P3 | UI | `wrapLines` tronca l'ultima riga a 18 caratteri + “..”; lo storico è comunque più largo del display |
| F22 | P3 | comandi | Il menu Comandi non ha un input libero: solo preset (niente `led 255 0 0` custom) |

## Findings

### F01 — P0 — Accesso SD concorrente tra task Wi-Fi e task UI

- **Dove**:
  - callback ESP-NOW: `EspNowMenu.cpp:584` (`openRxLocked` → `openWrite`),
    `:615-632` (`fileCrc` + `SD.rename` + sidecar in `finalizeReceive`),
    `:747-753` (`g_rx.file.write` in `handleData`);
  - task UI: `:1590` e `:1646-1661` (`fileCrc` + `file.read` in
    `transferFile`), `:1787` (`listEntries` nel file browser), `:1836`
    (`fileCrc` in `verifyInboxFile`), `:1870`/`:1890` (`listEntries`/`remove` in
    `deleteInboxFile`), `:1904` (`listEntries` in `inboxMenu`).
- **Impatto**: il callback `onReceive` gira nel task Wi-Fi; `handleData`,
  `handleMeta` e `finalizeReceive` leggono/scrivono/rinominano file sulla SD.
  Nello stesso momento il task UI può stare leggendo un file da inviare o
  verificando un file in Inbox. Anche se il driver serializza le singole
  transazioni, il modulo non serializza le proprie sequenze multi-step
  (CRC → rename → sidecar) e le operazioni FAT concorrenti: le operazioni
  possono intrecciarsi (finestra FAT condivisa, rename) con rischio di file
  corrotti, `file.read`/`write` falliti o crash. È esattamente lo scenario di **T07.3** (A→B e B→A
  simultanei) e di “Verifica” in Inbox durante una ricezione in background.
- **Fix**: una sola serializzazione per tutta la SD (mutex dedicato, meglio
  ricorsivo, preso da `storage::*` o da un wrapper del modulo) e spostare
  CRC/rename/sidecar fuori dal callback (worker o main loop). In alternativa
  disabilitare Inbox/verifica/invio mentre `g_rx.active || g_finalizing`, ma
  resta il full-duplex legittimo A→B + B→A. Aggiungere uno stress test con due
  trasferimenti simultanei e una verifica Inbox.

### F02 — P1 — Frame dati e ACK non vincolati al MAC del peer

- **Dove**: `g_rx.mac` è salvato in `openRxLocked` (`:599`), ma
  `handleData` (`:711-779`) non lo confronta mai con il MAC sorgente; anche il
  ramo `finishedDuplicate` (`:723-726`, ACK a `:770`) e `handleAck`
  (`:782-796`) accettano qualunque MAC. `beginTx` (`:380-386`) non memorizza il
  target della trasmissione.
- **Impatto**: `session` e `seq` viaggiano in chiaro nel frame (solo il payload
  ha CRC). Qualunque dispositivo sul canale può:
  - iniettare un blocco nella sessione attiva (stesso `session`, `seq` atteso)
    e far scrivere dati arbitrari nel file in ricezione; il CRC finale lo
    rifiuterà, ma il trasferimento legittimo viene distrutto;
  - forgiare ACK `StOk` per ogni blocco e l'ACK finale: il mittente mostra
    “File inviato e verificato” mentre il target non ha ricevuto nulla o ha un
    file incompleto;
  - duplicare/bloccare un trasferimento.
  Il `REPORT.md:175-177` dichiara che “l'identità usata dal protocollo è il
  MAC”: nel codice non è vero per dati e ACK. Non c'è cifratura (accettato
  dalla spec), ma il controllo del MAC sorgente è a costo zero.
- **Fix**: salvare il target in `beginTx` e verificare
  `memcmp(mac, g_txTarget, 6) == 0` in `handleAck`; in `handleData` verificare
  `memcmp(mac, g_rx.mac, 6) == 0` prima di accettare/ACKare, incluso il ramo
  duplicato. Non è autenticazione (il MAC è spoofabile), ma elimina iniezioni
  accidentali e il falso “verificato” da un peer qualsiasi. Documentare il
  limite residuo nel `REPORT.md`.

### F03 — P1 — Race uscita/finalize: callback oltre il wait di 5 s, mutex cancellato

- **Dove**: `finalizeReceive` (`:606-658`) gira nel callback Wi-Fi, imposta
  `g_finalizing=true` (`:758`), calcola il CRC di tutto il file, rinomina,
  scrive il sidecar e fa `delay(80)` (`:654`) prima di `g_finalizing=false`
  (`:657`). `abortReceiveForExit` (`:1079-1090`) aspetta al massimo
  100 × 50 ms = **5 s** e poi procede. `stopRadio` (`:1139-1157`) fa
  `esp_now_deinit()` e `runMenu`/`Cleanup` (`:2350-2363`) cancellano `g_mutex`
  (`:2358-2361`) mentre il callback potrebbe essere ancora dentro
  `finalizeReceive` (CRC di 512 KB su SD lenta + rename + sidecar).
- **Impatto**: con ESC durante la finalizzazione l'UI si blocca fino a 5 s
  (T08 “ESC reattivo”); se la SD è lenta e il CRC supera i 5 s, `stopRadio`
  disabilita ESP-NOW e cancella il mutex mentre il callback usa ancora
  SD/ESP-NOW e, più tardi, `setNotice()` prende un mutex cancellato →
  use-after-free/assert. Se `g_finalizing` resta true, tutti i META successivi
  ricevono `StBusy` fino al reboot. Lo stesso schema vale per un callback
  promiscuo in volo quando si cancella il mutex.
- **Fix**: non fare CRC/rename/sidecar nel callback: accodare un job al task
  principale (o a un worker) e far fare lì l'I/O; `stopRadio` deve attendere un
  flag “job RX concluso” senza tetto fisso e cancellare il mutex solo dopo
  `esp_now_deinit`/promiscuo confermati fermi; `g_finalizing` va azzerato in
  ogni percorso (inclusi errori). Niente `delay()` nel callback.

### F04 — P1 — Whitelist comandi più permissiva del test T05

- **Dove**: `lowerCopy` (`:817-825`) è usato per il verbo in
  `parseCommand` (`:833` `beep`, `:841` `led`, `:864` `msg`); `cleanText`
  (`:173-195`) converte tab/newline in spazi, collassa e trimmma. Inoltre
  `textinput::parseInt` (`src/utils/TextInput.cpp:180-191`) usa `strtol`, che
  accetta segno e spazi iniziali (`led +1 2 3`).
- **Impatto**: `BEEP`, `Beep`, `LED OFF`, ` beep`, `beep\n` vengono eseguiti;
  il T05 della review si aspetta che solo le forme esatte `beep`,
  `led <r> <g> <b>` / `led off`, `msg <testo>` abbiano effetto e che
  `BEEP`, ` beep`, `beep\n`, `beep 1` siano ignorati. Non c'è esecuzione
  arbitraria (restano solo beep/led/msg), ma la conformità al contratto di
  sicurezza e il test falliscono.
- **Fix**: confronto esatto dei token (niente `lowerCopy`) oppure dichiarare
  esplicitamente nel `REPORT.md`/spec che la whitelist è case-insensitive e
  aggiornare T05; per `led` usare un parser decimale stretto (solo cifre,
  0..255, niente segno/spazi).

### F05 — P1 — Clipboard: NVS riscritta a ogni frame, anche se invariata

- **Dove**: `handleClipboard` (`:916-942`) imposta `g_clipDirty = true` a ogni
  frame ricevuto (`:938`), senza confrontare con `g_clipboard`;
  `sendClipboardText` (`:1428-1433`) fa lo stesso anche se il testo è identico.
  `serviceBackground` (`:1348-1358`, chiamata da `serviceMesh` a `:2265` e
  `:2113`) esegue `saveClipboardIfDirty` a ogni giro.
  `saveClipboardIfDirty` (`:1161-1180`) azzera `g_clipDirty` **prima** della
  `putString` (`:1168` vs `:1177`).
- **Impatto**: un peer che ripete la stessa clipboard (o la invia in broadcast
  più volte) provoca una scrittura NVS per ogni frame servito. È contro il
  requisito T10 “scrittura NVS solo al cambio” e consuma cicli della partizione
  NVS; con un flood si può arrivare a usura/degrado e a UI lenta (la `put`
  avviene nel task UI). Se la `putString` fallisce, il valore è già stato
  marcato pulito e non viene ritentato: si perde la persistenza.
- **Fix**: confrontare `text != g_clipboard` prima di incrementare revision e
  impostare dirty; azzerare dirty solo dopo una `put/remove` riuscita;
  coalescing/rate limit (es. max una scrittura ogni 2–5 s) e ignore dei
  duplicati.

### F06 — P1 — Storico comandi rotto oltre 4 KB

- **Dove**: `kHistoryReadBytes = 4096` (`:46`), `kHistoryMaxLines = 60` (`:45`);
  `logCommand` (`:1263-1287`) chiama
  `storage::readText(kHistoryPath, history, kHistoryReadBytes, true)` (`:1269`)
  **ignorandone il valore di ritorno**, poi riscrive solo ciò che ha letto
  (`:1286`); `showHistory` (`:1289-1308`) tratta il `false` di `readText`
  (`:1291-1293`) come “Nessun comando registrato”.
  `storage::readText` (`src/utils/Storage.cpp:126-150`) restituisce `false` se
  il file supera `maxBytes` e lascia in `out` solo i **primi** 4096 byte.
- **Impatto**: una riga tipica (`[123456s] RX nome: testo`, nome 16 + testo 64)
  è ~95–100 byte; dopo ~41 comandi il file supera 4096 byte. Da quel momento:
  - “Mostra storico” mostra **“Nessun comando registrato”** anche se il file
    esiste;
  - `logCommand` legge solo la testa del file, cancella la coda e riscrive:
    i comandi più recenti vengono persi a ogni nuovo comando;
  - il file resta comunque ~4 KB, quindi il test “storico limitato” passa
    formalmente ma il contenuto è sbagliato.
  Inoltre ogni comando fa un read+rewrite completo della SD: un flood di
  comandi causa usura e blocchi del task UI.
- **Fix**: leggere la coda del file (seek da fine) o almeno 16 KB; controllare
  il ritorno di `readText`; tenere le ultime 60 righe dalla coda e riscrivere
  solo quelle (o append + rotazione periodica); aggiungere “ignorato” alla
  riga di storico per i comandi non validi.

### F07 — P2 — Il CRC32 non copre l'header (type/flags/session/seq)

- **Dove**: `encodePacket` (`:110-125`) scrive
  `crc32(packet.payload, packet.len)` (`:122`); `decodePacket` (`:127-150`)
  verifica solo il payload (`:143`). I campi `type`, `flags`, `session`,
  `seq`, `len` non sono coperti. Il self-test (`:153-204`) non prova nessuna
  corruzione dell'header.
- **Impatto**: una corruzione accidentale dell'header viene accettata; un
  `seq`/`type` alterato può causare `StSeq`/abort o far interpretare un frame
  come un altro tipo. Caso concreto: l'ultimo blocco da 1 byte ha `type=3`,
  `flags=2` (last) e `len=1`; se il `type` diventa 4 (Ack), `handleAck`
  (`:782-796`) lo accetta come ACK finale, con `seq` del blocco e
  `status = payload[0]` (un byte del file). Se il valore è 0 il mittente può
  mostrare “verificato” senza l'ACK finale del ricevente; altrimenti mostra un
  errore falso. La probabilità di una corruzione naturale è bassa; un peer
  malevolo può comunque forgiare (F02). Il CRC del file protegge il contenuto
  finale, non la semantica dei frame.
- **Fix**: calcolare il CRC su header (escluso il campo CRC) + payload, oppure
  aggiungere un CRC/checksum dell'header; validare in `handleAck` anche il
  tipo/kind e il range dello status; aggiungere al self-test la corruzione di
  un byte di header. Se il formato wire deve restare compatibile, dichiarare
  il limite nel `REPORT.md`.

### F08 — P2 — `msg` remoto bloccante e flood di comandi (DoS UI)

- **Dove**: `handleCommand` (`:884-914`) accoda fino a 4 comandi
  (`g_pending[4]`, `:154`; check `:906-908`), `processPending` (`:1318-1346`)
  per `CmdKind::Msg` chiama `ui::message(...)` (`:1338-1342`), che resta in
  attesa di un tasto; `serviceMesh` (`:1360-1397`) processa la coda nel menu
  principale/scan.
- **Impatto**: qualunque peer sul canale può aprire una modale a schermo intero
  che blocca il menu finché l'utente non preme un tasto; può riempire la coda
  (4) e ripetere, rendendo il dispositivo inutilizzabile a distanza. Anche
  `beep`/`led`/clipboard non hanno rate limit; `logCommand` fa I/O SD per ogni
  comando. La whitelist impedisce codice arbitrario, non l'abuso di
  disponibilità.
- **Fix**: per i messaggi remoti usare una notifica non bloccante (toast/notice
  con auto-dismiss), o al massimo una modale con timeout breve; rate limit per
  mittente (es. 1 comando/s, token bucket); coda piena = scarta e registra;
  limitare le scritture dello storico. Aggiornare T05/T07 con un test di flood.

### F09 — P2 — File da 0 byte rifiutati (T09)

- **Dove**: `handleMeta` (`:687`) rifiuta `fileSize == 0` con `StSize`;
  `chooseFile` (`:1762`) e `transferFile` (`:1575`) rifiutano `size == 0`;
  `finalizeReceive` (`:608`) assume `totalChunks >= 1`.
- **Impatto**: T09 elenca “file da 0 B” tra i casi edge attesi (“nessun crash,
  offset e CRC corretti”). Il rifiuto è pulito, non un crash, ma il test non
  passa e il formato non gestisce l'empty file. La spec parla di “max 512 KB”
  senza minimo.
- **Fix**: gestire esplicitamente `totalChunks == 0` (META accettato, nessun
  dato, `crc32` del vuoto = 0, finalize + ACK finale), oppure dichiarare nel
  `REPORT.md` e nella spec che i file vuoti sono rifiutati e aggiornare T09.

### F10 — P2 — Nessun controllo dello spazio libero SD prima della ricezione

- **Dove**: `openRxLocked` (`:568-604`) apre il `.part` senza verificare lo
  spazio; `handleMeta` (`:696-700`) controlla solo `hasSD` e la dimensione
  dichiarata (≤512 KB).
- **Impatto**: T09 si aspetta che una SD piena/quasi piena rifiuti la ricezione
  **prima** di scrivere. Qui il file viene scritto fino a riempire la card; il
  fallimento arriva a metà/fine, il ricevente mostra spesso `StCrc` (dimensione
  diversa) invece di “SD piena”, e il mittente spreca il trasferimento. Il file
  temporaneo viene comunque rimosso, quindi non viene pubblicato un file
  troncato.
- **Fix**: prima di aprire il `.part`, verificare
  `SD.totalBytes() - SD.usedBytes() >= fileSize + margine` (e lo spazio per il
  sidecar); rispondere `StIo` con un messaggio dedicato “SD piena”. Aggiungere
  il test con card quasi piena.

### F11 — P2 — Peer: canale assente, peer fantasma, canale errato non spiegato

- **Dove**: `drawPeerScan` (`:2048-2085`) mostra nome, MAC, RSSI e età, ma non
  il canale; `meshInfo` (`:2194-2204`) mostra il canale solo nel Setup.
  `touchPeerLocked` (`:327-348`) non scarta mai i peer vecchi: restano nella
  tabella (fino a 12) con `lastSeen` che invecchia finché non si esce dal
  modulo o si cambia canale (`startRadio` azzera `g_peerCount`).
- **Impatto**: T01.2 chiede “nome, MAC, canale, RSSI e ultimo visto”; il canale
  per-peer manca. Dopo che un peer si allontana/spegne, resta in elenco per
  sempre (peer fantasma con età crescente); se i due device sono su canali
  diversi la UI dice solo “Nessun peer”, senza suggerire il canale (T01.5
  chiede un messaggio chiaro).
- **Fix**: mostrare il canale corrente nell'header/riga “Io” (e/o per peer: su
  ESP-NOW tutti i peer sono sul canale corrente) e scartare/marcare offline i
  peer con `lastSeen` più vecchio di 15–30 s; dopo ~8 s senza peer mostrare
  “Verifica che i peer usino lo stesso canale (1–13)”.

### F12 — P2 — Pulizia WiFi: sleep, filtro promiscuo e connessione non ripristinati

- **Dove**: `WiFi.setSleep(false)` (`:1113`) è globale e non viene ripristinato
  in `stopRadio` (`:1148-1151`), che ripristina solo la **modalità**.
  `startSniffer` (`:1040-1058`) imposta il filtro MGMT e
  `stopSniffer` (`:1060-1067`) non lo ripristina.
  `WiFi.disconnect()` (`:1112`) chiude un'eventuale connessione STA precedente;
  `g_prevMode` (`:2332`) salva solo la modalità.
- **Impatto**: T08 chiede “rilasciare WiFi/ESP-NOW e ripristinare lo stato”.
  Se prima dell'ingresso il WiFi era in STA con una connessione, dopo l'uscita
  la modalità è STA ma la connessione è persa e il modem sleep resta disabilitato
  (consumi); il filtro promiscuo MGMT resta installato e può alterare il
  comportamento di un modulo successivo che abilita il promiscuo senza
  impostare il proprio filtro. Non è un crash, ma è uno stato globale sporco.
- **Fix**: salvare/ripristinare `WiFi.getSleep()` e il filtro promiscuo
  precedente; documentare che le connessioni STA non vengono ripristinate (o
  riconnettere se necessario); salvare/ripristinare anche il canale se la
  modalità precedente resta attiva.

### F13 — P2 — Comandi serviti solo nel menu principale/scan; coda da 4

- **Dove**: `serviceMesh()` è chiamata solo in `peerScan` (`:2113`) e
  `mainMenu` (`:2265`); `MenuManager::run` (`src/core/MenuManager.cpp:354-502`)
  non fa ticking del modulo. `handleCommand` (`:906-908`) scarta oltre 4.
- **Impatto**: mentre l'utente è in Inbox, Comandi, Clipboard, Setup o nel file
  browser, i comandi ricevuti non vengono eseguiti; oltre 4 vengono persi
  silenziosamente. Un `beep`/`msg` può arrivare molti secondi dopo o non
  arrivare. Il timeout RX di 6 s viene comunque controllato all'arrivo del
  frame successivo, quindi non blocca per sempre la ricezione, ma la latenza
  dei comandi non è quella attesa da T05/T07.
- **Fix**: un tick periodico unico (es. in `MenuManager::run` o in un callback
  della UI) che chiami `serviceMesh`/`serviceBackground`; oppure processare i
  comandi in un task dedicato con notifica non bloccante; aumentare/metricare
  la coda e registrare gli scarti.

### F14 — P2 — ACK finale perso: nessuna ritrasmissione; 80 ms di busy

- **Dove**: `finalizeReceive` (`:650-657`) invia l'ACK finale, poi `delay(80)`
  e lo reinvia; `handleData` (`:769-771`) risponde a un blocco finale
  duplicato solo con `AckData StOk`, non con un nuovo ACK finale.
  `transferFile` (`:1726-1744`) attende 5 s e non ritrasmette il blocco finale.
- **Impatto**: se l'ACK finale va perso (o il secondo arriva mentre il mittente
  non è ancora in `waitAck`), il file è già pubblicato ma il mittente mostra
  “Conferma finale assente” (falso negativo). Se un secondo trasferimento parte
  entro gli 80 ms, il ricevente è ancora `g_finalizing` e risponde `StBusy`:
  il mittente abortisce. Inoltre un ACK finale `StIo` (rename/sidecar fallito)
  cade nel ramo “conferma assente” invece di mostrare l'errore reale.
- **Fix**: su un blocco finale duplicato rinviare l'ACK finale (non solo
  `AckData`), o aggiungere una richiesta esplicita di ACK finale; azzerare
  `g_finalizing` prima del delay; togliere il `delay` spostando l'invio nel
  task principale; gestire `StIo` nel messaggio finale.

### F15 — P2 — Tabella peer ESP-NOW: crescita con MAC arbitrari, nessuna rimozione

- **Dove**: `kMaxPeers = 12` (`:37`) limita solo `g_peers`; `ensurePeer`
  (`:413-427`) aggiunge alla tabella ESP-NOW (limite IDF 20) il MAC di ogni
  mittente prima di un ACK (`replyAck` `:473`) o di una risposta clipboard
  (`:972`). `touchPeerLocked` (`:327-348`) espelle da `g_peers` ma non chiama
  `esp_now_del_peer`.
- **Impatto**: un peer ostile (o una rete affollata) può inviare META/ClipRequest
  con MAC sorgente arbitrari e riempire i 20 slot ESP-NOW; quando la tabella è
  piena `ensurePeer` fallisce e il dispositivo non riesce più a inviare ACK o
  file ai peer legittimi fino al riavvio. Il `REPORT.md` dichiara “12 peer +
  broadcast”, ma il limite reale della tabella è 20 e non viene gestito.
- **Fix**: tenere una mappa MAC→slot con LRU e chiamare `esp_now_del_peer`
  quando si espelle; limitare l'aggiunta a peer noti/visti (Hello) o a un
  numero massimo configurabile; se la tabella è piena rifiutare/registrare e
  non aggiungere; documentare il limite reale.

### F16 — P3 — Lacune del `REPORT.md` e taglia firmware

- **Dove**: `src/modules/espnow/REPORT.md`: i “Dati di base” (righe 33–40) non
  dichiarano la **dimensione massima clipboard (160)** né la **lunghezza nome
  peer (16)**; il path temporaneo descritto (righe 76–78) è
  `/espnow/tmp/rx_<session>.part`, mentre il codice usa
  `rx_<session>_<attempt>.part` (`:573-574`). La taglia (righe 146–156) è
  **2.426.096 B / 92,5% di 2.621.440 B**, riferita al worktree del modulo con
  la vecchia partizione; l'artefatto finale presente in
  `.pio/build/cardputer-adv-offensive/firmware.bin` è **2.618.256 B** e la
  `partitions.csv` finale ha slot app da **0x2B0000 = 2.818.048 B** (~195 KiB
  liberi, 92,9%). La build finale non è stata rigenerata in questa verifica.
- **Impatto**: la review richiede esplicitamente i limiti di clipboard/nome e
  la dimensione firmware finale; il report non li fornisce e la cifra è
  riferita a una configurazione diversa. Non è un bug di runtime, ma rende non
  verificabile il criterio di uscita T10 (“dimensione riportata in REPORT.md”).
- **Fix**: aggiungere i due limiti, correggere il path temporaneo e riportare
  la taglia dell'artefatto finale con la partizione usata (o rigenerare la
  build e aggiornare la tabella). Nota budget: con il vecchio slot da 2,5 MiB
  resterebbero solo ~3 KB; la nuova tabella da 2,6875 MiB è quella da usare.

### F17 — P3 — Inbox: file `.crc` nascosti e residui `.tmp`/`.bak`

- **Dove**: `isCrcSidecar` (`:259-262`) esclude ogni nome che termina in
  `.crc`; `inboxMenu` (`:1904-1911`) e `deleteInboxFile` (`:1869-1897`) usano
  lo stesso filtro. `storage::writeAtomic` (`Storage.cpp`) crea
  `<path>.tmp` e, in caso di replace, `<path>.bak` nella stessa directory.
- **Impatto**: un file ricevuto che si chiama `x.crc` viene pubblicato come
  `x.crc` + sidecar `x.crc.crc`, ma l'inbox lo nasconde e non lo si può
  verificare/cancellare dalla UI. Dopo un crash durante la scrittura del
  sidecar, `x.crc.tmp`/`x.crc.bak` possono comparire in elenco come file
  ricevuti. Non è perdita di dati, ma confusione e file non gestibili.
- **Fix**: filtrare solo i `.crc` il cui file base esiste (o spostare i sidecar
  in `/espnow/meta/`); escludere `.tmp`/`.bak` dall'elenco e ripulirli
  all'ingresso insieme ai `.part`.

### F18 — P3 — LED remoto non ripristinato; `M5.Led` da verificare sulla ADV

- **Dove**: `applyLed` (`:1312-1316`) usa `M5.Led.setAllColor/display`;
  `processPending` (`:1334-1336`) non salva lo stato precedente; `Cleanup`
  (`:2350-2363`) non resetta il LED. `REPORT.md:183` cita “WS2812 su
  Stamp-S3A, GPIO 21”.
- **Impatto**: dopo un comando `led` il LED resta acceso anche dopo l'uscita dal
  modulo, alterando lo stato visto dagli altri moduli/UI. Se `M5.Led` non è
  disponibile o mappa un GPIO diverso sulla Cardputer ADV, il comando è un
  no-op o (peggio) pilota un pin sbagliato: da verificare su hardware.
- **Fix**: salvare/ripristinare lo stato del LED nel cleanup (o spegnerlo),
  verificare la mappa `M5.Led` sulla ADV e, se non disponibile, rimuovere il
  comando `led` o usare l'API corretta; documentare il comportamento.

### F19 — P3 — `legalWarning`: la nota sul canale viene troncata

- **Dove**: `runMenu` (`:2321-2328`) passa 6 righe fisse + 4 extra
  (`Frame...`, `File...`, `Nessun router...`, `canale (default 1)`);
  `ui::legalWarning` (`src/core/Ui.cpp:147-149`) fa `lines.resize(8)` e
  sostituisce l'ottava con “Vedi README”. Le righe extra 2–4 (incluse
  “canale (default 1)”) spariscono.
- **Impatto**: l'avviso legale c'è e la parte critica resta, ma l'utente non
  vede l'indicazione operativa sul canale default; il messaggio è più povero
  del previsto. Non è una violazione del BRIEF (l'avviso è mostrato).
- **Fix**: ridurre le righe fisse o alzare il limite a 10; mettere la nota sul
  canale in una delle prime righe extra che sopravvivono al resize.

### F20 — P3 — `sendClipboardText`: clipboard locale cambiata anche se l'invio fallisce

- **Dove**: `sendClipboardText` (`:1423-1445`) aggiorna `g_clipboard`,
  `g_clipRevision`, `g_clipDirty` (`:1428-1433`) **prima** di
  `sendBroadcast` (`:1442`); se il send fallisce restituisce `false`
  (`:1444`) ma il valore locale è già cambiato e verrà salvato.
- **Impatto**: il menu dice “Invio fallito” ma la clipboard locale è stata
  sostituita; l'utente può perdere il valore precedente senza che sia stato
  condiviso. Basso impatto, ma UX incoerente.
- **Fix**: eseguire il broadcast prima di aggiornare lo stato locale, oppure
  ripristinare il valore precedente in caso di fallimento (e non marcare
  dirty).

### F21 — P3 — `wrapLines` tronca l'ultima riga a 18 caratteri; storico più largo del display

- **Dove**: `wrapLines` (`:280-298`) quando `lines.size() == maxLines` riduce
  l'ultima a 18 caratteri + “..” (`:290-296`); `showHistory` (`:1289-1308`)
  passa righe fino a ~96 caratteri a `ui::message`, che le disegna con font 0
  a partire da x=11 su 240 px (circa 38 caratteri utili) e non le manda a capo.
- **Impatto**: testo troncato/illeggibile nella clipboard lunga e nello storico;
  per `msg` (80 char, 5 righe da 38) il limite non viene raggiunto, per la
  clipboard 160 char a 38 = 5 righe, quindi il caso 18+“..” è raro, ma lo
  storico è comunque tagliato dal display. Basso impatto.
- **Fix**: usare un'ellissi a larghezza piena per l'ultima riga; avvolgere o
  troncare le righe dello storico alla larghezza reale del display.

### F22 — P3 — Menu Comandi senza input libero

- **Dove**: `commandMenu` (`:1958-1985`) offre solo Beep, LED verde/rosso/off,
  Messaggio e storico; non c'è un campo “Comando libero”. La whitelist del
  ricevente supporta `led <r> <g> <b>` arbitrario e qualunque testo `msg`.
- **Impatto**: la spec parla di “comandi: testo broadcast”; dall'UI non si può
  inviare un `led 255 0 0` custom né provare un comando non whitelisted (T05
  richiede un peer di test). I preset coprono i casi principali, ma la funzione
  “testo” è incompleta lato mittente.
- **Fix**: aggiungere “Comando libero...” con editor di testo, sanificazione e
  validazione locale; oppure dichiarare nel REPORT che il mittente supporta
  solo i preset e che i test T05 usano un peer esterno.

## Top 5 (i più gravi)

1. **F01 — P0 — I/O SD concorrente** (`:584`, `:615-632`, `:747-753` vs
   `:1590`, `:1646-1661`, `:1836`, `:1787`, `:1904`): T07.3 e “Verifica” in
   Inbox durante una ricezione possono corrompere la FAT o far fallire i
   trasferimenti; manca qualsiasi serializzazione. Fix: mutex SD unico +
   CRC/rename fuori dal callback Wi-Fi.
2. **F02 — P1 — Dati/ACK non vincolati al MAC** (`:599`, `:711-779`,
   `:782-796`): iniezione di blocchi e ACK falsi, falso “File inviato e
   verificato”; il REPORT dice che l'identità è il MAC ma il codice non lo
   controlla. Fix: target MAC in `beginTx`, `memcmp` in `handleData`/
   `handleAck`.
3. **F03 — P1 — Race di uscita/finalize** (`:606-658`, `:654`, `:758`,
   `:1079-1090`, `:2350-2363`): ESC durante il CRC di 512 KB blocca l'UI fino a
   5 s; oltre i 5 s `deinit`/delete del mutex mentre il callback è vivo →
   crash/use-after-free; `g_finalizing` bloccato → `StBusy` permanente. Fix:
   finalize fuori dal callback, wait senza tetto e mutex cancellato solo a
   radio ferma.
4. **F05 — P1 — Clipboard NVS a ogni frame** (`:938`, `:1168`, `:1177`,
   `:1349`): flash wear e I/O NVS inutile, contro T10; un flood di clipboard
   può degradare la UI/NVS; `put` fallita persa perché dirty è già false. Fix:
   confronto prima di dirty, dirty solo su `put` riuscita, rate limit.
5. **F06 — P1 — Storico comandi oltre 4 KB** (`:46`, `:1269`, `:1286`,
   `:1291-1293`; `Storage.cpp:126-150`): dopo ~41 comandi “Mostra storico” dice
   “Nessun comando registrato” e `logCommand` perde le righe recenti. Fix:
   leggere la coda (o 16 KB), controllare il ritorno, tenere le ultime 60.

**Subito dopo (P1/P2)**: F04 (whitelist accetta `BEEP`/` beep`/`beep\n`, T05),
F07 (header fuori dal CRC, type confusion con blocco da 1 byte), F08 (`msg`
remoto modale + flood), F11 (canale/peer fantasma), F12 (stato WiFi sporco).

## Copertura rispetto a T01–T10

| Test | Stato in questa verifica | Note |
|---|---|---|
| T01 peer/RSSI | **Non eseguito** (no hardware) | F11: manca il canale per peer, peer fantasma, hint canale assente; RSSI solo con promiscuo nello scan |
| T02 file 10 KB | **Non eseguito** | Logica di chunk/ACK/CRC presente; F01 (SD race), F07, F14; hash/MD5 non verificabili senza device |
| T03 CRC errato | **Non eseguito** | `finalizeReceive` rifiuta e rimuove il tmp; self-test payload; F07 (header non coperto); serve un sender QA per la corruzione |
| T04 timeout/ACK perso | **Non eseguito** | Costanti 450 ms × 3, finale 5 s, RX 6 s; F03 (5 s di uscita), F14 (falso negativo finale) |
| T05 comandi | **Non eseguito** | F04 (case/whitespace), F06 (storico), F08 (modale/flood); grep: nessuna `system`/`popen`/`exec`/eval |
| T06 clipboard | **Non eseguito** | Persistenza NVS presente; F05 (scritture non deduplicate), F20 (send fallito), F12 (namespace ok) |
| T07 end-to-end | **Non eseguito** | F01 (T07.3 simultaneo), F13 (coda/servizio), F14 (80 ms), F15 (tabella peer) |
| T08 stop WiFi | **Non eseguito** | `esp_now_deinit` + modalità WiFi nel percorso normale; F03 (race/freeze), F12 (sleep/filtro/connessione) |
| T09 limiti/edge | **Non eseguito** | 512 KB sì, path traversal sanificato, seq in-order; F09 (0 B), F10 (SD piena), F11 (peer) |
| T10 review statica | **Parziale** | Commit modulo solo 3 file (nessun file vietato); NVS ok; chunk 200; `.part`+rename; F03/F04/F05/F06/F07/F12/F16 sono le eccezioni |

## Limiti della verifica

- **Nessuna build** (vietata dal incarico): non ho rigenerato `firmware.bin`.
  L'artefatto esistente in `.pio/build/cardputer-adv-offensive/firmware.bin` è
  di 2.618.256 B (21/11 15:43) e la mappa include `EspNowMenu.cpp.o`; la
  `partitions.csv` finale ha slot da 2.818.048 B, quindi ~195 KiB liberi. Il
  `REPORT.md` del modulo riporta 2.426.096 B/92,5% di 2.621.440 B: cifra del
  worktree, non dell'integrato (F16).
- **Nessun test su due Cardputer**: RSSI/promiscuo, ACK generati dal callback
  (`esp_now_add_peer`/`esp_now_send` dentro `onReceive`), `M5.Led`, SD lenta e
  trasferimenti simultanei restano da provare su hardware. Il
  `REPORT.md:109-112` ammette che l'hardware non è stato usato.
- **La review QA è pre-implementazione**: il suo stato “BLOCCATO — modulo non
  implementato” non è più vero, ma i suoi T01–T10 restano non eseguiti e non
  forniscono evidenze (hash, screenshot, heap). Senza queste, i criteri di
  uscita della review non sono soddisfatti.
- **Nessun test host/unitario** del protocollo: l'unico test è `proto::selfTest`
  eseguito all'ingresso (`:2316`), che copre CRC noto, round-trip, frame
  troncato/magic/CRC payload, ma non l'header (F07), l'ACK, i path o il parser
  comandi.

## Raccomandazioni di priorità

1. Correggere F01/F03 (SD e finalize fuori dal callback, mutex e attesa di
   chiusura) prima di qualsiasi test T07.
2. Correggere F02 (MAC binding) e F05/F06 (NVS/storico) prima della review di
   accettazione.
3. Allineare F04 alla whitelist esatta (o aggiornare il contratto T05) e
   aggiungere i test host per header/CRC, seq, ACK, path traversal, parser
   comandi.
4. Eseguire T01–T08 su due Cardputer con evidenze (hash/MD5, screenshot, heap)
   e aggiornare `REPORT.md` con la taglia finale e i limiti mancanti (F16).
