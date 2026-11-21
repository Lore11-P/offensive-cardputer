# ORBIT OS 2.0.0 — report di integrazione

## Riepilogo

- 10 feature nuove, 4 ottimizzatori, 10 bug tester, 2 merger, 2 supervisori.
- Trunk: `feat/10-modules`.
- Build `cardputer-adv-offensive`: **SUCCESS**.
- `firmware.bin`: **2.662.784 B**.
- Slot app 2.0: **2.818.048 B**, usati 2.662.417 B (**94,5%**), liberi **155.264 B**.
- RAM statica: **146.976 / 327.680 B (44,9%)**; framebuffer ~64,8 KB; heap residuo stimato ~115 KB.
- LittleFS 2.0: **2.621.440 B (2,5 MB)**, piena; il database IR completo è su SD.

## Feature

IR completo su SD, Radio+ (playlist/ICY/REC), Audio (FFT/REC/memo), BLE HID/Bad BLE, Utility (clock/note/calcolatrice/QR/TOTP), WiFi Lab (PMKID/client/spettro), WPA-SEC, Sensori (livella/goniometro/pedometro/torcia), WiFi+ (Responder/Karma/Auto-Portal), ESP-NOW (file/comandi/clipboard).

Menu a 7 hub: Rete, Bluetooth, Analisi RF, Media, Strumenti, Giochi, Sistema.

## Verifiche

- 10 report di bug testing in `docs/integration/tests/`.
- P0 chiusi: SMB2/overflow WiFi+, sicurezza BLE HID, race SD ESP-NOW, memoria IR.
- P1 chiusi: potfile/upload WPA-SEC, WAV/memo Audio, NTP/atomicità Tools, LED/pedometro Sensori, whitelist/legal gate WiFi Lab, TZ/decoder/REC Radio.
- Ottimizzazioni: ~−32 KB flash, pool fissi rete/BLE, UI senza ridisegni a riposo.
- **Nessun test su hardware reale**: RF, hashcat, BLE pairing, I2S/codec, SD lenta e consumo non sono stati validati. I report descrivono i test manuali da eseguire.

## Limiti noti

- IR: 1 file >64 KB rifiutato; NEC42/Pioneer non supportati; comandi raw >512 marcati e non inviati.
- WPA-SEC: upload reale non provato; `setInsecure()` solo su errore certificato, con avviso.
- WiFi+: Responder SMB2, Karma e portal provati solo staticamente.
- ESP-NOW: in chiaro, senza autenticazione; broadcast best-effort; un trasferimento in ricezione alla volta.
- BLE HID: Just Works + conferma esplicita sull'indirizzo; lo spoofing dell'indirizzo resta un rischio.
- RAM: 44,9% statica + framebuffer; non aggiungere altri buffer globali senza misurare.
- LittleFS piena: nuovi asset vanno su SD.

## Flash 2.0 (una tantum, USB)

La tabella partizioni cambia: OTA da sola non basta.

| File | Offset |
|---|---:|
| `bootloader.bin` | `0x0` |
| `partitions.bin` | `0x8000` |
| `firmware.bin` | `0x10000` |
| `littlefs.bin` | `0x570000` |

Usare `flasher\flash.ps1`, il web flasher (Chrome/Edge) o `esptool`. Dopo il flash staccare e riattaccare il cavo USB (la Cardputer parte in download mode dopo il reset esptool). Il vecchio LittleFS a `0x510000` non viene letto; il database IR completo è su SD.

## Artefatti

- `flasher/bin` e `flasher/web`: bootloader, partitions, firmware, littlefs 2.0.
- `E:\orbit\flash`: copia dei quattro binari + manifest per il flasher.
- `E:\downloads\offensive-cardputer-firmware-v2.0.0.bin`.
- `E:\ir`: 2.255 file `.ir` (8 categorie).
- SD: `/radio`, `/audio`, `/tools`, `/wifilab`, `/wpasec`, `/wifiext`, `/espnow`.
