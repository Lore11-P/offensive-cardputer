# FIX_SEC — Storage, WPA-SEC, ESP-NOW, Tools, BadUSB

Worktree: `orbit-wt-fix-sec`, branch `review/fix-sec`, base `ec5a601`
(ORBIT OS 2.0.2).
Riferimento: `docs/integration/review/04-sec.md`.

Applicati i fix **Must #2** e **Should #3, #4, #5**. Il **Must #1 (OTA)** non
e' stato toccato: e' assegnato all'area net. Nessuna modifica a Wifi, BLE,
flasher o `partitions.csv`.

Nessun hardware collegato: build e dimensioni sono misurate, il resto e'
verificato staticamente. I test su Cardputer restano da eseguire (§5.2).

---

## 1. Modifiche

### 1.1 Tools — lettura SD fallita non riscrive piu' il file troncato

`src/modules/tools/ToolsMenu.cpp`, `src/utils/Storage.cpp`:

- `appendLine()` ora appende **solo** se il file non esiste oppure se
  `storage::readText()` lo ha letto per intero:
  `if (exists && !readText(...)) return false;`. Una card che perde un colpo
  non sostituisce piu' `/tools/totp.txt` o `/tools/calendar.txt` con la sola
  riga nuova.
- `writeFileVerified()` ha il parametro `dropBackup` (default `false`): il
  `.bak` viene rimosso solo dopo che il contenuto **precedente** e' stato
  letto/validato e il nuovo contenuto e' stato riletto. `appendLine()` e
  `saveNotes()` lo passano a `true` solo nei percorsi che hanno gia' validato
  la lettura.
- `loadCalendar()` e `loadTotp()` restituiscono `bool`: `false` = file
  presente ma illeggibile, distinto da file assente/vuoto (`true` + vettore
  vuoto). Le schermate Calendario e TOTP mostrano "File presente ma
  illeggibile" e **non** aprono l'editor; "aggiungi" riesegue il controllo.
- `notesScreen()`: se `notes.txt` esiste ma `readText` fallisce, mostra errore
  e non entra nell'editor; niente autosave sopra un file illeggibile.
- `Storage::writeAtomic()`: se il file principale manca, il rename di
  fallback non cancella piu' un eventuale `.bak` (che puo' essere l'unica
  copia buona). Il `.bak` viene rimosso solo quando il file principale esiste
  ed e' stato spostato con successo.

### 1.2 WPA-SEC — un solo backend e revoca reale

`src/modules/wpasec/WpaSecMenu.cpp`, `include/Storage.h`,
`src/utils/Storage.cpp`:

- La chiave usa **solo la microSD** per `loadKey()`/`saveKey()`. Inserire o
  togliere la card non cambia piu' la chiave vista dal modulo.
- `setKeyFlow()` richiede la SD e mostra un `ui::confirm` esplicito:
  "Salvare la chiave in chiaro sulla microSD rimovibile?".
- `saveKey()` elimina la vecchia copia LittleFS (`key.txt`, `.tmp`, `.bak`):
  migrazione esplicita, la chiave non ricompare togliendo la SD.
- `clearKeyFlow()` rimuove `key.txt`, `key.txt.tmp` e `key.txt.bak` su **SD e
  LittleFS**, poi verifica che nessuna copia resti; se ne resta una il
  messaggio e' "Cancellazione non riuscita", non "Chiave cancellata".
- "Stato" mostra `presente su SD` / `assente` / `assente (SD non montata)` e
  segnala `!Copia su LittleFS ignorata` per installazioni precedenti.
- Nuovi helper `storage::existsOn()` / `storage::removeFileOn()`: accesso a un
  backend preciso, senza il fallback LittleFS→SD di `exists()`/`removeFile()`.

### 1.3 Storage — tetto opzionale per le directory grandi

`include/Storage.h`, `src/utils/Storage.cpp` + chiamanti:

- `list()` e `listEntries()` accettano `maxEntries` (default **0 =
  illimitato**) e `bool* truncated`. Il vettore viene fermato **durante** la
  scansione, non dopo: nessuna allocazione oltre il tetto.
- Tetti applicati solo dove serve:
  - `SdMenu::browse()`: 200 voci, hint `solo 200 voci`;
  - ESP-NOW `browseFiles()`: 150, hint `prime 150 voci`;
  - ESP-NOW `inboxMenu()` / `deleteInboxFile()`: 256, hint/messaggio
    `prime 256 voci` / `cartella parziale`;
  - WPA-SEC `loadCaptures()` e "Stato": 256, messaggio `prime 256` o
    `!Solo le prime 256 catture`.
- IR, Pcap, BadUSB, Music e gli altri chiamanti restano **illimitati**:
  `maxEntries = 0` preserva la semantica precedente (i 2.255 file IR
  continuano a essere elencati tutti).
- Se il tetto viene riempito da voci non pertinenti (es. sidecar `.crc`
  nell'inbox), la UI dice "prime N voci senza file", non "cartella vuota".

### 1.4 BadUSB — nessun testo di script nei log

`src/modules/badusb/BadUsbScript.cpp`, `src/utils/Logger.cpp`,
`platformio.ini`:

- La riga non riconosciuta logga solo `riga %u non riconosciuta`, senza
  `%s`/`text`. Restano solo numero riga e motivo generico.
- `env:cardputer-adv-hid` compila con `-DORBIT_DISABLE_LOGGER=1`; in quella
  build `logger::detail::writeLine()` e' un no-op. La CDC USB e' la porta
  vista dal target, quindi non viene emesso alcun log.
- Le altre build (`cardputer-adv`, `cardputer-adv-offensive`) mantengono il
  logger seriale.

---

## 2. File toccati

| File | Modifica |
|---|---|
| `include/Storage.h` | `maxEntries`/`truncated`, `existsOn`/`removeFileOn`, `kListMax` |
| `src/utils/Storage.cpp` | tetto durante la scansione, helper per backend, `.bak` preservato |
| `src/modules/tools/ToolsMenu.cpp` | `appendLine` gated, `loadCalendar`/`loadTotp` con esito, note non illeggibili, `.bak` |
| `src/modules/wpasec/WpaSecMenu.cpp` | chiave SD-only + migrazione, clear su entrambi i backend + `.tmp`/`.bak`, avviso, stato, tetto catture |
| `src/modules/espnow/EspNowMenu.cpp` | tetto browser 150, inbox/delete 256, messaggi parziali |
| `src/modules/sd/SdMenu.cpp` | tetto 200 senza resize post-allocazione |
| `src/modules/badusb/BadUsbScript.cpp` | rimosso il testo dal log |
| `src/utils/Logger.cpp` | no-op con `ORBIT_DISABLE_LOGGER` |
| `platformio.ini` | flag logger nella build HID |
| `docs/integration/FIX_SEC.md` | questo documento |

OTA, Wifi, BLE, flasher e partizioni non sono stati modificati.

---

## 3. Build e dimensioni (misurate)

Comandi: `python -m platformio run -e cardputer-adv-offensive` e
`-e cardputer-adv-hid` (timeout 900.000 ms).

La baseline offensive e' stata misurata nello stesso albero con `git stash`
delle modifiche e ricompilazione incrementale, cosi' il delta non dipende da
oggetti di build diversi.

| Voce | Prima (HEAD) | Dopo | Delta |
|---|---:|---:|---:|
| Flash usata | 2.697.341 B (95,7%) | **2.705.193 B (96,0%)** | **+7.852 B (+0,29%)** |
| `firmware.bin` | 2.697.712 B | **2.705.552 B** | **+7.840 B** |
| RAM statica `.data+.bss` | 147.392 B (45,0%) | **147.392 B (45,0%)** | **0 B** |
| Libero nello slot app | 120.707 B | **112.855 B** | −7.852 B |

Build offensive completa: `SUCCESS` in 509,9 s; incrementali 29–47 s.
Build HID: `SUCCESS` in 204,5 s (poi 28,8 s incrementale), flash
2.616.945 B (92,9%), RAM 155.016 B (47,3%), `firmware.bin` 2.617.312 B.
La baseline HID non e' stata misurata: per quel delta non viene dichiarato
un valore.

La ricompilazione offensive dopo il ripristino ha prodotto lo stesso SHA-256
del primo artefatto (`75F8EE…1D56`): build deterministica.

---

## 4. Comportamento atteso

| Scenario | Comportamento |
|---|---|
| `/tools/totp.txt` esiste ma la lettura SD fallisce | "File presente ma illeggibile", nessun editor, nessuna scrittura; `.bak` intatto |
| Nota esistente illeggibile | "Nota presente ma illeggibile", nessun editor/autosave |
| Append su file assente | file creato con la riga, come prima |
| Append su file esistente leggibile | contenuto preservato, `.bak` rimosso solo dopo verifica |
| Chiave WPA-SEC senza SD | "microSD assente", nessun salvataggio; la chiave non va piu' su LittleFS |
| Chiave WPA-SEC con SD | conferma "in chiaro su SD rimovibile", salvataggio su SD, copia LittleFS rimossa |
| Cancella chiave con copie su SD e LittleFS | rimosse entrambe + `.tmp`/`.bak`; se resta qualcosa il messaggio e' di errore |
| Cancella chiave senza SD | rifiutato: "microSD assente"; senza card la revoca non sarebbe verificabile |
| Stato WPA-SEC | backend della chiave e avviso se resta una copia LittleFS |
| Cartella con >200/256 voci | lista limitata, hint/messaggio "prime N voci"; nessun crash |
| Cartella IR con 2.255 file | invariata: `maxEntries` default 0, elenco completo |
| BadUSB con riga non valida | log solo `riga N non riconosciuta`; nessun testo |
| Build HID | logger disabilitato, nessuna scrittura su CDC |

---

## 5. Test

### 5.1 Eseguiti

- Build `cardputer-adv-offensive`: `SUCCESS` (completa e incrementale).
- Build `cardputer-adv-hid`: `SUCCESS` (verifica che BadUSB e il flag logger
  compilino; il file BadUSB non e' incluso nella build offensive).
- Baseline e post-fix nello stesso albero, dimensioni in §3.
- Build deterministica: SHA-256 identico su due compilazioni.
- Verifica statica:
  - `BadUsbScript.cpp` non contiene piu' `logger::*` con `text`/`%s`;
  - tutti i `listEntries` dell'area hanno il tetto esplicito o il default 0;
  - i chiamanti IR/Pcap/Music/BadUSB non passano `maxEntries`;
  - `appendLine`/`loadCalendar`/`loadTotp`/`notesScreen` non ignorano piu' il
    valore di `readText`;
  - `clearKeyFlow` copre i tre nomi su entrambi i backend;
  - nessun riferimento a OTA nel diff.
- `git diff --check`: nessun whitespace error.

### 5.2 Da eseguire sulla Cardputer

1. **TOTP**: con `totp.txt` valido aggiungere un segreto (i precedenti
   restano); estrarre la card a meta' lettura e verificare il messaggio di
   errore e l'integrita' del file.
2. **Calendario**: stesso test con `calendar.txt`; aggiunta e riapertura.
3. **Note**: nota esistente, autosave dopo 15 s e salvataggio con ESC; con
   file illeggibile l'editor non deve aprirsi.
4. **`.bak`**: simulare un rename interrotto (card piena o rimozione durante
   il salvataggio) e verificare che `.bak` resti e sia recuperabile.
5. **Chiave WPA-SEC**: impostare con SD, poi togliere la SD e verificare che
   "Stato" dica "assente (SD non montata)" e che la chiave non torni da
   LittleFS; con SD assente "Cancella chiave" deve rifiutare; rimettere la SD
   e cancellare la chiave; controllare con un PC che `/wpasec/key.txt`, `.tmp`
   e `.bak` non esistano ne' sulla SD ne' sul filesystem interno.
6. **Catture**: creare >256 file in `/captures`, aprire "Catture"/"Upload" e
   verificare il messaggio "prime 256"; il device non deve riavviarsi.
7. **SD browser / ESP-NOW**: cartella root con >200 file; verificare hint e
   navigazione. Inbox con >256 voci (anche sidecar `.crc`) e messaggio
   "cartella parziale".
8. **IR**: verificare che i 2.255 file siano ancora tutti elencati.
9. **BadUSB** (solo build HID): script con una riga non valida contenente un
   segreto; monitorare la CDC e verificare che compaia solo il numero riga.
10. **Heap**: "Info dispositivo"/heap prima e dopo l'apertura di cartelle
    grandi; confrontare con la baseline.

---

## 6. Rischi e limiti noti

| Rischio / limite | Gravita' | Stato / mitigazione |
|---|---|---|
| WPA-SEC ora richiede la SD per la chiave | Media | Scelta del fix #3: un solo backend. Chi aveva la chiave su LittleFS la reinserisce con SD inserita (migrazione esplicita); "Stato" segnala la copia ignorata |
| Il tetto lista e' in ordine di directory, non alfabetico | Bassa | Con >200/256 voci quali voci compaiono dipende dal filesystem; la UI lo dichiara. IR e gli altri chiamanti restano illimitati |
| Conteggio catture in "Stato" parziale oltre 256 | Bassa | La riga mostra `prime 256` |
| Un `.bak` puo' restare fino al primo salvataggio validato | Bassa | Occupa al massimo la dimensione del file (<=8 KB per i Tools); e' la copia di sicurezza |
| Logger HID completamente disabilitato | Bassa | Diagnostica assente in quella build; scelta esplicita per non scrivere sulla CDC del target |
| `BleHidScript.cpp` logga ancora il testo delle righe non riconosciute | Media | Stesso pattern del #5 ma nel modulo BLE, fuori area: da correggere nell'area BLE con la stessa regola |
| Nessun test su hardware | Media | Tutto verificato staticamente + build; §5.2 e' la lista da eseguire |
| Margine flash ridotto | Media | 112.855 B liberi (96,0%); ogni nuova funzione va misurata |
| OTA non corretto | Alta | Fuori scope per assegnazione: resta il blocco #1 della review, a carico dell'area net |

---

## 7. Conclusione

I fix #2–#5 della review sono applicati: gli errori di lettura SD non
distruggono piu' TOTP/calendario/note, la chiave WPA-SEC ha un solo backend e
una revoca verificata su entrambi i filesystem, le cartelle grandi hanno un
tetto esplicito solo dove richiesto (IR invariato), e BadUSB non scrive il
testo degli script ne' sulla seriale HID. La build offensive cresce di
7.852 B (+0,29%) con RAM invariata; la HID compila con logger disabilitato.
Restano i test su Cardputer e il fix OTA dell'area net.
