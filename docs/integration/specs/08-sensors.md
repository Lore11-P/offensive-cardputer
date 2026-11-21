# 08 — Sensori: livella, goniometro, pedometro, torcia LED

## Obiettivo
Usare IMU BMI270 e LED RGB già presenti.

## Requisiti
- Modulo `src/modules/sensors/` con menu:
  - Livella: bolla 2D, angoli X/Y, calibrazione zero.
  - Goniometro: angolo rispetto al piano, lock con ENTER, reset.
  - Pedometro: rilevamento passi da accelerometro, passi giornalieri in NVS
    (`orbit_sensors`), reset, grafico minuto.
  - Torcia: `M5.Led` colori (bianco, rosso, verde, SOS), luminosità 0-100,
    spegnimento all'uscita.
- Usare `M5.Imu.update()` e `M5.Imu.getAccel()/getGyro()`; frequenza 20-50 Hz.
- UI italiana, niente flicker, header/footer ORBIT.
- Nessun hardware esterno.

## Accettazione
- Build OK.
- La bolla segue l'inclinazione.
- Il pedometro conta 10 passi simulati.
- Il LED cambia colore e si spegne all'uscita.
- `REPORT.md` con assi, soglie, comandi, dimensione firmware.

## Attenzione
- Non toccare `main.cpp`, `platformio.ini`, `ModuleFlags.h`.
- Ripristinare il LED spento all'uscita.
