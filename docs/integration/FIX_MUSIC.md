# FIX_MUSIC — SD rimontabile, diagnostica e fallback percorso

Worktree: `orbit-wt-music-fix`, branch `fix/music-sd`, base `52266e9`
(ORBIT OS 2.0.1).
Hardware di riferimento: M5Stack Cardputer ADV, ESP32-S3FN8, 320 KB SRAM,
nessuna PSRAM, microSD su SPI a 25 MHz (`CS=12`, `SCK=40`, `MISO=39`, `MOSI=14`).

Nessun hardware collegato in questo ambiente: la build e le dimensioni sono
misurate, il comportamento in `runMenu` e' verificato staticamente. I test
sulla Cardputer restano da eseguire (§5.2).

---

## 1. Modifiche

### 1.1 Storage — `remountSD()`

`src/utils/Storage.cpp`, `include/Storage.h`:

- Nuova `bool storage::remountSD()`. Se `g_sd` e' gia' true ritorna subito
  true; altrimenti riapre il bus con gli stessi pin di `begin()`
  (`SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN)`) e ritenta
  `SD.begin(SD_CS_PIN, SPI, 25000000, "/sd")`.
- Su successo imposta `g_sd = true` e logga `microSD montata (remount)`;
  su errore logga `microSD remount fallito` e lascia `g_sd = false`.
- LittleFS non viene toccato: `g_littlefs`, il mount e i file restano validi.

Il retry e' sicuro: nella libreria SD di Arduino-ESP32 un `SD.begin()` fallito
chiama `sdcard_unmount`/`sdcard_uninit` e riporta `_pdrv` a `0xFF`, quindi la
chiamata successiva riparte da zero. `SPI.begin()` viene ripetuto perche' la
card puo' essere stata inserita dopo il boot (bus non inizializzato o gia'
inizializzato: entrambi i casi sono gestiti).

### 1.2 MusicMenu — schermata di remount

`src/modules/music/MusicMenu.cpp`, `runMenu()`:

- Se `!storage::hasSD()`, prima di scansionare compare la schermata
  **"microSD non montata"** con: inserire la card, `R` (o ENTER) per
  riprovare, `ESC` per continuare in sola LittleFS.
- `R`/ENTER chiama `storage::remountSD()`. Se riesce, `runMenu` prosegue e
  riscansiona subito la libreria sulla SD. Se fallisce, la schermata diventa
  "Mount non riuscito" e resta in attesa di un nuovo tentativo o di ESC.
- `ESC` mantiene il comportamento storico: scansione di `/data/Music` su
  LittleFS (utile per i file di prova).
- La schermata non fa I/O nel loop di attesa: solo `navigation::poll()` e
  `delay(25)`.

### 1.3 MusicMenu — voce "Diagnostica"

Nuova voce in fondo alla libreria (icona `sd`, sempre selezionabile, anche se
la libreria e' vuota non viene mostrata perche' `runMenu` esce prima):

- SD: SI / NO
- Backend: `SD` oppure `LittleFS`
- Root: percorso effettivamente usato (`/data/Music`, `/music` o `/Music`)
- Album: numero album e numero totale brani (inclusi i brani sciolti)
- numero brani per album, una riga per album; oltre ~8 righe la schermata
  mostra `... +N album` (il limite e' di `ui::drawScreen`)
- gli stessi dati, con **tutti** gli album e il conteggio di "Senza album",
  vengono scritti con `logger::info` (`Musica diag: ...`).

### 1.4 MusicMenu — fallback di sola lettura

`scanAlbums()` ora usa `scanRoot(root, onSd, albums, loose)` e prova in ordine:

1. `/data/Music` (comportamento invariato);
2. se la SD e' montata e `/data/Music` non ha **album**, `/music`;
3. poi `/Music` (maiuscola).

Il primo candidato con almeno un album diventa `g_root`: la UI, i brani
sciolti e il player usano quel percorso. Il fallback e' **solo lettura**:
nessuna cartella viene creata, nessun file spostato o rinominato. Il percorso
scelto viene loggato (`Musica: uso fallback ...`).

Se `/data/Music` ha solo file audio sciolti (0 album), il fallback non scatta:
quei file restano visibili come "Senza album", come prima.

### 1.5 MusicMenu — scansione e log del motivo vuoto

`scanRoot()` logga, per ogni radice esaminata:

- `non trovata` se `storage::exists()` e' falso;
- `vuota (0 entry)` se la cartella esiste ma `listEntries` non restituisce
  nulla;
- `nessun album, N brani sciolti` se ci sono entry ma nessuna sottocartella
  (quindi 0 album; N puo' essere 0).

Il messaggio a schermo quando non c'e' nulla mostra il percorso esatto
(`!Nessun brano in <root>`), lo stato SD e dove creare gli album.
`storage::hasSD()` viene chiamato dopo un eventuale remount, quindi il
messaggio riflette lo stato reale.

### 1.6 Invariati

- Formato `cover.cvr` (64x64 RGB565 LE, 8192 B): nessuna modifica.
- Playback, decoder/I2S, volume, pausa, cambio brano: nessuna modifica.
- `countTracks`, `scanTracks`, `kMaxAlbums=40`, `kMaxTracks=150`: invariati.
- Nessun altro file condiviso toccato oltre `include/Storage.h` e
  `src/utils/Storage.cpp`.

---

## 2. File toccati

| File | Modifica |
|---|---|
| `include/Storage.h` | dichiarazione `remountSD()` |
| `src/utils/Storage.cpp` | `remountSD()` con stessi pin/mountpoint, log esito |
| `src/modules/music/MusicMenu.cpp` | schermata remount, Diagnostica, fallback `/music` e `/Music`, log motivi vuoto, `g_root` |
| `docs/integration/FIX_MUSIC.md` | questo documento |

---

## 3. Build e dimensioni (misurate)

Comando: `python -m platformio run -e cardputer-adv-offensive` (timeout
900.000 ms). Build completa con cache vuota: `SUCCESS`, 218,0 s. Build
incrementale: `SUCCESS`, 47,8 s.

La baseline e' stata misurata nello stesso albero con `git stash` delle sole
modifiche e ricompilazione incrementale, cosi' il delta non dipende da
oggetti di build diversi.

| Voce | Prima | Dopo | Delta |
|---|---:|---:|---:|
| `firmware.bin` | 2.691.472 B | **2.697.712 B** | **+6.240 B (+0,23%)** |
| Flash `text+data` | 2.691.109 B (95,5%) | **2.697.341 B (95,7%)** | +6.232 B |
| RAM statica `.data+.bss` | 147.368 B (44,97%) | **147.392 B (45,0%)** | **+24 B** |
| Libero nello slot app 2.0 | 126.939 B | **120.707 B** | −6.232 B |

Nota: il `firmware.bin` del flasher 2.0.1 in `flasher/bin` e' 2.663.424 B,
cioe' ~28 KB meno di una ricompilazione pulita dello stesso HEAD in questo
albero. Per questo il confronto utile e' quello con la baseline misurata qui
sopra, non con l'artefatto di release.

RAM dinamica: `g_root` e' un `std::string` statico (SSO, nessuna allocazione
finche' il percorso resta corto); la Diagnostica costruisce un
`vector<string>` di ~8 righe solo mentre la schermata e' aperta. Nessun nuovo
buffer grande, nessuna cache.

---

## 4. Comportamento atteso

| Scenario | Comportamento |
|---|---|
| Boot con SD inserita e `/data/Music` popolata | album come prima; Diagnostica: SD SI, backend SD, root `/data/Music` |
| Boot senza SD, card inserita dopo | entrando in Musica: schermata "SD non montata"; `R` monta la card e riscansiona subito |
| Boot senza SD, nessuna card | `ESC` → scansione LittleFS, come prima |
| SD montata, `/data/Music` assente o senza album | prova `/music`, poi `/Music`; log del percorso usato |
| SD montata, `/data/Music` con solo MP3 sciolti | nessun fallback; "Senza album" come prima |
| Libreria vuota | messaggio con percorso esatto + stato SD; log con il motivo |
| Album senza `cover.cvr` | segnaposto "NO COVER", come prima |
| Card rimossa a runtime dopo il mount | non rilevata da `hasSD()`; l'apertura del file fallisce con il messaggio esistente (vedi §6) |

---

## 5. Test

### 5.1 Eseguiti

- Build `cardputer-adv-offensive`: `SUCCESS` (completa e incrementale).
- Baseline e post-fix nello stesso albero, dimensioni in §3.
- Verifica statica: nessuna scrittura/spostamento nel fallback; `scanRoot`
  usa solo `listEntries`, `exists`, `countTracks`; il player usa ancora
  `album.onSd`; `cover.cvr` non toccato; `remountSD` e' l'unico nuovo accesso
  a `SD.begin`.
- Compilazione senza errori sui tre file modificati (i warning nel log sono
  quelli noti del framework ESP32, non del modulo).

### 5.2 Da eseguire sulla Cardputer

1. **Remount a caldo**: avviare senza SD, entrare in Musica, verificare la
   schermata; inserire la card (file in `/data/Music`) e premere `R`:
   gli album devono comparire senza riavvio.
2. **Remount fallito**: senza card premere `R`: deve comparire "Mount non
   riuscito" e restare in attesa; `ESC` deve portare alla scansione LittleFS.
3. **Diagnostica**: con SD inserita aprire "Diagnostica" e confrontare SD,
   backend, root, numero album e conteggi per album con il contenuto della
   card; controllare le stesse righe sul log seriale.
4. **Fallback**: rinominare temporaneamente la cartella sulla card in `/music`
   (o `/Music`) e verificare che gli album compaiano e che il log indichi il
   fallback. Nessun file deve essere spostato dal firmware.
5. **Vuoto**: card senza `/data/Music` ne' `/music` ne' `/Music`: messaggio
   con il percorso esatto e log `non trovata` / `0 entry`.
6. **LittleFS**: senza SD e con file di prova in `/data/Music` LittleFS:
   `ESC` dalla schermata di mount e verifica che i brani siano ancora
   elencati e riproducibili.
7. **Regressione playback**: album con `cover.cvr` da 8192 B, MP3 48 kHz;
   copertina, pausa, volume, cambio brano e fine brano invariati.
8. **Cavo/contatti**: con una card sporca o un adattatore lento, `R` deve
   fallire in modo pulito (nessun reset, nessun crash) e permettere un nuovo
   tentativo.
9. **Tasti**: nella schermata di mount, `R`/`r`/ENTER funzionano; le altre
   lettere non chiudono la schermata; `ESC` esce.

---

## 6. Rischi e limiti noti

| Rischio / limite | Gravita' | Stato / mitigazione |
|---|---|---|
| Card rimossa dopo un mount riuscito | Media | `g_sd` resta true; l'apertura fallisce con il messaggio esistente. Un hot-unplug non e' stato aggiunto (fuori scope); per recuperare, riavviare o uscire/rientrare non basta: serve un futuro `SD.end()` + remount |
| `remountSD()` non rileva una card sostituita con un'altra mentre `g_sd` e' true | Bassa | il remount e' pensato per il caso "card assente al boot"; la sostituzione a caldo non e' supportata |
| Fallback `/music` e `/Music` solo se 0 album in `/data/Music` | Bassa | scelta esplicita per non mescolare due librerie; i file sciolti restano "Senza album" |
| Diagnostica limitata a ~8 righe | Bassa | il log seriale contiene tutti gli album e i conteggi |
| Conteggi Diagnostica sono uno snapshot all'ingresso nel menu | Bassa | riaprire la voce per aggiornare |
| Rilevazione SD solo su richiesta (non automatica) | Bassa | l'utente preme `R`; evita retry a ogni frame e I/O nel draw |
| Nessun test su hardware | Media | tutti i comportamenti sopra sono verificati solo staticamente; §5.2 e' la lista da eseguire |
| `SD.begin()` ripetuto mentre un altro modulo usa la SD | Bassa | la musica e' in foreground e i menu sono esclusivi; nessun task di background apre la card durante il remount |

---

## 7. Conclusione

Il caso "SD montata solo all'avvio" ora ha un recupero esplicito (`R`), una
schermata che spiega cosa fare e una Diagnostica che mostra stato, backend,
radice e conteggi. Il fallback `/music`/`/Music` copre le card con la libreria
gia' nella root senza toccare i file. La build passa; il firmware cresce di
6.240 B e la RAM statica di 24 B. Restano da fare i test su Cardputer, in
particolare remount a caldo e fallback percorso.
