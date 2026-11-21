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
  `led <r> <g> <b>` / `led off`, `msg <testo>`; tutto il resto viene scartato
  e registrato come ignorato.
- **Clipboard**: ultimo testo condiviso salvato in NVS (namespace
  `orbit_espnow`, chiave `clip`), invio broadcast e sync su richiesta
  (`ClipRequest` -> risposta unicast).
- **Peer**: announce periodico, scan passivo con nome, MAC completo, RSSI e
  secondi dall'ultimo frame visto. Canale configurabile (default 1), nome mesh
  configurabile.
- **Sicurezza di uscita**: `stopRadio()` esegue `esp_now_deinit()`,
  disabilita il promiscuo, chiude/elimina i trasferimenti parziali e ripristina
  la modalita' WiFi precedente (di norma `WIFI_OFF`). Una guardia RAII in
  `runMenu()` garantisce la pulizia su ESC, errori e uscita dal menu.
- **Avviso legale**: `ui::legalWarning()` viene mostrato una volta per sessione,
  prima di avviare la radio e quindi prima di qualsiasi frame.

## Protocollo (ESP-NOW, payload massimo 250 byte)

Header fisso di 16 byte, little endian:

| Offset | Campo | Note |
|---|---|---|
| 0 | magic | `0x4F` ('O') |
| 1 | versione | `1` |
| 2 | tipo | vedi tabella |
| 3 | flags | bit0 = no-ACK (broadcast), bit1 = ultimo blocco; per ACK = tipo ACK |
| 4..7 | session | id casuale del trasferimento |
| 8..9 | seq | indice blocco / seq ACK |
| 10..11 | len | lunghezza payload (0..200) |
| 12..15 | crc32 | CRC32 del solo payload |
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
  ha il CRC del payload; il file completo ha un CRC nel META e nel sidecar.
- **ACK**: ogni blocco inviato in unicast attende un ACK con la stessa
  `session` e `seq` (timeout 450 ms, 3 tentativi). Il ricevente risponde
  `StOk`, `StCrc`, `StIo`, `StSeq`, `StBusy` o `StSize`. I duplicati
  (seq gia' scritta) ricevono `StOk` senza riscrivere.
- **Fine trasferimento**: dopo l'ultimo blocco il ricevente chiude il file
  temporaneo, ricalcola CRC32 e dimensione, e solo se coincidono rinomina il
  file in `kInboxDir` e scrive il sidecar. Poi invia l'ACK finale (attesa
  massima 5 s). In caso di CRC errato il file viene rimosso e l'ACK finale e'
  `StCrc`.
- **Broadcast**: META inviato due volte, blocchi inviati in sequenza con
  pacing di 2 ms e flag no-ACK. Il broadcast e' best-effort: non c'e' conferma
  per peer; il ricevente verifica comunque il CRC prima di pubblicare.
- **Timeout ricezione**: 6 s senza nuovi blocchi -> file temporaneo chiuso e
  rimosso; nessun parziale compare in inbox.
- **Path temporaneo**: `/espnow/tmp/rx_<session>.part`; nome finale
  sanitizzato (solo `A-Z a-z 0-9 . _ -`, massimo 28 caratteri) e reso unico
  nella inbox.

## UI e tasti

Menu principale (header/footer ORBIT):

- `;` / `.` = su/giu', `ENTER` = apri, `ESC` = esci e spegne la radio.
- **Invia file**: browser SD (`ENTER` su cartella per entrare, `ESC` per
  tornare), poi destinatario. Durante l'invio `ESC` annulla; il peer scarta il
  parziale per timeout.
- **Inbox**: `ENTER` su un file = verifica CRC; "Cancella file..." chiede
  conferma.
- **Comandi**: Beep, LED verde, LED rosso, LED off, Messaggio..., storico.
- **Clipboard**: invia testo, mostra ultimo, sync dai peer.
- **Peer**: scan live (announce ogni 2.5 s), `;` / `.` scorrono se ci sono
  piu' di 7 peer, `ESC` torna.
- **Setup**: nome mesh, canale 1..13, info mesh (nome, MAC, canale, peer,
  heap), invio announce manuale.

## Percorsi e persistenza

- `/espnow/inbox/<file>` e `/espnow/inbox/<file>.crc` (sidecar: `CRC32 size`).
- `/espnow/tmp/` per i trasferimenti in corso.
- `/espnow/commands.txt`: storico comandi, ultime 60 righe, timestamp in
  secondi da boot.
- NVS namespace `orbit_espnow`: `name` (nome mesh), `ch` (canale), `clip`
  (ultima clipboard). Nessuna credenziale.
- La clipboard e' in NVS; non viene duplicata su SD.

## Test

Stato in questo worktree: build eseguita con successo; l'autotest del
protocollo e' incluso e viene eseguito all'ingresso del modulo. I test con
hardware reale (due Cardputer) non sono stati eseguiti per assenza di
dispositivi. Le procedure sotto sono quelle da seguire in laboratorio.

1. **Build**: `python -m platformio run -e cardputer-adv-offensive` -> OK
   (dimensione sotto).
2. **Autotest protocollo** (eseguito all'ingresso, prima della radio):
   vettore CRC32 noto `123456789` -> `0xCBF43926`; round-trip di un frame;
   rifiuto di frame troncato, magic errato e CRC payload errato. Se fallisce,
   il modulo non si apre.
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
5. **Comando non whitelisted**: inviare un frame `Command` con testo
   `reboot` da un peer di test. Il ricevente registra `RX ... reboot` come
   ignorato, mostra "Comando ignorato" e non esegue nulla.
6. **Comandi whitelisted**: `beep`, `led 0 220 80`, `led off`, `msg ...` ->
   esecuzione sul peer; storico aggiornato su entrambi.
7. **Clipboard**: inviare un testo da A; B mostra la notifica e salva il testo;
   "Sync dai peer" su B recupera il testo da A.
8. **Uscita**: ESC dal menu principale; verificare che la radio sia spenta
   (`WIFI_OFF` o modalita' precedente) e che non ci siano file `.part`
   residui in `/espnow/tmp/`.

## Dimensione firmware

Build `cardputer-adv-offensive` (include tutto ORBIT OS, non solo il modulo):

- `.pio/build/cardputer-adv-offensive/firmware.bin`: **2.426.096 byte**
  (2,31 MiB), 92,5% della partizione applicativa da 2.621.440 byte
  (~191 KiB liberi).
- RAM statica: **116.792 byte** su 327.680 (35,6%).
- Testo/dati ELF: 2.425.737 byte.
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
- **Nessuna ripresa**: un timeout o ESC scarta il parziale; non c'e' resume.
- **Comandi eseguiti solo con modulo aperto**: alla chiusura la radio viene
  spenta. La coda comandi in ricezione e' di 4 elementi.
- **LED**: usa `M5.Led` di M5Unified (WS2812 su Stamp-S3A, GPIO 21).
- **Storico comandi**: richiede la microSD; senza SD i comandi funzionano ma
  lo storico non viene scritto.
- **File**: massimo 512 KB, nomi sanitizzati a 28 caratteri, nessun path
  traversal (le barre diventano `_`).
- **Canale**: tutti i peer devono usare lo stesso canale; cambiarlo azzera la
  lista peer e riavvia ESP-NOW.
- **ENABLE_OFFENSIVE**: il modulo non implementa funzioni offensive e non
  dipende dal flag; le uniche azioni remote sono quelle whitelisted.

## File toccati

Solo `src/modules/espnow/`:
`EspNowMenu.cpp`, `EspNowMenu.h` (invariato), `EspNowProtocol.h`,
`REPORT.md`. Nessuna modifica a `main.cpp`, `platformio.ini`,
`ModuleFlags.h`, `Config.cpp`.
