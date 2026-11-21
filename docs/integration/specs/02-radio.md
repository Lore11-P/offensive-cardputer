# 02 — Radio: playlist SD, metadati ICY, registrazione

## Obiettivo
Estendere la web radio esistente con playlist personale, metadati e
registrazione su SD.

## Requisiti
- Playlist `/radio/stations.txt` sulla SD, una riga per stazione:
  `Nome|URL`. Supportare anche `Nome,URL`. Se il file manca, usare le 5
  stazioni attuali. Aggiungere voce "Ricarica playlist".
- Metadati ICY: implementare i callback weak `audio_showstreamtitle` e
  `audio_showstation` dell'ESP32-audioI2S; mostrare artista/titolo nella
  schermata di riproduzione, con scorrimento se lungo.
- Registrazione: tasto `R` durante la riproduzione, scrive un WAV su
  `/audio/rec/radio_YYYYmmdd_HHMMSS.wav`. Catturare il PCM decodificato con
  il callback weak `audio_process_i2s` (o `audio_process_extern`) senza
  bloccare il task audio: ring buffer piccolo in RAM + scrittura su SD nel
  loop principale. Intestazione WAV 16-bit stereo alla sample rate reale.
- Indicatore REC, spazio libero SD, stop con `R` o all'uscita.
- Non introdurre un secondo oggetto `Audio`: usare `audioout::decoder()`.
- Rilasciare sempre I2S/SD all'uscita.

## Accettazione
- Build OK.
- Con playlist SD, il menu mostra le stazioni del file.
- I metadati appaiono quando lo stream li fornisce.
- Il WAV registrato si apre sul PC e dura quanto la registrazione.
- `REPORT.md` con formato playlist, comandi e dimensione firmware.

## Attenzione
- Il decoder non ha TLS: usare stream HTTP.
- Non toccare `main.cpp`, `platformio.ini`, `ModuleFlags.h`.
- Puoi modificare `src/modules/radio/RadioMenu.cpp`, `src/utils/AudioOut.*`.
