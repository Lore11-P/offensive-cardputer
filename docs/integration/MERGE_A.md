# Merge A — sensori, tools, IR, radio, audio

Worktree: `orbit-wt-merge-a`
Branch: `feat/merge-a`
Base: `feat/10-modules` (`c7456af`)
Build: `python -m platformio run -e cardputer-adv-offensive`

`docs/integration/ARCH.md` non esiste in questo worktree: la verifica è stata
fatta con `BRIEF.md`, le spec in `docs/integration/specs/` e i `REPORT.md` dei
moduli.

## Esito

Tutte e cinque le feature sono state unite con merge commit separati e la build
è passata dopo ogni merge. **Nessun conflitto** è stato rilevato: i branch
toccano file disgiunti, tranne `AudioOut` (solo feature 02).

| # | Feature | Commit feature | Merge commit | Conflitti | Build |
|---|---|---|---|---|---|
| 1 | sensors (08) | `843021c` | `cebe1d0` | nessuno | SUCCESS |
| 2 | tools (05) | `fd95041` | `0ea91dd` | nessuno | SUCCESS |
| 3 | ir (01) | `6cbd90a` | `cc5f6c8` | nessuno | SUCCESS |
| 4 | radio (02) | `f7216c9` + `14a26d5` | `7c9fd88` | nessuno | SUCCESS |
| 5 | audio (03) | `2bda7c3` | `f526bb0` | nessuno | SUCCESS |

Note sui branch:

- Tutti i branch avevano commit e `REPORT.md` nella propria cartella.
- Alla prima ispezione il worktree di `feat/radio` era dirty
  (`src/utils/AudioOut.cpp` modificato senza commit). Non è stato toccato;
  poco dopo è arrivato il commit `14a26d5` ("ring REC su heap e ciclo I2S con
  Audio") che ha reso il worktree pulito. Il merge `7c9fd88` include quindi
  **sia** `f7216c9` sia `14a26d5`, verificato con
  `git merge-base --is-ancestor 14a26d5 HEAD`.
- `feat/audio` non tocca `AudioOut`/`AudioOut.h`: nessun conflitto con la
  feature 02 ed `AudioOut` resta interamente nella versione di radio.

## Build per step

Misure PlatformIO sull'env `cardputer-adv-offensive` (slot 2.621.440 byte,
RAM 327.680 byte). Il primo build è a freddo, i successivi incrementali.

| Step | `firmware.bin` | Flash usata | RAM statica | Tempo |
|---|---:|---:|---:|---:|
| sensors | 2.370.656 B | 2.370.293 / 2.621.440 (90,4%) | 115.520 / 327.680 (35,3%) | 316,5 s |
| tools | 2.405.600 B | 2.405.229 / 2.621.440 (91,8%) | 115.736 / 327.680 (35,3%) | 184,1 s |
| ir | 2.410.272 B | 2.409.905 / 2.621.440 (91,9%) | 115.736 / 327.680 (35,3%) | 62,5 s |
| radio | 2.430.896 B | 2.430.525 / 2.621.440 (92,7%) | 116.064 / 327.680 (35,4%) | 64,0 s |
| audio | **2.461.488 B** | **2.461.125 / 2.621.440 (93,9%)** | **127.664 / 327.680 (39,0%)** | 37,9 s |

Delta rispetto al primo step: +90.832 byte di flash, +12.144 byte di RAM
statica. Il ring di registrazione radio (16.384 byte) non è più statico:
`14a26d5` lo alloca sull'heap solo durante la REC.

### Dimensione finale

- `firmware.bin`: **2.461.488 byte** (2,35 MiB).
- Flash applicativa: **2.461.125 / 2.621.440 byte = 93,9%**.
- Flash libera nello slot OTA: **160.315 byte** (~156,6 KiB).
- RAM statica: **127.664 / 327.680 byte = 39,0%**; liberi 200.016 byte, da
  cui vanno sottratti framebuffer (~64,8 KB allocato a runtime) e heap.

## File condivisi

Nessuna modifica, rispetto a `c7456af`, a:

- `src/main.cpp`
- `platformio.ini`
- `include/ModuleFlags.h`
- `src/utils/Config.cpp`
- `src/modules/settings/SettingsMenu.cpp`

Verificato con `git diff --stat c7456af..HEAD -- <file>` (output vuoto). Non
sono stati uniti ottimizzatori, Piano B partizioni né UI. Nessun push.

## Problemi aperti e rischi

1. **Test solo di compilazione**: nessuna delle cinque feature è stata provata
   su Cardputer ADV reale. Restano da eseguire i test manuali indicati nei
   rispettivi `REPORT.md`.
2. **Radio ↔ Audio (I2S)**: il ciclo è verificato solo sul codice di M5Unified
   0.3.0, non su hardware. `beginPlayback()` ferma il microfono se era attivo
   (interrompe una REC Audio in corso), riaccende il DAC ES8311 e reinstalla
   I2S0; `endPlayback()` lascia I2S0 libero per `M5.Mic.begin()`. Uso
   contemporaneo mic/radio impossibile (stesso periferico). Da provare:
   Radio → Audio → Radio, Mic attivo → Radio, beep UI dopo l'uscita.
3. **Flash al 93,9%**: restano ~156 KiB. Ulteriori moduli o tabelle grandi
   rischiano di non entrare; il prossimo gruppo di merge va misurato subito.
4. **Heap a runtime**: la REC radio richiede 16 KB oltre a framebuffer, WiFi,
   decoder e stack. C'è un controllo `ESP.getFreeHeap()` con margine 8 KB e
   `new (std::nothrow)`, ma il margine reale va verificato sul dispositivo.
5. **IR**: il database completo (2.255 file, 8,47 MB) sta sulla microSD
   (`/ir/<categoria>/<marca>/`) e **non è nel repository**; senza SD restano i
   307 file LittleFS. Un file oltre 64 KB non è apribile; NEC42, Samsung36 e
   Pioneer non vengono trasmessi come frame a 32 bit (avviso esplicito).
6. **Tools**: NTP, persistenza SD, scansione QR da telefono e confronto TOTP
   non provati su hardware. I segreti TOTP sono in chiaro su SD
   (`/tools/totp.txt`), protetti solo da conferma esplicita. Fuso orario
   fisso italiano.
7. **Sensori**: pedometro euristico, attivo solo con la schermata aperta;
   scritture NVS ogni 60 s; il verso della bolla può richiedere l'inversione
   di `nx`/`ny` su esemplari con IMU montato diversamente. Torcia senza
   `powertimer::Hold`: il timeout resta attivo e il LED viene spento prima
   della sospensione.
8. **Audio**: registratore/memo non provati su hardware; WAV 16 kHz mono,
   niente pausa/seek nel playback; senza RTC il nome file usa l'uptime;
   possibile buco tra blocchi con SD lenta.
9. **Radio**: ESP32-audioI2S 2.3.0 non invia `User-Agent` (test HTTP raw con
   200 OK, ma un 403 richiederebbe un URL alternativo o una patch della
   libreria); HTTPS costoso in RAM; sample rate variabile a metà stream
   produrrebbe un WAV con durata errata.

## Test minimi suggeriti prima del rilascio

1. Boot e navigazione di tutti i menu; nessun crash all'ingresso/uscita.
2. Sensori: livella, goniometro, 10 passi, torcia e spegnimento su timeout.
3. Tools: note/calendario su SD, QR letto da telefono, TOTP contro un
   authenticator, NTP con Wi-Fi salvata.
4. IR: categoria → marca → telecomando da SD, fallback senza SD, preferiti,
   ricerca e avviso sui protocolli non supportati.
5. Radio: playlist, ICY, REC 30 s con WAV valido, stop con ESC, heap che
   torna al valore pre-REC.
6. Audio: FFT a 1 kHz, REC con stop normale e con ESC, memo mono/stereo,
   rinomina/eliminazione.
7. Ciclo incrociato Radio → Audio → Radio e beep UI dopo ogni uscita.
