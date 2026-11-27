# OrbitOS 3.0 — architettura (bozza da approvare)

Data: 2026-11-26. Base: ESP-IDF 5.4, Cardputer ADV (ESP32-S3FN8, 8 MB flash,
512 KB SRAM, **niente PSRAM**). Prodotto: **OrbitOS**, non un fork di NucleoOS.
Nessun codice NucleoOS copiato: architettura reimplementata (clean-room).

## 0. Decisioni prese

- Base **ESP-IDF 5.x puro** (niente Arduino, niente PlatformIO per il firmware nuovo).
- Interfaccia: **solo la Home resta identica**; Store, Giochi e Impostazioni
  possono essere ridisegnati.
- Primo rilascio con **Home, Store, Giochi, Impostazioni** soltanto.
- Store **nostro, firmato**, catalogo nostro, aperto a terzi (catalogo centrale,
  revisione e firma nostra).
- **Due canali dal giorno 1**: app script (Wren) e app native (ELF Xtensa PIC).
- Solo **Cardputer ADV**.
- Ripartizione flash consentita; **tutti i dati utente preservati**.
- Primo traguardo: **poche app funzionanti + store**.

## 1. Vincoli

| Risorsa | Vincolo |
|---|---|
| Flash | 8 MB, due slot OTA + dati + scratch per le app native |
| RAM | ~512 KB SRAM, no PSRAM; heap utile ~200 KB al boot |
| Display | 240×135, RGB565, double buffer |
| Tastiera | TCA8418 su I2C (ADV), non matrice |
| SD | SPI condiviso, FAT32 |
| Store | app su SD, una app nativa residente alla volta |
| Rete | Wi-Fi STA per catalogo/download; niente TLS pesante durante i giochi |

## 2. Struttura del repository

```
firmware/                     nuovo progetto ESP-IDF
  CMakeLists.txt
  sdkconfig.defaults
  partitions.csv
  main/
  components/
    orbit_core/               log, eventi, task, memoria, tempo
    orbit_hal/                display, tastiera, SD, audio, power, IMU
    orbit_ui/                 Home, Store, Giochi, Impostazioni, widget, tema
    orbit_registry/           app installate, impostazioni, alias, recenti
    orbit_store/              catalogo, download, verifica, install/update
    orbit_pkg/                formato pacchetto, manifest, firma
    orbit_script/             runtime Wren + binding sandbox
    orbit_native/             loader ELF PIC, MMU, ABI, export table
    orbit_ota/                OTA A/B + rollback
  sdk/native/                 header, linker script, tabella export
store/                        catalogo (JSON + pacchetti) e tool di firma
tools/                        build/sign/catalog, test host
legacy/                       vecchio firmware Arduino, consultabile
```

`src/` e `platformio.ini` restano finché il nuovo firmware non raggiunge il
primo traguardo: servono come fallback e come sorgente della UI da portare.

### 2b. Mappa di porting dal firmware 2.x

| Componente | Azione | Note |
|---|---|---|
| `core/Display.*` | port piccolo | `M5Canvas→LGFX_Sprite`, `M5Cardputer.Display→LGFX`; disegno invariato |
| `core/MenuManager.*` | port piccolo | solo `millis/delay` + navigation |
| `OrbitIcons` + `PixelFont` + `OrbitPalette` | **riuso as-is** | primitive LGFX e tabelle, nessun asset |
| `core/Orbit.cpp` | port piccolo | cache pianeta `M5Canvas→LGFX_Sprite` |
| `core/Navigation.*` + `NavCore` | port driver | logica tasti invariata; driver TCA8418 nuovo |
| `core/PowerTimer.*` | port piccolo | LEDC, ADC batteria, wake tastiera |
| `utils/Config.*` | port piccolo | `nvs_flash`; stessi namespace/chiavi |
| `utils/Storage.*` | port/rewrite | `esp_littlefs` + `sdspi`; stesse regole path |
| `games/Scores.*` | port piccolo | stesso `/scores.txt` |
| 5 giochi nativi + menu | port piccolo | tempo/random/font; logica e disegno invariati |
| `store/StoreClient` | rewrite I/O | `esp_http_client` + TLS + cJSON; schema invariato |
| `store/GameRuntime` + `lib/wren` | port | Wren è C puro; togliere NimBLE/audio, implementare `beep` |
| `utils/WifiSetup.cpp` | port medio | `esp_wifi` + NVS, namespace `cpfw-net` |
| `utils/OtaUpdate.cpp` | drop, tenere helper | CA/allowlist/clock/verifyBlob servono allo Store |
| `modules/settings/*` | rewrite piccolo | solo luminosità/rotazione/tema/sleep/WiFi |
| Zorbo, IR, BLE, Radio, Music, Sensori, Tools, ecc. | drop | fuori dai 4 menu del 3.0 |
| `lib/wren` | **riuso as-is** | C99 puro, nessun include Arduino/ESP |

### 2c. Dati da preservare (M1)

| Dato | Dove | Chiavi/formato |
|---|---|---|
| Impostazioni | NVS namespace `cardputer-fw` | `bright,rot,theme,mods,ble_dur,…,sleep_s` |
| WiFi | NVS namespace `cpfw-net` | `ssid,pass` |
| Mascot/Zorbo | NVS namespace `orbit-mascot` | `score,aff,evt,pts` (opzionale) |
| Config JSON | LittleFS `/config.json` | v1, import solo al primo boot |
| Punteggi | `/scores.txt` (SD o LittleFS) | `game=score\n`, ultimo vince |
| Giochi installati | SD `/games/*.wren` | id `[a-z0-9_-]{1,24}`, max 8 KB |
| Firmware precedente | `.pio/build/cardputer-adv-serlog/` | bootloader + partitions + app |

NVS resta a 0x9000/0x5000: i namespace si leggono direttamente con
`nvs_get_*`, senza export. LittleFS resta a 0x570000 in M0–M2.

## 3. Registry

File `/system/registry.json` su LittleFS, scrittura atomica (`tmp` + rename),
checksum e backup `.bak`.

```json
{
  "version": 1,
  "apps": [
    {"id":"2048","version":"1.2","channel":"script","abi":1,
     "path":"/apps/2048","enabled":true,"installed_at":1760000000}
  ],
  "settings": {"theme":0,"rotation":1,"brightness":128},
  "recent": ["2048"]
}
```

Il registry è l'unica fonte per Giochi e Store. Le app non sono mai elencate
da una scansione cieca della SD: si mostra solo ciò che è stato installato e
verificato. La scansione serve solo alla migrazione iniziale.

## 4. Formato pacchetto `.oap`

Contenitore semplice, deterministico, senza ZIP:

```
magic "OAP1" (4)
u32 manifest_len
manifest JSON (UTF-8)
u32 payload_len
payload
firma ECDSA P-256 (64 byte, r||s) su SHA-256 di tutto quanto sopra
```

Manifest:

```json
{
  "schema": 1,
  "id": "2048",
  "name": "2048",
  "version": "1.2.0",
  "channel": "script",
  "abi": 1,
  "arch": "esp32s3",
  "entry": "main.wren",
  "min_os": "3.0.0",
  "desc": "Unisci le tessere.",
  "author": "OrbitOS",
  "permissions": ["display", "input", "audio"],
  "icon": "icon.bmp",
  "payload_sha256": "…"
}
```

- `channel`: `script` (payload Wren) o `native` (payload ELF).
- Limiti: manifest ≤ 8 KB, script ≤ 16 KB, nativo ≤ 256 KB, totale ≤ 512 KB.
- Firma: **ECDSA P-256 / SHA-256**, la stessa già usata dallo store attuale
  (`tools/sign_ota.py`); chiave privata fuori dal repository.
- Chiavi pubbliche nel firmware: corrente + prossima (rotazione).
- Icona: BMP RGB565 24×24 (o 32×32), dentro il payload.

Installazione atomica:

1. download in `/apps/.tmp/<id>.oap`;
2. verifica dimensione, SHA-256, firma, schema, `arch`, `abi`, `min_os`;
3. estrazione in `/apps/<id>.new`, poi rename su `/apps/<id>`;
4. aggiornamento registry (tmp + rename + checksum).

Su errore non si tocca l'app installata. Un'installazione interrotta lascia
solo `.tmp` e viene ripulita al boot.

## 5. Store

Catalogo pubblico: `catalog.json` + `catalog.sig` (stessa firma ECDSA).
Campi per app: id, nome, versione, canale, abi, size, sha256, url, icona,
permessi, min_os, descrizione, autore.

- Due schede: **Script** e **Native**.
- Operazioni: installa, aggiorna, disinstalla, dettagli, permessi.
- Rete: `esp_http_client` + TLS verso URL in allowlist (GitHub raw/Pages);
  timeout e limite di dimensione.
- Cache del catalogo su SD: lo Store funziona offline per le app installate.
- Pubblicazione: PR/zip al repository del catalogo; la CI ricompila i moduli
  nativi dalla sorgente, esegue i test, il maintainer firma. In futuro chiavi
  per autore + trust on first use.
- La firma è obbligatoria: niente app non firmate, nemmeno in sideload (in
  alternativa un "developer mode" esplicito, con avviso).

## 6. Runtime script (Wren)

- Wren 0.4 già presente (`lib/wren`), sandbox: niente `import`, filesystem,
  rete o moduli di sistema.
- Binding identici a quelli attuali: display, input, audio, tempo, random,
  `Game` preludio. Le app script esistenti (`/games/*.wren`) restano valide.
- Esecuzione in un task dedicato (stack 24–32 KB), con gate di memoria e
  messaggio "Memoria insufficiente" invece del reboot.
- La UI non viene distrutta: l'app disegna sulla superficie applicativa; il
  framebuffer di sistema resta double-buffered. Niente release/restore.
- Migrazione: i 5 `.wren` esistenti diventano pacchetti script firmati.

## 7. Runtime nativo (ELF PIC)

Formato payload: ELF32 Xtensa `ET_DYN`, PIC, esattamente tre segmenti:

| Segmento | Flag | Contenuto | A runtime |
|---|---|---|---|
| text | R+X | codice, PLT, literal pool | flash scratch mappata sul bus istruzioni |
| rodata | R | costanti, stringhe, jump table | stesse pagine, bus dati |
| data | RW | GOT, init/fini, `.data`, `.bss` | blocco heap allocato al load |

Loader (`orbit_native`):

1. riverifica firma e SHA-256 del pacchetto;
2. `elf_inspect`: ELF32 LE, tre segmenti, nessun `DT_NEEDED`, reallocazioni
   ammesse, import tutti nella tabella export;
3. mappa text+rodata dalla partizione `orbit_nap` con `esp_mmu_map` (bus
   istruzioni; alias bus dati per le costanti);
4. alloca `.data`/`.bss` sull'heap, applica le reallocazioni;
5. esegue `orbit_app_main()`, che riceve il puntatore all'API e ritorna per
   uscire;
6. all'uscita esegue i distruttori, smonta la mappatura e libera la RAM.

ABI v1 (C, nomi stabili, solo crescita):

- `orbit_gfx_*` (dimensione, clear, pixel, rect, testo, sprite, present)
- `orbit_input_*` (poll evento, stato tasti)
- `orbit_time_*`, `orbit_log`, `orbit_random`
- `orbit_fs_*` limitato a `/apps/<id>/data`
- `orbit_audio_*` (beep/tone)
- libc/libm/libgcc ammessi con una allowlist

Fingerprint del layout (versione M5GFX/LovyanGFX e offset dei tipi condivisi)
per rifiutare moduli compilati contro header incompatibili.

Una sola app nativa residente. Nessun reboot per l'uso normale; se un gioco
richiede il massimo heap, può chiedere una "modalità esclusiva" che libera
Wi-Fi/httpd/cache e, solo se serve, un riavvio pulito (decisione ancora
aperta).

## 8. UI

- **Home identica**: stesso layout, icone, tema, ordine, hint. Quattro voci:
  Store, Giochi, Impostazioni (+ eventuale System info).
- **Store**: due schede (Script/Native), lista, dettaglio, installa/aggiorna/
  disinstalla, barra di progresso, permessi, badge "verificata".
- **Giochi**: griglia/lista delle app installate (entrambi i canali), avvio,
  ultimo gioco, punteggi.
- **Impostazioni**: Wi-Fi, display/tema, audio, storage, aggiornamenti,
  chiavi store/about, migrazione dati.
- Input: driver TCA8418 con la stessa mappa tasti e gli stessi eventi
  (`Back`, `Select`, frecce) usati oggi.
- Framebuffer 240×135 RGB565; fallback 8 bpp solo se i 64,8 KB contigui non
  sono disponibili.
- Niente UI web in 3.0.

## 9. Partizioni e migrazione dati

**Staging (importante):** M0–M2 usano la **tabella partizioni attuale**, così
LittleFS, NVS e `/games` restano al loro posto e non si perde nulla. La nuova
tabella con `orbit_nap` si applica in **M3**, dopo il backup e con il canale
nativo già pronto. Questo evita di ripartizionare prima di avere qualcosa da
testare.

Tabella attuale (M0–M2): app0/app1 2,6875 MB, `spiffs` 2,5 MB a 0x570000,
coredump 64 KB. Il firmware IDF deve entrare in 2,6875 MB.

Tabella finale (M3, proposta):

```
# Name       Type  SubType   Offset    Size
nvs          data  nvs       0x009000  0x005000
otadata      data  ota       0x00E000  0x002000
app0         app   ota_0     0x010000  0x2D0000   # 2,8125 MB
app1         app   ota_1     0x2E0000  0x2D0000
orbit_nap    data  spiffs    0x5B0000  0x080000   # 512 KB scratch, mai formattata
littlefs     data  spiffs    0x630000  0x1C0000   # 1,75 MB dati
coredump     data  coredump  0x7F0000  0x010000
```

- **NVS resta a 0x9000/0x5000**: le impostazioni e le credenziali Wi-Fi
  attuali sopravvivono (stesso namespace/chiavi, letti con `nvs_get_*`).
- **LittleFS cambia offset**: prima del flash serve un backup su SD di
  `/config.json`, `/scores.txt` e degli altri file utente. Il nuovo firmware
  importa da `/orbit-backup/` al primo avvio e lascia una copia.
- **`/games` sulla SD non viene toccato**: i `.wren` esistenti vengono
  importati nel registry come app script (hash + manifest generato).
- **Punteggi**: migrati da `/scores.txt` a `/system/scores.json` (o mantenuti
  nel formato attuale, decidere in M1).
- **Backup**: `esptool read-flash` della vecchia partizione LittleFS + copia
  dei file su SD + bin del vecchio firmware, prima di ripartizionare.
- **Rollback**: la vecchia app resta nello slot OTA inattivo finché la nuova
  non è marcata valida; in caso di boot fallito, rollback automatico.

## 10. OTA

- Due slot, `otadata`, `esp_ota_mark_app_valid_cancel_rollback()` dopo un boot
  sano; rollback automatico sul primo boot fallito.
- Aggiornamento firmware firmato (stessa chiave ECDSA dello store) e
  checksum verificato prima di scrivere.
- Aggiornamenti delle app separati da quelli del firmware.

## 11. Build, firma, pubblicazione

- `tools/build_app.py <id>`: crea `.oap` script.
- `tools/build_native.py <id>`: compila contro `sdk/native`, linka con
  `nap.ld`, verifica gli import ABI, crea `.oap`.
- `tools/sign.py`: ECDSA P-256 (riuso di `sign_ota.py`).
- `tools/catalog.py`: genera e firma `catalog.json`.
- CI: schema JSON, ricompilazione nativa byte-per-byte, test host su parser
  pacchetto/firma/registry, nessun flash.
- Test host obbligatori per: manifest, firma, path traversal, overflow,
  installazione atomica, rollback.

## 12. Milestone

**M0 — Bring-up IDF (1–2 settimane)**
- Progetto ESP-IDF 5.4 che compila e flasha sull'ADV.
- Display 240×135, tastiera TCA8418, SD, LittleFS, log seriale.
- Home statica disegnata con la UI portata.
- Accettazione: boot, Home visibile, tasti e SD funzionano; vecchio firmware
  ripristinabile.

**M1 — Registry, impostazioni, migrazione (1–2 settimane)**
- Registry, NVS, backup/import, Impostazioni base, tema/rotazione.
- Accettazione: impostazioni e Wi-Fi preservati, `/games` intatto, nessuna
  perdita di dati.

**M2 — Store canale script (2–3 settimane)**
- Catalogo firmato, download, verifica, install/update/uninstall.
- Runtime Wren, 5 giochi esistenti come pacchetti script.
- Accettazione: installa un gioco dallo store, gioca, riavvia, è ancora lì.

**M3 — Store canale nativo (3–5 settimane)**
- SDK, loader ELF, ABI v1, export table, fingerprint.
- 1–2 app native (es. 2048 e Snake) come pacchetti.
- Accettazione: installa, carica, esegue e scarica un modulo nativo senza
  reboot né crash; un modulo corrotto viene rifiutato.

**M4 — Rifinitura e release**
- OTA A/B + rollback, test host/CI, documentazione, flusso di pubblicazione
  per terzi, migrazione definitiva.

## 13. Rischi

| Rischio | Mitigazione |
|---|---|
| HAL ADV sotto IDF (pannello, TCA8418, codec) | spike in M0, confronto con i sorgenti M5GFX/M5Cardputer già in `.pio`; se bloccante, fallback Arduino 3.x/IDF 5.x |
| Loader nativo (MMU, reallocazioni, ABI) | prototipo "hello world" prima della UI store; test host con corruzioni mirate |
| Flash insufficiente per due slot + scratch | misura in M0; ridurre LittleFS o scratch a 256 KB |
| Migrazione dati | backup su SD + lettura NVS invariata + test su copia |
| Firma e chiavi | chiave offline, rotazione, pubblicazione centralizzata |
| Licenza | clean-room; nessun codice NucleoOS copiato |
| "Tutti possono pubblicare" + codice nativo | revisione + firma obbligatoria; developer mode esplicito |

## 14. Punti ancora aperti

1. Versione IDF: **5.4** (come NucleoOS) — confermi?
2. Scratch nativo: **512 KB** (256 KB mappati) o 256 KB?
3. Giochi nativi: nessun reboot (preferito) o "solo boot" per heap massimo?
4. Firma: ECDSA P-256 (già in casa) o Ed25519?
5. Hosting catalogo: repo GitHub sotto il tuo account?
6. ABI v1: basta display/input/tempo/math/fs/audio?
7. Gli altri 20 moduli restano fuori per sempre o rientrano dopo il 3.0?

## 15. Criterio di "primo traguardo"

Firmware IDF che arriva alla Home, apre Store e Giochi, installa e avvia:
- **2 app script** (es. 2048 e Snake) dal canale Script;
- **1 app nativa** (es. 2048) dal canale Native;

con dati utente preservati, app firmate e verificate, nessun reboot durante
l'uso normale, e possibilità di tornare al firmware precedente.
