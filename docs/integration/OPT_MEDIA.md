# Ottimizzazione giochi / musica / storage — OPT_MEDIA

Worktree `orbit-wt-opt-media`, branch `feat/opt-media`, commit di partenza
`c7456af`. Ambiente `cardputer-adv-offensive`, PlatformIO 6.2.0,
`espressif32@6.7.0`, GCC xtensa 8.4.0, `-Os` con `-ffunction-sections`,
`-fdata-sections`, `--gc-sections`.

Obiettivo: meno flash e meno heap/frammentazione **senza cambiare nulla di
visibile**: stessi limiti utente, stessi overlay, stesso formato file.

## 1. Misure

| Voce | Prima | Dopo | Delta |
|---|---:|---:|---:|
| `firmware.bin` | 2.358.560 B | **2.351.408 B** | **−7.152 B (−6,98 KiB, −0,30%)** |
| Flash PlatformIO (`text+data`) | 2.358.193 B (90,0%) | **2.351.037 B (89,7%)** | −7.156 B |
| RAM statica (`.data+.bss`) | 115.520 / 327.680 B (35,3%) | **115.448 / 327.680 B (35,2%)** | −72 B |
| Slot app OTA | 2.621.440 B | 2.621.440 B | — |
| Libero nello slot | 262.880 B (256,7 KiB) | **270.032 B (263,7 KiB)** | +7.152 B |

La baseline è quella misurata dal worktree `orbit-wt-budget` sullo stesso
commit `c7456af` (`docs/integration/BUDGET.md`); il commit `62bf184` di quel
worktree aggiunge solo documentazione e script, quindi il binario è
confrontabile. Il valore "dopo" viene dalla build finale di questo worktree
(`SUCCESS`, incrementale).

La RAM statica cambia poco per scelta: i limiti delle liste sono stati
ripristinati. Il guadagno vero è sulle allocazioni temporanee (heap e
frammentazione), qui stimato sulle strutture, non misurato a runtime:

| Scenario | Prima (stima) | Dopo (stima) |
|---|---:|---:|
| Musica: 150 brani in menu | ~150 stringhe di percorso | 0 (il path si ricava) |
| SD: apertura file di testo | testo + copia righe + copia wrappata | testo + una sola lista di righe |
| 2048: ogni `spawn()` | 1 `std::vector` (fino a 16 coppie) | 0 allocazioni |
| Minesweeper: ogni `reveal()` | `std::vector<int>` con crescita | pila fissa su stack, 0 allocazioni |
| Connect 4: ogni turno CPU | `std::vector<int>` | 0 allocazioni |
| TextInput: ogni frame (~18 ms) | copia di `std::vector<char>` | array fisso di 96 bool |

## 2. Modifiche

### 2.1 Record dei giochi (`Scores.h`, `Scores.cpp`)

- Rimossi `std::map<std::string,uint32_t>`, `std::istringstream` e la funzione
  `all()` (mai chiamata): il parsing di `/scores.txt` è un singolo passaggio
  sul testo con `memcmp`/`strtoul`.
- API cambiata da `const std::string&` a `const char*`: tutte le chiamate
  passavano già string literal, quindi nessun gioco è stato toccato.
- Formato e limiti invariati: `nome=valore`, una riga per gioco, lettura fino a
  **4 KB** (come prima). Se il file esiste ma non è leggibile per intero,
  `submit()` non scrive: nessun record può essere troncato o perso.
- Le righe sconosciute vengono preservate; il file viene riscritto solo se il
  record migliora.
- L'ordine nel file non è più alfabetico (era un effetto collaterale di
  `std::map`). `SdMenu` ordina le righe prima di mostrarle, quindi la schermata
  "SD > Punteggi" resta identica; i backup copiano il file byte per byte e i
  lettori riga-per-riga non dipendono dall'ordine.

### 2.2 Attesa del game over (`GamesUi.h`, `GamesUi.cpp` — nuovi)

- Unica funzione `games::ui::waitRetry()` per il ciclo INVIO/ESC ripetuto in
  Simon, Connect 4 e Minesweeper. Non disegna nulla.
- **Gli overlay sono stati ripristinati identici all'originale**: stesse
  dimensioni, stessi offset, stessi colori e stessi testi per Snake, Pong,
  Breakout, Tetris, 2048, Flappy, Simon, Minesweeper, Connect 4, Pac-Man,
  Doom, StarSweep, Orbital, Lander, Pixel Jump. Il diff verso `c7456af` non
  contiene più nessuna riga di disegno dei pannelli.

### 2.3 Menu giochi (`GamesMenu.cpp`)

- Tabella `constexpr GameEntry kGames[]` con etichetta, icona e funzione: una
  sola lambda di ingresso con indice al posto di 17 lambda distinte. Ordine
  delle voci invariato.
- Rimossi gli include non usati (`Ui.h`).

### 2.4 Giochi

- **2048**: `spawn()` non costruisce più un `std::vector<std::pair<int,int>>`
  per ogni mossa; conta le celle vuote e sceglie l'ennesima (stessa
  distribuzione uniforme).
- **Minesweeper**: il flood-fill di `reveal()` usa una pila fissa
  `int[63]` + maschera `queued`; nessuna allocazione e nessun rischio di
  crescita illimitata.
- **Connect 4**: la scelta della colonna CPU non usa più `std::vector<int>`;
  due passaggi sulle 7 colonne, stesso comportamento (vittoria, blocco,
  preferenza per il centro, altrimenti scelta casuale).
- **Minecraft**: rimosso `randUnit()`, mai chiamata.
- **Orbital / StarSweep**: le stringhe di suggerimento statiche diventano
  `constexpr const char*` (niente `std::string` globale con inizializzazione
  dinamica).
- Tutti i giochi continuano a salvare il record con la stessa chiave
  (`tetris`, `breakout`, `pacman`, `doom`, `snake`, `pong`, `2048`, `flappy`,
  `simon`, `mines`, `connect4`, `chess`, `starsweep`, `lander`, `orbital`,
  `pixeljump`).

### 2.5 Musica (`MusicMenu.cpp`)

- **Limiti ripristinati: 40 album e 150 brani per album**, come prima.
- `Track` non duplica più il percorso: tiene solo il nome, il path si ricava
  da album + nome al momento dell'apertura (`/data/Music/<Album>/<brano>`).
  Una `std::string` in meno per brano, allocata e copiata in meno.
- Cover, player e gestione pausa/volume invariati.

### 2.6 SD (`SdMenu.cpp`)

- Rimosse `splitLines()`, `wrapLine()`, `parentPath()` e `looksText()`: le
  ultime due non erano mai chiamate.
- **Limiti ripristinati**: file fino a 12 KB e 1024 righe visualizzabili,
  200 voci per cartella, come prima.
- `showFile()` non costruisce più una copia intermedia di tutte le righe: legge
  il testo una volta e crea solo i segmenti da mostrare. Stesso contenuto a
  schermo, meno heap e meno frammentazione.
- `showScores()` ordina le righe prima di mostrarle (la schermata resta
  alfabetica come quando il file era scritto da `std::map`).

### 2.7 Impostazioni (`SettingsMenu.cpp`)

- Rimosso `brightnessPercent()` (mai chiamata).
- La stringa di stato `[ON]/[OFF]/[n/d]` è un `const char*` invece di una
  `std::string` costruita per ogni riga a ogni frame.

### 2.8 Editor di testo (`utils/TextInput.cpp`)

- La rilevazione dei tasti già premuti non copia più `std::vector<char>` a ogni
  frame: array fisso di 96 bool (caratteri 0x20-0x7F) riempito in loco.
- Rimosso `#include <algorithm>` e l'helper basato su `std::find`.

## 3. Cosa si potrebbe spostare su SD (non fatto)

Nessun gioco è stato rimosso o ridotto. Dimensione indicativa da map file:
Scacchi 12,4 KiB, Doom Lite 9,0 KiB, Minecraft 7,4 KiB, Pixel Jump 6,7 KiB;
gli altri 2,6-5,8 KiB.

Se servisse altro spazio, i candidati sensati sono i **dati di livello**, non
il codice:

1. **Minecraft 3D**: la heightmap è già generata proceduralmente (nessun asset
   in flash). Si potrebbe salvare/caricare un mondo da `/games/minecraft/*.mc`
   su SD, ma aggiungerebbe codice di I/O e un formato: sconsigliato finché non
   serve la persistenza.
2. **Doom Lite**: le 3 mappe 16x16 sono ~800 B in `.rodata`. Spostarle su SD
   farebbe risparmiare meno di 1 KiB ma aggiungerebbe parser e gestione errori:
   non conviene.
3. **Pixel Jump**: i 2 livelli sono ~1,8 KB di stringhe in `.rodata`;
   stessa conclusione.
4. **Musica**: già interamente su SD, cover compresa (nessun asset audio in
   flash).

In sintesi: la flash dei giochi è quasi tutta codice, non dati. Per un taglio
vero bisogna disabilitare un gioco intero da `GamesMenu.cpp` (2,6-12,4 KiB) o
`ENABLE_MODULE_GAMES` (94,3 KiB), decisione che richiede autorizzazione.

## 4. Rischi

1. **Ordine di `/scores.txt`**: il file non è più alfabetico. `SdMenu` ordina
   in visualizzazione, i backup sono copie byte-per-byte e il vecchio firmware
   rilegge qualunque ordine con `std::map`; nessun record cambia nome o valore.
   Il limite di lettura è tornato a 4 KB: un file più grande non viene
   sovrascritto, esattamente come prima.
2. **Nessuna regressione visiva volontaria**: overlay e limiti utente sono
   identici all'originale. Resta una differenza teorica nei pannelli solo se un
   gioco futuro verrà aggiunto senza copiare lo stile esistente.
3. **Memoria interna**: `showFile()` usa ancora una `std::vector<std::string>`
   per le righe (max 1024), ma non più due copie del testo. Il picco è più
   basso; su file enormi il comportamento resta "mostra i primi 12 KB".
4. **Stack**: Minesweeper aggiunge ~315 B in `reveal()` e `showScores()` tiene
   al massimo 4 KB di righe. Il task dei menu ha 16 KB di stack; margine ampio,
   ma va verificato in hardware con `Stack min` in Info dispositivo.

## 5. Test manuali

Precondizione: build `cardputer-adv-offensive`, SD inserita con
`/data/Music/<Album>/` popolata e `/scores.txt` eventualmente già presente
(anche generato dalla versione precedente).

1. **Tetris**
   - Avvia, muovi/ruota con `; . , /`, drop con SPC, arriva a game over.
   - Verifica pannello "GAME OVER" identico al passato (138x44, bordo rosso),
     INVIO riavvia, ESC torna al menu.
   - Esci a metà partita con punteggio > 0 e rientra: il record deve restare.
2. **Snake**
   - Mangia almeno un frutto, muori contro il muro e contro il corpo.
   - INVIO riprova; ESC esce. Il record in alto non deve regredire.
3. **2048**
   - Gioca finché il campo si riempie o arriva a 2048; verifica che le mosse
     restino legali e che la tile nuova compaia sempre in una cella vuota.
   - Game over: pannello identico al passato, INVIO riavvia, ESC esce.
4. **Minesweeper**
   - Primo INVIO su una cella: il campo si apre senza mina sotto il cursore.
   - Apri una zona grande (flood-fill) e verifica che si apra tutta, senza
     buchi; metti/togli bandiere con F; prova vittoria e mina.
5. **Connect 4**
   - Gioca una partita completa contro la CPU: deve vincere se ha una mossa
     vincente e bloccare quelle dirette dell'utente.
   - Verifica pareggio a campo pieno e contatore "Vittorie totali".
6. **Musica**
   - Con 2 album e pochi brani: entra, apri album, riproduci, pausa con INVIO,
     cambia brano con `, /`, volume con `; .` (anche tenendo premuto).
   - Verifica che il percorso aperto sia album + brano e che ESC fermi l'audio.
   - Con una libreria grande (fino a 40 album / 150 brani) deve elencare tutto
     come prima.
7. **SD > Sfoglia**
   - Apri un file di testo grande (10-12 KB) e uno con righe lunghe: scorri con
     `; . , /`, verifica a capo e contatore in basso.
   - Cartella con >200 file: deve mostrare 200 voci e il messaggio
     "solo 200 voci".
8. **SD > Punteggi** e backup
   - Con `/scores.txt` creato dalla versione precedente, la schermata deve
     mostrare gli stessi primi 8 record in ordine alfabetico.
   - Esegui "Backup": `/backup/scores.txt` deve essere identico al file
     originale.
9. **Impostazioni > Moduli attivi** e **Info dispositivo**
   - Toggle ON/OFF con INVIO, verifica `[ON]/[OFF]/[n/d]` e ESC.
10. **Editor di testo** (es. Tools > Note o WiFi Setup)
    - Scrivi una password con caratteri ripetuti e tieni premuto DEL: nessun
      carattere perso o duplicato; ESC annulla, INVIO conferma.

## 6. Verifica dimensione

```powershell
python -m platformio run -e cardputer-adv-offensive
Get-Item .pio\build\cardputer-adv-offensive\firmware.bin | Select-Object Length
```

Nessun push è stato eseguito; il commit è locale sul branch `feat/opt-media`.
