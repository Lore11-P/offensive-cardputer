# 03 — Microfono: FFT, registratore WAV, voice memo

## Obiettivo
Sfruttare il microfono MEMS e il codec ES8311 della ADV.

## Requisiti
- Modulo `src/modules/audio/` con menu: Spettro FFT, Registra, Memo, Info.
- Spettro: barre FFT (256/512 punti) da `M5.Mic.record()`, senza allocazioni
  per frame; mostrare livello e picco.
- Registratore: WAV 16-bit mono 16 kHz su `/audio/rec/`, start/stop con
  ENTER, nome timestamp, indicatore REC e spazio SD.
- Memo: elenco file in `/audio/memo/`, playback con `M5.Speaker`, rinomina
  semplice e cancellazione con conferma.
- Gestire `M5.Mic.begin()` / `M5.Mic.end()` e `M5.Speaker.begin()/end()`:
  mai mic e speaker insieme; ripristinare lo speaker all'uscita.
- Buffer statici piccoli (max ~8 KB) e nessuna PSRAM.
- UI italiana, header/footer coerenti con ORBIT.

## Accettazione
- Build OK.
- La FFT reagisce a voce/fischio.
- Il WAV registrato è valido e riproducibile.
- I memo si possono riprodurre e cancellare.
- `REPORT.md` con sample rate, comandi, dimensione firmware.

## Attenzione
- Non toccare `main.cpp`, `platformio.ini`, `ModuleFlags.h`.
- Il modulo è esclusivo: se la radio sta suonando, fermarla prima.
