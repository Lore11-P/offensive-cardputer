# 03 — Audio/Mic: bug report sul codice finale integrato

- Data: 2026-11-21
- Repo: `offensive-cardputer`, commit `f19b6d0` (working tree pulito).
- Metodo: sola lettura statica di BRIEF, `specs/03-audio.md`, review QA
  `orbit-wt-qa/docs/integration/reviews/03-audio.md`, `src/modules/audio/REPORT.md`,
  `AudioMenu.cpp`, utility condivise (`Display`, `Storage`, `Navigation`,
  `PowerTimer`, `TextInput`, `MenuManager`, `AudioOut`) e librerie vendored
  M5Unified / ESP32-audioI2S.
- **Nessuna modifica al codice, nessuna build, nessun test su hardware.**
  I finding sono statici: la conferma finale richiede i test manuali in §6.
- Il `.pio/build/cardputer-adv-offensive/firmware.bin` presente è di
  2.618.256 byte; non è stato ricompilato per questo report.

## 1. Esito sintetico

| Gravità | Conteggio | Note |
|---|---:|---|
| P0 | 0 | nessun crash/corruzione certa all'avvio |
| P1 | 3 | A-01, A-02, A-03 |
| P2 | 12 | A-04 … A-15 (A-05 da confermare su hardware) |

Nessun P0. I tre P1 sono tutti riproducibili con uso normale o con SD
piena/rimossa; A-01 in particolare si manifesta su **ogni** riproduzione memo.
A-02/A-03 richiedono un errore SD (piena, rimossa, read-only) ma sono proprio
i casi che la review W10/D2/D3 considera bloccanti.

## 2. Top 5

1. **A-01 (P1)** — Streaming memo: il buffer in riproduzione viene
   sovrascritto prima che lo speaker lo abbia finito. Click/glitch periodici
   su ogni memo; peggiore a 44,1 kHz stereo.
2. **A-02 (P1)** — `patchWavHeader()` può lasciare il cursore del file a metà
   (offset 4/8/40). Il blocco PCM successivo viene scritto nell'header: WAV
   corrotto e non riproducibile. Il valore di ritorno è ignorato.
3. **A-03 (P1)** — Errori di scrittura/flush non verificati (`getWriteError`,
   `flush`, dimensione finale). Con SD piena/rimossa il file può restare
   invalido ma la UI lo mostra come “salvato”.
4. **A-04 (P2)** — Il limite “max 40 memo” non limita l'heap: `listEntries`
   materializza e ordina **tutti** i file della directory, poi `scanMemos`
   taglia a 40. Con centinaia/migliaia di WAV si esaurisce lo heap (niente
   PSRAM) o si blocca la UI.
5. **A-05 (P2, da provare)** — Radio → “Stato / ferma” costruisce il decoder
   (`audioout::decoder()`) solo per leggere `isRunning()`: ~40 KB di heap e
   `I2S_NUM_0` installato. M5Unified al `M5.Mic.begin()` tenta
   uninstall/retry e può “rubare” la porta, ma lo stato del decoder resta
   incoerente; se il retry fallisce il mic non parte fino al reboot.

## 3. Findings

### A-01 — P1 — Buffer di playback sovrascritto (memo streaming)

- **Dove:** `src/modules/audio/AudioMenu.cpp:988-1005` (lettura in
  `g_pcm[next]` a :993, `playRaw` a :1001, `next ^= 1` a :1005).
- **Contratto violato:** M5Unified, `Speaker_Class.hpp:207-209`: con due
  buffer alternati bisogna aspettare che `isPlaying(channel)` scenda sotto 2
  prima di riempire il successivo, oppure usare `setBufferReleaseCallback`.
- **Sequenza:** `playRaw(buf0)` accoda e torna subito; `playRaw(buf1)` accoda
  e torna; il ciclo legge il blocco successivo in `buf0` **mentre** il task
  speaker sta ancora leggendo `buf0`. Solo la *terza* `playRaw` aspetta uno
  slot, ma la lettura è già avvenuta.
- **Impatto:** i primi millisecondi di ogni blocco rifinito contengono il
  blocco successivo. A 16 kHz mono il blocco è 64 ms → click periodico
  (~15 Hz) e voce “graffiata”; a 44,1 kHz stereo il blocco è 11,6 ms, quindi
  la percentuale di audio corrotto è molto maggiore. Nessun crash, ma il
  criterio M3/M4 “playback corretto” non è soddisfatto.
- **Fix:** prima di `file.read()` attendere `M5.Speaker.isPlaying(1) < 2`
  (con `navigation::poll()` nel wait), oppure registrare una release callback
  per buffer e riempire solo il buffer rilasciato; in alternativa usare 3
  buffer. Non basta la `playRaw` bloccante: protegge la coda, non il buffer
  già in uso.
- **Test:** memo 60 s 16 kHz mono e 44,1 kHz stereo, ascolto su speaker e
  registrazione dell'uscita; confronto con il file sorgente.

### A-02 — P1 — `patchWavHeader()` può corrompere il file

- **Dove:** `src/modules/audio/AudioMenu.cpp:223-237`; chiamate a :702-704
  (ogni secondo) e :719 (chiusura).
- **Difetto:** la funzione fa `seek(4)`, scrive, `seek(40)`, scrive,
  `seek(0, SeekEnd)`. Se una di queste operazioni fallisce, ritorna `false`
  **senza ripristinare la posizione**. Il chiamante ignora il valore e
  aggiorna comunque `r.lastPatch` (:704), quindi il retry avviene solo dopo
  altri 32 KB.
- **Impatto:** su SD piena/rimossa o errore transitorio durante il patch, il
  successivo `writeAll` scrive PCM a offset 4/8/40: RIFF, `fmt `, `data` e i
  primi campioni vengono sovrascritti. Il file può perdere l'header e non
  essere più riproducibile; con l'errore in chiusura si perde anche l'ultima
  finalizzazione.
- **Fix:** salvare `file.position()` e ripristinarla in ogni uscita (scope
  guard); su errore impostare `r.error`, fermare la registrazione e
  finalizzare o cancellare il file; controllare il valore di ritorno in
  `recorderWrite` e `recorderFinish`; verificare a fine stop che
  `file.size() == 44 + r.bytes` e che i campi RIFF/data siano corretti.
- **Test:** card con pochi MB liberi, REC di 5–10 s fino a errore; controllare
  header e riproducibilità con `ffprobe`/Audacity (W10/D2).

### A-03 — P1 — Errori di scrittura/flush non verificati, file “salvato” invalido

- **Dove:** `src/modules/audio/AudioMenu.cpp:211-221` (`writeAll`),
  `:708-726` (`recorderFinish`), `:797-803` (percorso errore in `runRecorder`).
- **Difetto:** `writeAll` guarda solo il conteggio ritornato da
  `File::write`; non legge `File::getWriteError()`. `File::flush()` ritorna
  `void` e non viene verificato; `recorderFinish` ignora il ritorno di
  `patchWavHeader`. Con il VFS/FAT l'errore può emergere solo al flush o alla
  chiusura, non alla `write()`.
- **Impatto:** SD piena, card rimossa o read-only → WAV troncato/header
  vecchio, ma `lastSaved` viene impostato comunque (:799-800, :805-806) e la
  schermata mostra “Ultimo: …”. Il test W10 non distingue un file valido da
  uno corrotto. Il progetto già usa il pattern corretto in
  `src/utils/Storage.cpp` (`writeAtomic` controlla `getWriteError`) e in
  `src/utils/AudioOut.cpp` (`drainChunk`); il registratore audio no.
- **Fix:** dopo `flush()`/`close()` controllare `getWriteError()`, la
  dimensione reale e (se possibile) rileggere i 44 byte di header; su errore
  non impostare `lastSaved`, mostrare “file non valido” e proporre/effettuare
  la cancellazione. Usare `f_sync`/`File::sync` se disponibile.
- **Test:** riempimento card durante REC, estrazione a caldo, card read-only
  (D2/D3/D5/W10).

### A-04 — P2 — Il limite di 40 memo non limita l'heap

- **Dove:** `src/modules/audio/AudioMenu.cpp:1041-1055` (`scanMemos`);
  `src/utils/Storage.cpp:170-213` (`listEntries`).
- **Difetto:** `listEntries()` apre la directory, inserisce **tutte** le voci
  in un `std::vector<Entry>` (nome `std::string`), ordina e deduplica; solo
  dopo `scanMemos` si ferma a 40. Il commento “shortcut: oltre 40 memo la
  lista si ferma qui” vale per la UI, non per memoria e tempo.
- **Impatto:** con 500–5.000 WAV in `/audio/memo` si allocano decine/centinaia
  di KB (niente PSRAM, 320 KB SRAM), con rischio di OOM, frammentazione e
  attesa lunga prima della schermata; in caso di fallimento appare “Nessun
  memo”. Il criterio G6 (“buffer piccoli, niente PSRAM”) non copre questo
  caso.
- **Fix:** iterare direttamente la directory con un cap a 40 (o aggiungere
  `storage::listEntries(path, max)`), senza ordinare l'intero contenuto;
  tenere solo i primi 40 nomi e liberare subito gli altri.
- **Test:** 500 file piccoli in `/audio/memo`; misurare heap prima/dopo,
  tempo di apertura e stabilità su 20 aperture.

### A-05 — P2 (da provare) — Radio “Stato” costruisce decoder e I2S0

- **Dove:** `src/modules/radio/RadioMenu.cpp:724`
  (`audioout::decoder().isRunning()`); `src/utils/AudioOut.cpp:276-282`
  (`decoder()` = `new Audio()`); `ESP32-audioI2S/src/Audio.cpp:227`
  (il costruttore installa `I2S_NUM_0`); `AudioMenu.cpp:83` / `:549` / `:740`
  (`M5.Mic.begin()`).
- **Difetto:** la schermata “Stato / ferma” della Radio, raggiungibile **prima
  di qualsiasi playback**, costruisce il decoder solo per leggerne lo stato.
  Questo alloca ~40 KB di heap e installa `I2S_NUM_0`, la stessa porta del
  microfono. M5Unified, se `i2s_driver_install` fallisce, fa
  `uninstall`+retry (`Mic_Class.inl:427-431`): quindi il mic può prendersi la
  porta e partire, ma il decoder resta con uno stato I2S non più valido; se
  il retry non riesce, FFT/Registra mostrano “Microfono non disponibile,
  riavvia” fino al reboot. È esattamente il caso X10/X9 della review.
- **Impatto:** heap ridotto di ~40 KB senza motivo, ownership I2S incoerente,
  possibile regressione mic dopo Radio → Stato → Audio. Non è una certezza
  di blocco perché M5Unified tenta il retry: va verificato su hardware.
- **Fix:** aggiungere `audioout::decoderRunning()` che ritorna `false` se
  `g_decoder == nullptr` e usarlo in Radio/Info; non chiamare `decoder()` per
  introspezione. In alternativa rendere il decoder lazy solo in
  `beginPlayback()` e liberare `I2S0` all'uscita dalla Radio quando non c'è
  stream.
- **Test:** boot pulito → Radio → Stato → ESC → Audio → FFT 5 s → ESC →
  Registra 2 s → ESC → Radio play; controllare mic, heap e ritorno audio.

### A-06 — P2 — Quattro `f_getfree` ogni 100 ms durante la registrazione

- **Dove:** `src/modules/audio/AudioMenu.cpp:532-533` (`fmtFree()` due volte
  per frame), `:145-160` (`sdFreeBytes` → `SD.totalBytes()` +
  `SD.usedBytes()`); core `SD.cpp:94-122` (`entrambe` chiamano `f_getfree`).
- **Impatto:** fino a 4 scansioni/cache FAT per ogni aggiornamento della
  schermata REC (10 Hz), più due `std::string` temporanee. Con FSInfo valido
  `f_getfree` è cached, ma al primo uso o con FSInfo invalido/card grande può
  bloccare per decine/centinaia di ms; durante il blocco il mic ha solo 2
  buffer (128 ms) e può perdere campioni. Lo spazio mostrato oltretutto è
  solo in GB (`fmtFree`), non in MB/minuti.
- **Fix:** calcolare il free una volta ogni 5 s (o usare `startFree - bytes`),
  salvarlo in una variabile e riusare la stessa stringa; evitare `f_getfree`
  nel percorso di registrazione.
- **Test:** card 32 GB, REC 5 min, monitorare i buchi nell'audio e il tempo
  del frame.

### A-07 — P2 — Microfono lasciato acceso nella schermata REC inattiva

- **Dove:** `src/modules/audio/AudioMenu.cpp:729-843`: `beginMic()` a :739,
  stop con ENTER a :780-786, `endMic()` solo all'uscita a :840; activity del
  power timer solo con `rec.active` a :792-795. Ritorni anticipati senza
  restore: `runMemos` :1074-1088, `playMemo` :951-960.
- **Impatto:** dopo lo stop il mic resta alimentato mentre la UI dice “MIC
  PRONTO” (privacy: il MEMS è attivo prima di ENTER); con timeout default
  120 s la schermata inattiva può andare in `powerTimer::powerOff()` con il
  mic ancora inizializzato e senza una re-init al risveglio (X8/E15). I
  ritorni anticipati non ripristinano lo speaker se una schermata precedente
  lo aveva lasciato spento.
- **Fix:** dopo lo stop chiamare `endMic()` + `restoreSpeaker()`; alla nuova
  registrazione `beginMic()` (accettando il warm-up) oppure tenere un
  `powertimer::Hold` finché la schermata REC è aperta. Centralizzare il
  cleanup con un RAII per ogni uscita.
- **Test:** REC stop, 3 min di inattesa, risveglio e nuova REC; 20 cicli;
  controllare `M5.Mic.isRunning()` e lo speaker.

### A-08 — P2 — `Speaker.end()` a ogni memo: pop, heap e coda DMA tagliata

- **Dove:** `src/modules/audio/AudioMenu.cpp:1014-1026` (attesa
  `isPlaying(1) == 0`), `:1033-1034` (`M5.Speaker.end()` + `restoreSpeaker`).
- **Impatto:** ogni memo chiude e riapre I2S1/DAC: possibile pop, due
  riallocazioni DMA per memo e frammentazione heap (E4/E6/X13). Inoltre
  `isPlaying(1) == 0` significa “slot sorgente rilasciati”, non “DMA I2S
  vuoto”: con 16 buffer da 512 frame a 48 kHz possono restare ~170 ms di
  audio in coda che `end()` scarta, tagliando la coda del memo.
- **Fix:** non chiamare `Speaker.end()` alla fine del memo: basta
  `stop(1)`/ripristino volume e lasciare lo speaker inizializzato (il mic lo
  chiuderà in `beginMic`). Se serve chiudere, attendere il drenaggio o
  gestire esplicitamente la coda. Non riaprire il codec per ogni traccia.
- **Test:** memo 60 s, ultima sillaba; 20 riproduzioni consecutive con
  `ESP.getFreeHeap()`; ascolto dei click.

### A-09 — P2 — Spettro FFT non adattivo alla rotazione portrait

- **Dove:** `src/modules/audio/AudioMenu.cpp:425-431` (`startX` e larghezza
  barre fisse), `:459-482` (y e tacche fisse). La rotazione è supportata da
  `SettingsMenu.cpp:93-103` e `Display.cpp:544-559`.
- **Impatto:** in portrait (0/2) `width()` è 135: `startX = (135 - 206)/2 =
  -35`; le prime barre escono a sinistra, le ultime a destra e le etichette
  100/1k/6k si sovrappongono. Non c'è crash, ma la schermata principale del
  modulo è illeggibile (E14).
- **Fix:** calcolare larghezza barra/gap da `display::width()` o forzare il
  layout landscape; in alternativa disegnare meno barre e ridimensionare le
  etichette.
- **Test:** impostare rotazione 0 e 2, aprire Spettro e Registra.

### A-10 — P2 — Nomi file: collisione oltre 99, nomi con punto iniziale, case

- **Dove:** `src/modules/audio/AudioMenu.cpp:606-612` (`uniqueRecPath`),
  `:853-873` (`sanitizeName`), `:888-896` (collisione rinomina).
- **Impatto:**
  - se esistono `_2` … `_99` per la stessa base, il ciclo esce con
    `path = base_99` e `openWrite` (FILE_WRITE) **tronca** il file esistente;
  - `sanitizeName` accetta un nome che inizia con `.` (es. “.”): il target
    diventa `..wav`, `listEntries` filtra i nomi che iniziano con `.` e il
    memo sparisce dalla UI;
  - su FAT un rename che cambia solo maiuscole/minuscole viene rifiutato come
    “esiste già” perché `storage::exists()` è case-insensitive.
- **Fix:** al termine del loop generare un errore invece di riusare `_99`;
  rifiutare/stripare il punto iniziale e i punti finali; gestire esplicitamente
  il case-only rename (confronto con `strcasecmp` del nome source).
- **Test:** 100 file con la stessa base; rename “.”, “..”, “foo.”, “ABC” →
  “abc”; reboot e verifica elenco.

### A-11 — P2 — Parser WAV: 16 chunk, 0xFFFE senza subformat, block align

- **Dove:** `src/modules/audio/AudioMenu.cpp:247-290`.
- **Impatto:** un WAV valido con più di 16 chunk (LIST/INFO/ID3) viene
  rifiutato; un WAV `WAVE_FORMAT_EXTENSIBLE` (0xFFFE) non-PCM viene accettato
  come PCM e riprodotto come rumore; `block align`/`byte rate` non sono
  verificati. M5/M6 chiedono “malformed → messaggio, mai rumore”.
- **Fix:** alzare il guard (es. 64) o saltare i chunk sconosciuti con limite
  di tempo; per 0xFFFE leggere il SubFormat GUID e accettare solo PCM;
  verificare `blockAlign == channels * bits/8` e `byteRate == rate * align`.
- **Test:** WAV con LIST/INFO, WAV extensible float, 8/24 bit, header troncato.

### A-12 — P2 — `REPORT.md` non descrive il build finale e alcune promesse sono false

- **Dove:** `src/modules/audio/REPORT.md:17` (“nessuna allocazione nel loop di
  cattura o di disegno”), `:23-24` (short write/header), `:75-79` (dimensioni),
  `:111-117` (limite I2S), `:128-129` (coda REC).
- **Impatto QA:**
  - il binario finale presente è **2.618.256 B**, non 2.389.504; la
    partizione `app0` è `0x2B0000` = 2.818.048 B, non 2.621.440; il libero
    reale è 199.792 B (~195 KiB), non ~232 KB. I numeri del REPORT sono
    pre-merge e vanno rigenerati;
  - “nessuna allocazione … disegno” è falso per registratore e memo:
    `drawRecorderIdle` :493/:502-504, `drawRecorderActive` :532-533,
    `drawPlayback` :923 producono `std::string` temporanee > SSO; FFT invece
    è pulito;
  - “header aggiornato … e alla chiusura” non vale se il patch fallisce
    (A-02/A-03);
  - il “limite I2S” è in parte superato da `AudioOut::beginPlayback()` /
    `reinit()` (`AudioOut.cpp:294-346`), ma resta il caso A-05; va riscritto.
  - mancano PGA ES8311 basso, comportamento a perdita di alimentazione e
    rimozione a caldo della SD (F14/W14/D9).
- **Fix:** rigenerare il REPORT dal commit finale, con `firmware.bin`, RAM
  statica, heap misurato, e aggiornare i limiti.

### A-13 — P2 — Modulo Audio non disattivabile a runtime

- **Dove:** `src/main.cpp:138` (`addItem("Audio", …)` senza `guardedModule`);
  `include/Config.h:15-30` non ha `Module::Audio`; `Config.cpp` non lo elenca.
- **Impatto:** un modulo che accende il microfono non può essere escluso da
  “Moduli attivi”; è una scelta di privacy/robustezza, non un crash. File
  condivisi: la feature 3 non può correggerli (va segnalato al supervisore).
- **Fix:** aggiungere la voce in `Module` (in fondo, prima di `Count`), il
  case in `moduleCompiled`, e usare `guardedModule("Audio",
  config::Module::Audio, audio::runMenu)` in `main.cpp`.
- **Test:** Audio disattivato → la voce mostra il messaggio e non apre il mic.

### A-14 — P2 — La lista memo non mostra la dimensione (M1)

- **Dove:** `src/modules/audio/AudioMenu.cpp:848-851` (`Memo` solo nome e
  durata), `:1045-1050` (`entry.size` disponibile ma scartato).
- **Impatto:** la review M1 chiede “dimensione visibile”; l'utente non può
  distinguere un memo da 10 s da uno troncato. Basso impatto, ma è un criterio
  di accettazione non coperto.
- **Fix:** aggiungere `uint32_t size` a `Memo`, copiarlo da `entry.size` e
  mostrarlo in KB/MB nella riga del menu.

### A-15 — P2 — SD rimossa a caldo: `hasSD` cached, nessun timeout/remount

- **Dove:** `src/utils/Storage.cpp:70-76` (flag `g_sd` mai aggiornato),
  `AudioMenu.cpp:145-152` (`SD.totalBytes/usedBytes`),
  `:211-221` (`writeAll` senza timeout), `:708-726` (finalizzazione).
- **Impatto:** dopo l'estrazione la UI continua a credere la SD montata;
  `runMemos` mostra “Nessun memo” invece di “SD assente”, `runRecorder`
  mostra “avvio non riuscito”, e `f_getfree`/`write` possono bloccare a lungo
  nel driver SPI. La review D3 chiede “nessun hang permanente”; non è
  verificabile staticamente e non c'è un timeout applicativo. Il file
  eventualmente corrotto è coperto da A-02/A-03.
- **Fix:** trattare l'errore di scrittura/lettura come “SD rimossa” e
  suggerire il reboot; evitare `f_getfree` mentre la card può essere rimossa;
  valutare un remount esplicito (`SD.end()`/`SD.begin`) al rilevamento.
- **Test:** card sacrificabile, estrazione durante REC e durante il playback
  (D3), misurare il tempo di risposta.

### A-16 — P2 — Errori di lettura/playback memo non segnalati

- **Dove:** `src/modules/audio/AudioMenu.cpp:993-1012` (`file.read`/`playRaw`
  falliti → `break`), `:1035` (log “riprodotto” in ogni caso).
- **Impatto:** un memo troncato o un errore SD durante la riproduzione
  termina in silenzio; la UI non distingue “finito” da “interrotto” e il log
  mente. M6/M7 chiedono messaggio/errore.
- **Fix:** tenere `bool failed`/`bool eof`; controllare `file.getReadError()`;
  se `playRaw` fallisce mostrare “riproduzione interrotta” e non loggare
  “riprodotto”.

### A-17 — P2 — Ultimi ~128 ms di ogni registrazione scartati (noto, ma correggibile)

- **Dove:** `src/modules/audio/AudioMenu.cpp:712-719`; documentato in
  `REPORT.md:126-129`.
- **Impatto:** a ogni stop (ENTER o ESC) i due buffer in coda (2 × 64 ms)
  vengono attesi ma **non scritti**: l'ultima sillaba può essere tagliata e
  W7/W10 non garantiscono “fino all'ultimo campione”. Il report lo dichiara,
  quindi non è un bug nascosto, ma è una perdita sistematica evitabile.
- **Fix:** in `recorderFinish` scrivere i buffer completati prima del patch
  (usando lo stesso `recorderWrite` senza riaccodare), oppure accodare un
  ultimo record e scriverlo; poi patchare l'header con il totale reale.

## 4. Aree verificate senza finding

- **FFT:** radix-2 512 punti, finestra Hann, bin 31,25 Hz, `static_assert`
  1 kHz = bin 32; scala `4·|X|/N` corretta per Hann; 26 barre log 100 Hz–6 kHz
  con `barEdges` monotoni e nessuna barra vuota; livello RMS, peak hold e
  clipping; nessuna allocazione dinamica nel percorso FFT/disegno e nessuna
  PSRAM. I buffer statici sono ~11,3 KB (verificato anche con `nm`:
  `g_pcm` 0x1000, `g_fftSamples` 0x400, tre array float 0x800 ciascuno).
- **WAV registrato:** header canonico 44 byte con offset e valori corretti
  (`RIFF`, 36+data, PCM=1, canali=1, rate 16000, byte rate 32000, align 2,
  bit 16, `data` a 36, size a 40); `r.bytes` conta solo PCM; 60 min < 4 GB.
- **Ping-pong REC:** 2 × 1024 campioni; `Mic_Class::isRecording()` ritorna
  0/1/2 ed è usato correttamente (`> 1` = coda piena), l'indice `next` segue
  il buffer più vecchio; nessuna race nel percorso normale. Warm-up ES8311
  gestito con `skipUntil`; file <100 ms scartato.
- **Mic/speaker (percorso normale):** `Speaker.stop/end` prima di
  `Mic.begin`, `Mic.end` prima di `Speaker.begin`, speaker ripristinato in
  `runSpectrum`, `runRecorder`, `playMemo`, `runMemos` e `runMenu`; nessuna
  chiamata ad `audioout::decoder()` dentro il modulo Audio. Restano i casi
  limite A-05/A-07.
- **Memo:** streaming a blocchi da 2 KB, nessun file intero in RAM; parser
  PCM 16-bit mono/stereo 8–48 kHz con clamp di `dataBytes`; rinomina confinata
  a `/audio/memo` (slash e `..` rimossi); cancellazione con conferma; nessuna
  scrittura fuori directory.
- **Heap/stack:** nessuna PSRAM, buffer statici piccoli, nessuna VLA né
  ricorsione profonda; le allocazioni residue sono quelle di A-04/A-06/A-08 e
  le stringhe di disegno di A-12.
- **Flag/build:** `ENABLE_MODULE_AUDIO` racchiude tutto il modulo; nessuna
  nuova `lib_deps`; nessuna modifica ai file condivisi.

## 5. Rischi residui / cose non verificabili staticamente

1. **M5Unified `Mic.begin()` su I2S0 già installato**: il retry
   uninstall/install (`Mic_Class.inl:427-431`) può mascherare A-05; serve
   hardware per sapere se il mic parte e se il decoder resta coerente.
2. **`M5.Mic.end()`/`M5.Speaker.end()` possono attendere il task senza un
   timeout applicativo** (libreria): un I2S bloccato può far sembrare un hang
   all'uscita. Non riproducibile senza scheda.
3. **Coda DMA dello speaker**: la coda residua dopo `isPlaying(1)==0` non è
   misurabile staticamente; A-08 va confermato all'ascolto.
4. **FAT/FSInfo**: il costo reale di `f_getfree` dipende dalla card e dallo
   stato di FSInfo; A-06 è un rischio condizionale.
5. **SD rimossa a caldo**: i tempi del driver SPI e l'eventuale watchdog non
   sono verificabili senza la card sacrificabile.

## 6. Test hardware prioritari

1. **A-01:** memo 60 s 16 kHz mono e 44,1 kHz stereo; ascolto e registrazione
   dell'uscita. Cercare click periodici e confrontare con il sorgente.
2. **A-02/A-03:** REC di 5–10 s su card con <1 MB liberi e estrazione durante
   REC; verificare header, `ffprobe`, messaggio UI e assenza di “salvato” su
   file invalido.
3. **A-05:** boot pulito → Radio → “Stato / ferma” → ESC → Audio → FFT 5 s →
   Registra 2 s → ESC → Radio play; monitorare heap, mic e audio radio.
4. **A-04:** 500 file in `/audio/memo`; heap prima/dopo e tempo di apertura.
5. **A-06/A-07:** REC 5 min su card 32 GB, poi 3 min di schermata REC
   inattiva e risveglio; controllare buchi audio, `Mic.isRunning()` e sleep.
6. **A-08:** 20 riproduzioni consecutive; ultima sillaba, click, heap.
7. **A-09:** rotazione 0/2, Spettro e Registra.
8. **A-10/A-11:** nomi “.”, “..”, case-only, 100 collisioni; WAV con LIST,
   extensible, 8/24 bit, troncato.
9. **Cicli completi:** Audio → FFT → ESC → Radio → play → ESC → Audio →
   Memo → ESC, 20 volte; beep UI e speaker alla fine.

## 7. Nota su build/REPORT

- Il binario finale in `.pio/build/cardputer-adv-offensive/firmware.bin` è
  2.618.256 B; la partizione `app0` è 2.818.048 B (`0x2B0000`), quindi il
  margine reale è 199.792 B (~195 KiB). Il REPORT della feature (2.389.504 B,
  slot 2.621.440 B, ~232 KB liberi) è pre-merge e non va usato per il gate G4.
- Il REPORT dichiara “nessuna allocazione nel loop di disegno” e “short write
  gestiti con retry”: la prima è vera solo per FFT, la seconda è troppo
  ottimistica (A-02/A-03). Anche il limite I2S (:111-117) va aggiornato dopo
  il fix di A-05.
- Non è stata eseguita una build in questo lavoro: se l'artefatto non
  corrisponde al commit finale, i numeri vanno rigenerati al merge.
