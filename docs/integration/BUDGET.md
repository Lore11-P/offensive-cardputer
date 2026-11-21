# Budget flash e RAM — integrazione 10 moduli

Misura del 21/11/2026 sul worktree `orbit-wt-budget`, branch `feat/budget`
(commit `c7456af`), ambiente `cardputer-adv-offensive`.
Toolchain: PlatformIO Core 6.2.0, `espressif32@6.7.0`, GCC xtensa 8.4.0.
Hardware: M5Stack Cardputer ADV, ESP32-S3FN8, 8 MB flash, 320 KB SRAM, no PSRAM.

## 1. Baseline misurata

Comando:

```powershell
python -m platformio run -e cardputer-adv-offensive
```

| Voce | Valore | Note |
|---|---:|---|
| Flash PlatformIO | 2.358.193 B (90,0%) | `text + data` stimati sul file ELF |
| `firmware.bin` | **2.358.560 B** | dimensione reale del binario |
| Slot app OTA | 2.621.440 B (2,5 MiB) | `0x280000` in `partitions.csv` |
| Libero nello slot | **262.880 B = 256,7 KiB** | 10,0% dello slot |
| RAM statica | 115.520 / 327.680 B (35,3%) | `.data + .bss` |
| RAM statica libera | 212.160 B | 207,2 KiB, prima di stack e heap |

> **La baseline attesa di 2.330.085 B non è più valida in questo worktree.**
> La build attuale misura 2.358.560 B (+28.475 B, circa +27,8 KiB). Gli 8 stub
> del commit `e153b43` spiegano solo ~5 KB; il resto era già nella build
> corrente e non nel valore di riferimento, che risulta più vecchio. Il margine
> reale è quindi **256,7 KiB, non 291 KB**: tutti i conti seguenti usano il
> valore misurato. Lo script di controllo legge `firmware.bin` (più conservativo
> di PlatformIO di 367 B di header).

Gli stub dei 10 moduli sono già compresi nella baseline: i budget sotto sono
**delta** rispetto allo stub, cioè il codice che l'integrazione può aggiungere.

## 2. Budget per modulo e budget totale

| # | Modulo | Flash (KiB) | RAM statica (KiB) | Heap di picco (KiB) | Nota |
|---|---|---:|---:|---:|---|
| 1 | IR (DB completo + clima) | 8 | 1 | 4 | DB su SD; in firmware solo il merge SD/LittleFS |
| 2 | Radio (playlist, ICY, REC) | 16 | 2 | 8 | ring buffer piccolo per il WAV |
| 3 | Audio (FFT, registratore, memo) | 16 | 10 | 8 | FFT 512 punti + buffer statici ~8 KB |
| 4 | BLE HID + Bad BLE | 20 | 2 | 16 | NimBLE HID; deinit obbligatorio |
| 5 | Tools (clock, note, calcolatrice, QR, TOTP) | 24 | 4 | 8 | QR e HMAC-SHA1 |
| 6 | WiFi Lab (PMKID, monitor, waterfall) | 28 | 4 | 16 | modulo nuovo più incerto |
| 7 | WPA-SEC (upload/potfile) | 14 | 1 | 48 | TLS: richiede heap contiguo |
| 8 | Sensori (livella, goniometro, passi, LED) | 10 | 1 | 2 | solo IMU e LED |
| 9 | WiFi+ (responder, karma, portal) | 24 | 2 | 12 | riusa AsyncWebServer già linkato |
| 10 | ESP-NOW (file, comandi, clipboard) | 18 | 4 | 8 | chunk da ~200 B |
| | **Totale moduli** | **178** | **31** | **48 max** | gli heap non si sommano: i menu sono seriali |
| | Contingenza condivisa | 14 | 1 | — | solo per sforamenti motivati |
| | **Tetto integrazione** | **192** | **32** | — | |
| | Riserva intoccabile | 64 | — | — | ~65.536 B di flash |

Aritmetica del budget flash:

```text
baseline            2.358.560 B
tetto integrazione    196.608 B  (192 KiB: 178 moduli + 14 contingenza)
---------------------------------------------------------------
tetto binario       2.555.168 B
slot app            2.621.440 B
margine finale         66.272 B  (64,7 KiB, >= 64 KiB richiesti)
```

Regole:

- Nessun modulo può sforare il proprio budget usando il margine di un altro
  senza una voce esplicita in questo file.
- Dopo ogni merge si misura con `.\tools\check_size.ps1`; il delta reale è
  `bin nuovo - bin precedente`, non la somma dei budget.
- Se un modulo sfora, deve liberare l'equivalente (feature rinviata o taglio
  nel piano sotto) prima del merge.
- Il budget RAM statica lascia ~176 KiB liberi per stack, framebuffer (~65 KB)
  e heap WiFi/BLE/TLS. Il vincolo vero a runtime è l'heap, non `.bss`.

Gate heap consigliati prima di aprire un modulo:

| Modulo | Heap libero minimo | Note |
|---|---:|---|
| WPA-SEC | 64 KB, di cui 48 KB contigui | TLS |
| BLE HID | 32 KB | NimBLE HID |
| WiFi Lab / WiFi+ | 32 KB | raw WiFi, server |
| Audio / Radio | 24 KB | buffer e decoder |
| Altri | 16 KB | |

## 3. Flag di compilazione

Verificati in `.pio/build/cardputer-adv-offensive/idedata.json` e nel map file:

| Flag | Stato | Evidenza |
|---|---|---|
| `-Os` | **attivo** | presente in `cc_flags` e `cxx_flags` |
| `-ffunction-sections` | **attivo** | presente in `cc_flags` e `cxx_flags` |
| `-fdata-sections` | **attivo** | presente in `cc_flags` e `cxx_flags` |
| `-Wl,--gc-sections` | **attivo** | il map contiene `Discarded input sections` |
| `-flto` | **non attivo** | assente dai flag |

Conclusioni:

- La configurazione sicura è già quella in uso: non c'è un flag ovvio da
  aggiungere. `--gc-sections` sta già eliminando il codice non referenziato.
- **Non attivare `-flto` durante l'integrazione.** Su GCC 8.4 xtensa con
  Arduino core, NimBLE e decoder audio ci sono rischi noti su sezioni IRAM,
  ISR, callback weak e ordine di inizializzazione. Il guadagno atteso (3–8%,
  indicativamente 70–190 KB) non è verificato su questa scheda e non va contato
  nel budget. Se serve, provarlo su un branch dedicato con test hardware
  completo (WiFi, BLE, audio, IR, SD, OTA) e solo dopo l'integrazione.
- `-fno-exceptions` non è proposto: le librerie C++ del core possono usare
  eccezioni e il risparmio non vale il rischio.
- Il decoder FLAC di ESP32-audioI2S è linkato (~8,4 KB) ma non ha uno switch
  supportato nella versione usata; non è un taglio sicuro senza patch della
  libreria.

## 4. Partizioni e LittleFS

Layout attuale (`partitions.csv`, 8 MiB totali):

| Nome | Offset | Size | Dimensione |
|---|---:|---:|---:|
| `nvs` | `0x009000` | `0x005000` | 20 KiB |
| `otadata` | `0x00E000` | `0x002000` | 8 KiB |
| `app0` | `0x010000` | `0x280000` | 2,5 MiB |
| `app1` | `0x290000` | `0x280000` | 2,5 MiB |
| `spiffs` (LittleFS) | `0x510000` | `0x2E0000` | 2,875 MiB |
| `coredump` | `0x7F0000` | `0x010000` | 64 KiB |

LittleFS:

- Payload in `data/`: **1.474.017 B** su 3.014.656 B di partizione (48,9%,
  ~1,4 MiB liberi al netto dell'overhead FS).
- Di cui IR: **1.435.981 B in 309 file** (97,4% del payload); icone 37.512 B
  (12 file); script BadUSB 524 B.
- `data/` **non entra in `firmware.bin`**: è un'immagine separata. Spostare il
  DB IR su SD non libera flash applicativa, libera solo LittleFS.

Cosa mettere su SD (e cosa no):

- **SD, sempre**: nuove categorie IR e crescita del DB (il fallback LittleFS
  resta congelato a 1,436 MB come richiede la spec IR); registrazioni
  `/audio/rec`, memo, catture `.hc22000`/`.pcap`, potfile, playlist radio,
  whitelist e PMKID, inbox/outbox ESP-NOW, note/calendario/TOTP, log e cache.
  Non duplicare questi file in LittleFS.
- **LittleFS**: solo il fallback IR attuale e le icone dei menu (servono prima
  del mount SD). Nessun dato utente o file che cresce nel tempo.
- Il filesystem **non è la pressione attuale**: il rischio è la flash
  applicativa, non LittleFS.

Piano B, solo se il tetto da 192 KiB viene esaurito: riequilibrare le
partizioni per dare **+256 KiB per slot app** (2,75 MiB), prendendoli da
LittleFS (2,375 MiB, ancora capiente per 1,436 MB di IR):

```text
app0    0x010000  0x2C0000
app1    0x2D0000  0x2C0000
spiffs  0x590000  0x260000
coredump 0x7F0000 0x010000
```

Costo: richiede un flash USB completo una tantum (la tabella non si aggiorna
via OTA), la LittleFS va riflashata e le immagini OTA già distribuite non sono
più valide perché `app1` cambia offset. Non farlo durante l'integrazione: è la
riserva per dopo. Se si applica, alzare anche il default `-MaxBytes` di
`check_size.ps1` (baseline + 256 KiB extra + budget moduli). Un piano C più
aggressivo (slot da 3,5 MiB) richiederebbe di ridurre il fallback IR sotto
~600 KB, quindi una modifica alla specifica IR.

## 5. Piano di riduzione se si sfora

Soglie, misurate come spazio libero nello slot (`firmware.bin`):

| Stato | Libero | Azione |
|---|---:|---|
| Verde | > 96 KiB | sviluppo normale |
| Giallo | 64,7–96 KiB | niente nuove feature; preparare i tagli |
| Rosso | < 64,7 KiB | applicare il piano; `check_size.ps1` fallisce sotto 66.272 B liberi |

Ordine di taglio (1 = primo a cadere). I giochi e le feature esistenti si
sacrificano prima dei 10 moduli nuovi.

| Ordine | Cosa | Risparmio flash | Risparmio RAM | Come |
|---:|---|---:|---:|---|
| 1 | Modulo Giochi intero | 94,3 KiB | 6,3 KiB | `-DENABLE_MODULE_GAMES=0` (solo supervisore) |
| 2 | Giochi singoli | 2,6–12,4 KiB ciascuno | 0–2,6 KiB | modifica `GamesMenu.cpp`, solo se serve un taglio parziale |
| 3 | BLEEXT intero | 53,5 KiB | **42,1 KiB** | `-DENABLE_MODULE_BLEEXT=0`; è il maggiore guadagno RAM |
| 4 | Musica locale | 15,0 KiB | — | `-DENABLE_MODULE_MUSIC=0`; la radio copre lo streaming |
| 5 | Radar | 28,7 KiB | 1,4 KiB | `-DENABLE_MODULE_RADAR=0`, solo se i precedenti non bastano |
| 6 | Sensori | 10 KiB | 1 KiB | nuovo modulo meno critico |
| 7 | Delta IR | 8 KiB | 1 KiB | tenere il DB su SD, fallback invariato |
| 8 | WPA-SEC | 14 KiB | 1 KiB | richiede rete e chiave esterna |
| 9 | ESP-NOW | 18 KiB | 4 KiB | nicchia, richiede due device |
| 10 | BLE HID | 20 KiB | 2 KiB | tastiera BLE |
| 11 | Tools (o solo QR/TOTP) | 24 KiB | 4 KiB | tagliabile a metà feature |
| 12 | WiFi+ | 24 KiB | 2 KiB | responder/karma/portal |
| 13 | WiFi Lab | 28 KiB | 4 KiB | il nuovo modulo più costoso |
| 14 | Radio | 16 KiB | 2 KiB | **ultimo**: è il modulo richiesto |
| 15 | Audio | 16 KiB | 10 KiB | **ultimo**: è il modulo richiesto |

Dettaglio giochi (dal map, dal più grande): Scacchi 12,4 KiB, Doom Lite 9,0 KiB,
Minecraft 7,4 KiB, Pixel Jump 6,7 KiB, Lander 5,8 KiB, Pac-Man 5,6 KiB; gli
altri 11 giochi sono 2,7–4,5 KiB ciascuno.

Dettaglio BLEEXT (RAM in KiB): GattEnum 12,5 + 6,8, BleCapture 8,7 + 4,5,
Wardriving 7,1 + 4,8, FlipperFinder 6,7 + 4,8, AirTag 6,2 + 4,8,
SkimmerDetect 4,4 + 4,8.

Da non tagliare mai: `core/` (Display, MenuManager, Navigation, UI), `utils/`
(Storage, Config), WiFi base, BLE base, IR base e la infrastruttura OTA.

## 6. Procedura di verifica

```powershell
# compila e controlla il tetto di budget
.\tools\check_size.ps1

# solo lettura del binario già compilato
.\tools\check_size.ps1 -SkipBuild

# soglia diversa (es. prova locale)
.\tools\check_size.ps1 -SkipBuild -MaxBytes 2560000
```

Exit code: `0` ok, `2` budget superato, `3` firmware oltre lo slot app
(non flashabile via OTA), `1` errore di build o file mancante.
Lo script legge lo slot da `partitions.csv`, quindi segue automaticamente un
eventuale piano B.

## 7. Rischi principali

1. **Baseline più alta del previsto.** Il margine reale è 256,7 KiB, non 291 KB:
   ci sono già ~28 KB in più. Se si pianifica sui numeri vecchi si sfora senza
   accorgersene.
2. **Quattro moduli nuovi valgono metà del budget.** WiFi Lab (28) + WiFi+
   (24) + Tools (24) + BLE HID (20) = 96 KiB, il 54% dei 178 KiB pianificati.
   Un overshoot del 25% su tutti mangia contingenza e riserva. Sono anche
   quelli che tirano dentro più codice di libreria (raw WiFi, HTTP/DNS, HID).
3. **La RAM a runtime, non `.bss`.** Framebuffer (~65 KB), TLS WPA-SEC (48 KB),
   NimBLE HID (16 KB), stack e heap WiFi: un deinit mancante o la
   frammentazione causano crash anche con flash abbondante. I gate heap e il
   deinit all'uscita non sono opzionali.
