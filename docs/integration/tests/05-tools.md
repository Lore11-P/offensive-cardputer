# 05 — Utility: test finale integrato (bug report)

Data: 2026-11-21
Codice esaminato: `offensive-cardputer`, HEAD `f19b6d0` (`merge: Piano B
partizioni e flasher`), commit feature `fd95041` (`feat(tools): clock, note,
calcolatrice, QR e TOTP`). File letti: `src/modules/tools/ToolsMenu.cpp`
(1241 righe), `src/modules/tools/CalcExpr.h`, `src/modules/tools/REPORT.md`,
`src/utils/Storage.cpp`, `src/utils/WifiSetup.cpp`, `src/utils/TextInput.cpp`,
`src/core/Display.cpp`, `src/core/Ui.cpp`, `src/core/PowerTimer.cpp`,
`include/*.h`, `platformio.ini`, `partitions.csv`.

Riferimenti: `docs/integration/BRIEF.md`, `docs/integration/specs/05-tools.md`,
`orbit-wt-qa/docs/integration/reviews/05-tools.md` (piano di verifica).

Limiti della verifica: sola lettura, nessuna modifica al codice, nessuna build
eseguita, nessun test su hardware. Le prove su SD, NTP, QR e authenticator
restano manuali. Il binario citato in P2-11 è l'artefatto già presente in
`.pio/build/cardputer-adv-offensive/` (21/11/2026 15:43:53), non una build di
questo test.

Esito: **NON ACCETTABILE** finché P1-01 e P1-02 non sono corretti. Nessun P0:
non sono state trovate corruzioni di memoria raggiungibili dai percorsi utente
attuali, né crash. P1-01 viola il teardown WiFi richiesto (C7/R6); P1-02 rende
non garantita l'atomicità di note/calendario/TOTP (N8). Restano inoltre C1/C5
(NTP) e K7/K13 (calendario) non superati come scritti nel piano QA.

## 1. Sintesi

| ID | Sev | Area | Esito |
|---|---|---|---|
| P1-01 | P1 | NTP / WiFi | Connessione NTP annullata o fallita: la radio resta accesa, `syncNtp` esce prima del cleanup |
| P1-02 | P1 | Note / calendario / TOTP | `writeAtomic` lascia il file primario assente nella finestra `.bak`; nessun recupero al load |
| P2-01 | P2 | Clock / TOTP | Data manuale 2000–2099 accettata, ma `timeValid` richiede ≥ 2023-11-14: "Impostato" e poi orologio non valido |
| P2-02 | P2 | NTP / UX | Nessun sync automatico all'apertura del Clock (C1); nessun messaggio su timeout/annullamento (C5) |
| P2-03 | P2 | QR / memoria | Buffer statico 176 B < 211 B richiesti per la versione 6; oggi irraggiungibile (testo max 100 B), latente |
| P2-04 | P2 | Calendario / SD | SD assente: mese vuoto senza errore (K13/R1); nessun `ensureToolsDir`/`hasSD` all'ingresso |
| P2-05 | P2 | Calendario / TOTP | Troncamento silenzioso (8 KB / 4 KB), cap 120/16 non applicati in aggiunta, righe malformate senza avviso (K5) |
| P2-06 | P2 | Calendario / UTF-8 | Byte UTF-8 → `?` nel font 5x7; gli accenti del fixture QA non sono visualizzati (K7) |
| P2-07 | P2 | TOTP / formato | `Nome|SECRET` salvato così com'è (minuscole/spazi); 17° segreto accettato ma ignorato; duplicati ammessi (F5) |
| P2-08 | P2 | Note | Auto-save fallito in silenzio; limite 1024 senza feedback; solo le ultime 7 righe visibili; CRLF riscritto LF |
| P2-09 | P2 | UI 240x135 | Footer calendario troncato (38 caratteri); contatore note a 1 px dal footer (R9) |
| P2-10 | P2 | Log | `WifiSetup` scrive SSID e IP su seriale durante NTP; nessuna password, ma C9 chiede nessun SSID non necessario |
| P2-11 | P2 | REPORT / build | Il REPORT dice 2.393.712 B e 228 KB liberi; l'artefatto finale è 2.618.256 B (199.792 B liberi nello slot da 2.818.048 B) |
| P2-12 | P2 | QR / BMP | Export BMP non atomico: un errore a metà lascia `/tools/qr.bmp` parziale (Q8; nessun dato utente) |

## 2. Findings P1

### P1-01 — NTP annullato/fallito: la radio WiFi resta accesa

**File**: `src/modules/tools/ToolsMenu.cpp:227-230` (`syncNtp` esce su
`ensureConnected` falso), `:269-271` (cleanup solo sul percorso di successo);
`src/utils/WifiSetup.cpp:191-215` (`scanNetworks` lascia `WIFI_STA`),
`:410-429` (`scanAndConnect` annulla senza spegnere), `:455-482`
(`ensureConnected`).
**Severità**: P1 (teardown richiesto dal BRIEF; C7/R6; batteria e heap).

`syncNtp()` fa:

```cpp
if (!wifisetup::ensureConnected(true, parent)) {
    return false;                 // <-- nessun disconnect/sntp_stop
}
```

`ensureConnected(true)` non è atomico: se non ci sono credenziali apre
`scanAndConnect`; `scanNetworks` mette la radio in `WIFI_STA` (riga 194) e non
la spegne dopo la scansione. Se l'utente annulla la scelta rete (`chooseNetwork`
< 0, righe 421-423) o la password (righe 426-429), `scanAndConnect` torna false
con la radio ancora accesa. Lo stesso se la connessione con credenziali salvate
fallisce e l'utente rifiuta la riconfigurazione. `syncNtp` esce alla riga 229 e
non arriva mai a `wifisetup::disconnect()`/`sntp_stop()` (269-270). Il REPORT
dichiara "Timeout 10 s, poi WiFi e SNTP vengono fermati" (`REPORT.md:149-151`):
vero solo sul percorso 269-271.

**Impatto**: dopo Utility → Orologio → ENTER → annulla la configurazione rete,
la radio resta in STA fino a un altro modulo o al riavvio. C7 ("WiFi spento,
heap recuperato") e R6 falliscono. Consumo e heap non tornano allo stato
pre-modulo. Lo stesso pattern è già stato segnalato in
`docs/integration/tests/07-wpasec.md` P1-3: la causa condivisa è in
`WifiSetup`.

**Fix**: un solo punto di uscita in `syncNtp` (guard RAII o `goto cleanup`) che
esegua sempre `wifisetup::disconnect(); sntp_stop();` prima di ogni `return`;
in alternativa correggere `scanAndConnect`/`ensureConnected` perché su
annullamento facciano `WiFi.scanDelete(); WiFi.mode(WIFI_OFF);`. Il fix in
`WifiSetup` è preferibile perché copre anche gli altri moduli.

**Verifica**: C4/C5 con monitor seriale e `WiFi.getMode()`; dopo ESC dal Clock
la radio deve essere `WIFI_OFF` e l'heap tornare al valore pre-NTP.

### P1-02 — Scrittura atomica: la finestra `.bak` può lasciare il file assente

**File**: `src/utils/Storage.cpp:280-319` (`writeAtomic`), in particolare
`:300` (rename diretto), `:304-310` (spostamento del vecchio in `.bak`),
`:311-312` (rename finale e rimozione `.bak`), `:316-317` (ripristino).
Chiamanti della feature: `ToolsMenu.cpp:220` (`appendLine` per calendario e
TOTP), `:676` (`saveNotes`), `:1176` (TOTP).
**Severità**: P1 (N8; possibile perdita percepita di note/calendario/TOTP).

Su FAT/`SD.rename` il rename diretto verso un target esistente fallisce, quindi
il percorso normale è:

1. `rename(tmp, path)` fallisce;
2. `rename(path, path.bak)` riesce (la destinazione ora non esiste);
3. `rename(tmp, path)` riesce.

Tra 2 e 3 il file primario **non esiste**: se manca l'alimentazione in quella
finestra, al reboot `path` è assente e il contenuto vecchio è solo in
`path.bak`. Il modulo non lo cerca: `notesScreen` fa `fileSize` → 0,
`readText` → false, note vuota; `loadCalendar`/`loadTotp` tornano vuoti. Il
REPORT dice "tmp + rename" e il piano QA N8 pretende "vecchia versione integra
oppure nuova versione integra, mai un file parziale": qui si ottiene **nessun
file visibile** (recuperabile solo da PC). In più, se `path` è assente e il
rename diretto riesce, un `.bak` residuo non viene rimosso (riga 300 non fa
`remove(bak)`), quindi N6 può fallire dopo un crash precedente.

Lo stesso schema vale per `appendLine`, che riscrive l'intero file di
calendario/TOTP: ogni aggiunta è esposta alla finestra.

**Impatto**: note, eventi e segreti possono "sparire" dopo un'interruzione di
alimentazione durante il salvataggio; il dato è ancora su SD in `.bak` ma
l'utente non lo vede e il firmware non lo recupera. N8 non è superabile.

**Fix**: recupero all'apertura: se `path` manca e `path.bak` esiste,
`rename(path.bak, path)` (o lettura diretta di `.bak`) prima di mostrare il
contenuto; in `writeAtomic`, dopo un rename diretto riuscito, rimuovere un
`.bak` residuo; nel fallback, non rimuovere il `.bak` finché il file nuovo non
è stato verificato (riaprirlo e confrontare la dimensione/CRC). La soluzione
minima e robusta è il recupero in lettura, perché copre anche i crash già
avvenuti.

**Verifica**: N8 con 10 interruzioni di alimentazione durante il salvataggio,
più un test manuale che rinomina `notes.txt` in `notes.txt.bak` e riapre Note:
deve comparire il contenuto vecchio, non una nota vuota.

## 3. Findings P2

### P2-01 — Data manuale accettata ma considerata "non impostata"

**File**: `ToolsMenu.cpp:43` (`kEpochValid = 1700000000`, 2023-11-14),
`:145-163` (`parseIsoDate` accetta 2000–2099), `:323-343` (conferma e
`mktime`/controlli), `:348-356` (`settimeofday` + "Impostato"), `:279`
(`timeValid` nello schermo), `:1115` (`timeValid` in TOTP).
**Severità**: P2.

Il campo "Data AAAA-MM-GG" accetta il 2000-01-01 e lo salva; subito dopo
`timeValid()` è falso perché la soglia è il 2023-11-14. Risultato: il messaggio
"Impostato: ..." è seguito da "--:--:--" e "ora non impostata", e il TOTP
rifiuta i codici. Allo stesso modo una data 2023-01-01 (valida per il parser)
viene rifiutata. La validazione è incoerente tra parser e `timeValid`. Caso
minore: nella transizione DST di primavera `mktime` normalizza 02:30→03:30, ma
il controllo (`tm_year/tm_mon/tm_mday`) e il messaggio mostrano l'ora digitata,
non quella normalizzata.

**Impatto**: l'utente crede di aver impostato l'orologio e il TOTP non
funziona; un tester che usa una data 2000/2022 per C6 vede un comportamento
contraddittorio. Con orologio non valido anche il calendario parte da gennaio
2026 (`ToolsMenu.cpp:562-564`) senza avviso: si possono aggiungere eventi nel
mese sbagliato credendoli nel mese corrente.

**Fix**: o limitare `parseIsoDate` (solo per il path manuale) a
`>= 2023-11-14`, con messaggio esplicito, oppure accettare 2000+ e considerare
"impostato" un orologio scritto manualmente (flag NVS separato da
`kEpochValid`, che serve solo a distinguere l'epoch 0 di SNTP). Aggiungere il
controllo di `tm_hour`/`tm_min` dopo `mktime` per il caso DST.

### P2-02 — Clock: nessun sync all'apertura (C1) e nessun messaggio di errore (C5)

**File**: `ToolsMenu.cpp:359-383` (`clockScreen`), `:374-376` (NTP solo su
`Event::Select`), `:227-271` (`syncNtp` ritorna bool), `:247-249` (timeout),
`:264-266` (ESC interrompe), `:269-271` (cleanup), `:299-301` (footer).
**Severità**: P2 (C1/C5 del piano QA).

Aprendo Utility → Orologio con WiFi salvata e orologio mai sincronizzato, il
modulo mostra "--:--:--" e resta lì: la sincronizzazione parte solo con ENTER.
Il piano QA C1 si aspetta "ora sincronizzata entro pochi secondi" all'apertura.
Inoltre `syncNtp` ritorna false su timeout (riga 247) e su ESC (riga 264) ma
`clockScreen` ignora il valore: nessun messaggio, nessuna distinzione tra
"annullato" e "rete/NTP non raggiungibile". C5 chiede "timeout breve con
messaggio; non resta appeso": il timeout c'è, il messaggio no.

**Impatto**: un utente senza rete non capisce perché l'ora resta vuota; C1/C5
non sono superati come scritti. Il fix è piccolo e migliora molto l'usabilità.
Inoltre `sntp_stop()` (riga 270) ferma il poll periodico: senza RTC con batteria
l'ora non viene più riallineata e la deriva C8 dipende dall'oscillatore interno;
il REPORT non dichiara una strategia (ri-sync periodico o all'apertura). L'ora
impostata manualmente non è persistita in NVS: dopo un power cycle si torna a
"non impostata" (coerente con l'assenza di RTC, ma da dichiarare).

**Fix**: all'ingresso, se `!timeValid(time(nullptr))` e ci sono credenziali,
chiamare `syncNtp` una volta (con timeout breve) o mostrare "ENTER per
sincronizzare"; far ritornare a `syncNtp` un enum `{Ok, Cancelled, Failed}` e
mostrare `ui::message` solo su `Failed`. Documentare in `REPORT.md` la scelta
(auto vs manuale).

### P2-03 — QR: buffer statico da 176 B, la versione 6 ne richiede 211

**File**: `ToolsMenu.cpp:942` (tabella capacità v1–v6), `:943-949` (scelta
versione), `:955-957` (commento e buffer statico
`((16*6+128)*6+64)/8 = 176`), `:959-960` (`qrcode_initText`). Libreria
installata: `.pio/libdeps/cardputer-adv-offensive/QRCode/src/qrcode.c:185-187`
(`bb_getGridSizeBytes(size) = (size*size+7)/8`), `:770-772`
(`qrcode_getBufferSize(version)`), `:815-819` (`memset` e griglia sulla
dimensione reale).
**Severità**: P2 latente (P0 se si alza `kQrTextMax` oltre 106).

Per la versione 6 (41 moduli) la libreria usa
`qrcode_getBufferSize(6) = (41*41+7)/8 = 211` byte; il buffer statico è 176.
`qrcode_initBytes` non valida la lunghezza (`@TODO: Return error if data is too
big`, qrcode.c:774) e `bb_initGrid` fa `memset(data, 0, 211)`: con la versione
6 si scrivono 35 byte oltre il buffer (BSS), con corruzione di variabili
adiacenti. Oggi `kQrTextMax = 100` e `Field.maxLen = 100`, quindi il massimo è
la versione 5: 37 moduli → 172 byte, che entrano nei 176. La tabella però
accetta fino a 134 byte (versione 6) e il REPORT dichiara "versione massima 6"
(`REPORT.md:132-138`): chi alza il limite a 134 per "usare la v6" ottiene una
corruzione di memoria. Il commento "qrcode_getBufferSize(6) = 176" è errato
per la libreria in `lib_deps`.

**Impatto**: nessuno oggi; latente e fuorviante. Il test Q3 non copre la v6
perché il campo si ferma a 100.

**Fix**: `static uint8_t modules[211];` (o `qrcode_getBufferSize(6)` calcolato
una volta e `static_assert`), oppure limitare esplicitamente la tabella a v5
(`kQrTextMax <= 106`) e correggere il REPORT. Se si vuole la v6, allocare 211 e
testare la decodifica.

### P2-04 — Calendario senza SD: mese vuoto senza errore

**File**: `ToolsMenu.cpp:554-564` (`calendarScreen`), `:394-399`
(`loadCalendar` con `preferSD=true`), `:502-506` (solo l'aggiunta mostra
l'errore).
**Severità**: P2 (K13/R1).

Con SD assente `loadCalendar` ritorna un vettore vuoto e la schermata mostra un
mese normale, senza alcun avviso; l'utente può credere che gli eventi siano
stati cancellati. L'errore compare solo se prova ad aggiungere. Note e TOTP
all'ingresso chiamano `ensureToolsDir()` e mostrano "microSD assente"; il
calendario no.

**Fix**: all'inizio di `calendarScreen`, `if (!ensureToolsDir())
ui::message("Calendario", {"!microSD assente.", "Gli eventi stanno su
/tools/calendar.txt."}, "Utility"); return;` (o check `hasSD()` se si vuole
mantenere la vista vuota read-only, con un banner).

### P2-05 — Calendario/TOTP: troncamento e righe scartate in silenzio

**File**: `ToolsMenu.cpp:45-46` (`kCalendarMax=8192`, `kTotpMax=4096`),
`:397` e `:1060` (`readText` con ritorno ignorato), `:401` (cap 120),
`:411-423` e `:1074-1087` (righe malformate/segreti invalidi `continue`),
`:203-221` (`appendLine`; `:211` ignora il ritorno di `readText`, `:207`
controlla solo `fileSize`).
**Severità**: P2.

`readText` documenta (`include/Storage.h:35-39`) che ritorna false anche quando
il file supera `maxBytes`, ma riempie `out` con la parte letta: `loadCalendar`
e `loadTotp` ignorano il bool e parsano la parte, quindi gli eventi/segreti
oltre il limite spariscono senza avviso. I cap 120/16 fermano il caricamento
ma **non l'aggiunta**: un 121° evento o un 17° segreto viene scritto con
successo e poi non mostrato. Le righe malformate sono ignorate senza il
"avviso" richiesto da K5. In `appendLine`, se `readText` fallisce a metà (errore
SD), `data` è parziale e `writeText` riscrive il file con la coda persa.

**Fix**: controllare il ritorno di `readText` e mostrare "file troppo grande /
letto parzialmente"; contare le righe scartate e visualizzarne il numero;
applicare i cap anche in aggiunta (`se events.size() >= 120` / `secrets.size()
>= 16` → messaggio); in `appendLine`, se `readText` è false e `fileSize > 0`,
annullare senza scrivere. Documentare i limiti in byte nel REPORT (F7).

### P2-06 — Accenti UTF-8 visualizzati come `?`

**File**: `ToolsMenu.cpp:420` (testo evento caricato così com'è), `:530`
(`shorten` a 36 **byte**), `src/core/Display.cpp:669-673` (`drawGlyph` mappa
`c > 0x7F` a `'?'`), `include/PixelFont.h:7-9` (font ASCII 0x20–0x7F).
**Severità**: P2 (K7).

Il fixture QA `2026-12-31|Fine anno con accenti: città, perché` viene letto e
mostrato come `citt??, perch??` (un `?` per byte UTF-8), e `shorten` può
tagliare a metà una sequenza. Non c'è crash, ma K7 ("accenti visualizzati
correttamente") fallisce. Il REPORT parla di "testo libero ASCII" per le note,
ma non per il calendario, che è pensato per file scritti da PC.

**Fix**: piccola traslitterazione UTF-8→ASCII (à→a, è→e, é→e, ì→i, ò→o, ù→u,
ç→c) prima del disegno, oppure dichiarare "solo ASCII" e sostituire ogni byte
non ASCII con un carattere singolo; in ogni caso non usare `shorten` sui byte
grezzi.

### P2-07 — TOTP: formato non canonico, 17° segreto ignorato, duplicati

**File**: `ToolsMenu.cpp:1138-1180` (`addTotpSecret`), `:1149-1153`
(name/source trim), `:1161` (decodifica), `:1170-1175` (consenso), `:1176`
(`appendLine`), `:1064` (cap 16 nel load), `:1084` (skip dei segreti invalidi).
**Severità**: P2 (F5, limite 16, T4/T5).

Il segreto viene scritto **così com'è digitato**: `base32Decode` accetta
minuscole, spazi, `-` e `=` ovunque (righe 1013-1014), quindi il file può
contenere `demo|gezd gn bv...` invece del formato F5 "Base32 maiuscolo, nessun
spazio interno". Un PC/authenticator che non è tollerante può rifiutarlo. In
aggiunta: il load si ferma a 16 segreti (riga 1064), ma `addTotpSecret` non
verifica il conteggio: il 17° viene salvato, l'utente non lo vede e non riceve
errori. I nomi duplicati non sono controllati (T5). Le righe con Base32 non
valido sono scartate senza avviso (T4).

**Fix**: normalizzare prima del salvataggio (`toupper`, rimozione di spazi,
`-`, `=` solo in coda), rifiutare/segnalare i duplicati, caricare il file e
verificare `secrets.size() >= 16` prima di appendere, e mostrare un avviso se
il load ha scartato righe.

### P2-08 — Note: auto-save silenzioso, limite senza feedback, editing solo in coda

**File**: `ToolsMenu.cpp:679-776` (`notesScreen`), `:687-695` (lettura e
`\r` removal), `:725-732` (ESC salva), `:733` (limite 1024 senza messaggio),
`:760-763` (ENTER), `:768-773` (auto-save; il fallimento non è mostrato),
`:642-673` (`drawNotes` mostra solo le ultime 7 righe).
**Severità**: P2.

Se l'auto-save ogni 15 s fallisce (SD piena/rimossa), l'utente continua a
digitare e non vede nulla; se spegne, perde il testo. Il limite di 1024 byte
blocca i caratteri in eccesso senza indicare perché (solo il contatore
"1024/1024"). Il file viene letto togliendo i `\r` e riscritto in LF: un file
CRLF creato da PC viene convertito anche solo premendo ESC. L'editor mostra
solo le ultime 7 righe e non ha cursore/frecce: il testo precedente non è
modificabile se non cancellando tutto quello che segue. Il REPORT dichiara
"editor minimale" ma non questi effetti.

**Fix**: mostrare un indicatore "!SD" persistente quando il salvataggio
fallisce (senza `ui::message` bloccante a ogni frame); mostrare "limite
raggiunto" al primo rifiuto; preservare CRLF o documentare la conversione;
aggiungere almeno uno scroll/cursore o dichiarare esplicitamente nel REPORT che
si edita solo la coda. Non è un P1 perché il percorso ESC avvisa, ma è un
rischio di perdita dati.

### P2-09 — UI: footer calendario troncato, contatore note a filo del footer

**File**: `ToolsMenu.cpp:487` (footer calendario, 38 caratteri), `:668-671`
(contatore a `y=116`), `src/core/Display.cpp:1022-1054` (`footer` occupa
`h-12=123`), `:1001-1018` (`textClipped` taglia con `..`).
**Severità**: P2 (R9).

`display::footer` ha circa 36 caratteri utili (`width - 9 - 12` / 6): la stringa
"ESC=esci </>=mese ;.=giorno ENTER=apri" (38 caratteri) viene tagliata a 34
caratteri + "..", perdendo gli ultimi 4 di "ENTER=apri". Inoltre il footer dice
`</>` ma i tasti reali sono `,` e `/` (Navigation.cpp:8-11): il suggerimento è
fuorviante anche quando non viene tagliato. Il contatore note a `y=116` con
font 0 arriva alla riga 123, che il footer ridisegna: l'ultima riga del
contatore è coperta (per i digiti può non essere visibile, ma il margine è
zero).

**Fix**: accorciare i footer (es. "ESC=esci  ,/=mese  ENTER=apri") e spostare
il contatore note a `y=112`; usare i caratteri reali `,` `/` `;` `.` nei testi.

### P2-10 — Log: SSID e IP su seriale durante NTP

**File**: `src/utils/WifiSetup.cpp:405-406` (`WiFi connesso: %s ip=%s`),
`:121` (`credenziali salvate per %s`); `ToolsMenu.cpp:228` (NTP chiama
`ensureConnected`).
**Severità**: P2 (C9; nessuna password).

La feature non logga nulla (nessun `logger`/`Serial` in `ToolsMenu.cpp`), e
questo è corretto. Però il percorso NTP usa `wifisetup`, che scrive l'SSID e
l'IP su seriale. Non è una credenziale, ma C9 chiede "nessuna password WiFi,
nessun SSID se non necessario"; l'SSID è persistito anche nel log seriale di
un monitor USB. Il REPORT dice "nessun log di segreti", vero per i segreti
TOTP/note, ma non per l'SSID.

**Fix**: nel percorso NTP (o in `WifiSetup`) loggare solo "WiFi connesso" e
l'heap, senza SSID; se serve per diagnosi, renderlo opt-in. Documentare la
scelta in `REPORT.md`.

### P2-11 — REPORT: dimensioni e capacità non aggiornate

**File**: `src/modules/tools/REPORT.md:125-128` (2.393.712 B, 91,3%, 228 KB
liberi), `:132-138` (QR "versione massima 6"), `:143-148` (limiti calendario e
TOTP).
**Severità**: P2 (documentazione; G5).

L'artefatto finale in `.pio/build/cardputer-adv-offensive/firmware.bin` è
**2.618.256 byte** (21/11/2026 15:43:53; oggetti `ToolsMenu.cpp.o` 15:41:55,
coerenti con HEAD `f19b6d0`). Lo slot `app0/app1` di `partitions.csv` è
`0x2B0000` = **2.818.048 byte**, quindi restano **199.792 byte** (7,1%).
Rispetto al limite BRIEF di 2,5 MB (2.621.440) restano solo **3.184 byte**:
il budget "2,5 MB" è al 99,88%. Il REPORT del modulo (2.393.712 / 228 KB) è
precedente ai merge e non descrive il firmware finale. Inoltre la "versione
massima 6" del QR non è sicura (P2-03) e i limiti in byte di
calendario/TOTP (8192/4096) non sono dichiarati (F7).

**Fix**: aggiornare il REPORT con la misura finale e la partizione usata;
chiarire "QR v1–v5 (max 100 byte), v6 non abilitata" e i limiti file; indicare
che il margine reale è 199.792 B nello slot da 2.6875 MB (o 3.184 B se si
applica il limite BRIEF di 2,5 MB).

### P2-12 — Export BMP del QR non atomico

**File**: `ToolsMenu.cpp:977` (`display::saveScreenshot`),
`src/core/Display.cpp:1312-1369` (`storage::openWrite` diretto, scrittura a
blocchi, nessun `.tmp`/rename).
**Severità**: P2 (Q8; nessun dato utente).

Un errore a metà scrittura lascia `/tools/qr.bmp` parziale o troncato; il file
precedente viene sovrascritto subito con `FILE_WRITE`. Non è un problema di
sicurezza (è un export), ma Q8/Q10 possono vedere un BMP non valido.

**Fix**: scrivere in `/tools/qr.bmp.tmp` e rinominare a fine scrittura, oppure
`remove` il parziale su errore; usare lo stesso schema di `writeText`.

## 4. Aree verificate senza findings

### Calcolatrice (`CalcExpr.h`)
La grammatica `expr/term/factor` con precedenza e associatività a sinistra è
corretta; i 10 casi QA (`1+2*3`, `(1+2)*3`, `10-2-3`, `20/4/5`, `7%4`,
`2*(3+4)-5`, `-5+2`, `((2+3)*4)/2`, `1+2*3%4`, `8/(2+2)`) danno i risultati
attesi per traccia manuale. `1/0`, `5%0`, `1+`, `(1+2`, `abc`, `1..2` sono
rifiutati; `depth > 24` limita le parentesi; `std::isfinite` (riga 147) rifiuta
l'overflow; l'input UI è limitato a 64 caratteri (`kCalcMax`). Non ho trovato
overflow di buffer né parse ambigui. Resta non testato su hardware il layout
dei tasti `( ) * / % +` (Shift), ma il mapping `Keyboard.h` li espone.

### TOTP (Base32, HMAC, rollover)
`base32Decode` (rifiuto dei caratteri non validi, capacity 64) è corretto;
`hotpCode` usa HMAC-SHA1 mbedtls, contatore big-endian a 8 byte, offset
`digest[19] & 0x0F` e maschera `0x7F`, cioè la dynamic truncation RFC 4226;
`code % 1000000` a 6 cifre; passo `time(nullptr)/30`; countdown
`30 - now%30`; se `!timeValid` non mostra codici e avvisa. I vettori RFC 6238
59→287082, 1111111109→081804, 1111111111→050471, 1234567890→005924,
2000000000→279037, 20000000000→353130 sono tutti coerenti con il codice
(64-bit `time_t` sul toolchain xtensa: `sys/_types.h` usa `__int_least64_t`
quando `long` è 32-bit). Il REPORT ne riporta solo 4: mancano 1111111111 e
20000000000, ma il codice è corretto. I problemi TOTP trovati sono solo di
formato/limiti (P2-07), non crittografici.

### QR v1–v5
La tabella `{17,32,53,78,106,134}` corrisponde alle capacità byte ECC_LOW della
libreria installata; per v1–v5 il buffer 176 copre 172 byte (v5) e
`qrcode_initText` riceve una versione sufficiente. La quiet zone è 4 moduli
(8·scala px), il bianco/nero è forzato (`0xFFFF`/`0x0000`) e il layout sta
sopra il footer. Il solo difetto è la v6 (P2-03).

### Formati SD
Percorsi `/tools/notes.txt`, `/tools/calendar.txt`, `/tools/totp.txt` corretti
e su SD (`preferSD=true`); `/tools` creata da `ensureToolsDir`; note LF senza
header; calendario `AAAA-MM-GG|testo`; TOTP `Nome|SECRET` (ma non canonico,
P2-07); nessun BOM scritto; nessun path alternativo. `storage::writeAtomic` è
usato per i tre file (atomicità P1-02 a parte). `ensureToolsDir` non verifica
che `/tools` sia una directory: se esiste come file, `mkdir` fallisce ma
`exists` è true e il salvataggio fallisce poi con un errore generico (caso
limite, non testato).

### Segreti nei log
`ToolsMenu.cpp` non contiene `logger`/`Serial`/`printf`: note, calendario,
testo QR, nomi e segreti TOTP, codici a 6 cifre non vengono mai scritti su
seriale. L'unica esposizione è l'SSID di `WifiSetup` (P2-10). Il consenso al
salvataggio in chiaro è richiesto (`ui::confirm`, righe 1170-1175) e il
rifiuto non scrive nulla. La SD non è cifrata ed è dichiarato.

## 5. Top 5

1. **P1-01** — NTP annullato/fallito: radio WiFi lasciata accesa
   (`ToolsMenu.cpp:228-230`, `WifiSetup.cpp:191-215/410-429`). C7/R6.
2. **P1-02** — Scrittura atomica: finestra `.bak` senza recupero; note,
   calendario e TOTP possono risultare assenti dopo un power cut
   (`Storage.cpp:300-318`, `ToolsMenu.cpp:220/676/1176`). N8.
3. **P2-01** — Data manuale 2000–2099 accettata ma `timeValid` richiede
   ≥ 2023-11-14: "Impostato" e poi orologio/TOTP non validi
   (`ToolsMenu.cpp:43/161/339`).
4. **P2-02** — Clock: nessun sync all'apertura (C1) e nessun messaggio su
   timeout/annullamento (C5) (`ToolsMenu.cpp:359-383/227-271`).
5. **P2-03** — QR: buffer 176 B contro i 211 B della v6; latente, ma memoria
   corrotta se si alza il limite a 134 byte (`ToolsMenu.cpp:955-959`).

## 6. Test manuali ancora necessari

Restano da eseguire su hardware, come già indicato dal REPORT e dal piano QA:
C1–C9 (NTP, fuso, cold boot, deriva, teardown), K1–K13 (calendario, inclusi
CRLF, bisestile, UTF-8, SD assente), N1–N13 (note, in particolare N8 con
interruzioni reali), Q1–Q10 (QR scansionato da due telefoni, temi, BMP), T1–T10
(TOTP contro Aegis/Google Authenticator, rollover, Base32, consenso), F1–F8
(formati reali sulla SD), R1–R12 (SD rimossa, ESC, 20 cicli, heap, log).
La build non è stata rifatta: il binario presente è citato solo come misura.
