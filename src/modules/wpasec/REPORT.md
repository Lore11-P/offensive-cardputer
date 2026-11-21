# 07 - WPA-SEC: report

## Cosa e' stato implementato

Modulo `src/modules/wpasec/` (solo `WpaSecMenu.cpp` e questo report; nessun
file condiviso toccato).

Menu: `Imposta chiave`, `Cancella chiave`, `Elenco catture`, `Carica catture`,
`Scarica potfile`, `Stato`, `Info`.

- **Chiave API**: editor a schermo mascherato, validazione rigorosa di 32
  caratteri esadecimali, salvataggio in `/wpasec/key.txt` (una riga, newline
  finale). La chiave non viene mai mostrata, loggata o scritta altrove; le
  risposte del server vengono ripulite e la chiave eventualmente riflessa e'
  sostituita con `[key]`. Cancellazione con conferma esplicita.
- **Elenco catture**: `/captures/*.hc22000` e `/captures/*.pcap`, con
  dimensione, stato upload (registro `/wpasec/uploaded.txt`, formato
  `nome|dimensione`) e stato password (`PW`, ricavato dal potfile). ENTER apre
  un dettaglio con tipo, dimensione, stato, numero righe hash e, se trovata, la
  password associata.
- **Upload**: HTTPS POST multipart/form-data in streaming, una cattura alla
  volta, scelta da elenco e conferma esplicita prima di qualsiasi invio. Il file
  non viene mai caricato interamente in RAM (chunk da 1 KB). Dopo un HTTP
  2xx il file viene marcato in `uploaded.txt`.
- **Potfile**: GET HTTPS, salvataggio in `/wpasec/potfile.txt` tramite file
  temporaneo e copia solo dopo validazione; le righe `WPA*...:password` sono
  indicizzate (impronta FNV-1a a 64 bit) per evidenziare le catture con
  password trovata.
- **TLS**: `WiFiClientSecure` con CA pinnata GTS Root R4 (Google Trust
  Services). Se la CA non valida o l'orologio non e' sincronizzato, viene
  mostrato un avviso esplicito (MITM, chiave API leggibile) e solo dopo
  conferma si usa `setInsecure()`; la schermata di lavoro mostra
  `TLS NON VERIFICATO`.
- **Heap gate**: prima di ogni connessione TLS richiede almeno 40 KB liberi e
  un blocco contiguo da 32 KB (`ESP.getFreeHeap()`, `ESP.getMaxAllocHeap()`);
  in caso contrario mostra i valori e non apre TLS.
- **Rete**: `wifisetup::ensureConnected()` per la connessione; NTP
  (`pool.ntp.org`, `time.google.com`, attesa max 8 s) per rendere verificabile
  il certificato; alla chiusura del modulo la radio viene spenta solo se non
  era gia' connessa all'ingresso.

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

Gestione risposta: timeout socket 20 s (handshake 12 s), redirect massimo 3
solo verso lo stesso host in HTTPS (per non far uscire la chiave dal dominio),
`Content-Length` oppure `Transfer-Encoding: chunked`, corpo di errore limitato
e ripulito prima della visualizzazione. Una risposta HTML o priva di righe
`WPA*` viene rifiutata per il potfile. Upload considerato riuscito con HTTP
200/201/202 e senza marcatori di errore/`invalid key` nel corpo.

## Limiti TLS e RAM

- Il progetto non include il cert bundle completo (peso nel firmware e nel
  runtime mbedTLS): e' pinnata la sola radice GTS Root R4. La catena attuale
  del servizio e' `stanev.org` <- WE1 <- GTS Root R4.
- La verifica del certificato richiede un orologio corretto: il firmware non
  aveva alcun SNTP, quindi il modulo sincronizza l'ora prima del primo uso.
  Se NTP o CA falliscono, si prosegue solo con conferma e `setInsecure()`.
- Gate: 40 KB liberi + 32 KB di blocco contiguo. `WiFiClientSecure` usa circa
  20 KB di buffer interni mbedTLS piu' lo handshake.
- Potfile: limite 4 MB per il download; l'indice delle password analizza al
  massimo 256 KB e 2048 impronte (oltre: `Scansione parziale`). Per ogni
  cattura si leggono al massimo 128 righe hash.
- Upload in streaming: nessun buffer dell'intero file; RAM costante.

## File su SD

```text
/wpasec/key.txt        chiave API, 32 hex (in chiaro: la SD e' rimovibile)
/wpasec/uploaded.txt   registro "nome|dimensione"
/wpasec/potfile.txt    ultimo potfile scaricato
/wpasec/potfile.tmp    file temporaneo, rimosso dopo la copia
```

## Test manuali suggeriti

1. `Imposta chiave` con 31 caratteri, con caratteri non hex e con 32 hex:
   solo l'ultimo deve essere salvato; verificare che la chiave non compaia nel
   monitor seriale ne' a schermo.
2. `Cancella chiave` e riavvio: `Stato` deve mostrare `Chiave: assente`.
3. `Scarica potfile` con chiave fittizia (32 hex casuali): il server deve
   rispondere con errore/HTML e il vecchio potfile deve restare intatto.
4. Con chiave valida e una cattura di test propria: upload, controllo della
   risposta HTTP e comparsa di `UP` nell'elenco; il file deve restare marcato
   dopo un riavvio.
5. Creare un potfile di prova con la riga hash di una cattura e una password
   finta: la cattura deve apparire `PW` e il dettaglio deve mostrare la
   password; nessuna chiave reale nel test.
6. Portare l'heap sotto la soglia (moduli aperti) e verificare il messaggio
   `Heap insufficiente` senza apertura TLS.
7. Uscire dal modulo con ESC: se la rete era stata accesa dal modulo, la radio
   deve spegnersi; se era gia' connessa, deve restare connessa.
8. Annullare upload e download con ESC: nessun file parziale in
   `/wpasec/` (il potfile precedente resta valido).

## Rischi e cose non fatte

- La chiave e' in chiaro su microSD rimovibile, come richiesto dal percorso
  `/wpasec/key.txt`: chi legge la SD la puo' leggere.
- Lo stato upload e' per `nome + dimensione`: una cattura diversa con lo stesso
  nome e la stessa dimensione risulterebbe gia' inviata.
- I `.pcap` non hanno un formato hash diretto: il match password usa il
  `.hc22000` con lo stesso nome, se presente; altrimenti la cattura non puo'
  essere marcata `PW` (l'upload resta possibile).
- Il potfile viene sostituito solo dopo download e validazione completati; se
  la copia finale sulla SD fallisce a meta', il file di destinazione puo'
  restare parziale.
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
- Dimensione `firmware.bin`: **2.412.432 byte** (2.30 MiB)
- Flash applicativa: 2.412.073 byte su 2.621.440 (92,0%)
- RAM statica: 115.560 byte su 327.680 (35,3%)
- File modificati: `src/modules/wpasec/WpaSecMenu.cpp`, `REPORT.md`.
- Non modificati: `main.cpp`, `platformio.ini`, `ModuleFlags.h`, `Config.cpp`.
