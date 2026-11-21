# FIX_IR — IR veloce: niente `exists` per file, cache RAM, parser 1 KB

Worktree: `orbit-wt-fix-irperf`, branch `fix/fix-irperf`, base `553f604`
(ORBIT OS 2.0.0). Diagnosi di partenza: `docs/integration/DIAG_IR.md`.
Hardware di riferimento: M5Stack Cardputer ADV, ESP32-S3FN8, 320 KB SRAM,
nessuna PSRAM, microSD su SPI a 25 MHz.

Nessuna misura su hardware in questo ambiente: i tempi qui sotto sono stime
derivate dal DIAG e dal conteggio statico delle chiamate FS, non benchmark.
L'unica misura reale è la build (flash e RAM statica, §4).

---

## 1. Modifiche

### 1.1 Storage — una sola enumerazione, niente `exists` preventivo

- `include/Storage.h`: `storage::Entry` ha `bool onSd` (backend della voce).
- `src/utils/Storage.cpp`:
  - `listEntries()` valorizza `onSd` per ogni voce (`false` da LittleFS,
    `true` da SD). Il campo viene dal `File` gia' aperto durante la
    enumerazione, quindi non aggiunge `stat`.
  - `openOn()` non chiama piu' `filesystem.exists()` prima di `open()`:
    `open()` su un path assente restituisce gia' un `File` invalido. Vale per
    tutti i chiamanti di `storage::openRead()` (IR, musica, WAV, PcapTools,
    WpaSec, BadUSB, BLE-HID): un `stat` in meno per apertura.

### 1.2 IR — `listRemotes()` senza doppia enumerazione

`src/modules/ir/IrMenu.cpp`:

- Rimosse `brandBases()` e la `storage::exists(path)` per **ogni** file.
- `listRemotes()` fa **una** `storage::listEntries(base, false)` per base
  (cartella categoria +, solo per TV, vecchio albero piatto `/ir/<marca>`),
  filtra `!isDir && isIrFile(name)`, deduplica per nome come prima e salva
  `Entry.onSd` nel nuovo `Remote::onSd`.
- Il file e' appena stato elencato: il vecchio `exists` non poteva mai
  aggiungere informazione, costava 2 lookup (LittleFS + SD) per file.
- `loadCommands()`, `openFile()` e lo scanner accettano il backend noto e
  aprono direttamente il file su SD (`storage::openRead(path, true)`), senza
  il miss LittleFS garantito per `/ir/<categoria>/...`.
- `runFavorites()` e `runSearch()` passano `FavoriteBackend::Sd`/`onSd` a
  `openFile()`.

Comportamento preservato: fusione TV + albero legacy, dedup case-insensitive
per nome, filtro `.ir`, conteggi visibili (2.015 voci, 240 duplicati nascosti).

### 1.3 Cache RAM

- **Marche + conteggi**: `CategorySnapshot` tiene `brands` e `counts` di
  **una** categoria. `runCategory()` e `runScannerCategory()` usano lo
  snapshot; tornare indietro nella stessa categoria non fa I/O.
  Cache a uno slot: la categoria piu' grande (Altro, 343 marche) occupa
  ~10-11 KB; non si somma a tutte le categorie.
- **Visibilita' categoria**: `categoryHasFiles()` memorizza un bool per
  categoria (8 byte) e si ferma alla prima marca non vuota, come prima. Il
  menu "Telecomandi"/"Scanner" non riscansiona le categorie gia' viste.
- **Preferiti**: `cachedFavorites()` legge NVS una volta; `isFavorite()` e
  `favoriteHint()` usano il vettore in RAM. `toggleFavorite()` modifica la
  copia, salva e sostituisce la cache. Aprire Samsung passa da 57 letture NVS
  a 0 (una sola lettura alla prima richiesta).
- **Ultimo telecomando**: `loadLastRemote()` legge NVS una volta;
  `saveLastRemote()` scrive solo se il path cambia. Riaprire lo stesso file
  non produce piu' una scrittura NVS.
- **Invalidazione**:
  - cambio di stato SD (`storage::hasSD()`) → `invalidateCache()` al primo
    accesso successivo;
  - "Database IR" chiama `invalidateCache()` e rilegge l'albero: e' il
    refresh esplicito e ripopola la cache;
  - toggle preferiti aggiorna la cache dei preferiti;
  - nessun altro percorso IR modifica `/ir`, quindi non esistono altre
    invalidazioni da agganciare oggi (se in futuro SdMenu o un import
    scrivono in `/ir`, devono chiamare il refresh di "Database IR").
- I menu non fanno I/O nel draw: la cache si legge/scrive solo nelle funzioni
  di costruzione del menu, prima di `menu.run()`.

### 1.4 Parser

- `IrLineReader::chunk_` da 256 a **1024 B**. Su un file da 60.922 B le
  `read` scendono da 238 a 60 (−178, −75%).
- Verificato che non c'e' riparsazione per comando: `openFile()` parsa una
  volta e le lambda catturano `commands` per riferimento + indice
  (`IrMenu.cpp`, `menu.addItem` in `openFile`). I tasti rapidi usano lo
  stesso vettore gia' parsato.

### 1.5 Indice persistente `/ir/.iridx` — NON fatto

Lo Step 4 del brief era condizionale ("se resta margine e RAM <20 KB").
Motivi per non farlo in questo intervento:

- la cache RAM copre gia' il costo che l'utente paga navigando (ritorno alla
  stessa categoria, preferiti, riapertura marca);
- un indice completo richiede ~200 righe nuove (formato, build, CRC,
  self-heal, fallback, invalidazione), ~77 KB sulla SD e la riscrittura di
  `runSearch`/`runDatabaseInfo`; senza una misura di heap reale il rischio di
  regressione e' superiore al guadagno certo;
- il gate `kMinIrHeap` resta a 64 KB e il parser puo' arrivare a ~36 KB: non
  c'e' evidenza misurata che 20 KB di indice in piu' siano sicuri.

Conseguenza dichiarata: "Cerca" e "Database IR" restano O(enumerazione),
migliorati ~2x dallo Step 1, ma non istantanei. Se servira', l'indice va
costruito sopra `listEntries()` di questo fix e misurato sul dispositivo.

---

## 2. File toccati

| File | Modifica |
|---|---|
| `include/Storage.h` | `Entry::onSd` |
| `src/utils/Storage.cpp` | `listEntries` propaga `onSd`; `openOn` senza `exists` |
| `src/modules/ir/IrMenu.cpp` | `listRemotes` a una enumerazione; `Remote::onSd`; cache categoria/conteggi/visibilita'; cache preferiti; cache ultimo path; parser 1 KB; backend in `openFile`/`loadCommands` |
| `docs/integration/FIX_IR.md` | questo documento |
| `docs/integration/REPORT.md` | sezione 6 con esito e misure |

Nessun cambio a formati file, NVS, menu, tasti o messaggi utente.

---

## 3. Operazioni FS risparmiate (conteggio statico)

Convenzione del DIAG: `exists(path)` default = 2 lookup (LittleFS + SD);
`listEntries`/`list` = open dir + `readdir` per voce. Dataset: 2.255 `.ir`,
932 cartelle di marca, 38 marche legacy piatte.

| Percorso | Prima | Dopo | Risparmio |
|---|---:|---:|---:|
| `listRemotes` su tutto il DB | 2.255 file × 2 `exists` = **4.510 lookup** | 0 | **−4.510** |
| Basi marca (`exists` dir) | 932 × 2 = **1.864 lookup** (+~342 TV legacy) | 0 (open dir diretto) | **−~2.200** |
| Apertura marca (Samsung, 57 file) | ~177 op FS + **57 letture NVS** | ~60 op FS + 0 NVS | **−~120 op, −57 NVS** |
| Categoria Altro (514 file, 343 marche) | ~2.900 op | ~1.200 op | **−~1.700 (~2,4x)** |
| Cerca / Database IR (tutto il DB) | ~12.300 op | ~5.600-6.000 op | **−~6.300-6.700 (~2,1x)** |
| Categoria gia' aperta (warm) | ~2.900 op | **0** | **−tutte** |
| `openRead` di un `.ir` su SD | `exists` (2) + LFS open (2) + SD open (2) | SD open (2) | **−4 syscall** |
| Parser file 60.922 B | 238 `read` | 60 `read` | **−178 (−75%)** |
| Riapertura stesso telecomando | 1 scrittura NVS | 0 | **−1 NVS** |

`runRemotes`/`runScannerBrands` erano gia' short-circuit (si fermano alla
prima marca non vuota): il guadagno li' e' la cache dei bool per categoria,
non la rimozione di `exists` per-file.

Le stime di tempo del DIAG dopo lo Step 1 (Cerca/Database 1-3 s, categoria
0,5-1,5 s, marca 50-150 ms, ritorni a cache calda <30 ms) restano **stime**:
non sono state misurate su Cardputer.

---

## 4. Build e dimensioni (misurate)

Comando: `python -m platformio run -e cardputer-adv-offensive` (timeout
900.000 ms). Build completa: `SUCCESS`, 179,8 s; incrementale 43,5 s.
Baseline misurata facendo stash delle sole modifiche e ricompilando lo stesso
tree (`HEAD 553f604`).

| Voce | Prima | Dopo | Delta |
|---|---:|---:|---:|
| `firmware.bin` | 2.690.816 B | **2.691.520 B** | **+704 B (+0,03%)** |
| Flash `text+data` | 2.690.449 B (95,47%) | **2.691.153 B (95,50%)** | +704 B |
| RAM statica `.data+.bss` | 147.280 B (44,94%) | **147.368 B (44,97%)** | **+88 B** |
| Libero nello slot OTA | 127.599 B | **126.895 B** | −704 B |

La RAM statica cresce di 88 B (stato cache). La cache dinamica peggiore
(Altro: 343 `Brand` + 343 `uint16_t`) e' ~10-11 KB di heap, allocata solo
dopo il primo ingresso in una categoria; preferiti e ultimo path ~1 KB.
Il picco del parser e il gate `kMinIrHeap` (64 KB) non cambiano. L'heap reale
dopo cache + parse di un file da 60 KB non e' stato misurato.

---

## 5. Test

### 5.1 Eseguiti

- Build `cardputer-adv-offensive`: `SUCCESS` (baseline e dopo, stesso tree).
- Compilazione senza warning/errori sui tre file modificati.
- Verifica statica: nessun `storage::exists` per-file in `listRemotes`;
  nessun `openFile`/`loadCommands` senza backend noto nei percorsi che lo
  conoscono; lambda dei comandi legata al vettore gia' parsato.

### 5.2 Da eseguire sul dispositivo (non eseguiti: nessun hardware qui)

1. SD inserita: "Telecomandi" → categoria → marca → telecomando; confrontare
   i conteggi `Marca (N)` con il REPORT (2.015 totali, 240 duplicati TV/legacy
   nascosti).
2. Tornare indietro e riaprire la stessa categoria: nessun ritardo e stessi
   conteggi (cache).
3. "Database IR": totali per categoria corretti; seconda apertura veloce
   (refresh esplicito).
4. "Cerca": `samsung` e `split` devono includere TV e Clima; nessun duplicato.
5. Preferiti: aggiunta/rimozione con `F`, limite 20, indicatore `*` immediato
   dopo il toggle, persistenza dopo reboot.
6. "Riprendi": apre l'ultimo file; riaprendo lo stesso non deve esserci una
   nuova scrittura NVS (log temporaneo o `Preferences` instrumentation).
7. SD assente: 307 file LittleFS, 38 marche TV legacy, preferiti SD conservati
   (non cancellati), messaggio corretto.
8. SD inserita/rimossa a runtime: la cache si invalida al primo accesso
   (conteggi coerenti, nessun path fantasma).
9. Scanner: Power inviato, `[n/d]`, `[raw >512]`, file >64 KB saltato.
10. File grandi: `NEC_RU_M124.ir` (60.922 B) apre senza reset; misurare heap
    prima/dopo con un log temporaneo se serve.
11. Draw loop: nessuna chiamata FS tra ingresso e uscita menu senza input.

---

## 6. Rischi e limiti noti

| Rischio | Gravita' | Stato / mitigazione |
|---|---|---|
| Cache stale se `/ir` viene modificato mentre l'app gira (import esterno, SdMenu) | Media | nessun percorso IR scrive `/ir`; il refresh e' "Database IR"; se in futuro un import scrive in `/ir`, deve invalidare la cache |
| Cache di una sola categoria: cambiare categoria la ricostruisce | Bassa | costo = una enumerazione della categoria, gia' ridotta ~2,4x; la cache resta piccola (~11 KB) |
| Cache ~11 KB + parser ~36 KB | Media | gate `kMinIrHeap` invariato a 64 KB; heap reale da misurare sul file da 60 KB; se insufficiente, liberare lo snapshot prima di `openFile` |
| `onSd` scelto dalla dedup quando lo stesso nome esiste su entrambi i FS | Bassa | entrambe le copie sono valide; il file si apre sul backend elencato, stesso contenuto atteso |
| `openRead` senza `exists` per tutti i moduli | Bassa | `open()` su path assente restituisce `File` invalido; build completa OK; i chiamanti controllano gia' `if (!file)` |
| Rimozione SD dopo l'elenco e prima dell'apertura | Bassa | `openRead(path, true)` fallisce e mostra "Impossibile leggere il file"; nessun crash |
| Conteggi in `uint16_t` | Molto bassa | nessuna marca reale si avvicina a 65.535 file |
| "Cerca"/"Database IR" non sono istantanei (indice non fatto) | Media | dichiarato: ~2,1x dallo Step 1; l'indice persistente resta il passo successivo se misurato |
| Nessun benchmark hardware | Media | tutti i tempi in questo documento sono stime; la build e' l'unica misura |

---

## 7. Conclusione

Il costo dominante (2.255 `exists` doppi + 932 `exists` di base per ogni
scansione) e' eliminato: una sola `listEntries` per cartella, backend noto per
l'apertura. La navigazione ripetuta e i preferiti non toccano piu' NVS/FS a
ogni voce. Il firmware cresce di 704 B e la RAM statica di 88 B (build misurata).
Restano fuori indice persistente e benchmark su hardware, dichiarati sopra.
