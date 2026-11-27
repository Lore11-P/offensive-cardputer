# OrbitOS 3.0 — firmware ESP-IDF

Progetto ESP-IDF 5.4 puro (niente Arduino), C++17, per Cardputer ADV
(ESP32-S3FN8, 8 MB, niente PSRAM). La tabella partizioni resta quella del 2.x:
NVS (0x9000), LittleFS (0x570000) e i dati utente non vengono toccati.

## Moduli

- `orbit_app` — Home a 9 voci, Store, Giochi, Impostazioni, SD, WiFi setup
- `orbit_ui` / `orbit_hal` — display ST7789V2, tastiera TCA8418, batteria ADC
- `orbit_audio` — ES8311/I2S, MP3 (minimp3) e WAV, player album/cover
- `orbit_ble` — scanner e BLE spam (con avviso legale)
- `orbit_wifi` — scanner, deauth, beacon spam, evil twin + portal, canali
- `orbit_ir` — database Flipper `.ir`, RMT 38 kHz, jammer (con avviso)
- `orbit_tools` — orologio, note, calcolatrice, TOTP, QR, info, SD
- `orbit_chat` — chat BLE, `orbit_mascot` — Zorbo
- `orbit_store` / `orbit_wren` / `orbit_native_games` — store firmato e giochi

## Build

```powershell
powershell -ExecutionPolicy Bypass -File tools\idf.ps1 build
```

## Build offensiva (deauth abilitata)

IDF 5.4 blocca i frame deauth con un controllo chiuso in `libnet80211.a`.
Il tool `tools/patch_raw_tx.py` patcha l'ELF dopo la build e `esptool`
rigenera l'immagine con checksum e SHA-256 corretti:

```powershell
powershell -ExecutionPolicy Bypass -File tools\build_offensive.ps1
powershell -ExecutionPolicy Bypass -File tools\build_offensive.ps1 -Flash -Port COM5
```

Il binario patchato è anche in `C:\Users\utente\Desktop\opencode\orbit OS 3.0.bin`.
Un `idf.py build` normale produce un binario **senza** deauth: rifare la patch.

## Flash

```powershell
powershell -ExecutionPolicy Bypass -File tools\idf.ps1 -p COM5 flash
```

Il flash completo scrive bootloader, tabella, otadata e app; NVS e LittleFS non
vengono toccate. Per il solo aggiornamento dell'app:

```powershell
esptool --chip esp32s3 -p COM5 -b 460800 write_flash 0xe000 build\ota_data_initial.bin 0x10000 build\orbitos-patched.bin
```

## Test seriale

```powershell
powershell -ExecutionPolicy Bypass -File tools\idf.ps1 -DORBIT_DIAG_SELFTEST=ON build
```

La variante stampa `RESULT ... PASS/FAIL` per display, framebuffer, codec,
NVS, SD, LittleFS, playback MP3, scan WiFi + raw TX e scan BLE. Il riepilogo
dei test hardware è in `docs/TEST-3.0-SERIALE.md`.

## Avviso legale

Le funzioni offensive (deauth, beacon spam, evil twin/portal, BLE spam,
jammer IR) mostrano **sempre** `Avviso legale` prima di trasmettere e devono
essere usate solo su reti e dispositivi propri, in ambiente isolato. Vedi
`docs/AVVISO-LEGALE.md` e `COSA-NON-FATTO.md`.

## Struttura

- `main/` — entry point, selftest seriale, patch deauth
- `components/` — moduli sopra
- `partitions.csv` — tabella 2.x (M0–M2)
- `sdkconfig.defaults` — target S3, 8 MB, DIO 80 MHz, NimBLE, SoftAP

## Note ambiente

L'export PowerShell di IDF 5.4 richiede `ESP_IDF_SHELL=powershell.exe` e
Python 3.12; `tools/idf.ps1` imposta entrambi.
