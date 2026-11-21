# 06 — WiFi Lab: test statici sul codice finale integrato

Data: 21/11/2026. Verificatore: bug tester (sola lettura).

Target: `C:\Users\utente\Desktop\opencode\offensive-cardputer`, commit HEAD
`f19b6d0`; modulo `src/modules/wifilab/WifiLabMenu.cpp` (commit `183477c`,
2.270 righe), `REPORT.md`, `partitions.csv`.

Metodo: revisione statica del codice finale e degli artefatti di build già
presenti (`.pio/build/cardputer-adv-offensive/firmware.bin`, `firmware.map`).
Nessuna build, nessuna modifica al codice, nessun commit, nessun test su
hardware. Le prove RF restano manuali e vanno eseguite solo su reti proprie,
come da `reviews/06-wifilab.md` §1.

## Esito sintetico

- **Nessun P0 statico**: i tre bloccanti/alti del review precedente risultano
  corretti o sostanzialmente corretti:
  - **F1 chiuso**: `drawSpectrum()` è ora chiamata (`WifiLabMenu.cpp:2212`);
  - **F2 chiuso**: `savePmkidFile()` non scrive `.hc22000` senza ESSID
    (`WifiLabMenu.cpp:1039`);
  - **F4 chiuso in gran parte**: `loadWhitelist()` è fail-closed per file
    assente, > 4 KB, `readText` false, riga senza alcun campo e > 24 voci
    (`WifiLabMenu.cpp:265-324`); restano P1-03 (add) e P2-13 (BSSID
    malformato con SSID non vuoto);
  - **F3 chiuso sul gate di compilazione**: `esp_wifi_80211_tx` compare solo
    dentro `#if ENABLE_OFFENSIVE` (`WifiLabMenu.cpp:1225,1276,1811`);
- **Restano 3 P1 e 13 P2** (sotto). I P1 non sono "teorici": due riguardano
  controlli di sicurezza (gate legale e whitelist), uno produce un falso
  positivo PMKID.
- La build finale esiste ed è verde, ma il `REPORT.md` riporta una dimensione
  vecchia (2.398.048 B) mentre il binario finale è **2.618.256 B**; vedi P2-11.
- Senza hardware e hashcat non è possibile firmare l'accettazione (PMKID
  reale, deauth reale, spettro reale, heap su 30 min).

## Top 5 (i più gravi)

1. **P1-01 — Gate legale: Client Monitor e Whitelist fanno scan attive senza
   `ui::legalWarning`.** `WifiLabMenu.cpp:381` (`WiFi.scanNetworks(true,true)`),
   chiamata da `:1852` (Client Monitor) e `:2007` (Whitelist > A). Lo scan
   attivo trasmette probe request; `beginRadio()` (`:182`) può anche inviare
   un disassoc se il device era connesso. Il commento `:2` ("tutte le
   trasmissioni passano da legalWarning") è falso. Violazione del BRIEF
   ("prima di qualsiasi trasmissione RF") e del test L6.
2. **P1-02 — `.hc22000` vecchio non rimosso: falso positivo PMKID.**
   `savePmkidFile()` (`:1036-1067`) scrive solo in caso di successo; nessun
   `removeFile` all'avvio della sessione (`:1468-1469`) e il pcap viene
   troncato (`:1103`) ma l'hash no. Dopo una sessione riuscita, una sessione
   fallita su AP senza PMKID o con SSID nascosto lascia il vecchio hash: un
   tester che guarda il file può credere a un nuovo successo (P6/P7/P13).
3. **P1-03 — Whitelist fail-open in `addWhitelistEntry()`.** `:336` ignora il
   ritorno di `storage::readText()`; se il file esiste ma la lettura fallisce
   (errore SD transitorio, file cambiato dopo il load), `:347` riscrive un
   file con **solo la nuova voce**, cancellando le protezioni precedenti.
   Dopo il reload `g_whiteReady` torna true: le reti prima escluse diventano
   attaccabili. È l'opposto del fail-closed dichiarato nel REPORT.
4. **P2-01 — Client Monitor senza invecchiamento + freshness deauth non
   rivalutata.** `mergeObservation()` (`:1637-1656`) non rimuove mai un
   client; a 16 voci i nuovi sono scartati in silenzio (`:1648-1650`).
   `deauthClient()` copia il client prima dell'avviso (`:1756`), poi ricontrolla
   `millis()-client.lastSeen` sulla **copia vecchia** (`:1777`): un client
   fresco può essere rifiutato se l'avviso richiede > 5-15 s, mentre un client
   sparito durante l'avviso può ancora passare il controllo. C6/C7/C8 non
   sono affidabili.
5. **P2-02 — Peak hold dello spettro senza decadimento.** `g_specPeaks[i] =
   max(...)` (`:2235`) non cala mai; la scala delle barre include i peak
   (`:2117-2121`) e la linea peak (`:2134-2138`). Un burst iniziale schiaccia
   permanentemente tutte le barre: S3/S5 falliscono anche dopo F1.

Subito dopo: **P2-03** (anello PMKID da 6 slot che accetta EAPOL di qualsiasi
BSSID), **P2-04** (pcap con flush per frame ed errori a metà non segnalati),
**P2-05** (TX auth/assoc senza controllo del ritorno).

## Findings

| ID | Gravità | Area | File:riga | Sintesi |
|----|---------|------|-----------|---------|
| P1-01 | **P1** | Gate legale | `WifiLabMenu.cpp:381,1852,2007,179-184,2` | Scan attive/probe request senza avviso legale in Client Monitor e Whitelist; possibile disassoc da `WiFi.disconnect()` |
| P1-02 | **P1** | PMKID / hc22000 | `WifiLabMenu.cpp:1036-1067,1468-1469,1496-1498` | Il vecchio `.hc22000` resta se la nuova sessione non trova PMKID/ESSID: falso positivo |
| P1-03 | **P1** | Whitelist fail-closed | `WifiLabMenu.cpp:326-351,1994-1999` | `readText` ignorato: la whitelist può essere sovrascritta con una sola voce; fail-open |
| P2-01 | P2 | Deauth / client fresco | `WifiLabMenu.cpp:1637-1656,1756-1782` | Tabella senza aging, cap 16 silenzioso, ricontrollo post-avviso su copia stale |
| P2-02 | P2 | drawSpectrum | `WifiLabMenu.cpp:2117-2121,2134-2138,2232-2235` | Peak hold senza decay e incluso nella scala: barre permanentemente compresse |
| P2-03 | P2 | PMKID / parsing RX | `WifiLabMenu.cpp:623-644,686-697,1190-1205` | L'anello da 6 slot accetta EAPOL di qualsiasi BSSID: M1 del target può essere scartato |
| P2-04 | P2 | pcap | `WifiLabMenu.cpp:978-985,1542-1544` | Flush a ogni frame; errore dopo i primi frame non segnalato; pcap può essere troncato e sembrare valido |
| P2-05 | P2 | PMKID TX | `WifiLabMenu.cpp:1225,1276,1297-1319` | `esp_wifi_80211_tx` ignorato in auth/assoc: nessuna diagnosi, 10 round e "n/d" |
| P2-06 | P2 | PMKID target | `WifiLabMenu.cpp:1245-1275,1405-1421` | Nessun filtro open/WEP/WPA3/Enterprise: target destinati a fallire; rischio hash type-01 non crackabile |
| P2-07 | P2 | Whitelist UI/limiti | `WifiLabMenu.cpp:317-323,336-350,1976-1987,2059-2070` | Add può superare 4 KB e bloccare tutto; niente delete/repair; solo 6 voci visibili |
| P2-08 | P2 | Client Monitor UI | `WifiLabMenu.cpp:1694-1695,1733-1736` | Voce SSID-only non mostra "WL" (usa solo BSSID) anche se il deauth è bloccato |
| P2-09 | P2 | Integrazione | `WifiLabMenu.cpp:40,1045,1102`; `WpaSecMenu.cpp:623`; `PcapMenu.cpp:68` | PMKID in `/wifilab/pmkid/`, WPA-SEC e Capture leggono solo `/captures`: file non caricabili/sfogliabili |
| P2-10 | P2 | Cleanup / passivo | `WifiLabMenu.cpp:179-184,702,2166,186-193` | `WiFi.disconnect()` all'ingresso può trasmettere un disassoc; lo spettro non è "zero TX" se il device era connesso |
| P2-11 | P2 | Report / release | `REPORT.md:172-175`; `partitions.csv:15-16`; `flasher/bin/firmware.bin` | Report 2.398.048 B vs binario finale 2.618.256 B; slot e flasher non allineati |
| P2-12 | P2 | Menu / config | `main.cpp:117` | WiFi Lab non è in `config::Module`: non disattivabile a runtime (R9), non dichiarato nel REPORT |
| P2-13 | P2 | Whitelist fail-closed | `WifiLabMenu.cpp:293-312` | BSSID malformato con SSID non vuoto accettato come voce solo-SSID: il typo non attiva il fail-closed dichiarato |

### P1-01 — Gate legale: scan attive senza avviso

**Dove.**
`scanAps()` usa `WiFi.scanNetworks(true, true)` (`WifiLabMenu.cpp:381`): il
secondo argomento è `show_hidden`, non `passive`; la scansione è **attiva** e
trasmette probe request. `scanAps()` è chiamata da:

- `runClientMonitor()` — `WifiLabMenu.cpp:1852`, prima di qualsiasi avviso;
- `addWhitelistFromScan()` (Whitelist > A) — `WifiLabMenu.cpp:2007`;
- `runPmkidHunt()` — `WifiLabMenu.cpp:1377`, ma qui l'avviso è a `:1371`,
  quindi il percorso PMKID è corretto.

Inoltre `beginRadio()` (`:179-184`) esegue `WiFi.mode(WIFI_STA)` e
`WiFi.disconnect()`: se il dispositivo era connesso a un AP, `disconnect()`
può inviare un frame di disassociazione, anche questo senza avviso. Il
commento di testa (`:2`) afferma il contrario.

**Impatto.** Violazione del BRIEF ("Prima di qualsiasi trasmissione RF:
`ui::legalWarning(...)`") e del test L6 ("elenco AP/client e whitelist non
mostrano l'avviso e non trasmettono"): la seconda metà è falsa. Un utente può
emettere probe request (e potenzialmente un disassoc verso la rete a cui è
connesso) senza aver accettato l'avviso. È un problema di policy/legal gate,
non un attacco, ma il progetto tratta il gate come requisito di sicurezza.

**Fix.** Chiamare `ui::legalWarning` prima di `scanAps()` in
`runClientMonitor()` e in `addWhitelistFromScan()` (oppure usare una scan
passiva, `scanNetworks(true, true, true)`, se si accetta di vedere meno AP).
Valutare `WiFi.status()` prima di `WiFi.disconnect()` in `beginRadio()` e
documentare l'eventuale disassoc; aggiornare il commento `:2` e il REPORT.

**Test.** L2/L6 con un secondo ricevitore in monitor: aprire Client Monitor e
Whitelist > A senza confermare alcun avviso; osservare probe request. Con il
device connesso a un AP proprio, aprire Spettro e cercare il disassoc.

### P1-02 — `.hc22000` vecchio non rimosso: falso positivo PMKID

**Dove.** `savePmkidFile()` (`WifiLabMenu.cpp:1036-1067`) scrive solo se
`target.hasPmkid && target.ap.ssidLen > 0` (`:1039`); non esiste alcun
`storage::removeFile` per il `.hc22000`. `runPmkidHunt()` crea solo le
directory (`:1468-1469`) e apre/tronca i pcap (`openTargetPcap()`, `:1103`),
ma non tocca gli hash. Se la sessione successiva non trova PMKID, o lo trova
senza ESSID, il file della sessione precedente resta.

**Impatto.** Un test P6 (AP senza PMKID), P7 (SSID nascosto) o P13 (SD
assente/errore) eseguito dopo un P2 riuscito sullo stesso BSSID trova un
`.hc22000` vecchio nella directory e può essere registrato come "successo".
Il REPORT dice "viene sovrascritto alla sessione successiva", ma la
sovrascrittura avviene solo in caso di nuovo hash valido. Il messaggio finale
può dire "hash non scritto" mentre il file vecchio è ancora lì. È un falso
positivo di accettazione, non solo un residuo.

**Fix.** All'avvio della sessione per ogni target, rimuovere
`/wifilab/pmkid/<bssid>.hc22000` (e il pcap se si vuole coerenza), oppure
scrivere prima in `<path>.tmp` e fare rename solo su successo, cancellando il
tmp in caso di errore/ESC. Aggiungere il timestamp o il nome sessione al
messaggio finale per distinguere i file nuovi.

**Test.** P2 riuscito -> P6/P7 sullo stesso BSSID -> controllare che il file
non esista o che il contenuto/timestamp sia nuovo. Ripetere con ESC durante
il hunt.

### P1-03 — Whitelist fail-open in `addWhitelistEntry()`

**Dove.** `addWhitelistEntry()` (`WifiLabMenu.cpp:326-351`):

```cpp
std::string text;
storage::readText(kWhitePath, text, 4096, true);   // :336, ritorno ignorato
...
if (!storage::writeText(kWhitePath, text, true)) { // :347
    return false;
}
loadWhitelist();                                   // :350
```

`addWhitelistFromScan()` blocca il caso "file esistente e non pronto" solo
con `if (!g_whiteReady && storage::exists(kWhitePath, true))`
(`:1994-1999`). Se `g_whiteReady` è true da un caricamento precedente e la
lettura fallisce adesso (errore SD transitorio, file modificato/rimosso e
ricreato, file cresciuto oltre 4 KB), `text` resta vuoto o parziale e
`writeText()` sostituisce l'intero file con la sola nuova voce. Dopo
`loadWhitelist()` il file è di nuovo valido: `g_whiteReady` torna true e le
reti che erano protette non lo sono più.

**Impatto.** Fail-open di un controllo di sicurezza: un errore di lettura
trasforma la whitelist in una lista con una sola rete, e PMKID/deauth tornano
possibili verso tutte le altre. È esattamente il comportamento che il REPORT
dichiara impossibile ("fail-closed"). In più, se l'append supera 4096 byte,
il reload successivo imposta `g_whiteReady=false` e blocca **tutti** gli
attacchi, mentre la UI ha già detto "Aggiunto".

**Fix.** Prima di scrivere:

- se il file esiste, richiedere `storage::readText(...) == true`, altrimenti
  abortire con un messaggio ("Whitelist non leggibile, non modificata");
- calcolare la dimensione risultante e rifiutare se > 4096 byte o > 24 voci;
- dopo il write, verificare `loadWhitelist()`/`g_whiteReady` e riportare
  l'esito reale, non solo il ritorno di `writeText`.

**Test.** W12/W13/W14: simulare un errore di lettura (card rimossa e
reinserita, file corrotto) e aggiungere un AP; verificare che il file
precedente resti intatto. Portare il file a 24 voci e aggiungerne una
venticinquesima.

### P2-01 — Client Monitor: aging, cap e freshness deauth

**Dove.**

- `mergeObservation()` (`:1637-1656`) aggiorna i client esistenti e, se la
  tabella è piena (`:1648-1650`), scarta i nuovi senza segnalarlo.
- Non esiste rimozione per inattività: un client visto una volta resta in
  tabella per sempre; `lastSeen` è solo usato dal deauth.
- `deauthClient()` copia `ClientInfo` con `findClient()` (`:1756`), controlla
  `millis() - client.lastSeen > 10000` (`:1763`), mostra l'avviso (`:1769`) e
  ricontrolla `millis() - client.lastSeen > 15000` (`:1777`) sulla **stessa
  copia**, senza rifare `findClient()` né drenare `g_observations`.

**Impatto.**

- C6 (client disconnesso) fallisce: il client resta elencato; non è marcato
  stale.
- C7 (cap tabella) è solo parziale: il limite 16 è mostrato, ma lo scarto dei
  nuovi non è segnalato; dopo 16 MAC unici i client nuovi non compaiono più.
- Il ricontrollo post-avviso non verifica la freschezza reale: se l'avviso
  richiede più di 5-15 s, un client ancora attivo viene rifiutato; se il
  client sparisce durante l'avviso, il vecchio timestamp può ancora passare e
  il deauth parte verso un MAC assente. La finestra di 10 s è una buona base,
  ma manca la rivalutazione al momento del TX.

**Fix.** Aggiungere aging (es. 30-60 s) con rimozione o marcatura "stale";
quando la tabella è piena, rimpiazzare il più vecchio o mostrare "tabella
piena/scartati". Dopo `ui::legalWarning`, drenare il ring e rifare
`findClient()`; usare una finestra breve (es. 2 s) calcolata subito prima del
TX. Mostrare `lastSeen`/stale nella lista.

**Test.** C6/C7/C8; riempire la tabella con 16 MAC; lasciare l'avviso aperto
10-20 s e confermare; verificare che un client attivo passi e uno sparito no.

### P2-02 — Spettro: peak hold senza decadimento

**Dove.** `runSpectrum()` aggiorna i peak con `std::max` (`:2232-2235`) e non
li azzera mai durante la sessione; `drawSpectrum()` calcola la scala includendo
`g_specPeaks` (`:2117-2121`) e disegna la linea peak (`:2134-2138`).
`g_specPeaks` viene azzerato solo all'ingresso (`:2176-2178`).

**Impatto.** Un burst iniziale (o un canale molto forte) fissa un peak alto;
tutte le barre successive sono ridimensionate su quel valore e sembrano
spente. S3 ("traffico spento") e S5 ("peak hold") non sono conformi: il peak
non decade e non c'è un reset esplicito. Dopo F1 lo spettro è visibile, ma
può sembrare morto dopo il primo burst.

**Fix.** Decadimento per sweep, ad esempio
`g_specPeaks[i] = max(counts[i], g_specPeaks[i] - max(1u, g_specPeaks[i]/8))`,
oppure un tasto di reset; in alternativa usare per la scala solo `counts` e
disegnare il peak con una scala separata. Documentare la scelta.

**Test.** S3/S5: burst su un canale, poi silenzio; le barre devono recuperare
in pochi sweep e il peak deve scendere o essere resettabile.

### P2-03 — Anello PMKID saturato da EAPOL non target

**Dove.** Nel callback `promiscuousCb()`, il ramo `RxPmkid` per i frame data
(`:623-644`) verifica solo che sia EAPOL-Key; non confronta BSSID/STA con i
target. L'anello ha 6 slot (`:34,686-697`) e viene drenato ogni 4 ms
(`drainPmkidRx`, `:1190-1205`), ma su un canale affollato gli EAPOL di altre
reti possono riempirlo e far scartare l'M1 del target. `g_rxDropped` è
mostrato nella UI ma non c'è modo di recuperare i frame persi.

**Impatto.** PMKID inaffidabile in ambienti con altro traffico WPA2 (un
handshake di un altro client sullo stesso canale è sufficiente); il test P2
può fallire senza che l'AP sia il problema.

**Fix.** Nel callback, prima di accodare, verificare che il BSSID (e
possibilmente lo STA) sia tra i target; in alternativa aumentare gli slot o
drenare più spesso. Il confronto deve restare nel callback, senza allocazioni.

**Test.** P2 con un secondo handshake WPA2 sullo stesso canale; confrontare il
tasso di M1 con e senza traffico estraneo; osservare `g_rxDropped`.

### P2-04 — pcap: flush per frame ed errori a metà non segnalati

**Dove.** `writePcapFrame()` (`:968-986`) fa `flush()` a ogni frame (`:985`) e
imposta `pcapOk=false` al primo errore (`:981`). Nel riepilogo,
`writeError` è true solo se `!pcapOk && pcapFrames == 0` (`:1542-1544`):
un pcap troncato dopo il primo frame viene considerato OK e il messaggio
finale può dire "pcap disponibile" anche se il file è incompleto.

**Impatto.** Latenza SD e jitter durante il hunt; usura della card; P13
("SD piena/rimossa a metà") non rilevata correttamente. Il file pcap può
essere formalmente valido ma senza l'M1, e il tester non lo sa.

**Fix.** Flush solo alla chiusura (o ogni N frame); mantenere un flag
`writeError` persistente impostato su qualsiasi write fallita e mostrarlo nel
riepilogo; scrivere il pcap in `.tmp` e rinominarlo solo a fine sessione,
cancellandolo se incompleto.

**Test.** P13: rimuovere la SD a metà hunt o riempirla; verificare messaggio e
integrità dei file precedenti.

### P2-05 — TX auth/assoc senza controllo del ritorno

**Dove.** `sendAuth()` (`:1208-1226`) e `sendAssoc()` (`:1228-1277`)
chiamano `esp_wifi_80211_tx` e ignorano il valore (`:1225,1276`). Il ciclo
considera solo `rounds`/`m1Count` (`:1297-1319`).

**Impatto.** Se il driver rifiuta i frame (canale/regione/stato), il modulo
continua per 10 round e finisce "n/d" senza distinguere un AP che non manda
PMKID da un TX fallito. F10 resta aperto.

**Fix.** Controllare il ritorno, conteggiare i fallimenti e abortire con un
messaggio dopo N fallimenti consecutivi; opzionalmente riprovare su un canale
diverso o controllare `esp_wifi_get_channel`.

**Test.** P8/P10: forzare un canale non consentito o un AP che rifiuta; il
messaggio deve dire "TX rifiutato", non solo "n/d".

### P2-06 — Target PMKID senza filtro di sicurezza

**Dove.** La selezione (`:1395-1425`) mostra tutti gli AP; `sendAssoc()`
(`:1245-1275`) usa un RSN IE PSK/CCMP per ogni rete non-open. `securityName()`
(`:142-165`) conosce WPA3/Enterprise ma non viene usato per filtrare.

**Impatto.** AP open, WEP, WPA3-only ed Enterprise sono selezionabili e
falliscono sempre (o rifiutano l'assoc). Un AP WPA3/transition che mandasse
un M1 con PMKID potrebbe produrre una riga `WPA*01` non crackabile con PSK
WPA2 (falso positivo, P8). Il REPORT dichiara il limite ma la UI non avvisa.

**Fix.** Marcare/filtrare gli AP non WPA2-PSK (e WPA2/WPA3 transition);
salvare l'hash solo per auth PSK compatibili; mostrare un messaggio esplicito
per WPA3/Enterprise.

**Test.** P8: selezionare un AP WPA3-only e un AP open; attendersi rifiuto o
messaggio, nessun `.hc22000` inventato.

### P2-07 — Whitelist: add oltre 4 KB, niente repair, lista non scrollabile

**Dove.** `addWhitelistEntry()` non calcola la dimensione risultante
(`:326-351`); `loadWhitelist()` blocca > 4 KB (`:272`); `runWhitelist()` > N
crea un file vuoto solo se il file non esiste (`:2059-2070`); `drawWhitelist()`
mostra solo 6 voci su 24 (`:1976-1987`).

**Impatto.** Aggiungere una voce a una whitelist quasi piena può portare il
file oltre 4 KB: il reload imposta `g_whiteReady=false` e blocca PMKID e
deauth, mentre la UI ha detto "Aggiunto". Se il file è invalido, `N` non lo
ripara e non c'è delete: serve un PC. L'utente non può verificare le voci
oltre la sesta.

**Fix.** Rifiutare l'add se il risultato supererebbe 4096 byte/24 voci;
permettere `N`/`D` (riscrittura/cancellazione con conferma) anche su file
esistente invalido; aggiungere scroll o paginazione.

**Test.** W12/W14: file a 24 voci, aggiunta; file corrotto, tentativo di
repair; verifica delle voci 7-24.

### P2-08 — Indicatore WL incoerente per voci SSID-only

**Dove.** `drawClientMonitor()` calcola l'indicatore WL con
`bssidWhitelisted(ap.bssid)` (`:1694-1695`), mentre il footer e il deauth
usano anche `apWhitelisted(ap)` (`:1733-1736`, `:1751`).

**Impatto.** Una voce `|SSID` protegge l'AP e blocca il deauth, ma la lista
client non mostra "WL": l'utente può credere che l'AP non sia protetto.

**Fix.** Usare `apWhitelisted(ap)` anche per l'indicatore.

**Test.** W9/W16 con una voce solo SSID; verificare footer e indicatore.

### P2-09 — File PMKID non visibili a WPA-SEC e Capture

**Dove.** Il modulo scrive in `/wifilab/pmkid/` (`:40,1045,1102`); WPA-SEC
legge `/captures` (`WpaSecMenu.cpp:623` e messaggi `:1689,1714`); PcapMenu
elenca `/captures` (`PcapMenu.cpp:68,539,617,665,733`). Non esiste copia/alias
nel codice finale.

**Impatto.** Dopo un PMKID riuscito, l'utente non può caricare l'hash da
WPA-SEC né sfogliare il pcap da Capture: deve estrarre la SD e usare un PC.
R7/R8 restano aperti nell'integrazione.

**Fix.** Salvare una copia in `/captures` (nome univoco) oppure estendere
WPA-SEC/PcapMenu ai path `/wifilab/pmkid` (fuori area 06; coordinare col
supervisore).

**Test.** R7/R8: creare un PMKID, aprire WPA-SEC > Elenco catture e Capture >
Sfoglia; il file deve comparire.

### P2-10 — `WiFi.disconnect()` all'ingresso e spettro non strettamente passivo

**Dove.** `beginRadio()` (`:179-184`) fa `WiFi.mode(WIFI_STA)` e
`WiFi.disconnect()`; è chiamata da `startRx()` (`:702`) e da `scanAps()`
(`:378`). `runSpectrum()` usa `startRx(RxSpectrum)` (`:2166`). `stopRadio()`
(`:186-193`) ripete `WiFi.disconnect()` prima di `WIFI_OFF`.

**Impatto.** Se il dispositivo era connesso, l'ingresso in Spettro/Client
Monitor/Whitelist può inviare un disassoc (TX) senza avviso e interrompere la
connessione. Il test S7 ("zero frame TX in modalità spettro") non è
strettamente vero in quello scenario; il test L6 anche.

**Fix.** Controllare `WiFi.status()` e evitare `disconnect()` se non
connessi; documentare l'eccezione o mostrare l'avviso anche in questi modi.
Non è un attacco, ma è una TX non dichiarata.

**Test.** Connessione a un AP proprio, apertura Spettro, cattura su un secondo
monitor: cercare il disassoc; ripetere da WiFi off.

### P2-11 — REPORT, slot e flasher non allineati al build finale

**Dove.** `REPORT.md:172-175` dichiara `firmware.bin` 2.398.048 B e slot
2.621.440 B (91,5%, 223 KB liberi). Il binario finale presente in
`.pio/build/cardputer-adv-offensive/firmware.bin` è **2.618.256 B**
(21/11/2026 15:43), quindi:
- sul vecchio slot da 2.621.440 B restano **3.184 B**, non 223 KB;
- sul layout finale `partitions.csv:15` (`app0 0x2B0000` = 2.818.048 B)
  restano **199.792 B**.
Il flasher (`flasher/bin/firmware.bin`, 2.323.600 B, 19/11) è il vecchio
build pubblico 1.21.0 e non contiene il codice finale/WiFi Lab.

**Impatto.** G4 e il REPORT sono inaccurati; il margine OTA reale è molto
più piccolo di quanto dichiarato. Chi flasha il flasher non prova il codice
finale. Non è un bug del modulo, ma è un rischio di consegna.

**Fix.** Ricostruire e copiare il binario finale; aggiornare REPORT con
dimensione, slot e RAM reali; ricostruire anche la build pubblica
`cardputer-adv` e il flasher pubblico; indicare la migrazione partition table.

**Test.** Confrontare size/hash di `.pio`, REPORT e flasher; flash del flasher
e verifica delle voci WiFi Lab (build pubblica: funzioni offensive no-op).

### P2-12 — WiFi Lab non disattivabile a runtime

**Dove.** `main.cpp:117` aggiunge la voce con una lambda diretta, senza
`guardedModule(...)`/`config::Module`; `Config.cpp`/`SettingsMenu.cpp` non
hanno una voce WiFi Lab.

**Impatto.** L'utente non può escludere il modulo dalla UI (R9); il REPORT
non dichiara la scelta. Non è un problema di sicurezza perché il gate
`ENABLE_OFFENSIVE` resta, ma è una deviazione dal pattern degli altri moduli.

**Fix.** Aggiungere il modulo a `config::Module`/Settings (se il supervisore
lo vuole) oppure dichiararlo esplicitamente nel REPORT come scelta.

### P2-13 — BSSID malformato + SSID: la riga diventa "solo SSID", non fail-closed

**Dove.** `loadWhitelist()` (`WifiLabMenu.cpp:293-312`):

```cpp
const size_t bar = line.find('|');
std::string macPart = bar == std::string::npos ? line : line.substr(0, bar);
std::string ssidPart = bar == std::string::npos ? std::string() : line.substr(bar + 1);
...
entry.hasBssid = parseMac(macPart, entry.bssid);
...
entry.hasSsid = !ssidPart.empty();
if (!entry.hasSsid && !entry.hasBssid) { invalid = true; continue; }
```

Se `macPart` è non vuoto ma non è un MAC valido (typo, cifre mancanti, caratteri
extra) e `ssidPart` non è vuoto, la riga viene accettata come voce **solo
SSID**: `hasBssid=false`, `hasSsid=true`. Non viene marcata `invalid`, quindi
`g_whiteReady` resta true. Il REPORT dice "riga malformata ⇒ attacchi
bloccati", ma questo caso non è bloccato. La protezione dipende solo dal
match dell'SSID; se anche l'SSID è sbagliato, la rete intesa non è esclusa.

**Impatto.** Un typo nel BSSID non attiva il fail-closed dichiarato. È un
buco residuo del controllo di sicurezza, anche se la policy "meglio escludere
una rete in più" mitiga il caso in cui l'SSID sia corretto. Rende inoltre
ambigua la distinzione tra una voce `|SSID` intenzionale e un `BSSID|SSID`
malformato.

**Fix.** Distinguere i due formati: se `bar == 0` (riga che inizia con `|`) è
una voce solo-SSID esplicita; se `bar > 0` e `macPart` non è un MAC valido,
marcare la riga `invalid` (fail-closed). In alternativa richiedere un prefisso
esplicito per le voci solo-SSID e documentare il formato.

**Test.** W7/W9: scrivere `AA:BB:CC:DD:EE|MiaRete` e verificare che il modulo
blocchi gli attacchi (o che la UI segnali la riga non valida), non che la
tratti come `|MiaRete`.

## Verifiche che risultano corrette (nessun finding)

- **F1 — spettro disegnato.** `drawSpectrum()` è chiamata in
  `runSpectrum()` (`WifiLabMenu.cpp:2212`), con barre, waterfall, PPS, peak e
  canale evidenziato. Resta solo il decay (P2-02). S1/S10 sono verificabili
  solo su hardware.
- **F2 — ESSID.** `savePmkidFile()` esce se `!hasPmkid || ssidLen == 0`
  (`:1039`); su rete nascosta lo stato è `HuntNoEssid` e non viene scritto
  `.hc22000` (`:1174-1178`). Un ESSID che arriva dopo da beacon viene salvato
  (`:1144-1149`). P7 è coperto staticamente.
- **F4 — caricamento whitelist fail-closed (con riserve).** File assente,
  > 4096 B, `readText` false, riga senza alcun campo valido o > 24 voci ⇒
  `g_whiteReady=false` (`:265-324`); `runPmkidHunt` e `deauthClient` bloccano
  (`:1364,1745`); `apWhitelisted` è usato in selezione (`:1406`), in
  `driveTarget` (`:1282`) e nel deauth (`:1751`); `bssidWhitelisted` è usato
  nel footer. Restano P1-03 (add) e P2-13 (BSSID malformato con SSID non
  vuoto): il REPORT che dice "riga malformata ⇒ bloccato" è troppo ampio.
- **F3 — gate di compilazione.** `esp_wifi_80211_tx` compare solo alle righe
  `1225`, `1276` (dentro `#if ENABLE_OFFENSIVE`, 768-1573) e `1811` (dentro
  `#if`, 1743-1848); le versioni pubbliche mostrano un messaggio
  (`:1567-1572` e `:1912-1917`). Con flag 0 nessun TX offensivo è compilato.
  Il gate legale a runtime ha il buco P1-01 per le scan passive/attive.
- **Parsing 802.11.** Offset EAPOL/SNAP, QoS (`data[0]&0x80`), HT control
  (`fc1&0x80`), mapping BSSID/STA toDS/fromDS/none (`:858-867`), IE vendor
  PMKID `00:0F:AC:04` (`:883-886`), beacon/probe `pos=36`
  (`:1070-1096`): non ho trovato letture fuori buffer; le lunghezze sono
  controllate prima delle copie. Il parsing dell'anello è ridondante ma
  sicuro.
- **FCS.** `length = sig_len - 4` (`:595`) è coerente con
  `WifiMenu.cpp:643-645`; il pcap linktype 105 non contiene FCS. Resta
  l'assunzione del driver: se una versione non includesse l'FCS, il pcap e
  il parsing perderebbero 4 byte. Va verificato in P14 con Wireshark, non è
  un bug nuovo del modulo.
- **Cleanup promiscuo/WiFi.** `RxGuard` (`:746-762`) e `stopRadio()`
  (`:186-193`) eseguono `esp_wifi_set_promiscuous(false)`, callback null,
  filtro null, canale 1, `WiFi.mode(WIFI_OFF)`. I pcap sono chiusi in
  `closeTargetPcaps()` (`:1122-1129`); il percorso di uscita PMKID è
  `guard.stop()` + `closeTargetPcaps()` (`:1528-1529`). Non ho trovato
  percorsi che lascino il promiscuo attivo.
- **Heap / buffer.** Statici dal map finale: `g_rxSlots` 0xC18 (3.096 B),
  `g_aps` 0x560 (1.376 B), `g_white` 0x3D8 (984 B), `g_targets` 0x138
  (312 B), `g_clients` 0x100 (256 B), `g_observations` 0x70 (112 B),
  `g_waterfall` 0xD0 (208 B): circa 6,3 KB, nessun buffer da 64 KB. Il
  callback non alloca; `std::string`/`std::vector` sono usati solo nel task
  principale. H2/H3/H5 restano da misurare su hardware.
- **Menu.** Quattro voci (`:2260-2263`), `runMenu()` bloccante con
  `MenuManager`; nessun problema di navigazione. R9 è P2-12.
- **Formato hc22000.** La riga `WPA*01*<32hex>*<12hex>*<12hex>*<ESSID
  hex>***\n` è costruita e validata (`:995-1034,1047-1064`); il path è
  `<bssid>.hc22000` senza `:`. Non ho potuto lanciare hashcat.
- **Pcap.** Header globale, linktype 105, snaplen 512, timestamp monotoni
  (millis), frame FCS-stripped, solo target. Il problema è il flush/error
  (P2-04), non il formato.

## Test manuali minimi consigliati (oltre al piano del review)

1. **P2/P6/P7/P13**: eseguire P2 su un AP, poi P6/P7/P13 sullo stesso BSSID;
   verificare che il `.hc22000` vecchio non resti (P1-02).
2. **W12/W13/W14**: whitelist quasi piena, errore di lettura, add; il file
   precedente deve restare intatto (P1-03).
3. **L2/L6**: monitor su un secondo ricevitore mentre si apre Client Monitor
   e Whitelist > A; nessun probe request prima dell'avviso (P1-01).
4. **C6/C7/C8**: 16 client, uno sparisce, avviso deauth lasciato aperto
   10-20 s; verificare aging e freschezza (P2-01).
5. **S3/S5**: burst poi silenzio; il peak deve decadere o essere resettabile
   (P2-02).
6. **P2 con traffico estraneo**: secondo handshake sullo stesso canale;
   verificare che l'M1 del target non venga scartato (P2-03).

## Limiti di questa verifica

- Nessuna build eseguita (vincolo del task); il binario finale è stato solo
  ispezionato, non rigenerato. La build pubblica `cardputer-adv` non ha un
  artefatto `.pio` nel tree finale: G5 non è verificato.
- Nessun hardware: PMKID, deauth, spettro, heap e cleanup sono giudicati per
  codice/artefatti; le prove RF restano manuali.
- `hashcat` non disponibile: la riga 22000 è validata solo per struttura.
- Le ipotesi RF (es. mancata installazione del MAC STA casuale
  sull'interfaccia, 8 ms tra auth e assoc) non sono state testate; vanno
  verificate su almeno 3 AP propri prima di dichiarare un difetto del modulo.
