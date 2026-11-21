# Feature 03 — Audio: spettro FFT, registratore WAV, voice memo

Modulo: `src/modules/audio/` (unico file di codice: `AudioMenu.cpp`).

## Cosa è stato implementato

- Menu `Audio` con quattro voci: **Spettro FFT**, **Registra WAV**,
  **Voice memo**, **Info e limiti**. UI in italiano, header/footer e palette
  Orbit, navigazione con `MenuManager`.
- **Spettro FFT**
  - FFT radix-2 a **512 punti** su cattura `M5.Mic.record()` di 512 campioni
    (32 ms a 16 kHz), finestra Hann, 26 barre logaritmiche da 100 Hz a 6 kHz.
  - Bin di 31,25 Hz: un tono a 1 kHz cade nel bin 32, verificato con
    `static_assert`.
  - Livello RMS, picco con hold/decadimento, indicatore di clipping, tacche
    dB e scala in Hz.
  - Buffer statici; nessuna allocazione nel loop di cattura o di disegno.
- **Registratore WAV**
  - WAV **PCM 16-bit mono 16 kHz** in `/audio/rec/`, 32.000 B/s, header
    44 byte.
  - Ping-pong di 2 buffer da 1024 campioni (64 ms): mentre uno viene scritto
    su SD l'altro è in cattura.
  - Header RIFF aggiornato ogni secondo e alla chiusura, anche con ESC a metà
    registrazione; short write gestiti con retry e stop pulito.
  - Start/stop con **ENTER**; ESC ferma e esce. Indicatore REC lampeggiante,
    tempo trascorso, byte scritti, spazio SD libero.
  - Stop automatico con meno di 512 KB liberi, errore di scrittura o limite
    di 60 minuti. Registrazioni sotto ~100 ms vengono scartate.
  - Nome file `REC_<timestamp>.wav`: data/ora se è disponibile un RTC, altrimenti
    secondi dall'accensione (`REC_00123.wav`), con suffisso `_2`, `_3`… se il
    nome esiste già.
  - I primi ~1,2 s di campioni dall'avvio del codec ES8311 vengono scartati se
    la registrazione parte subito (finestra di warm-up, vedi limiti).
- **Voice memo**
  - Elenco di `/audio/memo/*.wav` (max 40, durata letta dall'header).
  - Playback in streaming con `M5.Speaker.playRaw()` e 2 buffer statici da
    2 KB: nessun file intero caricato in RAM (i memo lunghi funzionano).
  - Supporta WAV PCM 16-bit mono o stereo, 8–48 kHz.
  - ENTER ascolta, ESC interrompe, **R** rinomina (editor `textinput::edit`),
    **X** elimina con conferma `ui::confirm`. Volume salvato e ripristinato.
- **Gestione esclusiva mic/speaker**
  - Prima di `M5.Mic.begin()`: `M5.Speaker.stop()` + `M5.Speaker.end()`.
  - Prima del playback: `M5.Mic.end()`, poi `M5.Speaker.begin()`.
  - All'uscita da ogni schermata e dal modulo: mic chiuso e
    `M5.Speaker.begin()` di ripristino. Fallimento del ripristino loggato.
  - Mic e speaker non sono mai accesi insieme.

## Comandi

| Schermata | Tasti |
|---|---|
| Menu Audio | `;`/`.` naviga, ENTER apre, ESC indietro |
| Spettro FFT | ESC esce |
| Registra WAV (pronto) | ENTER avvia, ESC esce |
| Registra WAV (REC) | ENTER ferma, ESC ferma ed esce |
| Voice memo | `;`/`.` naviga, ENTER ascolta, ESC indietro |
| Voice memo in ascolto | ESC/ENTER interrompe |
| Voice memo — rinomina | testo libero, ENTER conferma, ESC annulla |
| Voice memo — elimina | ENTER conferma, ESC annulla |

## Sample rate e formato

- Cattura, FFT e registratore: **16.000 Hz**.
- WAV: PCM lineare, 1 canale, 16 bit, byte rate 32.000 B/s, block align 2.
- Spettro: FFT 512 punti, bin 31,25 Hz; barre log 100 Hz–6 kHz.
- Buffer statici (BSS, nessuna PSRAM): ~11 KB totali
  (512 campioni FFT = 1 KB, re/im/finestra 6 KB, PCM ping-pong 4 KB).
- FFT/registratore non usano heap; il playback memo legge 2 KB alla volta.

## Build

Comando: `python -m platformio run -e cardputer-adv-offensive`

- Esito: **SUCCESS**, nessun warning.
- `.pio/build/cardputer-adv-offensive/firmware.bin`: **2.389.504 byte**.
- Flash: **2.389.133 / 2.621.440 byte (91,1%)**.
- RAM statica: **127.120 / 327.680 byte (38,8%)**.
- Rispetto allo stub iniziale (2.358.193 byte flash, 115.520 byte RAM):
  +30,2 KB flash, +11,6 KB RAM statica. Restano ~232 KB di flash nello slot
  OTA da 2,5 MB.

## Test manuali suggeriti

1. **Spettro**: fischio a ~1 kHz e voce; le barre devono reagire, il picco
   deve salire e decadere; in silenzio il livello resta al fondo scala.
2. **Registrazione**: 10 s con ENTER, stop con ENTER; copiare il file su PC e
   verificare con `ffprobe`/Audacity: 16 kHz, mono, 16-bit, durata coerente.
3. **ESC a metà REC**: fermare con ESC e verificare che il WAV sia ancora
   riproducibile (header finalizzato).
4. **Warm-up**: avviare REC entro 1 s dall'ingresso nella schermata e
   verificare che non ci siano secondi di silenzio in testa; fermare e
   riavviare subito (il mic resta inizializzato, nessuna pausa di 1 s).
5. **SD piena**: usare una card piccola o riempirla; la registrazione deve
   fermarsi e il file deve restare valido.
6. **Memo**: copiare in `/audio/memo` un WAV mono 16 kHz e uno stereo 44,1 kHz;
   ascoltare, interrompere con ESC, rinominare con R, eliminare con X
   (conferma e annulla).
7. **Ripristino speaker**: uscire da Audio e aprire Radio o Musica; il bus I2S
   dello speaker deve funzionare. Sul seriale compare
   `[FW][I] Audio: uscita, speaker ripristinato`.
8. **Ciclo Audio → Radio → Audio** dopo il merge della feature 02: verificare
   il limite I2S descritto sotto.

## Limiti e cose non possibili

- **Nessun RTC sulla Cardputer ADV**: il nome usa i secondi dall'accensione,
  non la data. Se un RTC è presente e abilitato viene usato automaticamente.
- **Mic e speaker condividono BCLK/WS** sull'ES8311: non possono funzionare
  insieme. Durante spettro e registrazione lo speaker è spento; durante il
  playback il mic è spento.
- **Limite I2S con Radio/Musica (AudioOut, feature 02)**: `audioout::decoder()`
  crea un canale su `I2S_NUM_0`, lo stesso usato da `M5.Mic`. Se il decoder è
  già stato creato, `M5.Mic.begin()` può fallire finché non si riavvia; dopo
  l'uso del mic il decoder può avere un handle I2S non più valido. Non è
  correggibile senza toccare `AudioOut` (vietato in questa feature). Il codice
  segnala l'errore all'utente e ripristina sempre lo speaker; il ciclo
  completo va verificato dopo il merge della 02.
- **SD obbligatoria** per registratore e memo. Senza microSD le voci mostrano
  un messaggio e non toccano il codec.
- **Nessuna PSRAM**: i buffer sono statici e piccoli; il playback è streaming,
  non viene mai caricato un memo intero.
- **Solo WAV PCM 16-bit** nel player memo (mono/stereo). MP3/AAC/8-bit/24-bit
  non sono supportati: per la musica c'è il modulo Musica.
- **Nessuna pausa/seek/controllo volume** nel playback memo: solo stop. Il
  volume è fissato a 190/255 e ripristinato all'uscita.
- **Coda di registrazione**: con SD molto lenta una scrittura >64 ms può
  lasciare un buco tra i blocchi; non c'è una seconda coda per la RAM.
- **Stop REC**: gli ultimi ~128 ms ancora in coda non vengono scritti.
- **File**: le registrazioni stanno in `/audio/rec` e non compaiono
  nell'elenco memo, che per specifica legge solo `/audio/memo`. Per ascoltarle
  dal modulo vanno copiate/spostate.
- **Lista memo** limitata a 40 file; i nomi sono limitati a 24 caratteri.
- **Registrazioni**: il file WAV non è cifrato; la SD FAT è leggibile da
  chiunque abbia la card.

## File modificati

- `src/modules/audio/AudioMenu.cpp` (implementazione completa)
- `src/modules/audio/REPORT.md` (questo file)

Nessuna modifica a `main.cpp`, `platformio.ini`, `ModuleFlags.h`,
`Config.cpp`, `AudioOut` o altri moduli.
