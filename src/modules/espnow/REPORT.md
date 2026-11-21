# 10 — ESP-NOW: file, comandi e clipboard mesh

Modulo: `src/modules/espnow/` (`EspNowMenu.cpp`, `EspNowProtocol.h`).
Entry point: `espnow::runMenu()`.
Build verificata: `cardputer-adv-offensive`.

## Cosa e' stato implementato

- **Invia file**: browser della microSD, scelta del destinatario (peer
  scoperto o broadcast), trasferimento a blocchi da 200 byte con META, ACK per
  blocco, ritrasmissione e timeout. Massimo 512 KB, streaming: il file non
  viene mai caricato interamente in RAM.
- **Inbox**: file ricevuti in `/espnow/inbox/`, elenco, verifica CRC32 contro
  il sidecar `.crc`, dettagli e cancellazione con conferma.
- **Comandi**: broadcast di comandi con storico in `/espnow/commands.txt`.
  In ricezione vengono eseguiti **solo** i comandi whitelisted `beep`,
  `led <r> <g> <b>` / `led off`, `msg <testo>` con matching **esatto**;
  `BEEP`, ` beep`, `beep\n` e tutto il resto vengono scartati e registrati
  come ignorati. I `msg` remoti sono limitati per peer e mostrati come banner,
  mai come modale bloccante.
- **Clipboard**: ultimo testo condiviso salvato in NVS (namespace
  `orbit_espnow`, chiave `clip`), invio broadcast e sync su richiesta
  (`ClipRequest` -> risposta unicast). Il testo identico non viene marcato
  dirty; `dirty` resta finche' `putString` non riesce, con backoff sui
  fallimenti. Rate limit per peer.
- **Peer**: announce periodico, scan passivo con nome, MAC completo, RSSI e
  secondi dall'ultimo frame visto. Canale configurabile (default 1), nome mesh
  configurabile. I trasferimenti sono vincolati al MAC del peer scelto.
- **SD serializzata**: un solo mutex per tutte le operazioni sul filesystem
  (callback, browser, inbox, storico, trasferimenti). Il callback WiFi scrive
  solo il blocco corrente e chiude il file; CRC finale, rename, sidecar e ACK
  finale sono differiti al task UI (`serviceFinalize`). T07.3 non puo'
  intrecciare due operazioni SD.
- **Sicurezza di uscita**: `stopRadio()` esegue `esp_now_deinit()`,
  disabilita il promiscuo, chiude/elimina i trasferimenti parziali, attende i
  callback e ripristina la modalita' WiFi precedente (di norma `WIFI_OFF`).
  I mutex vengono distrutti solo se la radio e' ferma e nessun callback e'
  attivo. Una guardia RAII in `runMenu()` garantisce la pulizia su ESC, errori
  e uscita dal menu.
- **Avviso legale**: `ui::legalWarning()` viene mostrato una volta per sessione,
  prima di avviare la radio e quindi prima di qualsiasi frame.

## Fix bug tester 10 (P0/P1)

- **P0 SD concorrente**: `g_sdMutex` unico; `SdGuard` su aperture, letture,
  scritture, liste, rename, sidecar e storico. CRC/rename/sidecar/finalize
  sono nel task UI (`serviceFinalize`), non nel callback WiFi. T07.3 non
  intreccia operazioni SD.
- **P1 MAC binding**: MAC target salvato in `beginTx`; `handleAck` scarta ACK
  da altri MAC; `handleData` scarta blocchi da MAC diverso da quello del META;
  cache dell'ultimo blocco legata al MAC. Inoltre il CRC copre
  type/flags/session/seq.
- **P1 uscita/finalize**: niente `delay()` nel callback; `abortReceiveForExit`
  attende i callback gia' entrati e chiude il parziale; i mutex vengono
  distrutti solo con radio ferma, callback a zero e nessun finalize in coda;
  `g_finalize` viene azzerato su ogni esito/errore.
- **P1 clipboard**: confronto prima del dirty; dirty azzerato solo dopo
  `putString` riuscita (backoff sui fallimenti); rate limit 1 s per peer.
- **P1 storico**: lettura della coda (16 KB), controllo degli errori, ultime
  60 righe; un errore non viene mostrato come "nessun comando".
- **P1 comandi**: whitelist a matching esatto (`BEEP`, ` beep`, `beep\n`
  rifiutati); rate limit per peer (300 ms generico, 3 s per `msg`); i `msg`
  sono banner non bloccanti.

## Protocollo (ESP-NOW, payload massimo 250 byte)

Header fisso di 16 byte, little endian:

| Offset | Campo | Note |
|---|---|---|
| 0 | magic | `0x4F` ('O') |
| 1 | versione | `2` |
| 2 | tipo | vedi tabella |
| 3 | flags | bit0 = no-ACK (broadcast), bit1 = ultimo blocco; per ACK = tipo ACK |
| 4..7 | session | id casuale del trasferimento |
| 8..9 | seq | indice blocco / seq ACK |
| 10..11 | len | lunghezza payload (0..200) |
| 12..15 | crc32 | CRC32 dei primi 12 byte + payload |
| 16.. | payload | massimo 200 byte |

Frame totali massimi: 216 byte. Tipi:

| Tipo | Valore | Payload |
|---|---|---|
| `Hello` | 1 | `[nameLen][name]` |
| `FileMeta` | 2 | `[nameLen][nome file][size u32][crc32 file u32][nameLen mittente][nome mittente]` |
| `FileData` | 3 | blocco da 200 byte (ultimo blocco piu' corto) |
| `Ack` | 4 | 1 byte stato; `flags` = kind (0 meta, 1 data, 2 final) |
| `Command` | 5 | `[nameLen][nome mittente][testo comando]` |
| `Clipboard` | 6 | `[nameLen][nome mittente][testo]` |
| `ClipRequest` | 7 | `[nameLen][nome mittente]` |

- **CRC32**: polinomio riflesso `0xEDB88320`, calcolato a blocchi. Ogni frame
  ha il CRC dei primi 12 byte dell'header (magic/versione/tipo/flags/session/
  seq/len) **piu'** il payload: type/flags/session/seq non sono piu'
  modificabili senza ricalcolare il CRC. Il file completo ha un CRC nel META e
  nel sidecar.
- **MAC binding**: `beginTx` salva il MAC del destinatario. Il mittente
  accetta ACK solo da quel MAC; il ricevente accetta blocchi solo dal MAC che
  ha aperto la sessione (META) e ignora duplicati/ACK di altri peer.
- **ACK**: ogni blocco inviato in unicast attende un ACK con la stessa
  `session` e `seq` (timeout 450 ms, 3 tentativi). Il ricevente risponde
  `StOk`, `StCrc`, `StIo`, `StSeq`, `StBusy` o `StSize`. I duplicati
  (seq gia' scritta) ricevono `StOk` senza riscrivere.
- **Fine trasferimento**: dopo l'ultimo blocco il callback chiude il file
  temporaneo e accoda la finalizzazione. Il task UI ricalcola CRC32 e
  dimensione, e solo se coincidono rinomina il file in `kInboxDir` e scrive il
  sidecar; poi invia l'ACK finale (attesa massima 10 s, inviato due volte a
  40 ms di distanza). In caso di CRC errato il file viene rimosso e l'ACK
  finale e' `StCrc`.
- **Broadcast**: META inviato due volte, blocchi inviati in sequenza con
  pacing di 2 ms e flag no-ACK. Il broadcast e' best-effort: non c'e' conferma
  per peer; il ricevente verifica comunque il CRC prima di pubblicare.
- **Timeout ricezione**: 6 s senza nuovi blocchi -> file temporaneo chiuso e
  rimosso; nessun parziale compare in inbox. All'uscita i `.part` vengono
  rimossi e i mutex distrutti solo dopo il deinit e l'attesa dei callback.
- **Path temporaneo**: `/espnow/tmp/rx_<session>.part`; nome finale
  sanitizzato (solo `A-Z a-z 0-9 . _ -`, massimo 28 caratteri) e reso unico
  nella inbox.
- **SD**: ogni accesso al filesystem del modulo passa da `g_sdMutex`
  (`SdGuard`): aperture, letture, scritture, liste, rename, sidecar e storico.
  Il callback non esegue CRC/rename/sidecar e non contiene `delay()`.

## UI e tasti

Menu principale (header/footer ORBIT):

- `;` / `.` = su/giu', `ENTER` = apri, `ESC` = esci e spegne la radio.
- **Invia file**: browser SD (`ENTER` su cartella per entrare, `ESC` per
  tornare), poi destinatario. Durante l'invio `ESC` annulla; il peer scarta il
  parziale per timeout.
- **Inbox**: `ENTER` su un file = verifica CRC; "Cancella file..." chiede
  conferma.
- **Comandi**: Beep, LED verde, LED rosso, LED off, Messaggio..., storico.
  I comandi ricevuti sono accettati solo con matching esatto; `msg` appare
  come banner (6 s) senza aprire schermate bloccanti. Storico: coda del file,
  ultime 60 righe; se illeggibile mostra un errore, non "nessun comando".
- **Clipboard**: invia testo, mostra ultimo, sync dai peer. Testo identico =
  nessun dirty/NVS; rate limit 1 s per peer.
- **Peer**: scan live (announce ogni 2.5 s), `;` / `.` scorrono se ci sono
  piu' di 7 peer, `ESC` torna.
- **Setup**: nome mesh, canale 1..13, info mesh (nome, MAC, canale, peer,
  heap), invio announce manuale.

## Percorsi e persistenza

- `/espnow/inbox/<file>` e `/espnow/inbox/<file>.crc` (sidecar: `CRC32 size`).
- `/espnow/tmp/` per i trasferimenti in corso.
- `/espnow/commands.txt`: storico comandi, ultime 60 righe, timestamp in
  secondi da boot. Lettura della coda (max 16 KB), non del solo inizio: un
  file grande non viene scambiato per "nessun comando". Se la coda non e'
  leggibile il file non viene sovrascritto.
- NVS namespace `orbit_espnow`: `name` (nome mesh), `ch` (canale), `clip`
  (ultima clipboard). Nessuna credenziale.
- La clipboard e' in NVS; non viene duplicata su SD. Un testo identico non
  marca dirty; dirty viene azzerato solo dopo un `putString` riuscito.

## Test

Stato in questo worktree: build eseguita con successo; l'autotest del
protocollo e' incluso e viene eseguito all'ingresso del modulo. I test con
hardware reale (due Cardputer) non sono stati eseguiti per assenza di
dispositivi. Le procedure sotto sono quelle da seguire in laboratorio.

1. **Build**: `python -m platformio run -e cardputer-adv-offensive` -> OK
   (dimensione sotto).
2. **Autotest protocollo** (eseguito all'ingresso, prima della radio):
   vettore CRC32 noto `123456789` -> `0xCBF43926`; round-trip di un frame;
   rifiuto di frame troncato, magic errato, CRC payload errato e **header
   alterato** (type/flags/session/seq). Autotest separato della whitelist:
   `beep` valido; `BEEP`, ` beep`, `beep `, `beep\n`, `MSG ciao` invalidi.
   Se fallisce, il modulo non si apre.
3. **File 10 KB (due Cardputer)**:
   - creare `/test10k.bin` da 10240 byte sulla SD del mittente;
   - Cardputer A: ESP-NOW > Invia file > selezionare il file > peer B;
   - attesi 52 blocchi, progresso, "File inviato e verificato";
   - su B: Inbox > il file compare come `test10k.bin`, "Verifica" mostra
     `CRC valido`.
4. **CRC errato**:
   - ricezione: inviare un file con CRC META alterato, oppure alterare un
     blocco e ricalcolare il CRC del frame (se il CRC del frame resta quello
     originale il frame viene scartato e ritrasmesso): il ricevente mostra
     "CRC errato: file rifiutato" e in inbox non compare nulla; il mittente
     riceve `StCrc`;
   - verifica locale: modificare un byte del file ricevuto con il modulo SD,
     poi Inbox > Verifica -> "File corrotto o modificato" (il sidecar non
     coincide).
5. **T07.3 / SD concorrente**: mentre B riceve un file, aprire su B
   Inbox > Verifica su un file da 512 KB e poi Inbox > Cancella file; in
   parallelo far partire un secondo invio da A. Atteso: nessun file corrotto,
   nessun `.part` visibile in inbox, CRC valido su tutti i file pubblicati,
   ACK/ritrasmissioni gestiti.
6. **MAC binding**: con un peer di test inviare un `FileData` o un `Ack` con
   session/seq corretti ma MAC sorgente diverso. Atteso: blocco ignorato,
   nessun ACK di conferma al MAC estraneo, nessuna scrittura sul file.
7. **Comando non whitelisted**: inviare frame `Command` con `reboot`, `BEEP`,
   ` beep`, `beep\n`. Atteso: nessuna esecuzione, registrazione come ignorato
   (o scarto per i caratteri di controllo), nessuna modale.
8. **Comandi whitelisted**: `beep`, `led 0 220 80`, `led off`, `msg ...` ->
   esecuzione sul peer; storico aggiornato su entrambi. Inviare 10 `msg` in
   2 s dallo stesso peer: rate limit 3 s, un solo banner/beep, nessuna modale.
9. **Clipboard**: inviare un testo da A; B mostra la notifica e salva il testo;
   inviare lo stesso testo: nessun dirty/NVS/beep. Inviare due testi diversi
   in meno di 1 s: il secondo e' scartato dal rate limit per peer. "Sync dai
   peer" su B recupera il testo da A.
10. **Storico grande**: portare `/espnow/commands.txt` oltre 16 KB; inviare
    un comando e verificare che restino le ultime 60 righe (non le prime) e
    che "Mostra storico" non dica "nessun comando".
11. **Uscita**: ESC dal menu principale; verificare che la radio sia spenta
    (`WIFI_OFF` o modalita' precedente), che non ci siano file `.part`
    residui in `/espnow/tmp/` e che i log `callback ancora attivo` non
    compaiano.

## Dimensione firmware

Build `cardputer-adv-offensive` (include tutto ORBIT OS, non solo il modulo):

- `.pio/build/cardputer-adv-offensive/firmware.bin`: **2.434.896 byte**
  (2,32 MiB), 92,9% della partizione applicativa da 2.621.440 byte
  (~182 KiB liberi).
- RAM statica: **117.040 byte** su 327.680 (35,7%).
- Testo/dati ELF: 2.434.529 byte.
- Il contributo del modulo e' nell'ordine di pochi KB: il grosso del binario
  e' il firmware preesistente (1.21.0 era ~2,21 MB).

## Limiti e rischi

- **Nessuna cifratura/autenticazione ESP-NOW**: i frame sono in chiaro e
  chiunque sia sul canale puo' inviare file, comandi o clipboard. La
  whitelist impedisce l'esecuzione arbitraria, ma `beep`, `led` e `msg`
  possono essere attivati da un peer non fidato. Usare solo in ambienti
  controllati.
- **Broadcast senza ACK**: il mittente non sa quanti peer hanno ricevuto il
  file; il CRC end-to-end fa scartare i file incompleti/corrotti.
- **Un trasferimento per volta in ricezione**: un secondo META riceve
  `StBusy` (se unicast). Il peer table ESP-NOW e' limitato a 12 peer + 
  broadcast; se e' pieno gli ACK possono fallire.
- **RSSI**: ricavato in modo passivo dai frame action vendor Espressif con
  promiscuo attivo solo nella schermata Peer. Puo' restare `--` finche' non
  arriva un frame; non e' calibrato. Da verificare su hardware che il
  promiscuo non disturbi il trasferimento (viene spento appena si esce dallo
  scan).
- **Nome peer**: e' un'etichetta, non un'identita' crittografica; due
  dispositivi possono usare lo stesso nome. L'identita' usata dal protocollo
  e' il MAC.
- **ACK finale perso**: il file puo' essere gia' stato pubblicato sul peer,
  ma il mittente mostra "conferma finale assente". Non e' una corruzione.
- **Finalizzazione nel task UI**: se il ricevente e' dentro un sottomenu
  (es. browser SD) la verifica CRC/rename attende; il mittente ha 10 s e poi
  mostra "conferma assente" ma il file viene pubblicato appena la UI torna
  al loop principale o all'uscita. Non e' una corruzione.
- **Lock SD unico**: una verifica Inbox da 512 KB puo' ritardare di qualche
  centinaio di ms la scrittura dei blocchi in arrivo; il mittente ritrasmette
  e il file resta valido. Non ci sono accessi SD fuori dal lock.
- **Nessuna ripresa**: un timeout o ESC scarta il parziale; non c'e' resume.
- **Comandi eseguiti solo con modulo aperto**: alla chiusura la radio viene
  spenta. La coda comandi in ricezione e' di 4 elementi; rate limit 300 ms
  generico per peer e 3 s per `msg`.
- **LED**: usa `M5.Led` di M5Unified (WS2812 su Stamp-S3A, GPIO 21).
- **Storico comandi**: richiede la microSD; senza SD i comandi funzionano ma
  lo storico non viene scritto.
- **File**: massimo 512 KB, nomi sanitizzati a 28 caratteri, nessun path
  traversal (le barre diventano `_`).
- **Canale**: tutti i peer devono usare lo stesso canale; cambiarlo azzera la
  lista peer e riavvia ESP-NOW.
- **ENABLE_OFFENSIVE**: il modulo non implementa funzioni offensive e non
  dipende dal flag; le uniche azioni remote sono quelle whitelisted.
- **Protocollo v2**: il CRC esteso e la versione 2 non sono compatibili con
  build v1; i due Cardputer devono usare lo stesso firmware.

## File toccati

Solo `src/modules/espnow/`:
`EspNowMenu.cpp`, `EspNowMenu.h` (invariato), `EspNowProtocol.h`,
`REPORT.md`. Nessuna modifica a `main.cpp`, `platformio.ini`,
`ModuleFlags.h`, `Config.cpp`.
