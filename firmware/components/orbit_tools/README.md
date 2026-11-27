# orbit_tools - Utility / Strumenti

Porta il modulo Utility di OrbitOS 2.x su OrbitOS 3.0 (ESP-IDF 5.4,
Cardputer ADV, ESP32-S3, 8 MB flash, senza PSRAM).

## Integrazione

```cpp
#include "orbit_tools/Tools.h"

orbit_tools::runMenu();
```

Menu `Strumenti`, parent `Home`:

- Orologio
- Note
- Calcolatrice
- TOTP
- QR code
- Info
- microSD

Nessuna modifica a `main/`, `orbit_app/`, `sdkconfig` o altri componenti.
Il chiamante deve aggiungere la voce "Strumenti" alla Home.

## Funzioni

### Orologio
- Ora e data dal clock di sistema (`time()` / `localtime_r`), fuso fisso
  italiano `CET-1CEST,M3.5.0,M10.5.0/3`.
- Se l'ora e' precedente al 2020-01-01 mostra `NON SINCRONIZZATO`: il modulo
  non avvia NTP, usa solo la sincronizzazione gia' fatta da WiFi/NTP.
- Uptime da `esp_timer_get_time()`.
- Cronometro con ENTER avvia/ferma, `L` registra il giro (tempo parziale),
  `R` azzera. Non e' persistente: uscendo si perde.

### Note
- Fino a 8 note da 64 caratteri ASCII stampabili, NVS namespace
  `orbit-tools`, chiavi `note0`..`note7`.
- Menu con snippet, "Nuova nota", "Modifica" / "Elimina".
- Editor locale a una riga: INVIO conferma, Backspace cancella, backtick
  annulla. Solo aggiunta in coda / backspace: nessun cursore mobile.
- La UI trasforma `; . , /` in frecce: gli editor li rimappano ai caratteri
  originali, quindi si possono digitare tutti i simboli ASCII della tastiera.
- Svuotare una nota e confermare la elimina.

### Calcolatrice
- Valutatore ricorsivo (nessuna allocazione pesante) con `+ - * / % ( )`,
  decimali e unario.
- Massimo 48 caratteri; risultato `%.6g`.
- Divisione o resto per zero: `DIV/0`, nessun NaN/Inf mostrato.
- INVIO o `=` calcola, Backspace cancella, backtick esce.

### TOTP
- RFC 6238 HMAC-SHA1, 6 cifre, periodo 30 s.
- Segreto Base32 (A-Z, 2-7, case-insensitive, spazi/`-` ignorati, padding
  `=` accettato solo in coda) validato prima dell'uso.
- Segreto salvato in NVS (`totp`) in chiaro, mai scritto nei log.
- Mostra codice, secondi residui e barra di avanzamento. `M` cambia segreto.
- Con orologio non sincronizzato non calcola codici.

### QR code
- Encoder MIT vendorizzato in `third_party/` (fork LovyanGFX di
  ricmoo/QRCode, gia' presente nel repo). Nessuna dipendenza Arduino.
- Versioni 1..4, ECC LOW, modalita' byte, massimo 78 byte.
- Rendering bianco/nero con quiet zone di 4 moduli, scalato per stare in
  240x135. Il buffer dei moduli (max 137 byte) viene liberato dopo il disegno.
- Nessun export BMP.

### Info
- Chip/modello/core/revisione, flash, heap libero e minimo, versione IDF, MAC
  WiFi STA.

### microSD
- Browser read-only di `/sd`: cartelle prima dei file, dimensione file,
  nessun delete/write.
- Monta la SD con `orbit_store::mountSd()` (stesso bus/percorso del resto del
  firmware).
- Limiti: 64 voci per cartella, path 160 caratteri, profondita' 10.
- Nessuna voce "..": ESC/backtick torna al menu della cartella precedente.

## File

```
include/orbit_tools/Tools.h   API pubblica
src/ToolsMenu.cpp             menu
src/Util.cpp                  NVS, textInput, helper UI
src/ClockUi.cpp               orologio + uptime + cronometro
src/NotesUi.cpp               note NVS
src/CalcExpr.h                parser espressioni
src/Base32.h                  decoder/validatore Base32
src/CalcUi.cpp                calcolatrice
src/TotpUi.cpp                HMAC-SHA1 + TOTP
src/QrUi.cpp                  rendering QR
src/InfoUi.cpp                informazioni sistema
src/SdBrowserUi.cpp           browser SD
third_party/lgfx_qrcode.c/.h  encoder QR (MIT)
third_party/qrcode_alloca.h   wrapper alloca portabile
tests/host_test.cpp           test host di calcolatrice e Base32
```

## Memoria e sicurezza
- Nessun buffer statico grande; il QR usa un `std::vector` locale.
- Note e segreto TOTP sono in chiaro nella NVS: non usare segreti reali se
  la flash non e' considerata attendibile.
- Il segreto TOTP non viene loggato ne' mostrato nella schermata del codice.
- Il testo delle note e' limitato a 64 caratteri e filtrato su ASCII
  stampabile in visualizzazione.

## Test e limiti
- Sorgenti C++ e C compilati in oggetto con il cross-compiler IDF 5.4
  (stessi flag del firmware): nessun errore.
- Test host eseguito con MinGW:
  ```
  g++ -std=c++17 -Wall -Wextra -I src tests/host_test.cpp -o host_test
  ./host_test
  ```
  Copre le 10 espressioni della calcolatrice, gli errori di sintassi,
  divisione/resto per zero e la decodifica Base32 (vettore RFC, lowercase,
  spazi, padding e casi invalidi).
- QR verificato su host: matrice generata con lo stesso encoder vendorizzato
  e decodificata con OpenCV per versioni 1..4 (fino a 78 byte).
- Vettori TOTP RFC 6238 attesi (segreto ASCII `12345678901234567890`):
  59 -> 287082, 1111111109 -> 081804, 1234567890 -> 005924,
  2000000000 -> 279037. Verificati con HMAC-SHA1 host; il percorso mbedtls
  non e' stato eseguito su hardware.
- Non e' stato eseguito `idf.py build` ne' test su hardware in questa sede.
- Fuso orario fisso italiano; nessuna impostazione manuale dell'ora.
- QR versioni 3-4 usano scala 2 px: su 240x135 la scansione funziona nei
  test host, ma su hardware serve buona luce e una distanza ragionevole.
- L'SD browser dipende da `orbit_store::mountSd()`; se un altro codice monta
  `/sd` fuori da `orbit_store`, il mount puo' fallire. Un mount fallito puo'
  lasciare il bus SPI inizializzato: in tal caso riavviare il dispositivo.
- Nessun modulo sensori/IMU.
