# 04 — BLE HID + Bad BLE

## Obiettivo
Far funzionare la Cardputer come tastiera Bluetooth e permettere l'esecuzione
di script DuckyScript via BLE.

## Requisiti
- Modulo `src/modules/blehid/` con menu: Tastiera BLE, Bad BLE, Layout, Info.
- Tastiera BLE: HID over GATT con NimBLE, nome `ORBIT-<nick>`, pairing,
  layout IT/US, invio di testo digitato sulla Cardputer a PC/telefono.
- Bad BLE: scegliere uno script da `/badusb` (SD, fallback LittleFS) ed
  eseguirlo via BLE. Riusare il parser di `BadUsbScript` se possibile,
  altrimenti implementare un parser Ducky minimo compatibile.
- Mostrare stato connessione, caratteri inviati, avanzamento, abort con ESC.
- `ui::legalWarning` prima dell'esecuzione; nessuna trasmissione senza
  conferma esplicita.
- Deinit NimBLE all'uscita per restituire RAM.
- Niente mouse, niente USB: solo BLE.

## Accettazione
- Build OK.
- La Cardputer viene vista come tastiera BLE e scrive su un PC.
- Uno script `hello.txt` viene eseguito.
- `REPORT.md` con nome dispositivo, comandi, limiti e dimensione firmware.

## Attenzione
- Non toccare `main.cpp`, `platformio.ini`, `ModuleFlags.h`.
- Puoi modificare `src/modules/badusb/BadUsbScript.*` solo per esporre il
  parser, mantenendo compatibile il modulo BadUSB USB.
