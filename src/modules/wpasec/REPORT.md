# 07 - WPA-SEC: report

## Cosa e' stato implementato

Modulo `src/modules/wpasec/` (solo `WpaSecMenu.cpp`, `REPORT.md` e le fixture;
nessun file condiviso toccato).

Menu: `Imposta chiave`, `Cancella chiave`, `Elenco catture`, `Carica catture`,
`Scarica potfile`, `Stato`, `Info`.

- **Chiave API**: editor a schermo mascherato, validazione rigorosa di 32
  caratteri esadecimali, salvataggio in `/wpasec/key.txt` (una riga, newline
  finale). La chiave non viene mai mostrata, loggata o scritta altrove; le
  risposte del server vengono ripulite e la chiave eventualmente riflessa e'
  sostituita con `[key]`. Cancellazione con conferma esplicita.
- **Elenco catture**: `/captures/*.hc22000` e `/captures/*.pcap`, con
  dimensione, stato upload (registro `/wpasec/uploaded.txt`, formato
  `nome|dimensione`) e stato password (`PW`). Il match usa il **BSSID**: il
  potfile reale del servizio ha righe
  `bssid:station_mac:ssid:password`; sono accettate anche righe `WPA*...` per
  compatibilita'. ENTER apre un dettaglio con tipo, dimensione, stato, numero
  righe hash e, se trovata, la password associata.
- **Upload**: HTTPS POST multipart/form-data in streaming, una cattura alla
  volta, scelta da elenco e conferma esplicita prima di qualsiasi invio. Il file
  non viene mai caricato interamente in RAM (chunk da 1 KB). La cattura viene
  marcata `UP` **solo** con un esito esplicito del server (accepted/queued/
  submitted/uploaded/success/...). Un 200 con pagina HTML, corpo troppo grande,
  corpo incompleto, errore o esito ambiguo lascia il file `LOCAL` e mostra il
  corpo della risposta.
- **Potfile**: GET HTTPS, scrittura su `/wpasec/potfile.new`, controllo di
  `getWriteError()` e della dimensione, validazione dei due formati e rename
  finale su `/wpasec/potfile.txt`. Un corpo di 0 byte viene salvato solo dopo
  conferma esplicita. Se il rename diretto non e' supportato dal filesystem si
  usa un `.bak` con ripristino: il vecchio potfile non viene distrutto da un
  download fallito. Le righe sono indicizzate per BSSID (fino a 2048 reti).
- **TLS**: `WiFiClientSecure` con CA pinnata GTS Root R4 (Google Trust
  Services). La CA viene **ritentata a ogni operazione**. Il fallback
  `setInsecure()` scatta solo per un errore di certificato/data (X.509 o
  `-0x7A00`), con avviso esplicito (MITM, chiave API leggibile) e conferma;
  DNS, TCP, timeout e OOM non attivano mai il fallback e mostrano l'errore
  mbedTLS reale. La schermata di lavoro mostra `TLS NON VERIFICATO` quando si
  usa il fallback.
- **Heap gate**: prima di ogni connessione TLS richiede almeno 40 KB liberi e
  un blocco contiguo da 32 KB (`ESP.getFreeHeap()`, `ESP.getMaxAllocHeap()`);
  in caso contrario mostra i valori e non apre TLS.
- **Rete**: `wifisetup::ensureConnected()` per la connessione; NTP
  (`pool.ntp.org`, `time.google.com`, attesa max 8 s) per rendere verificabile
  il certificato. Se il modulo ha toccato la radio (`prepareNetwork`), all'uscita
  chiama sempre `wifisetup::disconnect()` e `WiFi.mode(WIFI_OFF)`, anche dopo
  timeout, errori o annullamenti.

## Endpoint e formato richiesta

Host: `wpa-sec.stanev.org`, porta 443, solo HTTPS.

Upload:

```http
POST /?submit&key=<32-hex> HTTP/1.1
Host: wpa-sec.stanev.org
User-Agent: ORBIT-OS-WPASEC
Connection: close
Cookie: key=<32-hex>
Content-Type: multipart/form-data; boundary=----ORBITwpaSec<random>
Content-Length: <totale>

--<boundary>
Content-Disposition: form-data; name="file"; filename="<nome>"
Content-Type: application/octet-stream

<contenuto del file>
--<boundary>--
```

Potfile:

```http
GET /?api&dl=1&key=<32-hex> HTTP/1.1
Host: wpa-sec.stanev.org
User-Agent: ORBIT-OS-WPASEC
Connection: close
Cookie: key=<32-hex>
```

Formato potfile reale (una riga per rete trovata):

```text
bssid:station_mac:ssid:password
020000000001:020000000002:ORBIT-TEST:test-password
```

E' accettato anche il formato hashcat `WPA*...:password`; il BSSID e' il campo 3
della riga. Righe vuote e commenti (`#`) sono ignorati.

Gestione risposta: timeout socket 20 s (handshake 12 s), redirect massimo 3
solo verso lo stesso host in HTTPS (per non far uscire la chiave dal dominio),
`Content-Length` oppure `Transfer-Encoding: chunked`. Per l'upload si leggono
fino a 16 KB di corpo e si richiede che il corpo sia completo. Sono rifiutati i
documenti HTML (`<!doctype`, `<html`, `<head`, `<body`) e i marcatori di errore;
il successo richiede un marcatore esplicito. Un 200 con corpo vuoto o non
riconosciuto lascia la cattura `LOCAL`. Per il potfile il corpo deve contenere
almeno una riga `bssid:...:...:password` o `WPA*...`, non deve essere un
documento HTML e deve essere coerente con la dimensione scritta.

## Limiti TLS e RAM

- Il progetto non include il cert bundle completo (peso nel firmware e nel
  runtime mbedTLS): e' pinnata la sola radice GTS Root R4. La catena attuale
  del servizio e' `stanev.org` <- WE1 <- GTS Root R4.
- La verifica del certificato richiede un orologio corretto: il firmware non
  aveva alcun SNTP, quindi il modulo sincronizza l'ora prima del primo uso.
  Solo un errore di certificato/data puo' portare al fallback insicuro; un
  errore DNS/TCP/timeout/OOM viene mostrato e non viene mai declassato.
- Gate: 40 KB liberi + 32 KB di blocco contiguo. `WiFiClientSecure` usa circa
  20 KB di buffer interni mbedTLS piu' lo handshake.
- Potfile: limite 4 MB per il download; l'indice analizza al massimo 256 KB e
  2048 BSSID (oltre: `Scansione parziale`). Per ogni cattura si leggono al
  massimo 128 righe hash.
- Upload in streaming: nessun buffer dell'intero file; RAM costante.
- Il rename e' atomico dove il filesystem lo supporta (LittleFS/POSIX); su FAT,
  se il rename su file esistente viene rifiutato, si usa un `.bak` con
  ripristino. In quel caso c'e' una breve finestra non atomica.

## File su SD

```text
/wpasec/key.txt        chiave API, 32 hex (in chiaro: la SD e' rimovibile)
/wpasec/uploaded.txt   registro "nome|dimensione"
/wpasec/potfile.txt    ultimo potfile scaricato
/wpasec/potfile.new    file temporaneo, rinominato solo dopo validazione
/wpasec/potfile.txt.bak  backup temporaneo usato solo se il rename FAT lo richiede
```

## Fixture

In `src/modules/wpasec/fixtures/`:

- `potfile_colon.txt`: formato reale, incluso un SSID con `:` (la password e'
  l'ultimo campo).
- `potfile_wpa.txt`: stessa rete in formato `WPA*...:password`.
- `capture_sample.hc22000`: hash PMKID inventato con BSSID `020000000001`.
- `README.md`: istruzioni per il test locale.

Tutti i MAC sono localmente amministrati e i dati sono inventati; nessuna
credenziale o cattura reale.

## Test manuali suggeriti

1. `Imposta chiave` con 31 caratteri, con caratteri non hex e con 32 hex:
   solo l'ultimo deve essere salvato; verificare che la chiave non compaia nel
   monitor seriale ne' a schermo.
2. `Cancella chiave` e riavvio: `Stato` deve mostrare `Chiave: assente`.
3. `Scarica potfile` con chiave fittizia (32 hex casuali): il server deve
   rispondere con errore/HTML, `/wpasec/potfile.txt` deve restare intatto e
   `potfile.new` non deve restare.
4. Con chiave valida e una cattura di test propria: upload, controllo della
   risposta. Se il server non restituisce un esito riconoscibile, il file deve
   restare `LOCAL` e il corpo della risposta deve essere visibile. Solo con
   `accepted/queued/submitted/...` deve comparire `UP`.
5. Copiare `fixtures/capture_sample.hc22000` in `/captures/` e
   `fixtures/potfile_colon.txt` in `/wpasec/potfile.txt`: il file deve apparire
   `PW` e il dettaglio deve mostrare `test-password`. Ripetere con
   `potfile_wpa.txt`.
6. Portare l'heap sotto la soglia (moduli aperti) e verificare il messaggio
   `Heap insufficiente` senza apertura TLS.
7. Uscire dal modulo con ESC dopo un upload, un timeout o un annullamento: se
   il modulo ha aperto la radio, `WiFi.status()` deve essere disconnesso e la
   radio spenta. Se il modulo non ha toccato la rete, la connessione esistente
   resta.
8. Annullare download e upload con ESC: nessun file parziale in `/wpasec/` e
   il potfile precedente deve restare valido. Un potfile di 0 byte deve
   richiedere una seconda conferma.
9. Simulare un errore DNS/TCP (rete assente): il messaggio deve riportare
   l'errore reale e non deve comparire la richiesta di fallback insicuro.

## Rischi e cose non fatte

- La chiave e' in chiaro su microSD rimovibile, come richiesto dal percorso
  `/wpasec/key.txt`: chi legge la SD la puo' leggere.
- Lo stato upload e' per `nome + dimensione`: una cattura diversa con lo stesso
  nome e la stessa dimensione risulterebbe gia' inviata.
- I `.pcap` non hanno un formato hash diretto: il match password usa il
  `.hc22000` con lo stesso nome, se presente; altrimenti la cattura non puo'
  essere marcata `PW` (l'upload resta possibile).
- Il servizio puo' rispondere con un corpo vuoto o con testo non standard: in
  quel caso la cattura resta volutamente `LOCAL`; non c'e' un marker manuale
  per forzare `UP`.
- Il potfile viene sostituito solo dopo download, validazione, controllo di
  scrittura e rename; il fallback `.bak` su FAT ha una breve finestra non
  atomica, ma ripristina il vecchio file in caso di errore.
- Nessun upload automatico o in background; nessuna coda persistente.
- La CA pinnata puo' cambiare: in quel caso il modulo lo dice e chiede
  conferma per il fallback insicuro. Aggiornare `kRootCa` in
  `WpaSecMenu.cpp` quando il servizio cambia CA.
- Non e' stata fatta una prova reale contro il servizio con una chiave vera:
  niente credenziali reali in questo worktree. La build e' l'unica verifica
  automatica; i test 4-5 richiedono una chiave e una cattura proprie.

## Build

- Comando: `python -m platformio run -e cardputer-adv-offensive`
- Esito: **SUCCESS**
- Dimensione `firmware.bin`: **2.419.088 byte** (2,31 MiB)
- Flash applicativa: 2.418.729 byte su 2.621.440 (92,3%)
- RAM statica: 115.560 byte su 327.680 (35,3%)
- File modificati: `src/modules/wpasec/WpaSecMenu.cpp`, `REPORT.md`,
  `fixtures/`.
- Non modificati: `main.cpp`, `platformio.ini`, `ModuleFlags.h`, `Config.cpp`.
