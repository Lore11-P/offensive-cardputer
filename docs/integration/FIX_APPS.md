# FIX_APPS — flasher affidabile e punteggi coerenti

Worktree `orbit-wt-fix-apps`, branch `review/fix-apps`, base `ec5a601`
(ORBIT OS 2.0.2). Chiude i finding **Must 1-3** e **Should 4-5** di
`docs/integration/review/05-apps.md`. Nessuna modifica a partizioni, OTA,
storage condiviso o moduli fuori area.

Hardware di riferimento: M5Stack Cardputer ADV, ESP32-S3FN8, 8 MB flash,
320 KB SRAM, nessuna PSRAM.

---

## 1. Modifiche

### 1.1 `flasher/flash.ps1` — otadata e baud

- Dopo la verifica dei binari e **prima** della scrittura, se non è stato
  passato `-Erase`, lo script esegue `erase-region 0xE000 0x2000`
  (`erase_region` con esptool 4). La regione è `otadata`: il bootloader,
  trovandola vuota, avvia `app0` (il firmware appena scritto) e non la
  vecchia `app1` rimasta da un OTA precedente. La NVS a `0x9000` non viene
  toccata; `app1` non viene riscritta né invalidata.
- Il baud rate predefinito passa da 1.500.000 a **460.800**, lo stesso
  valore dichiarato in `platformio.ini:22` per la USB-Serial-JTAG nativa.
  Nuovo parametro `-Baud` per override (es. `-Baud 115200`).
- `flasher/README.md`: documentati baud, `-Baud` e cancellazione otadata.

### 1.2 `Scores` — un solo backend, merge al primo mount

`src/modules/games/Scores.cpp` / `Scores.h`:

- `submit()` ora restituisce `bool` (`false` se il file non è leggibile per
  intero o se la scrittura fallisce); i giochi possono loggare il fallimento.
- Esportati `scores::preferSD()` (stessa funzione usata in scrittura) e
  `scores::mergeBackends()`.
- `mergeBackends()`, chiamata da `best()`, da `SdMenu::showScores()` e da
  `SdMenu::backupConfig()`: se la SD è montata e su LittleFS esiste
  `/scores.txt`, copia su SD i record mancanti o più alti (massimo per
  gioco). Il file SD viene riscritto solo se cambia qualcosa; la copia
  LittleFS non viene cancellata.
- Parser rifattorizzato in `parseEntry` (chiave qualsiasi) + `parseLine`
  (chiave nota) + `setEntry` (sostituisce/aggiunge una riga): stesso formato
  `gioco=valore`, ultima occorrenza vince, righe malformate preservate.

`src/modules/sd/SdMenu.cpp`:

- `showScores()` legge `/scores.txt` dal backend attivo
  (`scores::preferSD()`), non più forzato su SD, e mostra il backend nel
  footer (`SD` o `LittleFS`).
- `backupConfig()` chiama `mergeBackends()` prima di copiare i punteggi:
  `/backup/scores.txt` contiene anche i record scritti su LittleFS senza
  card.

### 1.3 Salvataggi solo a fine partita/ESC

- **Simon** (`Simon.cpp`): rimossa la `submit` a ogni livello; `best`
  aggiornato in RAM. Un solo `submit` alla morte o su ESC (sequenza,
  ripetizione, pausa, schermata finale); il ritorno viene controllato e
  loggato con `logger::warn`.
- **Pacman** (`Pacman.cpp`): rimossa la `submit` al completamento del
  livello (resta l'aggiornamento di `g_best`). Salvataggio a morte e su ESC,
  con controllo del ritorno.
- **Lander** (`Lander.cpp`): l'atterraggio aggiorna solo `pendingScore` e
  `best` in RAM; la scrittura avviene una volta sola su ESC o quando si
  lascia la partita (INVIO dopo WIN/GAME OVER per il nuovo terreno), con
  controllo del ritorno.
- **Snake** (`Snake.cpp`) e **Orbital** (`Orbital.cpp`): il ramo `Back` salva
  il punteggio se `score > 0`, come già facevano Tetris/Pacman.

Comportamento invariato per gli altri giochi (`submit` con valore di ritorno
ignorato compila senza warning).

---

## 2. Build

Comando (PlatformIO Core 6.2.0, `espressif32@6.7.0`):

```powershell
python -m platformio run -e cardputer-adv-offensive
```

Esito: **SUCCESS**, 1 ambiente, 524,78 s (8m44s).

| Voce | Valore | Note |
|---|---:|---|
| Flash PlatformIO | 2.698.581 B (95,8%) | `text + data` stimati sull'ELF |
| `firmware.bin` | **2.698.944 B** | dimensione reale del binario |
| Slot app OTA | 2.818.048 B (0x2B0000) | `partitions.csv` |
| Libero nello slot | **119.104 B = 116,3 KiB** | 4,2% dello slot |
| RAM statica | 147.392 / 327.680 B (45,0%) | `.data + .bss`, nessuna PSRAM |
| RAM statica libera | 180.288 B = 176,1 KiB | prima di stack e heap |
| Baseline `flasher/bin/firmware.bin` | 2.669.584 B | build 2.0.2 |
| Delta `firmware.bin` | **+29.360 B** | codice merge/parser e log |

Il firmware entra ancora nello slot con 119 KB di margine. Il delta include
anche le stringhe di log e i nuovi rami di uscita; il framebuffer da ~65 KB
resta invariato e non viene allocato staticamente.

---

## 3. Test

- **Host harness** (temporaneo, non committato): `Scores.cpp` compilato con
  MinGW 16.2 e uno storage finto in memoria. Verifica: submit senza SD su
  LittleFS; merge al mount; valore SD più alto non abbassato; submit scrive
  solo su SD senza duplicare la riga; record invariato non riscrive; merge
  ripetuto non riscrive. Esito: `scores_test: OK (4 scritture)`.
- **`flash.ps1`**: parsing PowerShell senza errori; con esptool 5.6.0
  installato il comando `erase-region ADDRESS SIZE` esiste ed è quello usato
  dallo script (`erase_region` per esptool 4).
- **Non testato su hardware**: nessun flash USB reale, nessun inserimento a
  caldo della microSD, nessuna misura dei tempi SD. I salvataggi a fine
  partita/ESC vanno provati a mano sui giochi toccati.

---

## 4. Rischi e limiti

- Ogni flash USB azzera `otadata`: dopo l'installazione il device parte da
  `app0` e lo stato OTA precedente è perso (è il comportamento voluto).
  `app1` resta la vecchia immagine finché un OTA non la riscrive.
- Il merge legge `/scores.txt` (max 4 KB) da entrambi i filesystem all'avvio
  di un gioco con SD montata; la scrittura avviene solo se un record cambia.
  Su SD lenta è una lettura in più all'ingresso del gioco, non nel loop.
- La copia LittleFS non viene cancellata: se la SD viene rimossa, i record
  più recenti (su SD) non sono visibili e si rilegge la copia interna, che
  può essere più vecchia.
- Lander salva all'uscita/nuova partita dopo un atterraggio: spegnere il
  device sulla schermata WIN senza premere INVIO/ESC perde quel record.
- Restano fuori scope i finding 6-11 (AI scacchi, redraw, musica, PCAP,
  viewer SD, metadata di release).
