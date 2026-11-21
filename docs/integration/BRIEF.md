# ORBIT OS — integrazione 10 moduli

Questa cartella contiene il contratto di lavoro per l'integrazione dei 10 moduli.
Ogni agente lavora in un worktree separato e modifica **solo** i file della
propria area. I file condivisi sono già stati preparati dal supervisore.

## Hardware e limiti

- M5Stack Cardputer ADV: ESP32-S3FN8, 8 MB flash, **320 KB SRAM, nessuna PSRAM**.
- Slot applicativo OTA: **2.5 MB**. Il firmware 1.21.0 occupa ~2.21 MB: resta
  poco spazio. Preferire `-Os`, stringhe corte, buffer statici piccoli e dati su SD.
- Display 240x135, tastiera 56 tasti, IR **solo TX**, microfono MEMS, speaker,
  IMU BMI270, LED RGB (M5.Led), microSD su `/sd`, USB nativo.
- Niente moduli esterni: non usare RFID, CC1101, NRF24, GPS, 5 GHz, Ethernet.

## Build

```powershell
cd <worktree>
python -m platformio run -e cardputer-adv-offensive
```

La build deve passare. Riportare in `REPORT.md` la dimensione di
`.pio/build/cardputer-adv-offensive/firmware.bin` e l'heap statico se noto.

## Struttura e stile

- Un modulo = una cartella `src/modules/<nome>/` con `<Nome>Menu.h` e
  `<Nome>Menu.cpp`.
- Entry point: `namespace <nome> { void runMenu(); }`. Blocca fino a ESC,
  come gli altri moduli.
- Usare `MenuManager`, `ui::message`, `ui::confirm`, `ui::legalWarning`,
  `display::*`, `navigation::*`, `storage::*`, `config::*`.
- UI in italiano, testo breve, niente font di sistema: usare `display::text`,
  `display::header`, `display::footer`, `display::progressBar`.
- Non chiamare `delay()` in loop lunghi: usare `millis()` e `navigation::poll()`.
- Persistenza: namespace NVS dedicato (`orbit_<modulo>`), mai credenziali in
  chiaro se evitabile; file su SD con `storage::writeText`/`openWrite`.
- Prima di qualsiasi trasmissione RF: `ui::legalWarning(...)` e rispetto di
  `ENABLE_OFFENSIVE`. Le funzioni offensive devono essere no-op se il flag è 0.
- Liberare sempre WiFi/BLE/TLS/I2S all'uscita (deinit/stop/end), perché il menu
  principale continua a usare la UI e lo speaker.
- Non modificare `src/main.cpp`, `platformio.ini`, `include/ModuleFlags.h`,
  `src/utils/Config.cpp`, `src/modules/settings/SettingsMenu.cpp`: sono già
  predisposti. Se serve un cambiamento, scriverlo in `REPORT.md` e segnalarlo.
- Feature 1 può modificare `src/modules/ir/IrMenu.cpp` e i dati IR.
- Feature 2 può modificare `src/modules/radio/RadioMenu.cpp`,
  `src/utils/AudioOut.cpp` e `include/AudioOut.h`.
- Feature 3 crea solo `src/modules/audio/` e utility proprie.

## SD

Percorsi logici (la SD è montata su `/sd`):

- IR: `/ir/<marca>/<file>.ir` (LittleFS ha il fallback; leggere entrambi).
- Radio: `/radio/stations.txt` (una stazione per riga: `Nome|URL`).
- Registrazioni: `/audio/rec/<nome>.wav`, voice memo `/audio/memo/`.
- WPA-SEC: `/captures/*.hc22000`, chiave `/wpasec/key.txt`.
- WiFi Lab: `/wifilab/whitelist.txt`, `/wifilab/pmkid/`.
- Sensori: `/sensors/`.
- ESP-NOW: `/espnow/inbox/`, `/espnow/outbox/`.
- Utility: `/tools/notes.txt`, `/tools/calendar.txt`, `/tools/totp.txt`.

## Report

Ogni agente scrive `REPORT.md` nella propria cartella con:

1. cosa è stato implementato;
2. comandi/tasti;
3. cosa non è stato possibile e perché;
4. dimensione firmware;
5. test manuali suggeriti e rischi.

## Regole di sicurezza

- Non esfiltrare dati, non usare credenziali reali nei test.
- Le funzioni offensive vanno usate solo su reti proprie e con autorizzazione.
- Non committare segreti, chiavi API o file di cattura reali.
