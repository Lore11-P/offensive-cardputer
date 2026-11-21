# 05 — Utility: clock, calendario, note, calcolatrice, QR, TOTP

Stato: completato. Build `cardputer-adv-offensive` OK.

## Cosa e' stato implementato

Menu unico `Utility` (`src/modules/tools/ToolsMenu.cpp` + `ToolsMenu.h`) con:

- **Orologio** — ora locale da NTP (CET/CEST) usando la Wi-Fi salvata, oppure
  impostazione manuale di data e ora. Schermata grande con secondi.
- **Calendario** — vista mese con giorni evidenziati se hanno eventi, vista
  giorno e aggiunta eventi in `/tools/calendar.txt`.
- **Note** — editor di testo semplice su `/tools/notes.txt`, scrittura
  atomica (`tmp` + rename) con verifica di rilettura.
- **Calcolatrice** — espressioni con `+ - * / % ( )`, precedenza e unario;
  risultato in tempo reale dopo ENTER/`=`.
- **QR code** — input testo, QR a schermo con `ricmoo/QRCode`, export BMP
  opzionale in `/tools/qr.bmp` (screenshot 240x135).
- **TOTP** — segreti Base32 in `/tools/totp.txt`, codice a 6 cifre,
  HMAC-SHA1 con mbedtls, countdown di 30 s.

Nessuna modifica fuori da `src/modules/tools/`. Nessun log di segreti o
contenuti utente. Niente PSRAM: buffer statici piccoli (QR 211 byte).

## Comandi / tasti

Menu: `; . , /` o frecce, `ENTER` apre, `ESC` torna.

- **Orologio**: `ENTER` sincronizza NTP, `M` imposta data/ora, `ESC` esce.
  Inserimento manuale: due campi, `AAAA-MM-GG` e `HHMM`; `ENTER` conferma,
  `ESC` annulla.
- **Calendario**: `<`/`>` cambiano mese, `;`/`.` cambiano giorno, `ENTER`
  apre il giorno, `A` aggiunge un evento, `ESC` torna.
- **Note**: caratteri liberi, `ENTER` va a capo, `DEL` cancella (con
  auto-repeat), `ESC` salva ed esce. Salvataggio automatico ogni 15 s se ci
  sono modifiche.
- **Calcolatrice**: cifre e operatori, `ENTER` o `=` calcolano, `DEL`
  cancella, `ESC` esce.
- **QR**: `TextInput` per il testo (`ENTER` conferma campo, `ESC` annulla);
  nella schermata QR `S` salva il BMP, `ESC` esce.
- **TOTP**: `;`/`.` cambiano segreto, `A` aggiunge un segreto, `ESC` esce.

## Formati file (microSD, `/tools/`)

```
/tools/notes.txt
    testo libero, max 1024 byte. Accetta UTF-8: le lettere accentate
    vengono mostrate senza accento dal font pixel; il file resta invariato.
    Righe visuali da 32 caratteri; il firmware salva anche gli "a capo".

/tools/calendar.txt
    una riga per evento: AAAA-MM-GG|testo
    2026-11-21|Riunione
    2026-12-24|Cena

/tools/totp.txt
    una riga per segreto: Nome|SECRET_BASE32
    Demo|GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ
    (segreto di test RFC 6238, non reale)

/tools/qr.bmp
    export opzionale dello schermo QR, 240x135, BMP 24 bit.
```

I file sono letti/scritti solo sulla microSD. Note, calendario e TOTP
mostrano un errore esplicito se la SD manca. Orologio, calcolatrice e QR
funzionano anche senza SD (il QR non puo' essere esportato).

## Test eseguiti

### Calcolatrice — 10 espressioni

Eseguite con lo stesso `CalcExpr.h` compilato su host (g++), tutte OK:

| Espressione | Risultato |
|---|---|
| `(1+2)*3` | 9 |
| `10/4` | 2.5 |
| `2+3*4` | 14 |
| `50%7` | 1 |
| `(2+3)*(4-1)` | 15 |
| `-3*-4` | 12 |
| `100/10/2` | 5 |
| `2*(3+4)-5` | 9 |
| `1.5+2.25` | 3.75 |
| `((2+3)*2)/(4-1)` | 3.33333 |

Rifiutate correttamente: stringa vuota, `1+`, `(1+2`, `1/0`, `5%0`,
`2^3`, `abc`, `1..2`.

### QR — decodifica

Il QR e' stato renderizzato su host con la stessa libreria e lo stesso
layout del firmware (240x135, quiet zone 4 moduli, scala 2/3) e decodificato
con `cv2.QRCodeDetector`:

- `https://example.com/orbit` -> versione 2, letto correttamente;
- `ORBIT OS 2026 CARDputer utility test QR` -> versione 3, letto;
- 100 cifre -> versione 5, letto;
- 134 caratteri misti -> versione 6 con buffer da 211 byte, letto.

La scansione con un telefono reale resta da fare sul dispositivo; il test
host verifica matrice, quiet zone e dimensioni.

### TOTP — vettori RFC 6238 (SHA-1)

Base32 e troncamento dinamico verificati con gli stessi passaggi del
firmware (HMAC-SHA1 sostituito da un'implementazione host solo nel test):

| Unix time | Codice 6 cifre |
|---|---|
| 59 | 287082 |
| 1111111109 | 081804 |
| 1234567890 | 005924 |
| 2000000000 | 279037 |

Tutti coincidono con i vettori RFC 6238 (ultime 6 cifre).

### Build

```
python -m platformio run -e cardputer-adv-offensive
```

Esito: SUCCESS.

- `firmware.bin`: **2.399.808 byte** (2,29 MiB)
- Flash applicativa: 2.399.445 / 2.621.440 byte = **91,5%**
- RAM statica: 115.768 / 327.680 byte = **35,3%**
- Spazio libero nello slot OTA: circa **217 KiB**

## Note e limiti

- **Fix bug tester (P1/P2)**:
  - NTP: cleanup RAII (`WiFi OFF` + `sntp_stop`) su ogni uscita, esito
    `{Ok, Cancelled, Failed}`, sync automatico all'apertura se l'ora non e'
    valida, messaggio esplicito in caso di errore.
  - Atomicita': se manca il file principale, il modulo recupera
    `/tools/*.txt.bak`, rilegge e verifica il contenuto prima di rimuovere il
    backup. Le scritture passano da una verifica di rilettura. `Storage.cpp`
    non e' stato toccato (area condivisa): il fallback vive nel modulo.
  - Ora manuale: accettata solo da `kEpochValid` (15/11/2023) in poi; dopo
    `mktime` vengono controllati anno/mese/giorno/ora/minuto e dopo
    `settimeofday` si verifica che l'ora sia davvero valida.
  - QR: buffer statico portato a **211 byte** (griglia 41x41 della versione 6).
  - SD assente: errore esplicito per calendario, note, TOTP e export BMP.
  - Cap: eventi calendario 120, testo evento 70 caratteri, nome TOTP 20,
    segreto TOTP max 64, segreti TOTP max 16 (il 17° viene rifiutato con
    messaggio), note 1024 byte.
  - UTF-8: le lettere accentate provenienti dai file vengono trasliterate in
    ASCII per il font pixel; il contenuto su SD non viene modificato.
  - Auto-save Note: stato visibile (`salvato` / `modificato` / `!SALVA`) e
    retry ogni 15 s in caso di errore, senza perdere le modifiche.
- **Ricmoo QRCode non valida la lunghezza**: `qrcode_initText()` non
  restituisce errore se il testo supera la capacita' della versione scelta
  (TODO nella libreria). Il modulo seleziona la versione con una tabella di
  capacita' byte (ECC_LOW, versioni 1..6) prima di chiamare la libreria, per
  evitare overflow del buffer.
- QR limitato a 100 byte di testo e versione massima 6; con il limite attuale
  si arriva in pratica alla versione 5 (37 moduli, scala 2 px).
- L'editor Note e' volutamente minimale: aggiunge in coda, `DEL` cancella,
  `ENTER` va a capo; non ci sono frecce per spostare il cursore. Il file e'
  limitato a 1024 byte; file piu' grandi vengono rifiutati invece di essere
  troncati.
- Il calendario carica al massimo 120 eventi e ne mostra 7 per giorno; non
  prevede modifica/eliminazione degli eventi (solo aggiunta). Anni gestiti:
  2000–2099.
- TOTP: massimo 16 segreti, nome max 20 caratteri, segreto Base32 max 64
  caratteri. La SD non e' cifrata: il salvataggio in chiaro avviene solo dopo
  conferma esplicita. Senza orologio valido il codice non viene mostrato.
- NTP: usa le credenziali Wi-Fi gia' salvate; se mancano apre il flusso di
  configurazione rete. Timeout 10 s; Wi-Fi e SNTP vengono fermati anche su
  annullamento o errore.
- Fuso orario fisso italiano `CET-1CEST,M3.5.0,M10.5.0/3`.
- Il menu occupa 91,5% dello slot OTA: ulteriori moduli o tabelle grandi
  rischiano di non entrare. Il QR e le icone non aggiungono asset grafici.
- Non testato su hardware reale in questa sessione: NTP, persistenza su SD,
  scansione QR da telefono e confronto TOTP con un authenticator sono i test
  manuali consigliati.
