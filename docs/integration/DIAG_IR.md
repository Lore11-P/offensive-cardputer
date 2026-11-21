# DIAG — Perché la parte IR è lenta (ORBIT OS 2.0.0)

Worktree: `orbit-wt-fix-irperf`, branch `fix/fix-irperf`, HEAD `553f604`.
File esaminati: `src/modules/ir/IrMenu.cpp` (1.660 righe), `include/Storage.h`,
`src/utils/Storage.cpp`, `src/core/MenuManager.cpp`,
`src/modules/ir/REPORT.md`, `docs/integration/tests/01-ir.md`,
`git log -p -- src/modules/ir/IrMenu.cpp`.
Hardware: M5Stack Cardputer ADV, ESP32-S3FN8, 320 KB SRAM, **nessuna PSRAM**,
microSD su SPI a 25 MHz (`Storage.cpp:70-71`).

Metodo: lettura statica e conteggio delle chiamate FS lungo ogni percorso utente.
Il dataset `E:\ir` non è presente su questa macchina: i conteggi di file e
cartelle vengono dal REPORT (2.255 `.ir`, 8,47 MB, 2.015 voci visibili) e dal
codice; i tempi sono stime dichiarate, **non misure su hardware**. Nessuna
modifica al codice, nessuna build.

---

## 0. Sintesi

**Causa 1 — Enumerazione ricorsiva senza indice, ripetuta a ogni livello.**
`listRemotes()` apre ogni cartella di marca e, per **ogni** file trovato, chiama
`storage::exists(path)` (`IrMenu.cpp:246`). `storage::list` ed `exists` senza
`preferSD` enumerano/statano **prima LittleFS e poi la SD**
(`Storage.cpp:157-164`, `:121-131`). Una sola ricerca tocca ~12.300 operazioni
FS su 2.255 file; una categoria pesante ~3.700.

**Causa 2 — Nessuna cache: i conteggi si ottengono enumerando i file.**
`runCategory` e `runScannerCategory` chiamano `listRemotes(brand).size()` per
**ogni** marca solo per scrivere `(N)` nell'etichetta
(`IrMenu.cpp:1338`, `:1257`); `categoryHasFiles` ri-scansiona le marche a ogni
apertura (`:256-268`, usata a `:1360`, `:1279`). Ogni ritorno al menu riparte
da zero; "Riprendi", preferiti e scanner rifanno lo stesso lavoro.

**Causa 3 — Costi per voce e per apertura.**
`openBrand` riapre l'NVS per **ogni** telecomando per disegnare `* `
(`IrMenu.cpp:1091` → `:342-347`); `openFile` scrive l'NVS a ogni apertura
(`:1008`) e ne legge un'altra per l'hint (`:1037`); `storage::openRead` fa
`exists()` + `open()` e prova LittleFS prima della SD
(`Storage.cpp:29-32`, `:88-119`); il parser legge a blocchi da **256 B**
(`IrMenu.cpp:744`). Sono costi secondari rispetto a 1-2, ma si sommano su file
grandi e su marche con decine di modelli.

**Fix prioritario**: togliere `storage::exists` per-file e la doppia
enumerazione (Step 1), poi costruire l'indice persistente (Step 2) e la cache
RAM (Step 3). Lo Step 1 vale ~2,2× da solo; con lo Step 2 la ricerca scende a
0,1-0,3 s.

**Guadagno atteso complessivo**: ricerca e "Database IR" da ~4-10 s a
~0,1-0,4 s; apertura categoria da ~1,5-5 s a <100 ms (dopo la prima volta);
apertura marca da ~0,2-0,5 s a <30 ms. Stime, da confermare col benchmark §7.

---

## 1. Il costo delle API `storage`

| Chiamata | Cosa fa davvero | Costo |
|---|---|---|
| `list(path, dirsOnly)` con `preferSD` omesso (default `false`) | apre la dir su LittleFS **e** su SD, `openNextFile` per voce, `sort` + `unique` | 1 open LFS (miss) + 1 open SD + N `readdir` |
| `exists(path)` default | `LittleFS.exists` poi `SD.exists` | 2 lookup per path |
| `exists(path, true)` | solo SD | 1 lookup |
| `openRead(path)` default | `exists` (LFS+SD) + `open` (LFS poi SD) | fino a 4 lookup per apertura |
| `listEntries(path)` | come `list`, in più `entry.size()` | doppia enumerazione + stat per voce |
| `readText` | buffer da 256 B | 1 `read` ogni 256 B (non usato più da IR) |

`storage::list` sull'SD è un accesso FAT reale a ogni chiamata: non esiste
nessuna cache di directory o di path. Con 932 cartelle di marca e 2.255 file,
ogni visita all'albero è una raffica di open/readdir/stat.

> Nota: `list("/ir/TV", true)` in `IrMenu.cpp:186` passa `true` come
> **dirsOnly**, non come `preferSD`: il terzo parametro resta `false`. Quindi
> *tutte* le enumerazioni IR sono doppie (LittleFS + SD).

---

## 2. Tracciamento dei percorsi utente

Conteggi stimati dal codice; `S`/`L` = lookup SD/LittleFS, `D`/`M` = open dir
SD/LittleFS, `R` = voci lette con `readdir`, `N` = accessi NVS. Il dataset ha
8 categorie, 932 cartelle di marca (133 TV, 228 Clima, 128 Audio, 37 DVD,
54 Proiettori, 4 Console, 5 Universale, 343 Altro) + 38 marche legacy piatte.

| Percorso | I/O stimati | NVS | Tempo stimato |
|---|---|---|---|
| Apertura menu IR (`runMenu`) | `exists(last)` = 1 S + 1 L | 1 lettura | <20 ms |
| Preferiti (`runFavorites`) | fino a 20 `exists` = 40 S/L | 1 lettura | 20-60 ms |
| Telecomandi / elenco categorie (`runRemotes`) | 8× `categoryHasFiles`: ~1.030 R + 16 D/M + ~60 S/L | — | **0,5-1,5 s** |
| Categoria TV (`runCategory`) | 771 file nei due alberi (464 TV + 307 legacy): ~3.700 operazioni totali (stat, open dir, readdir) | — | **1,5-5 s** |
| Categoria Altro | 514 file + 343 marche: ~1.700 S/L + ~690 D/M + ~514 R ≈ 2.900 | — | **1,5-5 s** |
| Apertura marca, es. Samsung 57 file (`openBrand`) | `listRemotes` rifà tutto: ~120 S/L + ~60 R | **57 letture** (una per `isFavorite`) | **0,2-0,5 s** |
| Apertura telecomando (`openFile`) | `openRead` 3-4 lookup + `size` + 16-250 `read` da 256 B | 1 scrittura (`last`) + 1 lettura (hint) | 50-400 ms |
| Invio comando / tasti rapidi | 0 FS (solo IR) | — | 10-40 ms |
| Cerca (`runSearch`) | 8 `categoryBrands` + 932 `listRemotes`: ~3.300 S + ~3.300 L + ~1.100 D + ~1.100 M + ~3.500 R ≈ 12.300 | 1 lettura | **4-10 s** |
| Scanner, scelta categoria (`runScannerBrands`) | come Telecomandi (~1.000) | — | **0,5-1,5 s** |
| Scanner, elenco marche (`runScannerCategory`) | come Categoria (~3.700) | — | **1,5-5 s** |
| Scanner, passo su un telecomando (`runScanner`) | `listRemotes` marca + `loadCommands` del file + `isFavorite` | 1 lettura/passo | 100-300 ms/passo |
| "Riprendi" | `exists` + `openFile` | 1 lettura + 1 scrittura | 50-400 ms |
| "Database IR" (`runDatabaseInfo`) | scansione completa: ~12.300 operazioni | — | **4-10 s** |
| Refresh | non esiste; uscire e rientrare in un menu rifà tutto | — | come sopra |

### Perché "Cerca" è il caso peggiore

`runSearch` esegue per **tutte** le 932 marche:

```cpp
for (const auto& remote : listRemotes(brand)) { ... }   // IrMenu.cpp:1415
```

e `listRemotes` (righe 233-254) per ogni marca:

1. `brandBases()` → 1-2 `storage::exists` doppi (`:211`, `:216`);
2. `storage::list(base, false)` → enumerazione LittleFS **e** SD (`:237`);
3. per **ogni** file `.ir`: `storage::exists(path)` doppio (`:246`);
4. dedup lineare `std::find` su `seen` (`:242`).

Su 2.255 file: ~3.500 lookup SD + ~3.500 lookup LittleFS + ~2.000 open dir
(metà dei quali su LittleFS, che non contiene quelle cartelle) + ~3.500
`readdir`. A 0,3-1 ms per operazione SD sono 4-10 s, senza `navigation::poll()`
e senza possibilità di annullare (già segnalato come IR-15 in
`tests/01-ir.md:346-359`).

---

## 3. Evidenze file:linea

| Evidenza | Posizione | Impatto |
|---|---|---|
| `categoryBrands` elenca le dir su entrambi i FS | `IrMenu.cpp:186`, `:190` | 2 open dir per categoria (+1 per TV) |
| `brandBases` fa `exists` doppio per base | `IrMenu.cpp:211`, `:216` | ~2.100 lookup (1 S + 1 L per base) |
| `listRemotes` elenca su entrambi i FS | `IrMenu.cpp:237` | ~1.000 open dir SD + ~1.000 open LFS (quasi tutti miss) |
| **`exists` per ogni file elencato** | `IrMenu.cpp:246` | **+4.510 lookup per una ricerca** (introdotto in `6cbd90a`, non presente prima della feature) |
| `brandHasFiles` / `categoryHasFiles` rifanno `listRemotes` | `IrMenu.cpp:256-268` | scansione marche a ogni livello |
| Conteggio marche = `listRemotes(...).size()` | `IrMenu.cpp:1338`, `:1257`, `:1597` | enumerazione completa solo per l'etichetta `(N)` |
| `isFavorite` ricarica l'NVS ogni volta | `IrMenu.cpp:342-347`, chiamata a `:1091` | 57 letture NVS aprendo Samsung |
| `loadFavorites` apre `Preferences` e parsa il blob | `IrMenu.cpp:289-322` | allocazioni + flash a ogni item |
| `saveLastRemote` a ogni apertura | `IrMenu.cpp:1008`, `:419-425` | scrittura NVS inutile se il path non cambia |
| `openRead` fa `exists` + `open`, LFS prima di SD | `Storage.cpp:28-33`, `:88-119` | 3-4 lookup per file |
| `list` unisce LFS+SD quando `preferSD=false` | `Storage.cpp:157-164` | doppia enumerazione |
| `exists` controlla LFS poi SD | `Storage.cpp:121-131` | 2 lookup per path |
| Parser a blocchi da 256 B | `IrMenu.cpp:719-720`, `:744` | 235 `read` per un file da 60 KB |
| Ricerca senza polling/progresso | `IrMenu.cpp:1403-1448` | UI bloccata per secondi |
| `MenuManager::run` ridisegna solo se `dirty` | `MenuManager.cpp:419-517`, `:494-496` | **nessun I/O nel draw loop** |
| Azione eseguita su Select, fuori dal draw | `MenuManager.cpp:451-456` | I/O concentrato nell'azione, non nel frame |

---

## 4. Diagnosi

### 4.1 Il collo di bottiglia è l'enumerazione, non il rendering

L'ipotesi "MenuManager ridisegna e riseleziona facendo I/O" è **falsa**:
`MenuManager::run` chiama `navigation::poll()` e ridisegna solo su `dirty`
(`MenuManager.cpp:419-517`); non apre file né NVS. Il costo è tutto prima di
`menu.run(true)`: le funzioni `runCategory`, `openBrand`, `runSearch`,
`runDatabaseInfo`, `runScanner*` costruiscono la lista di voci enumerando la SD.
`openFile` costruisce il menu una volta e le lambda catturano un **riferimento**
a `commands` (`IrMenu.cpp:1019-1035`), quindi il parser non viene rieseguito a
ogni comando. Anche l'ipotesi "parse per ogni comando" è falsa: il parse avviene
una volta per apertura file (e una volta per passo di scanner, `:1154-1167`).

### 4.2 Il difetto strutturale: nessun indice, nessuna cache

La struttura `/ir/<categoria>/<marca>/<file>.ir` è corretta, ma viene
"riscoperta" da FAT ogni volta:

- elenco categorie → 8 `categoryHasFiles` → 932 `readdir` solo per capire se
  una categoria ha file;
- elenco marche → per ogni marca un'altra `listRemotes` per il conteggio;
- apertura marca → `listRemotes` **di nuovo**, poi `isFavorite` per voce;
- ricerca → tutte le marche, tutti i file, due FS;
- "Database IR" → la stessa scansione completa, in versione conteggio.

Non c'è nessun oggetto intermedio: né un indice persistente, né una cache RAM,
né un "backend" memorizzato per path. Il caso medio (2,4 file per marca)
potrebbe costare quasi zero: una marca ha in media 2 file, ma per saperlo si
aprono 932 directory e si fanno 2.255 stat.

### 4.3 Il secondo difetto: i conteggi costano quanto l'apertura

`runCategory`/`runScannerCategory` enumerano **tutti** i file della categoria
solo per mostrare `Marca (N)`. La stessa informazione può stare in un indice di
2 byte per marca: 932 × 2 B = 1,9 KB. Oggi costa ~2.900 accessi FS.

### 4.4 I costi accessori

- `isFavorite` riapre `Preferences` e riparsa il blob per ogni voce: su 57 file
  sono 57 letture NVS e decine di allocazioni, per un dato che cambia solo
  quando l'utente preme `F`.
- `openFile` scrive `last` in NVS a ogni apertura, anche se il path è lo stesso.
- `openRead` fa `exists` + `open` e prova LittleFS prima della SD: per i path
  `/ir/<categoria>/...` (solo SD) è un miss garantito.
- Il parser legge 256 B per volta: su file da 50-60 KB sono ~235 syscall; con
  1 KB scendono a ~59.

---

## 5. Fix proposto, a step

### Step 1 — Hotfix enumerazione (prerequisito, ~30-40 righe)

Obiettivo: eliminare i 2.255 `exists` per-file (4.510 lookup) e la doppia
enumerazione, mantenendo il comportamento attuale (fusione LFS+SD, dedup per
nome).

1. Aggiungere `bool onSd` a `storage::Entry` e valorizzarlo in `collect`
   (`Storage.cpp:172-191`). `listEntries` esiste già e restituisce anche
   `size`: si riusa invece di aggiungere API nuove.
2. `brandBases` restituisce `{path, onSd}`: `storage::exists(dir, true)` (1
   lookup SD) per le cartelle di categoria, `storage::exists(dir, false)` come
   fallback LittleFS. Per il vecchio albero piatto si tiene una base "merged"
   (`listEntries(legacy, false)`) così i file LFS+SD restano fusi come oggi.
3. In `listRemotes`, sostituire `storage::list(base,false)` +
   `storage::exists(path)` con **una** `storage::listEntries(base.path, base.onSd)`,
   filtrando `!isDir && isIrFile(name)`. Il path è `base + "/" + name`: il file
   è appena stato elencato, il controllo `exists` è ridondante per costruzione
   (non può mai essere falso). Per le ~894 marche che stanno solo su SD,
   `onSd=true` evita anche l'open dir LittleFS.
4. Estendere `Remote` con `size_t size` e `bool onSd`; usare `size` per il gate
   `>64 KB` senza aprire e `onSd` per `openRead(path, onSd)`.
5. `openBrand`: caricare i preferiti **una volta** prima del loop, non
   `isFavorite` per voce.
6. (Opzionale, stesso step) in `storage::openRead` togliere l'`exists`
   preliminare: `open()` su un path inesistente restituisce già un `File`
   invalido. Vale per tutti i moduli, guadagno 1-2 lookup per apertura.

Rischio: basso. `listEntries` ordina le directory prima dei file e deduplica
per `(nome, isDir)`; filtrare `isDir` e deduplicare per nome come oggi
(`seen`) preserva il comportamento.

**Guadagno stimato**: ricerca ~12.300 → ~5.700 operazioni (≈2,2×); categoria
~2.900 → ~1.300 (≈2,2×); apertura marca ~250 → ~60 (≈4×, grazie anche ai
preferiti in RAM).

### Step 2 — Indice persistente su SD (una volta)

Obiettivo: primo accesso, elenco categorie e ricerca per modello senza
enumerazione. È la spina dorsale: la cache RAM dello Step 3 si popola da qui.

**Posizione**: `/ir/.iridx/0` … `/ir/.iridx/7` (un file per categoria). Il
prefisso `.` è già filtrato da `storage::list` (`Storage.cpp:144`), quindi la
cartella non compare come marca legacy nel menu TV. In alternativa un solo file
`/ir/.iridx` con tabella offset in testa; 8 file sono più semplici e
indipendenti.

**Formato** (binario, little-endian, nomi con lunghezza):

```
Header:  magic "OIR1" | uint8 cat | uint8 reserved | uint16 brandCount
         | uint16 fileCount | uint32 crc32(payload)
Payload: per marca: uint8 nameLen, char name[]
                     uint16 fileCount
                     per file: uint8 nameLen, char name[]
```

Dimensione: ~38 B/record × 2.015 ≈ **77 KB** totali; la categoria più grande
(`Altro`, 514 file) ~20 KB, `TV` ~18 KB.

**Costruzione** (`buildIndex()`):

1. per ogni categoria, `categoryBrands` + `listEntries` per marca (stesso
   giro dello Step 1, senza `exists` per-file);
2. scrittura su `*.tmp` con buffer 1-2 KB, `flush`, `close`, `rename` (serve un
   `storage::renameFile` o l'accesso diretto a `SD`); CRC calcolato durante la
   scrittura;
3. se manca spazio/permessi: si resta sull'enumerazione dello Step 1 e si
   mostra "indice non disponibile", mai un crash.

Costo una tantum con Step 1: ~1-3 s (enumerazione 5.000-6.000 operazioni) +
0,2-0,5 s (scrittura). Va mostrata una schermata "Costruzione indice..." con
progresso; niente I/O nel draw loop, solo nella funzione di build. Costruire
l'indice **lazy** alla prima apertura di "Telecomandi" o "Cerca", non
all'ingresso del modulo: chi usa solo "Riprendi"/"Preferiti" non deve pagarlo.
Se lo script host che importa Flipper-IRDB genera già gli 8 file, la build
on-device è solo il fallback.

**Lettura**: aprire il file della categoria, verificare magic+crc, parsare nel
formato dello Step 3. ~20 KB sequenziali = 5-10 `read` da 2 KB, **30-80 ms**.
La ricerca legge gli 8 file in sequenza (~77 KB, 0,1-0,3 s) e costruisce solo i
40 `MenuItem` che servono: nessun albero completo in RAM.

**Invalidazione** (il punto delicato):

- header con `fileCount`: la voce "R = verifica/ricostruisci" di "Database IR"
  fa un conteggio reale (è già una scansione completa, quindi non peggiora) e
  se differisce ricostruisce;
- self-heal: se `openRead` di un path dell'indice fallisce, l'indice è segnato
  sporco e viene ricostruito al prossimo ingresso;
- se l'SD viene sostituita o rimossa, `hasSD()`/apertura fallita → rebuild o
  fallback all'albero LittleFS (307 file, enumerazione ~30-80 ms, nessun
  indice necessario);
- limite noto: un file **aggiunto** dall'esterno non compare finché non si
  ricostruisce (nessun segnale economico e affidabile di "directory changed" su
  FAT32; il timestamp della dir di marca non è esposto in modo affidabile e
  richiederebbe 932 stat). Da documentare nella UI dell'indice.

### Step 3 — Cache RAM di marche, conteggi e preferiti (~17-20 KB)

Obiettivo: evitare di rileggere l'indice a ogni ritorno indietro e togliere
l'ultimo costo per voce (NVS).

```cpp
// Una sola arena per i nomi, niente vector<std::string> per 932 marche.
struct BrandEntry { uint32_t nameOff; uint8_t nameLen; uint16_t files; };
std::string brandArena;                       // nomi "\0"-separati
std::vector<BrandEntry> brands[kCategoryCount];
std::vector<Favorite> favorites;              // caricati una volta
std::vector<std::string> fileCache;           // solo categoria corrente
```

- All'apertura di una categoria si parsano 30-80 ms di indice in `brands[]` e
  `fileCache`; i menu successivi della stessa categoria sono **0 I/O**.
- Se l'indice non è disponibile: cache lazy per categoria (la prima apertura
  paga l'enumerazione, ~0,3-1 s per Altro), poi istantanea.
- `runRemotes`, `runCategory`, `runScannerCategory`, `runScannerBrands`,
  `runDatabaseInfo` leggono solo la cache; il comando esplicito
  "R = verifica/ricostruisci" in "Database IR" è l'unico che rilegge l'albero.
- `openBrand` usa la cache (lista file già pronta) e non riapre la SD.
- `isFavorite` usa `favorites` in RAM; `toggleFavorite` aggiorna vettore e NVS
  una volta.
- Invalidare la cache quando `hasSD()` cambia (card inserita/rimossa) e quando
  l'utente chiede "R" da "Database IR".
- Liberare `fileCache` prima di `openFile` (il parser può arrivare a ~36 KB:
  meglio non sommare 15-20 KB di nomi).

RAM: 932 × 8 B ≈ 7,5 KB di struct (padding incluso) + ~11 KB di arena + 1 KB
preferiti = **~20 KB** nel caso peggiore (343 marche Altro); `fileCache` della
categoria corrente ~13-16 KB, rilasciata all'ingresso del telecomando.

**Guadagno stimato**: categoria/marche/scanner <30 ms a cache calda; ricerca
0,1-0,3 s (uno o due giri di indice). Il primo accesso in assoluto paga la
build (1-3 s); con l'indice già presente si leggono 0,1-0,4 s.

### Step 4 — Letture più efficienti

1. `IrLineReader::chunk_` da 256 a **1024 B** (`IrMenu.cpp:744`): 4× meno
   `read`; su un file da 60 KB si passa da ~235 a ~59 syscall. Stack +768 B,
   accettabile (il reader è su stack e il buffer riga è già 3.584 B).
2. `openRead(path, onSd)` con backend noto (Step 1/2): elimina il miss LittleFS
   per i path SD.
3. In `openFile`, `saveLastRemote` solo se il path è cambiato (risparmia una
   scrittura NVS per ogni riapertura dello stesso telecomando).
4. `parseRawValues`/`reserve` restano invariati: non sono il collo di
   bottiglia e toccarli cambia il budget RAM già validato.

**Guadagno stimato**: apertura telecomando medio 50-150 ms → 20-60 ms; file
grandi 0,3-0,8 s → 0,15-0,3 s.

### Step 5 — Cache dell'ultimo telecomando (opzionale, solo dopo misura)

Tenere l'ultimo `path` + `commands` in RAM evita il parse quando si riapre lo
stesso telecomando ("Riprendi", uscita accidentale). Va limitata: se
`rawData` totale > ~2.048 valori o l'heap dopo il parse è < 64 KB, non cacheare.
Rischio RAM (fino a ~36 KB) superiore al beneficio finché lo Step 4 non è
misurato: **non farlo per primo**.

### Step 6 — I/O fuori dal draw loop (già rispettato)

Stato attuale verificato: `MenuManager::run` non fa I/O. Regola per il fix:
l'indice si legge/scrive in `runMenu`, `runCategory`, `openBrand`, `runSearch`,
**mai** in `drawBody`/`drawGrid`/`drawList`. Se in futuro l'indice venisse letto
in `draw`, ogni tasto (anche il repeat ogni 110 ms) tornerebbe sulla SD.

### Riepilogo guadagni (stime)

| Flusso | Oggi | Dopo Step 1 | Dopo Step 2 (indice) | Dopo Step 3 (cache) |
|---|---|---|---|---|
| Telecomandi (categorie) | 0,5-1,5 s | 0,3-1 s | <100 ms | <50 ms |
| Categoria (Altro) | 1,5-5 s | 0,5-1,5 s | 30-100 ms | <30 ms |
| Marca (Samsung) | 0,2-0,5 s | 50-150 ms | <50 ms | <20 ms |
| Cerca | 4-10 s | 1-3 s | 0,1-0,3 s | 0,1-0,3 s |
| Database IR | 4-10 s | 1-3 s | 0,1-0,3 s | <100 ms |
| Telecomando | 50-400 ms | 50-400 ms | 20-150 ms | 20-150 ms |
| Primo ingresso IR | <20 ms | <20 ms | 0,1-0,4 s (indice presente) / 1-3 s (build) | idem |

---

## 6. Rischi e mitigazioni

| Rischio | Gravità | Mitigazione |
|---|---|---|
| RAM: cache categoria (~20 KB) + parser raw (~36 KB) + menu (~6 KB) | Media | liberare `fileCache` prima di `openFile`; tenere il gate `kMinIrHeap` a 64 KB (`IrMenu.cpp:844-850`); misurare con il file da 60 KB |
| Indice stale (file aggiunti/rimossi dall'esterno) | Media | self-heal su open fallita, rebuild da "Database IR", conteggio nell'header; documentare il limite |
| Prima scansione/build percepita come "blocco" | Bassa | schermata di progresso, build una tantum, `tmp`+`rename`, nessun indice parziale |
| Scrittura su SD piena/read-only | Bassa | fallback all'enumerazione dello Step 1 e alla cache RAM dello Step 3; messaggio esplicito, nessun crash |
| Indice su SD che sparisce (card rimossa) | Bassa | `hasSD()` + fallback LittleFS; i preferiti hanno già il backend salvato |
| Concorrenza con altri moduli che usano la SD (ESP-NOW ha un suo mutex, `EspNowMenu.cpp:131-160`) | Bassa | IR è in foreground e i menu sono esclusivi; se in futuro l'IR girasse in background, serve un lock SD condiviso |
| Nomi file con `\t`/`\n` o >255 caratteri | Bassa | formato binario con lunghezza + validazione; i nomi importati sono già sanificati (REPORT) |
| Indice che altera il conteggio visibile (duplicati legacy/TV) | Media | costruire l'indice con la stessa dedup di `listRemotes` (nome case-insensitive per marca), così 2.015 resta il numero mostrato |
| Cache non invalidata dopo copia file via `SdMenu` | Media | `SdMenu` non tocca `/ir`; in ogni caso invalidare su `R` e su open fallita |

---

## 7. Test e benchmark da eseguire

### 7.1 Host (senza hardware)

1. **Contatore di operazioni**: avvolgere `storage::list`, `exists`,
   `openRead` con un contatore e simulare i percorsi su `E:\ir` (2.255 file).
   Asserzioni dopo il fix:
   - `runSearch`: 8 open dei file indice e 0 `exists` per-file (dopo lo Step 2);
   - `runCategory`: 0 open dir se l'indice è caldo;
   - `openBrand`: 0 open dir (lista file dall'indice/cache), 2,4 file medi.
2. **Confronto conteggi**: l'indice deve produrre esattamente le 2.015 voci
   visibili del REPORT (464/403/371/124/126/8/5/514) e i 240 duplicati
   TV/legacy nascosti.
3. **Corruzione**: troncare un file indice, cambiare un byte, cancellare una
   categoria → rebuild senza crash e conteggi corretti.
4. **Dedup case-insensitive**: fixture con `Samsung`/`samsung` e file omonimi
   nei due alberi.

### 7.2 Dispositivo (benchmark)

Aggiungere log temporanei `esp_timer_get_time()` (o `logger::info`) attorno a:
`buildIndex`, prima apertura di `Altro`, `openBrand(Samsung)`, `runSearch`,
`openFile` (file medio e `NEC_RU_M124.ir` da 60.922 B), un passo di scanner.
Misurare anche `ESP.getFreeHeap()` prima/dopo ogni flusso e il picco durante il
parse.

Soglie di accettazione suggerite:

| Operazione | Target |
|---|---|
| Apertura menu IR | < 50 ms |
| Telecomandi → categorie | < 100 ms (dopo cache/indice) |
| Categoria Altro | < 150 ms cold, < 30 ms warm |
| Marca Samsung (57 file) | < 100 ms cold, < 20 ms warm |
| Telecomando medio | < 150 ms |
| `NEC_RU_M124.ir` (60 KB) | < 600 ms, nessun reset |
| Cerca | < 400 ms cold, < 100 ms warm |
| Database IR | < 400 ms (esclusa rebuild esplicita) |
| Build indice | < 2 s |
| Heap residuo dopo cache+parse | ≥ 64 KB (gate attuale) |

### 7.3 Casi funzionali da non rompere

- SD assente: 307 file LittleFS, 38 marche TV legacy, preferiti e "Riprendi"
  ancora validi.
- SD inserita: 2.015 voci, fusione TV+legacy, dedup.
- Preferiti: aggiunta/rimozione, limite 20, path SD conservati a card rimossa.
- Scanner: invio Power, `[n/d]`, `[raw >512]`, file >64 KB saltato con motivo.
- Tasti rapidi `P M + - C X`: nessun I/O, invio immediato.
- `R`/rebuild indice mentre si è in un sottomenu: nessun puntatore pendente
  alle stringhe della cache (usare indici/offset, non `const char*` nel menu).

### 7.4 Verifica "niente I/O nel draw"

Contare le chiamate FS tra l'ingresso e l'uscita di `menu.run` senza input:
deve essere **0**. Ripetere con repeat di `;`/`.` (110 ms) e con l'animazione
del carosello attiva.

---

## 8. Appendice — conteggi per categoria (dal REPORT)

| Categoria | Marche | File visibili | Operazioni stimate oggi (apertura categoria) |
|---|---:|---:|---:|
| TV (+ legacy) | 133 (+38) | 464 (+307 legacy) | ~3.700 |
| Clima | 228 | 403 | ~2.100 |
| Audio | 128 | 371 | ~1.600 |
| DVD/Blu-ray | 37 | 124 | ~520 |
| Proiettore | 54 | 126 | ~600 |
| Console | 4 | 8 | ~40 |
| Universale | 5 | 5 | ~40 |
| Altro | 343 | 514 | ~2.900 |
| **Totale** | **932 (+38)** | **2.015 (+307)** | **~12.300 (ricerca)** |

Formula usata per l'apertura categoria, con `B` marche e `T` file:

```
S ≈ T + B          (stat SD: 1 per file + 1 per base)
L ≈ T + B          (stat LittleFS, quasi sempre miss)
D ≈ B + 1          (open dir SD: categoria + marche)
M ≈ B + 1          (open dir LittleFS, quasi sempre miss)
R ≈ T (+ file legacy per TV)
```

TV aggiunge una seconda base (`/ir/<marca>`) e quindi raddoppia `S`, `L`, `D`,
`M` per le sue 133 marche.

Per la ricerca, `B`=932 e `T`=2.255 su tutte le categorie.

---

## 9. Conclusione operativa

L'ordine di lavoro è: **Step 1 → Step 2 → Step 3 → Step 4**, con misura dopo
lo Step 2. Lo Step 1 è obbligatorio anche per l'indice: senza togliere
`exists` per-file, la build pagherebbe gli stessi 4.510 lookup. Lo Step 2
risolve primo accesso, elenco categorie e ricerca per modello; lo Step 3 rende
istantaneo il ritorno sui menu. Lo Step 5 (cache comandi) va valutato solo dopo
aver misurato l'heap, perché il parser raw può già arrivare a ~36 KB.
