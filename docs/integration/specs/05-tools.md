# 05 — Utility: clock, calendario, note, calcolatrice, QR, TOTP

## Obiettivo
Un modulo unico di strumenti quotidiani, tutto su SD/NVS.

## Requisiti
- Modulo `src/modules/tools/` con menu:
  - Clock: ora da NTP (WiFi salvata), aggiornamento manuale, data/ora grande.
  - Calendario: vista mese, eventi in `/tools/calendar.txt` (`AAAA-MM-GG|testo`).
  - Note: editor di testo semplice su `/tools/notes.txt`, salvataggio atomico.
  - Calcolatrice: espressioni `+ - * / % ( )` con precedenza, display.
  - QR: input testo -> QR a schermo (libreria `ricmoo/QRCode`), export BMP opzionale.
  - TOTP: segreti Base32 in `/tools/totp.txt` (`Nome|SECRET`), codice a 6 cifre,
    HMAC-SHA1 con mbedtls, countdown 30 s.
- Text input con `TextInput` esistente o editor locale; tastiera fisica.
- Nessun dato sensibile in log; TOTP su SD in chiaro solo se l'utente accetta.
- UI italiana, font pixel, schermate 240x135.

## Accettazione
- Build OK.
- Note e calendario sopravvivono al reboot.
- La calcolatrice dà risultati corretti su 10 espressioni di prova.
- Il QR è scansionabile da un telefono.
- Il TOTP coincide con un authenticator di riferimento.
- `REPORT.md` con formati file, comandi, dimensione firmware.

## Attenzione
- Non toccare `main.cpp`, `platformio.ini`, `ModuleFlags.h`.
- Niente librerie grafiche pesanti: QRCode è già in lib_deps.
