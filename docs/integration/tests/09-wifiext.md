# 09 — WiFi+ (Responder, Karma, Auto-Portal): bug test sul codice integrato

Stato: **NON PASS** (presenti P0). Analisi statica del codice finale integrato,
commit `f19b6d0` (`merge: Piano B partizioni e flasher`), working tree pulito.
Nessuna modifica al codice, nessuna build, nessun test su hardware: le prove
radio/hash/SD restano da eseguire in laboratorio secondo
`orbit-wt-qa/docs/integration/reviews/09-wifiext.md`.

File esaminati:

- `src/modules/wifiext/Responder.cpp`
- `src/modules/wifiext/Karma.cpp`
- `src/modules/wifiext/AutoPortal.cpp`
- `src/modules/wifiext/WifiExtCommon.cpp`
- `src/modules/wifiext/WifiExtMenu.cpp`
- `src/modules/wifiext/REPORT.md`
- `src/utils/WifiSetup.cpp` (solo i percorsi usati dal modulo)
- `src/utils/Storage.cpp` (solo i percorsi usati dal modulo)

## Top 5

| # | Sev | Finding | File:linea | Perché è grave |
|---|-----|---------|------------|----------------|
| 1 | **P0** | Offset del security buffer SMB2 SESSION_SETUP sbagliati: Type1 mai risposto, nessun hash catturabile | `Responder.cpp:470-478` | La feature Responder non produce mai un Type2/Type3: `/wifiext/ntlm.txt` resta vuoto. H1–H5 falliscono. |
| 2 | **P0** | Overflow di stack nella risposta LLMNR (e mDNS) con nome DNS lungo | `Responder.cpp:158-186`, `Responder.cpp:257-283` | Un host della LAN può inviare una query LLMNR con nome lungo e sovrascrivere fino a ~638 byte oltre `out[640]`: crash, corruzione, possibile controllo di flusso. |
| 3 | **P1** | Stop Auto-Portal non sincronizzato con AsyncTCP: uso dopo free di `g_html`/`server` | `AutoPortal.cpp:295-303` | ESC con richiesta HTTP in volo può far girare un handler su oggetti già distrutti: crash/reset. È il caso S4 indicato dalla review QA. |
| 4 | **P1** | Wrap a 32 bit degli offset del Type3: OOB read e crash con Type3 craftato | `Responder.cpp:407-417` | Un client SMB ostile/curioso può mandare `domainOff=0xFFFFFFF0` e far leggere ~4 GB oltre `g_smbPayload`: load fault/reset, possibile leak nel file hash. |
| 5 | **P1** | NTLMv1 da 24 byte non viene scartato: riga 5600 malformata | `Responder.cpp:406-411` | Contraddice il REPORT e H6: un client NTLMv1 finisce in `ntlm.txt` come finto NetNTLMv2, inquinando il file e ingannando l'operatore. |

Altri P2 rilevanti, sotto: radio STA lasciata accesa dopo connessione
fallita/annullata, CSV che perde `;`, coda credenziali 6→5 con drop non
mostrato, limite body HTTP, mDNS QU, append parziale su SD, fallback silenziosi.

## Findings

### F-01 — P0 — SMB2 SESSION_SETUP: offset security buffer sbagliati

**File:** `src/modules/wifiext/Responder.cpp:470-478` (lettura a `:473-474`,
guardia a `:475-478`).

**Cosa succede.** Il codice legge `SecurityBufferOffset` e
`SecurityBufferLength` da `message + 72` e `message + 74`. Nella richiesta
SMB2 SESSION_SETUP (MS-SMB2 2.2.5), dopo l'header da 64 byte, la parte fissa è:

| Offset | Campo |
|--------|-------|
| 64 | StructureSize (25) |
| 66 | Flags |
| 67 | SecurityMode |
| 68 | Capabilities |
| 72 | Channel (riservato, 0) |
| 76 | SecurityBufferOffset |
| 78 | SecurityBufferLength |
| 80 | PreviousSessionId (8) |
| 88 | Buffer |

Quindi `securityOffset` viene letto dal campo `Channel` (0) e
`securityLength` dai byte alti di `Channel` (0). La guardia
`securityOffset < 64 || securityLength < 12` scatta sempre e la funzione
ritorna **senza rispondere**. Il client non riceve mai il Type2, non manda mai
il Type3 e `captureType3()` non viene mai chiamata.

Gli offset 64/66/68/70 e buffer a 72 sono corretti **per la risposta**
SESSION_SETUP (2.2.6), non per la richiesta: è quasi certamente una confusione
tra i due layout.

**Impatto.** La funzione principale del Responder non funziona: nessun hash
NTLMv2 catturato, `/wifiext/ntlm.txt` non cresce, H1–H5 non superabili. La UI
può sembrare viva (contatori UDP salgono) ma “Hash” resta 0. Il REPORT dichiara
la cattura come implementata e testata solo in build, mai su hardware.

**Fix.**

```cpp
// SESSION_SETUP request: fixed part 24 byte, buffer a 88.
const uint16_t securityOffset = le16(message + 76);
const uint16_t securityLength = le16(message + 78);
if (securityOffset < 88 || securityLength < 12 ||
    static_cast<size_t>(securityOffset) + securityLength > len) {
    return;
}
```

Poi cercare `NTLMSSP\0` da `securityOffset` fino a
`securityOffset + securityLength`. Aggiungere una prova end-to-end:
Windows `net use \\<ip>\share /user:qa.user` o `smbclient` e
`hashcat -m 5600` con password fittizia; senza crack reale il test non è
passato.

---

### F-02 — P0 — Overflow di stack in LLMNR (e mDNS) con nome DNS lungo

**File:** `src/modules/wifiext/Responder.cpp:145-186` (`out` a `:158`,
seconda copia del nome a `:169`); stesso schema in
`Responder.cpp:244-283` (`out` a `:257`, copie a `:264-266`).

**Cosa succede.** `skipDnsName()` limita solo il numero di label (128) e la
lunghezza del pacchetto (640), non la lunghezza del nome. La guardia
`nameEnd + 4 <= n` permette `nameLen = nameEnd - 12` fino a **624 byte**.

Nella risposta LLMNR il buffer è `uint8_t out[640]` e vengono scritti:

- header: 12 byte;
- copia della domanda: `nameLen + 4`;
- nome ripetuto nella answer: `nameLen`;
- type/class/TTL/rdlength/IP: 14 byte.

Totale = `2 * nameLen + 30`, fino a **1278 byte**: overflow di **638 byte**
oltre `out`, sul frame dello stack. Basta una query LLMNR con un nome di ~305
byte per superare i 640 byte del buffer; un host qualsiasi della LAN può
inviarla. Il risultato è corruzione dello stack/crash e, in teoria, controllo
di flusso.

In mDNS il totale è `nameLen + 30` (fino a 654): overflow più piccolo (14
byte) ma della stessa classe.

**Impatto.** Crash/reset del firmware del Cardputer, potenzialmente
sfruttabile, proprio mentre lo strumento è in ascolto su una rete non
fiduciosa. Rilevabile dal fuzz X1 della review.

**Fix.** Rifiutare i nomi non validi prima di costruire la risposta:

```cpp
// DNS name massimo: 255 byte incluso lo zero finale.
if (nameLen == 0 || nameLen > 255) return;
if (2 * nameLen + 30 > sizeof(out)) return;   // LLMNR
if (nameLen + 30 > sizeof(out)) return;       // mDNS
```

In alternativa `out` da 1300 byte **più** il controllo di bound; il solo
buffer più grande non basta. Aggiungere il limite anche in `skipDnsName()` e
testare con scapy nomi 255/305/624 byte, label 63, compression loop e pacchetti
troncati.

---

### F-03 — P1 — NTLMv1 da 24 byte non scartato: riga NetNTLMv2 malformata

**File:** `src/modules/wifiext/Responder.cpp:406-411` (condizione a `:407`).

**Cosa succede.** Il commento dice “sotto le 24 byte è NTLMv1”, ma la
condizione è `ntLen < 24`. Una risposta NTLMv1 ha esattamente **24 byte**:
passa il controllo, `proof` prende i primi 16 byte e `blob` solo gli 8
rimanenti. Il codice scrive una riga `utente::dominio:challenge:proof:blob`
con blob da 16 hex (8 byte) invece dei ≥32 hex richiesti da NetNTLMv2.
`g_skipped` non viene incrementato, quindi la UI non segnala nulla.

**Impatto.** Contraddice `REPORT.md:79-81` (“le risposte da 24 byte vengono
contate come scartati”) e il test H6: un client forzato a NTLMv1 produce una
riga 5600 non valida, hashcat la rifiuta o la ignora, e il file si riempie di
dati ingannevoli. Se in futuro si aggiunge 5500, questa riga è anche nel file
sbagliato.

**Fix.** Rifiutare `ntLen <= 24`; per NetNTLMv2 il minimo reale è 16 + 28 =
44 byte di blob, quindi il controllo robusto è `ntLen < 44` (o almeno
`ntLen <= 24` più `blob >= 28`). Contare in `g_skipped` e mostrare il motivo
(“NTLMv1”) in UI. Se si vuole supportare NTLMv1, salvarlo in
`/wifiext/ntlmv1.txt` con `-m 5500`, separato.

---

### F-04 — P1 — Offset Type3: wrap a 32 bit, OOB read e crash

**File:** `src/modules/wifiext/Responder.cpp:399-417` (controlli a `:407-408`,
usi a `:413-417`).

**Cosa succede.** `ntOff`, `domainOff` e `userOff` sono `uint32_t`, mentre
`ntLen`, `domainLen` e `userLen` sono `uint16_t`. Le somme
`ntOff + ntLen`, `domainOff + domainLen`, `userOff + userLen` vengono fatte in
32 bit e possono overfloware. Esempio: `domainOff = 0xFFFFFFF0`,
`domainLen = 0x20` → `0x10 <= len`, il controllo passa; poi
`utf16Ascii(ntlm + 0xFFFFFFF0, 32)` legge ~4 GB oltre `g_smbPayload`
(1536 byte). Stesso discorso per `ntOff` in `hexBytes()`.

**Impatto.** Un client che apre TCP 445 (la porta è aperta per definizione)
può mandare un Type3 craftato e causare un load fault/reset del Cardputer, o
far finire in `ntlm.txt` byte arbitrari letti fuori dal buffer. La tool è
pensata per reti non fiduciose, quindi il caso è raggiungibile.

**Fix.** Validare ogni campo con `size_t` e senza somme che possano
overfloware:

```cpp
auto inRange = [len](uint32_t off, uint16_t n) {
    return off <= len && n <= len - off;
};
if (!inRange(ntOff, ntLen) || !inRange(domainOff, domainLen) ||
    !inRange(userOff, userLen)) { ++g_skipped; return; }
```

Aggiungere limiti massimi per user/domain (es. 256 byte) e fuzz X2 con Type3
troncato/offset estremi.

---

### F-05 — P1 — Auto-Portal: stop non sincronizzato, UAF su richiesta in volo

**File:** `src/modules/wifiext/AutoPortal.cpp:295-303` (`server.end()` a
`:296`, `dns.stop()` a `:297-299`, `g_html.clear()`/`shrink_to_fit()` a
`:302-303`).

**Cosa succede.** `AsyncWebServer::end()` non è sincrono: chiude il listening
socket ma non attende i callback già in esecuzione nel task AsyncTCP. Subito
dopo il codice:

1. svuota e riduce `g_html` (buffer servito da `handlePortalPage` e
   `onNotFound` con `g_html.c_str()`);
2. esce dalla funzione e distrugge l'oggetto locale `AsyncWebServer server`
   (e i suoi handler).

Se un GET/POST è in volo, il callback può leggere `g_html` liberato o passare
per un handler distrutto: use-after-free, crash/reset. La review QA (§8, S4)
indica proprio questo come il bug più probabile e il REPORT non riporta una
prova con richieste in volo.

Inoltre `drainQueue()` a `:295` può non svuotare la coda se una richiesta
arriva tra `:295` e `:296`: l'ultima credenziale viene persa.

**Impatto.** Crash all'uscita (ESC) se il telefono sta caricando o inviando il
form; possibile corruzione heap e reset. S5 (10 cicli) e S4 falliscono.

**Fix.** Dopo `server.end()`:

- chiudere/aspettare i client per un tempo limitato (es. loop 100–200 ms con
  `delay(5)` e `millis()`), poi fare un ultimo `drainQueue()`;
- non fare `clear()`/`shrink_to_fit()` finché il task async non è fermo;
- meglio, tenere `server` e `g_html` statici e distruggerli solo al prossimo
  avvio, oppure proteggere `g_html` con un reference count/shared ownership;
- testare ESC durante GET, POST, upload lento e 10 cicli aperti/chiusi.

---

### F-06 — P2 — Responder lascia la radio WiFi accesa se la connessione fallisce o viene annullata

**File:** `src/modules/wifiext/Responder.cpp:631-636`;
`src/utils/WifiSetup.cpp:388-398` e `:484-487`.

**Cosa succede.** `runResponder()` chiama `ensureConnected(true, "WiFi+")` e,
se restituisce false, mostra un messaggio e ritorna senza spegnere la radio.
`WifiSetup::connect()` sui percorsi di timeout/ESC fa solo
`WiFi.disconnect(true)`: la modalità resta `WIFI_STA` e la radio resta
alimentata. `wifisetup::disconnect()` (che fa anche `WiFi.mode(WIFI_OFF)`)
esiste ma non viene chiamato su questo percorso.

**Impatto.** Dopo un avvio fallito o un ESC nella schermata di connessione, la
radio resta accesa in STA, può continuare a scandire/associarsi e viola
l'aspettativa A4/S1 (“WiFi resta off” / “nessun TX dopo l'uscita”). Non è un
TX offensivo, ma è un gate/cleanup incompleto.

**Fix.** Chiamare `wifisetup::disconnect()` (o `WiFi.mode(WIFI_OFF)`) su
**ogni** return dopo `ensureConnected`, e correggere i return di
`WifiSetup::connect()` perché spengano la modalità. Aggiungere un test che,
dopo ESC/timeout, verifichi `WiFi.getMode() == WIFI_OFF` e nessuna probe/assoc
dal Cardputer.

---

### F-07 — P2 — CSV: `;` e newline vengono sostituiti, credenziale alterata

**File:** `src/modules/wifiext/AutoPortal.cpp:119-139` (ramo a `:130-136`).

**Cosa succede.** `csvField()` sostituisce con uno spazio `\r`, `\n` **e `;`**.
I campi sono già racchiusi tra virgolette, quindi un `;` può stare dentro le
virgolette senza rompere il CSV. Una password catturata come `Fake;Pass`
finisce su SD come `Fake Pass`: il dato non è più quello digitato.

La protezione contro le formule scatta solo se il **primo** byte è `=`, `+`,
`-` o `@`; un valore che inizia con spazio/tab e poi `=` non viene prefissato.
QA P8/X6 copre `=cmd`, virgolette, newline e virgola, ma non questo caso.

**Impatto.** Perdita silenziosa di credenziali (falso negativo nel crack/test
manuale), e possibile formula CSV con whitespace iniziale in alcuni fogli di
calcolo. Non è injection di righe: il newline viene rimosso, quindi una riga
per richiesta resta garantita.

**Fix.** Conservare `;` dentro le virgolette (normalizzare solo CR/LF o
rifiutarli), prefissare `'` anche dopo whitespace iniziale, e testare
`Fake;Pass`, `qa,user`, `Fake,"Pass`, `=cmd`, `\t=cmd`, newline.

---

### F-08 — P2 — Coda credenziali: capacità reale 5, drop non mostrato in UI

**File:** `src/modules/wifiext/AutoPortal.cpp:63-66` (`kQueueSize = 6`),
`:108-116` (drop), `:176-200` (UI), `REPORT.md:51-52`.

**Cosa succede.** Con anello a 6 slot e condizione `next == g_queueTail`, la
capacità massima è **5** elementi, non 6. Quando è piena, `g_dropped` aumenta
ma la UI mostra solo “Credenziali N in coda M”; `g_dropped` viene solo
stampato nel log alla chiusura. Un burst di login (o un client che invia più
POST) perde credenziali senza avviso.

**Impatto.** Dati persi in silenzio, incoerenza con il REPORT, P6/X5 non
dimostrabili. I contatori `g_requests`, `g_saved`, `g_writeErrors`, `g_dropped`
sono `volatile` ma non atomici e sono incrementati dal task AsyncTCP: possibili
conteggi persi.

**Fix.** Mostrare anche `drop`/`writeErrors` in UI, usare una capacità reale
pari a `kQueueSize - 1` dichiarata e/o una coda più grande, e proteggere i
contatori con `portMUX` o `std::atomic<uint32_t>` (i secondi sono a 32 bit
allineati, ma il read-modify-write non è atomico).

---

### F-09 — P2 — Auto-Portal: nessun limite esplicito al body HTTP

**File:** `src/modules/wifiext/AutoPortal.cpp:264-271` (registrazione POST e
`onNotFound`).

**Cosa succede.** Il modulo non impone un limite a `Content-Length`/body né
chiude le richieste troppo grandi. `ESPAsyncWebServer` accumula il body/argomenti
per il parsing del form; con 320 KB di SRAM e nessuna PSRAM, un POST molto
grande inviato all'AP aperto può esaurire l'heap. Il REPORT non dichiara
limiti di body, client o timeout (QA P13/X2).

**Impatto.** Potenziale OOM/reboot su richiesta di un client collegato all'AP,
e impossibilità di verificare P13. Va confermato in laboratorio perché il
comportamento dipende dalla versione esatta della libreria (esphome 3.4.0).

**Fix.** Rifiutare presto `request->contentLength() > 1024` (o usare un
`onBody` che chiuda la connessione oltre soglia), dichiarare nel REPORT i
limiti e testare POST enormi, richieste lente e 4 client.

---

### F-10 — P2 — mDNS: QU/unicast ignorato e non dichiarato

**File:** `src/modules/wifiext/Responder.cpp:239-284` (invio a `:281-283`).

**Cosa succede.** La risposta mDNS viene sempre inviata a
`224.0.0.251:5353`, anche se la query ha il bit QU (qclass 0x8000) e chiede
una risposta unicast. Il codice non legge `qclass` (`nameEnd + 2`), quindi non
distingue. La review QA R8 chiede risposta unicast alla porta sorgente
**oppure** comportamento dichiarato nel REPORT; il REPORT non lo dichiara.

**Impatto.** Gap di conformità/accettazione, possibile risposta non vista da
client che usano QU; nessun crash.

**Fix.** Leggere `qclass`; se `qclass & 0x8000`, inviare a
`g_mdns.remoteIP()`/`remotePort()`; altrimenti documentare esplicitamente il
comportamento multicast nel REPORT.

---

### F-11 — P2 — Append su SD: riga parziale possibile su SD piena/errore

**File:** `src/modules/wifiext/WifiExtCommon.cpp:71-90` (`file.write` a
`:84-87`); usato da `Responder.cpp:381-391` e `AutoPortal.cpp:156-172`.

**Cosa succede.** `appendSd()` apre in `FILE_APPEND`, scrive tutta la riga in
una `write` e controlla `written == text.size()`/`getWriteError()`, ma se la
scrittura è parziale (SD piena o rimossa a caldo) non ripristina la dimensione
precedente. La riga troncata resta nel file e l'append successivo si attacca
alla riga rotta, corrompendo hash o credenziali. Il test H10/P10 si aspetta
“append senza corruzione” e “file precedente integro”.

**Impatto.** Perdita/corruzione di hash o credenziali su SD piena o instabile.
Inoltre il REPORT non dichiara il residuo di cancellazione su FAT (X8) e non
menziona eventuali `.tmp`/`.bak` (in questo modulo non vengono creati, ma la
cancellazione non azzera i blocchi).

**Fix.** Salvare la dimensione precedente e, su short write/errore, troncare
alla dimensione originale (riaprire in write e riscrivere l'ultima riga
valida, o usare un file temporaneo + rename); mostrare l'errore in UI.
Dichiarare nel REPORT il limite “cancellazione logica, residuo recuperabile su
FAT”.

---

### F-12 — P2 — Karma: fallback silenzioso su lista illeggibile/troncata; promiscuous fallita non mostrata

**File:** `src/modules/wifiext/Karma.cpp:65-81` (`readText` a `:67`),
`:52-63` (troncamento), `:332-334` (promiscuous).

**Cosa succede.**

- `storage::readText(kListPath, text, 2048)` non passa `preferSD=true`:
  `Storage::openRead` cerca prima su LittleFS; un vecchio
  `/wifiext/karma.txt` in LittleFS può avere precedenza sulla SD. Se il file
  supera 2048 byte o la lettura fallisce, `loadListFile()` restituisce false e
  `loadFallback()` avvia una scansione, **senza messaggio**: l'operatore crede
  di usare la lista e invece il Cardputer risponde con gli SSID vicini.
- Le righe oltre 33 byte vengono troncate (`line.size() < 33`) e le voci oltre
  20 vengono scartate senza avviso (QA K10 chiede messaggi chiari).
- Se `esp_wifi_set_promiscuous(true)` fallisce, viene solo loggato un warning:
  l'AP resta su e la UI non indica che non arriveranno probe response.

**Impatto.** Comportamento imprevedibile di Karma, test K2/K10 non ripetibili;
possibile AP attivo senza risposte e senza diagnosi.

**Fix.** Usare `readText(..., 2048, true)`, mostrare in UI “lista troncata /
illeggibile / fallback scan”, limitare/validare le righe con messaggio, e se
la promiscuous fallisce mostrare un errore e fermare (o marcare lo stato in
UI). Documentare capacità dell'anello (3 pending su 4 slot) e rate limit.

---

### F-13 — P2 — `portal.html` oltre 16 KB: fallback interno senza avviso in UI

**File:** `src/modules/wifiext/AutoPortal.cpp:237-240`.

**Cosa succede.** `readText` restituisce false se il file supera 16384 byte;
il codice usa la pagina interna e scrive solo `logger::info`, non un messaggio
a schermo. Un operatore che ha caricato una pagina custom troppo grande vede
il portale funzionare ma con la pagina sbagliata. Stesso pattern per
`karma.txt` (F-12). QA P4/P11/X11 chiedono gestione esplicita.

**Impatto.** Test P4 può sembrare superato con la pagina sbagliata; nessun
crash.

**Fix.** Mostrare un avviso UI all'avvio (“portal.html non valido, uso pagina
interna”) e dichiarare il limite di 16 KB nel REPORT.

---

### F-14 — P2 — REPORT/accettazione: limiti e prove mancanti

**File:** `src/modules/wifiext/REPORT.md:68-102`, `:119-143`; build finale in
`docs/integration/MERGE_B.md:35,51-57`.

**Cosa manca.**

- Il REPORT dichiara la build del modulo (`2.390.544` byte) ma il firmware
  integrato finale è `2.586.768` byte (98,7% dello slot, **35.039 byte**
  residui): il margine reale è molto inferiore a quello del REPORT.
- Non dichiara i limiti di body/client/timeout HTTP (P13), il comportamento
  mDNS QU (R8), la capacità reale delle code/anelli (K8), il residuo di
  cancellazione su FAT (X8) e la gestione esatta di NTLMv1 (H6, vedi F-03).
- Non ci sono evidenze hardware: H1–H5, K1–K9, P1–P14, S1–S8 e X1–X12 non
  sono stati eseguiti (solo build); la review QA è precedente allo stub.
- `ntlm.txt`/`portal.csv` sono in chiaro su SD (accettabile per la spec, ma
  va ribadito che la SD è materiale sensibile; la cancellazione non è
  crittografica).

**Impatto.** Rischio di dichiarare “accettato” ciò che è solo compilato; con
35 KB liberi, ogni correzione (es. buffer LLMNR, gestione stop) va misurata
sul binario integrato.

**Fix.** Aggiornare il REPORT con i limiti e le prove, riportare la dimensione
integrata, eseguire il piano QA su hardware e allegare hashcat, catture e
listato SD (con dati fittizi).

---

## Cosa risulta corretto (per non rifare il lavoro)

- **Gate offensivo.** Le tre funzioni sono compilate solo con
  `ENABLE_MODULE_WIFIEXT && ENABLE_OFFENSIVE` (`Responder.cpp:22`,
  `Karma.cpp:21`, `AutoPortal.cpp:21`); la build pubblica mostra “Funzione non
  inclusa” (`WifiExtMenu.cpp:73-81`) e non accende la radio. `legalWarning` è
  chiamato prima di ogni avvio (`Responder.cpp:624`, `Karma.cpp:287`,
  `AutoPortal.cpp:220`). Nessun percorso alternativo di bypass trovato.
- **Karma senza deauth.** Non c'è codice che invii 0x0A/0x0C; l'unico frame raw
  è la probe response `0x50` (`Karma.cpp:182-219`). BSSID/source/destination
  sono impostati (`:188-190`), l'SSID è limitato a 32 byte (`:62,164,200`) e
  le IE sono controllate (`:151-160`). Stop: promiscuous off, callback null,
  `softAPdisconnect(true)`, `WiFi.mode(WIFI_OFF)` (`:356-359`).
- **Responder, parte UDP.** Bind distinti su 5355/137/5353 e join dei gruppi
  multicast (`Responder.cpp:647-649`); lettura con `parsePacket`/`read` e
  ritorno su `len <= 0`; risposte LLMNR/NBT unicast alla sorgente
  (`:184,233`); NBT `out[128]` dimensionato correttamente (offset massimo 100).
  Le porte non in elenco non vengono aperte in questa modalità.
- **Auto-Portal, dati.** `portal.html` è letto solo da SD
  (`AutoPortal.cpp:237`, `preferSD=true`); `portal.csv` è scritto solo tramite
  SD (`WifiExtCommon.cpp:71-90`), path fisso, nessun path derivato da input,
  nessuna scrittura su LittleFS. I campi sono troncati a 63 byte
  (`AutoPortal.cpp:76-83`), il body non viene riflesso nella pagina, non c'è
  redirect costruito da `Host`, non c'è NAT/uplink. La cancellazione è nel menu
  (`WifiExtMenu.cpp:84-87`).
- **Stop normale.** Responder: stop dei tre UDP, `smbServer.end()`, `smbDrop()`,
  `wifisetup::disconnect()` (`Responder.cpp:693-698`). Auto-Portal:
  `server.end()` → `dns.stop()` → `softAPdisconnect(true)` → `WiFi.mode(WIFI_OFF)`
  (`AutoPortal.cpp:296-301`), ordine corretto (resta il problema F-05 sulla
  sincronizzazione). Karma: vedi sopra.
- **Nessun log di segreti.** I log del Responder riportano solo contatori
  (`Responder.cpp:699-702`), il portale solo richieste/salvate/perse
  (`AutoPortal.cpp:304-306`), Karma solo SSID/canale e contatori. Nessun hash,
  utente o password nel log.
- **Concorrenza di base.** Coda credenziali e anello probe usano
  `portMUX_TYPE`; il callback Karma non alloca; la coda viene svuotata dal loop
  principale, non dal task async (`AutoPortal.cpp:142-174, 285`).

## Prossimi passi minimi (in ordine)

1. Correggere F-01 e F-02; sono bloccanti e non richiedono hardware per la
   revisione del diff.
2. Correggere F-03/F-04 (validazione Type3) e F-05 (stop portale).
3. Eseguire su hardware: H1–H5 con hashcat, K1–K9 con monitor mode, P1–P14,
   S1–S8, X1–X12; misurare heap/min free heap e 10 cicli.
4. Aggiornare `REPORT.md` con limiti, dimensioni integrate e residui SD.

## Limiti di questa analisi

Nessuna build e nessun test su hardware (vietati dal mandato). Non sono stati
verificati: comportamento reale di `ESPAsyncWebServer::end()`, limiti interni
della libreria sul body, heap a runtime, visibilità dell'AP, cattura monitor
mode, crack hashcat, SD piena/rimossa, 10 cicli. Le severità riflettono
l'analisi statica e i criteri di accettazione della review 09.
