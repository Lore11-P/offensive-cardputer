# 07 — WPA-SEC: test finale integrato (bug report)

Data: 2026-11-21
Codice esaminato: `offensive-cardputer`, branch `feat/10-modules`, HEAD `f19b6d0`
(`merge: Piano B partizioni e flasher`), `src/modules/wpasec/WpaSecMenu.cpp`
(2018 righe), `src/modules/wpasec/REPORT.md`, `src/utils/Storage.cpp`,
`src/utils/WifiSetup.cpp`, `src/utils/TextInput.cpp`, `src/core/Ui.cpp`.
Riferimenti: `docs/integration/BRIEF.md`, `docs/integration/specs/07-wpasec.md`,
`orbit-wt-qa/docs/integration/reviews/07-wpasec.md`.

Limiti della verifica: sola lettura, nessuna modifica al codice, nessuna build,
nessun test su hardware. Non è stata caricata alcuna cattura reale e non è
stata usata alcuna chiave reale. Le prove sul servizio sono state fatte con una
chiave sintetica di 32 zeri e con un POST senza parte file; i dati restituiti
dal servizio non sono stati salvati né riportati (nessun SSID/password reale in
questo documento).

Esito: **NON ACCETTABILE** finché P0-1 e P0-2 non sono corretti. Il resto è
quasi tutto P1/P2; due difetti rendono la feature non funzionante o pericolosa
proprio nei due percorsi principali (potfile e upload).

## 0. Metodo e prove non distruttive

1. Lettura del codice finale e confronto con la spec e la review QA.
2. `GET https://wpa-sec.stanev.org/?api&dl=1&key=<32 zeri>` (chiave non valida,
   nessun segreto): HTTP 200, `Content-Type: application/octet-stream`,
   `Transfer-Encoding: chunked`, 115 righe decodificate, **0 righe che iniziano
   con `WPA*`**, 3 `:` per riga. Il formato reale è
   `<bssid>:<station_mac>:<ssid>:<password>`, lo stesso documentato dal plugin
   wpa-sec di pwnagotchi ("Output-Format: bssid, station_mac, ssid, password").
3. `POST https://wpa-sec.stanev.org/?submit&key=<32 zeri>` con multipart **senza
   parte file** (nessun dato di cattura): HTTP 200,
   `Content-Type: text/html; charset=UTF-8`, chunked, 2385 byte. È la pagina
   HTML del form, non un esito di upload.
4. Nessuna build: la dimensione firmware dichiarata nel REPORT non è stata
   riverificata.

## 1. Sintesi

| ID | Sev | Area | Esito |
|---|---|---|---|
| P0-1 | P0 | Potfile / matching | Sempre scartato: il formato reale non è `WPA*`; il matching non può funzionare |
| P0-2 | P0 | Upload / errori HTTP | Un 200 HTML o un corpo >480 B viene classificato "File inviato" e marcato `UP` |
| P1-1 | P1 | Potfile / atomicità | La copia tronca il potfile precedente; un corpo vuoto lo azzera |
| P1-2 | P1 | CA / fallback TLS | Qualsiasi errore di rete o NTP spinge l'utente su `setInsecure()` |
| P1-3 | P1 | Cleanup WiFi | Connessione fallita/annullata: la radio resta accesa e non viene spenta |
| P1-4 | P1 | Timeout / UI | Nessuna scadenza assoluta e ESC non raccolto in `sendAll`/header/body |
| P1-5 | P1 | Chiave | Il servizio restituisce un potfile anche con chiave non valida: nessuna verifica della chiave |
| P2-1 | P2 | Potfile / limiti | Il limite di scansione 256 KB è aggirabile con una riga unica enorme |
| P2-2 | P2 | Stato upload | Solo `nome+dimensione`; file modificato con stessa dimensione resta `UP` |
| P2-3 | P2 | Upload | Nessun batch: un file per conferma; U5 non coperto |
| P2-4 | P2 | Multipart | `safeFilename` non rimuove i caratteri di controllo diversi da CR/LF |
| P2-5 | P2 | Conferma | La dimensione mostrata è quella dell'elenco, non quella riaperta al momento dell'invio |
| P2-6 | P2 | Segreti | Redazione della chiave case-sensitive: un eco in maiuscolo non viene oscurato |
| P2-7 | P2 | Storage | Con SD assente la chiave finisce su LittleFS in silenzio; il REPORT dice SD |
| P2-8 | P2 | Chiave / residui | `Cancella chiave` non rimuove `.tmp`/`.bak` lasciati dalle scritture atomiche |
| P2-9 | P2 | Registro upload | Cap 200 voci e voci obsolete conteggiate; stato non fedele |
| P2-10 | P2 | HTTP | Massimo 48 header; un set più grande diventa "Risposta HTTP non valida" |
| P2-11 | P2 | Potfile / validazione | Si valida solo il primo blocco di 2048 B; preamboli lunghi o coda spazzatura |
| P2-12 | P2 | Heap gate / TLS | Soglie nominali non misurate; gate prima di NTP; OOM di connect = fallback insicuro |
| P2-13 | P2 | Redirect | `//host/path` accettato come path; il REPORT dice 3 redirect, il codice ne segue 2 |
| P2-14 | P2 | Stato/Info | "Password: N" conta le voci del potfile, non le catture abbinate |

## 2. Findings P0

### P0-1 — Il potfile reale non è nel formato atteso: download sempre scartato e password mai abbinate

**File**: `src/modules/wpasec/WpaSecMenu.cpp:477-495` (`potfileLooksValid`),
`:385-425` (`CrackIndex::build`), `:445-475` (`potfilePassword`),
`:583-621` (`captureCracked`), `:1517-1521` (reiezione download).
**Severità**: P0 (bloccante per tutta la parte potfile).

`potfileLooksValid` accetta il file solo se almeno una riga inizia con `WPA*`
(`line.rfind("WPA*", 0) == 0`, riga 486). Il servizio reale restituisce invece
righe a 4 campi separati da `:`:

```text
<bssid>:<station_mac>:<ssid>:<password>
```

Prova: `GET /?api&dl=1&key=<32 zeri>` → HTTP 200, `application/octet-stream`,
115 righe, 0 righe `WPA*`, 3 `:` per riga. Quindi `downloadPotfile` esegue
`storage::removeFile(kPotfileTmpPath)` e mostra "Risposta non valida: chiave o
servizio." (`:1517-1521`): **il potfile non viene mai salvato**, con qualsiasi
chiave. La schermata "Stato" continuerà a dire "Potfile: assente".

Anche ipotizzando di accettare il formato, il matching è sbagliato:

- `CrackIndex::build` prende `colon = line.find(':')` (`:405`) e indicizza il
  **BSSID** (primo campo), non l'hash completo;
- `captureCracked` calcola `hashPrefix(trimmed, trimmed.size())` sulla **riga
  hc22000 completa** (`WPA*02*...`, `:608`): non potrà mai coincidere con il
  BSSID indicizzato;
- `potfilePassword` confronta la riga completa con il prefisso prima del primo
  `:` e restituisce `line.substr(colon+1)` (`:463-465`), cioè
  `station_mac:ssid:password`, non la password.

Risultato: il flag `PW` non compare mai; il dettaglio mostra "Password: non
trovata"; la feature di download/abbinamento è al 100% non funzionante. In più
il REPORT (righe 26-28 e 92-94) dichiara un formato `WPA*...:password` che non è
quello del servizio, quindi la documentazione conferma l'errore invece di
segnalarlo.

**Fix**:
1. Accettare il formato reale: almeno una riga con primo e secondo campo di 12
   hex e almeno 3 `:`; mantenere `WPA*...:password` solo come compatibilità.
2. Indicizzare per **BSSID** (primo campo, lowercase, senza separatori); per il
   matching estrarre dal file hc22000 il campo 3 (0-based) della riga
   `WPA*01*...`/`WPA*02*...`, cioè `MAC_AP`, e confrontarlo con il BSSID.
   Per un `.pcap` usare il companion `.hc22000` (già fatto).
3. Per la password usare l'**ultimo** campo (o `rfind(':')`), non
   `substr(primo ':' + 1)`, e conservare password con `:` per quanto possibile.
4. Validare un campione più ampio (es. 8 KB) e richiedere almeno una riga
   completa valida; un file vuoto non è una prova di successo.
5. Aggiornare REPORT e fixture QA: i test P2/P3 con `WPA*...:password` passano
   su un fixture sintetico ma non sul servizio reale.

### P0-2 — Upload: un 200 HTML (o un corpo >480 B) viene classificato come successo e marcato `UP`

**File**: `src/modules/wpasec/WpaSecMenu.cpp:1317-1328` (lettura risposta e
`result.ok`), `:862-864` e `:882-886` (rifiuto del corpo oltre `maxBytes`),
`:911-922` (`readSnippet`), `:1761` (`markUploaded`).
**Severità**: P0 (falso successo, stato corrotto).

`uploadFile` legge il corpo con `readSnippet(client, head, 480)` e **ignora il
valore di ritorno**. In `readBodyDecoded`:

- se `Content-Length > maxBytes` restituisce `false` **prima di emettere un solo
  byte** (`:882-886`);
- se il primo chunk è più grande di `maxBytes` fa lo stesso (`:862-864`).

`readSnippet` restituisce quindi una stringa vuota; `result.ok` viene impostato
solo dallo status 200/201/202 (`:1320`); i marcatori `invalid key`,
`not authorized`, `error:` non possono essere trovati in una stringa vuota
(`:1321-1327`). Il modulo mostra `>File inviato.` (`:1750`) e chiama
`markUploaded` (`:1761`), quindi il file compare `UP` nell'elenco.

Prova: un POST reale al servizio con multipart **senza parte file** (nessun dato
di cattura) restituisce HTTP 200, `Content-Type: text/html; charset=UTF-8`,
chunked, primo chunk 2373 B (2385 B sul filo): 2373 > 480. Il codice vedrebbe
corpo vuoto, status 200 e marcerebbe "inviato". Lo stesso vale per una chiave
non valida o per un rifiuto del server che restituisce la pagina HTML del form:
il modulo non controlla mai `Content-Type` sull'upload e non richiede un
marcatore positivo di successo. `uploaded.txt` viene quindi avvelenato e
l'utente non riprova.

**Fix**:
1. Trattare `Content-Type: text/html` come **non successo** sull'upload (la
   pagina del form è HTML).
2. Non interrompere la lettura del corpo solo perché è grande: leggere un
   prefisso limitato (es. 1024 B), poi fermarsi e restituire quel prefisso.
   Modificare `readBodyDecoded` perché emetta i byte disponibili e ritorni
   `true`/`partial` invece di `false` senza output.
3. Marcare `result.ok` solo con un esito esplicito: corpo non HTML e token di
   successo (`uploaded`, `already submitted`, `ok`) oppure una risposta vuota
   **non HTML** con 2xx; in tutti gli altri casi mostrare il corpo ripulito e
   non chiamare `markUploaded`.
4. Aggiungere un test con un 200 HTML di 2 KB e con un 200 vuoto: entrambi non
   devono produrre `UP`.

## 3. Findings P1

### P1-1 — Sostituzione del potfile non atomica: la copia tronca il file precedente e un corpo vuoto lo azzera

**File**: `src/modules/wpasec/WpaSecMenu.cpp:1353-1383` (`copyFile`),
`:1454-1458` (limite), `:1501-1528` (download/copia), `:477-480` (vuoto
accettato).
**Severità**: P1 (perdita del potfile precedente; T4/P1/P5).

`copyFile` apre la destinazione con `storage::openWrite(to, sd)` (`:1358`):
`FILE_WRITE` tronca subito `potfile.txt`. Se la lettura del tmp o la scrittura
falliscono a metà, il vecchio potfile è già stato distrutto e resta un file
parziale. `out.flush()` e `out.close()` non vengono controllati (`:1380-1381`),
quindi un errore di flush (SD piena) può essere ignorato e la funzione può
restituire `ok=true`. Inoltre `potfileLooksValid("")` restituisce `true`
(`:477-480`): un 200 con `Content-Length: 0` passa la validazione, `copyFile`
copia zero byte e il vecchio potfile viene **azzerato**, con messaggio
">Potfile salvato. Dimensione: 0B".

È lo stesso rischio dichiarato nel REPORT ("se la copia finale fallisce a metà,
il file di destinazione può restare parziale"), ma è un requisito di
accettazione (P1/T4/P5) e va corretto, non solo documentato.

**Fix**:
1. Scaricare in `potfile.new` (già si usa `potfile.tmp`), verificare
   `out.getWriteError()==0` dopo `flush`/`close`, poi **rinominare** il nuovo
   file sopra `potfile.txt` (rename atomico). Non aprire mai la destinazione
   con `FILE_WRITE` prima di avere il nuovo file completo.
2. Se il rename non è disponibile in `storage`, aggiungere un helper
   `storage::finalize(tmp, final)`; in alternativa usare `SD.rename`.
3. Rifiutare il corpo vuoto, a meno che l'utente non confermi esplicitamente
   "il servizio non ha risultati"; in caso di dubbio conservare il vecchio
   potfile e mostrare "nessun dato".
4. Controllare anche `in.getReadError()`/`got<0` e la dimensione finale.

### P1-2 — Qualsiasi errore di connessione o NTP porta al fallback `setInsecure()`

**File**: `src/modules/wpasec/WpaSecMenu.cpp:954-994` (`openTls`),
`:959-967` (CA fallita → `g_caFailed`), `:969-986` (conferma),
`:1096-1127` (`ensureClock`, `g_caFailed` a `:1126`).
**Severità**: P1 (downgrade TLS; la chiave API viaggia nel URL).

`openTls` interpreta **qualsiasi** fallimento di `client.connect(kHost, 443)`
come "CA non valida": DNS, TCP, timeout, server irraggiungibile o OOM durante
l'handshake producono `g_caFailed = true` (`:965`) e portano alla conferma per
`setInsecure()` (`:969-986`). Da quel momento `g_caFailed` resta vero per tutta
la sessione: i tentativi successivi **non riprovano la CA**, anche se la rete è
tornata. Anche un fallimento NTP imposta `g_caFailed = true` (`:1126`) e salta
del tutto la verifica del certificato. L'utente vede un avviso onesto, ma
indotto da un errore non certificato; se accetta, `g_insecureAck` vale per tutta
la sessione e la chiave può essere letta da un MITM.

Il log `logger::warn("verifica CA fallita...")` (`:966`) non riporta il motivo,
quindi in campo non è distinguibile un certificato scaduto da un DNS rotto.
Anche un OOM di `connect` (malgrado il gate) viene presentato come problema di
CA, non come heap insufficiente.

**Fix**:
1. Classificare l'errore: fallback solo per errori di **verifica
   certificato/time** o su richiesta esplicita; per DNS/TCP/timeout mostrare
   "rete non raggiungibile" e non toccare `g_caFailed`.
2. Non impostare `g_caFailed` in modo permanente; riprovare la CA al tentativo
   successivo (o dopo una riconnessione) e conservare un flag separato per
   "utente ha accettato insecure in questa sessione".
3. Se `connect` fallisce subito dopo il gate, ricontrollare l'heap e mostrare
   "Heap insufficiente" invece della conferma TLS.
4. Registrare il codice/errore mbedTLS (senza chiave) per la diagnosi.

### P1-3 — Cleanup WiFi: con connessione fallita o annullata la radio resta accesa

**File**: `src/modules/wpasec/WpaSecMenu.cpp:1990` (`wasConnected`),
`:2005-2009` (disconnessione condizionata); `src/utils/WifiSetup.cpp:389-397`
(timeout/annullamento di `connect`), `:420-422` (annullamento scelta rete),
`:191-272` (`scanNetworks` lascia `WIFI_STA`), `:484-487`
(`disconnect` = `WIFI_OFF`).
**Severità**: P1 (consumo batteria, interferenze, I5/BRIEF).

`runMenu` spegne la radio solo se **non era connessa all'ingresso** e **al
momento dell'uscita è connessa**:

```cpp
if (!wasConnected && WiFi.status() == WL_CONNECTED) {
    wifisetup::disconnect();
}
```

Se `ensureConnected` fallisce (timeout, password errata, nessuna rete) o
l'utente annulla la scelta della rete, `WiFi.status()` non è `WL_CONNECTED`:
la condizione è falsa e non viene chiamato `wifisetup::disconnect()`. Però
`WifiSetup::connect` in timeout/annullamento chiama solo
`WiFi.disconnect(true)` (`:390`, `:396`), non `WiFi.mode(WIFI_OFF)`;
`scanNetworks` ha già impostato `WiFi.mode(WIFI_STA)` (`:194`) e
`scanAndConnect` esce su `chooseNetwork < 0` (`:420-422`) senza spegnere.
Risultato: STA/radio accesa dopo il ritorno al menu, fino a un altro modulo o
al riavvio. Se la WiFi era già connessa all'ingresso, il modulo la lascia
comunque accesa all'uscita, mentre il BRIEF richiede di liberare sempre
WiFi/BLE/TLS/I2S.

**Fix**:
1. Tracciare se il modulo ha **acceso o usato** la radio (`wifiStarted`) e
   chiamare sempre `wifisetup::disconnect()` all'uscita in uno scope guard,
   indipendentemente dallo stato corrente.
2. Se si vuole rispettare una connessione preesistente, salvarla e ripristinarla
   esplicitamente; in ogni caso non lasciare la STA accesa quando il modulo
   l'ha avviata.
3. Correggere `WifiSetup::connect` (timeout/annullamento) e
   `scanAndConnect` (annullamento) perché facciano `WiFi.mode(WIFI_OFF)` quando
   la radio è stata avviata da quella chiamata.
4. `WiFi.setSleep(false)` impostato da `connect` (`WifiSetup.cpp:380`) non
   viene mai ripristinato: documentarlo o ripristinarlo all'uscita.

### P1-4 — Nessuna scadenza assoluta e ESC non raccolto durante invio/lettura

**File**: `src/modules/wpasec/WpaSecMenu.cpp:691-709` (`NetReader::readByte`),
`:924-944` (`sendAll`), `:779-819` (`readResponseHead`), `:911-922`
(`readSnippet`), `:1253-1279` (loop upload).
**Severità**: P1 (UI bloccata, T3/U6/I6).

`sendAll` ha solo un timeout di stallo di 15 s **per scrittura** e resetta il
contatore a ogni byte scritto; un server che legge lentamente può tenere
l'upload aperto indefinitamente. `readResponseHead` legge fino a 48 header con
`NetReader` (timeout 20 s) senza chiamare `navigation::poll()`;
`readSnippet` ha un timeout di 8 s e non raccoglie ESC; `readByte` usa
`delay(2)` e non polla la tastiera. Durante l'handshake TLS (fino a 12 s) e la
lettura della risposta l'utente non può annullare; T3 (risposta a singhiozzo) e
U6 (ESC durante l'upload) non sono soddisfatti. Il loop di upload polla ESC solo
tra un chunk da 1 KB e l'altro (`:1266`), quindi va bene per un chunk normale
ma non durante uno stallo di `sendAll`.

**Fix**:
1. Passare una deadline assoluta a `sendAll`, `readResponseHead`,
   `readSnippet` (es. 30-40 s per l'intera operazione, non per read) e
   controllarla in ogni loop.
2. Chiamare `navigation::update()`/`poll()` nei loop di attesa e su `Back`
   chiudere il client, eliminare il tmp e tornare con `cancelled`.
3. Per l'upload, controllare la deadline anche dentro `sendAll` e interrompere
   la richiesta; non lasciare che un server lento blocchi il menu.

### P1-5 — Il servizio restituisce un potfile anche con chiave non valida: nessuna verifica della chiave

**File**: `src/modules/wpasec/WpaSecMenu.cpp:1385-1458`
(`downloadPotfile`), `:1438-1451` (200 + non HTML = successo),
`:1529-1538` (conteggi).
**Severità**: P1/P2 (dipende dal servizio; K5 non verificabile).

La prova con 32 zeri ha restituito HTTP 200 e un potfile valido (115 righe):
il servizio non rifiuta la chiave sul download. Il modulo considera successo
qualsiasi 200 non-HTML con almeno una riga valida (`:1438-1451`) e mostrerà
">Potfile salvato" anche con una chiave errata/revocata. Dopo il fix del
formato (P0-1), l'utente potrebbe credere che la chiave sia valida e vedere
password di reti che non ha caricato. Il REPORT/QA K5 presuppone un
comportamento del servizio che non è quello osservato.

**Fix**:
1. Non usare il download del potfile come prova di validità della chiave;
   validare la chiave con una richiesta autenticata o con l'esito dell'upload.
2. Documentare che il potfile del servizio è globale/pubblico (se confermato) e
   che l'abbinamento per BSSID può mostrare reti non caricate dall'utente.
3. In alternativa, mostrare un avviso esplicito nel flusso "Scarica potfile".

## 4. Findings P2

### P2-1 — Il limite di scansione del potfile è aggirabile con una riga unica

**File**: `WpaSecMenu.cpp:324-362` (`LineReader`), `:400-418`
(`scanned`), `:460-472` (`potfilePassword`).

`LineReader::next` tronca `out` a `maxLen` ma continua a consumare l'intero
file fino a `\n`; `scanned += line.size() + 1` conta la lunghezza **troncata**,
non i byte consumati. Una singola riga da 4 MB fa avanzare `scanned` di ~513
byte per riga, quindi il limite di 256 KB non scatta e il modulo legge l'intero
potfile (fino a 4 MB) più volte. Non è OOM, ma è tempo CPU/SD e può bloccare la
UI; il limite dichiarato non è rispettato.

**Fix**: far restituire a `LineReader` il numero di byte consumati (o esporre un
contatore) e sommare quello; interrompere appena il budget è superato.

### P2-2 — Stato upload solo `nome+dimensione`

**File**: `WpaSecMenu.cpp:501-529` (`isUploaded`), `:531-569`
(`markUploaded`), `:563-566` (formato), `:1761`.

Il registro è `nome|dimensione`: una cattura diversa con lo stesso nome e la
stessa dimensione resta `UP`. Il REPORT lo dichiara (righe 131-132), quindi non
è un falso negativo silenzioso: la conferma mostra "Gia' inviato in
precedenza." e l'utente può comunque procedere. Resta però uno stato non
fedele (F2) e un rischio di saltare un file modificato se in futuro si
introducesse un salto automatico.

**Fix**: aggiungere mtime o un'impronta leggera (dimensione + primi/ultimi 4 KB
+ nome) o richiedere conferma esplicita per il re-upload quando il file è
`UP`; non saltare mai automaticamente.

### P2-3 — Nessun upload batch

**File**: `WpaSecMenu.cpp:1593-1646` (`pickCapture`), `:1700-1779`
(`uploadFlow`).

Il flusso carica **un** file per conferma. La review U5 prevede la selezione di
più file e il caricamento in sequenza; il REPORT dice "una cattura alla volta".
Non è un bug di sicurezza (la conferma per file è corretta), ma è una
deviazione funzionale e un costo d'uso per chi ha molte catture.

**Fix**: multi-selezione con coda, conferma che mostra tutti i nomi/dimensioni,
stato indipendente per file e interruzione che non perde i successi già
completati.

### P2-4 — `safeFilename` non rimuove i caratteri di controllo

**File**: `WpaSecMenu.cpp:1043-1057`.

Sostituisce solo `"`, `\r`, `\n` e `\`; gli altri caratteri di controllo
(0x01-0x08, 0x0B, 0x0C, 0x0E-0x1F, 0x7F) passano nel nome del file dentro
l'header multipart. U2 chiede che i caratteri di controllo siano rimossi o
rifiutati. Non c'è una CRLF injection diretta (CR/LF sono coperti), ma il
server può rifiutare il multipart o interpretare male il nome.

**Fix**: sostituire ogni byte `c < 0x20 || c == 0x7F` con `_`, limitare a 96
byte, e opzionalmente percent-encodare i non-ASCII; mantenere il basename.

### P2-5 — La conferma mostra una dimensione che può differire da quella inviata

**File**: `WpaSecMenu.cpp:1725-1726` (dimensione da `item.size`), `:1176`
(`file.size()` riaperto), `:1220` (`Content-Length`).

`item.size` viene dalla directory listing al momento di `loadCaptures`; il
corpo usa `file.size()` del file riaperto in `uploadFile`. Se il file cambia
tra l'elenco e l'invio (SD/USB), l'utente conferma una dimensione e ne invia
un'altra. La conferma S5/U5 non è quindi vincolante.

**Fix**: passare `item.size` a `uploadFile` e abortire con un messaggio se
`file.size() != item.size`; oppure rileggere la dimensione prima della conferma
e usare quella per il `Content-Length`.

### P2-6 — Redazione della chiave sensibile al case

**File**: `WpaSecMenu.cpp:225-236` (`redactKey`), `:274`, `:1319`, `:1441`.

`redactKey` cerca la stringa esatta (la chiave è normalizzata in minuscolo).
Se il server riflette la chiave in maiuscolo o in una forma URL-encoded, la
chiave resta visibile nella UI e nel messaggio di errore. La chiave in chiaro
è già nel URL per requisito; la redazione deve essere robusta.

**Fix**: redigere qualunque sequenza di 32 hex (case-insensitive) e le varianti
URL-encoded, non solo la stringa esatta; meglio ancora, non mostrare mai il
corpo grezzo ma solo un messaggio classificato.

### P2-7 — Chiave salvata su LittleFS in silenzio se la SD è assente

**File**: `WpaSecMenu.cpp:285-287` (`useSD`), `:309`, `:317`, `:1851-1858`;
`Storage.cpp:321-331`.

Con SD assente `useSD()` è false e `writeText("/wpasec/key.txt", ..., false)`
scrive su LittleFS. Il REPORT dice `/wpasec/key.txt` sulla SD; K4 chiede che il
modulo lo dica o dichiari il fallback. `setKeyFlow` mostra "Chiave API salvata"
senza indicare il backend, quindi l'utente crede che sia sulla SD.

**Fix**: richiedere la SD per i path `/wpasec` (e mostrare "Inserisci la SD"),
oppure dichiarare il fallback LittleFS nel REPORT e mostrare il backend in
`Stato`/al salvataggio. Non mescolare chiavi su due filesystem senza dirlo.

### P2-8 — `Cancella chiave` non rimuove `.tmp`/`.bak`

**File**: `WpaSecMenu.cpp:1861-1875`; `Storage.cpp:280-319`.

`clearKeyFlow` rimuove solo `key.txt`. Se una scrittura atomica precedente è
stata interrotta, possono restare `key.txt.tmp` o `key.txt.bak` con la chiave
in chiaro; K3 chiede che dopo la cancellazione non restino residui. La
cancellazione non è completa.

**Fix**: dopo `removeFile(key.txt)`, rimuovere anche `key.txt.tmp` e
`key.txt.bak`; verificare con `exists`.

### P2-9 — Registro upload: cap 200 e voci obsolete

**File**: `WpaSecMenu.cpp:531-569` (cap `kMaxUploadedEntries`),
`:1901-1920` (conteggio stato), `:511-529`.

`markUploaded` si ferma dopo 200 righe mantenute e riscrive solo quelle: con un
registro più grande perde silenziosamente le voci oltre il limite. `statusFlow`
conta tutte le righe parseabili, anche se il file non esiste più; il numero
"Inviati" può essere maggiore dei file presenti. F3/F4 chiedono uno stato
coerente.

**Fix**: compattare il registro (deduplica, prune dei file assenti), alzare o
eliminare il cap, e conteggiare solo le voci con file esistente; mantenere la
scrittura atomica già presente.

### P2-10 — Massimo 48 header HTTP

**File**: `WpaSecMenu.cpp:793-818`.

`readResponseHead` legge al massimo 48 righe di header; con un set più grande
(Cloudflare, proxy, cookie) restituisce "Risposta HTTP non valida" anche se la
risposta è valida. U3/T1 chiedono una gestione robusta.

**Fix**: alzare il limite (es. 96) o usare un budget di byte (es. 8 KB) e
ignorare gli header sconosciuti; non fallire solo per il numero di righe.

### P2-11 — Validazione potfile solo sui primi 2048 byte

**File**: `WpaSecMenu.cpp:1469-1478` (`sample`), `:477-495`
(`potfileLooksValid`).

`sample` è limitato a 2048 byte. Un potfile valido con un preambolo lungo
(commenti/licenza) viene rifiutato; viceversa una prima riga valida seguita da
spazzatura/HTML viene accettata. La validazione non è rappresentativa.

**Fix**: validare un campione più grande (8-16 KB) e richiedere un numero
minimo di righe valide o una struttura completa; per il formato reale
verificare i 4 campi e la lunghezza dei primi due.

### P2-12 — Gate heap: soglie nominali non misurate, gate prima di NTP, OOM = fallback insicuro

**File**: `WpaSecMenu.cpp:49-50` (40 KB/32 KB), `:1139-1152` (gate),
`:1152` (`ensureClock` dopo il gate), `:959-967` (connect fallito → CA).

Il gate rispetta la soglia della spec (40 KB liberi + 32 KB di blocco
contiguo), ma il REPORT non riporta il picco reale durante handshake/upload e
il gate viene misurato **prima** di `ensureClock`, che alloca socket/DNS. Se
l'allocazione TLS fallisce nonostante il gate, `openTls` la interpreta come
errore di CA e propone `setInsecure()` (P1-2). H3/H4 chiedono una misura, non
solo la soglia.

**Fix**: misurare il picco con framebuffer e BLE attivi; ricontrollare l'heap
subito prima di `setCACert`/`connect` (dopo NTP); alzare la soglia se il picco
la supera; distinguere OOM da errore di certificato e mostrare "Heap
insufficiente".

### P2-13 — Redirect: `//host` accettato come path e conteggio massimo diverso dal REPORT

**File**: `WpaSecMenu.cpp:997-1029` (`redirectPath`), `:1187`, `:1391`,
`:1334`, `:1541`.

`redirectPath` accetta qualsiasi `Location` che inizia con `/`, compreso
`//evil.example/path`. Il client resta su `kHost` (non c'è leak della chiave
né SSRF), ma il path viene inviato al server nella forma `//evil.example/...`;
è meglio rifiutarlo. Inoltre il ciclo `for (hop = 0; hop < 3; ...)` segue al
massimo 2 redirect, mentre il REPORT dice 3.

**Fix**: rifiutare `loc.rfind("//", 0) == 0`; allineare il limite a 3 o
correggere il REPORT.

### P2-14 — "Password: N" nello stato conta le voci del potfile, non le catture abbinate

**File**: `WpaSecMenu.cpp:1922-1934`.

`index.hashes.size()` è il numero di hash/BSSID indicizzati (fino a 2048), non
il numero di catture con password trovata. L'etichetta "Password" è ambigua:
con il potfile reale mostrerebbe 115 anche senza alcuna cattura corrispondente.

**Fix**: mostrare separatamente "Voci potfile: N" e "Catture con password: M"
(calcolato da `loadCaptures`/`captureCracked`).

## 5. Cosa è corretto (per non perdere i positivi)

- **Multipart/Content-Length**: il corpo è `prefix + file + suffix` con CRLF
  corretti (`:1215-1220`, `:1229-1230`), il file è in streaming a chunk da 1 KB
  (`:1251-1265`) e il boundary è casuale (`:1035-1041`). Il calcolo è corretto
  per un file non modificato durante l'invio. Manca solo la verifica con proxy
  e la gestione dei caratteri di controllo nel nome (P2-4).
- **Redirect**: solo path relativi o HTTPS same-host, porta 443, nessun
  downgrade a HTTP; la chiave non va su host diversi (`:997-1029`). Il caso
  `//host` è P2-13, non un leak.
- **Chiave**: input mascherato (`textinput::Field.secret`), validazione 32 hex
  (`:293-306`), salvataggio atomico (`Storage::writeText`), nessun log della
  chiave, risposta redatta (`:225-236`), cancellazione con conferma
  (`:1861-1875`). Restano P2-6/P2-7/P2-8.
- **TLS**: CA pinnata GTS Root R4, fallback insicuro solo dopo conferma
  esplicita, footer "TLS NON VERIFICATO", stato che lo dichiara. Il problema è
  **quando** scatta il fallback (P1-2), non la presenza dell'avviso.
- **Heap gate**: valori mostrati all'utente e blocco prima di aprire TLS
  (`:1139-1151`). Manca la misura del picco (P2-12).
- **Conferma upload**: `ui::confirm`, default ESC = annulla (`Ui.cpp:108-132`),
  nome, dimensione, host e avviso "servizio esterno"; nessun upload automatico.
  Restano P2-3/P2-5 e la formulazione "pubblico" (sotto).
- **Stato upload**: `markUploaded` usa `storage::writeText` atomico
  (`Storage.cpp:321-331`), quindi la scrittura del registro non è troncata a
  metà. Restano il formato nome+dimensione e il cap (P2-2/P2-9).

## 6. Top 5 (ordine di gravità)

1. **P0-1 — Potfile nel formato sbagliato**: il servizio restituisce
   `bssid:station_mac:ssid:password`; il codice pretende `WPA*` e scarta tutto.
   Il download non salva mai il potfile e `PW` non compare mai. Correggere
   validazione, indice per BSSID e parsing della password. Aggiornare REPORT e
   fixture QA.
2. **P0-2 — Falso successo upload**: un 200 HTML (o un corpo >480 B) non viene
   letto, quindi `result.ok` resta true e il file viene marcato `UP`. Il
   servizio reale restituisce 200 HTML 2385 B a un POST senza file. Controllare
   `Content-Type`, leggere un prefisso del corpo e richiedere un esito esplicito
   prima di `markUploaded`.
3. **P1-1 — Potfile precedente distrutto**: `copyFile` tronca la destinazione
   prima di copiare e non controlla flush/close; un 200 vuoto azzera il file.
   Scrivere su `.new`, verificare e rinominare in modo atomico; rifiutare il
   corpo vuoto.
4. **P1-2 — Fallback TLS su qualsiasi errore**: DNS/TCP/timeout/NTP portano a
   `setInsecure()` e la CA non viene più ritentata. Classificare gli errori,
   riprovare la CA e non degradare per un errore di rete.
5. **P1-3 — Radio WiFi non spenta**: se la connessione fallisce o viene
   annullata, `WiFi.status()` non è `WL_CONNECTED` e `runMenu` non chiama
   `disconnect()`; la STA resta accesa. Tracciare l'avvio della radio e
   spegnerla sempre all'uscita (o correggere `WifiSetup`).

## 7. Test mancanti / rischi residui

- Nessuna build eseguita: la dimensione del REPORT (2.412.432 B) non è
  riverificata. I test con chiave reale e cattura di laboratorio restano
  bloccati.
- La prova live ha usato una chiave sintetica non valida; il servizio ha
  comunque restituito un potfile. Non sono state salvate né mostrate password
  reali. Va ripetuta con una chiave di test dedicata e una cattura propria, su
  rete di laboratorio.
- La catena TLS live è `stanev.org` ← WE1 ← GTS Root R4 (cross-signed da
  GlobalSign); OpenSSL accetta la CA pinnata self-signed, mbedTLS non è stato
  provato su hardware. Verificare H1/H2 con proxy e certificato di test.
- Il multipart e il `Content-Length` sono corretti per ispezione, ma vanno
  verificati dietro proxy (SHA-256 del file = SHA-256 della parte) e con un
  file da 2 MB.
- Il servizio restituisce un potfile popolato anche con chiave non valida (115
  righe): chiarire nel REPORT se l'abbinamento può mostrare reti non caricate
  dall'utente.
- La review QA usa fixture `WPA*...:password` e `P-base` con `hash:password`:
  passerebbero su un formato che il servizio non usa. Aggiornare le fixture al
  formato reale prima di dichiarare P1-P5 superati.
