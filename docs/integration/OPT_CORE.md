# ORBIT OS — ottimizzazione core e audit statico

Ambito: `src/utils/Config.cpp`, `src/utils/OtaUpdate.cpp`, `include/Logger.h`,
`src/core/Compass.cpp` e relativi header. L'audit copre tutto `src/` e
`include/`, ma le modifiche restano confinate ai soli file posseduti.

Metodo: revisione statica manuale + grep dei pattern richiesti
(`strcpy`, `sprintf`, `malloc/new`, `delay()` nei loop, buffer fissi, ritorni
ignorati, overflow, stack dei task, credenziali nei log). Nessuna analisi
dinamica: le misure sono quelle della build `cardputer-adv-offensive`.
I numeri di riga si riferiscono al commit base `c7456af` (prima delle patch di
questo branch), salvo dove indicato.

---

## 1. Modifiche applicate

### 1.1 Config (`src/utils/Config.cpp`, `include/Config.h`)

| Modifica | Perché |
| --- | --- |
| `applyJson()` applica moduli e timeout senza chiamare i setter che salvano, poi fa **un solo** `save()` | prima un import JSON eseguiva fino a 14 commit NVS separati (uno per `setModuleEnabled`/`setSleepTimeoutSec`): latenza e usura flash |
| `begin()` legge nickname/destinatario in buffer da 64 byte con `Preferences::getString(key, char*, len)` | elimina le `String` temporanee e le conversioni `String -> std::string` all'avvio (heap e frammentazione); la sanificazione taglia poi a 6 caratteri come prima |
| rimossi i `putUInt("magic", ...)` ridondanti in `begin()` e `reset()` | `save()` scrive già la magic; erano scritture NVS in più con esito ignorato |
| `chatNickname()` / `chatRecipient()` restituiscono `const std::string&` | evita una copia a ogni pacchetto/presenza chat; nessun chiamante richiede una copia propria |

Formato NVS e JSON invariato (stesse chiavi, stesso schema, stessa magic
`CPF1`). Comportamento osservabile invariato: dopo un import o un reset lo
stato persistito è lo stesso, cambia solo il numero di scritture.

### 1.2 OTA (`src/utils/OtaUpdate.cpp`)

| Modifica | Perché |
| --- | --- |
| verifica dell'immagine scritta con `esp_ota_get_partition_description(target, &desc)` prima del riavvio; se fallisce ripristina la partizione di boot corrente | controlla struttura e descrittore dell'app appena scritta, non solo la lunghezza; evita di riavviare su un'immagine non valida |
| log di `project_name`/`version` dell'immagine verificata | tracciabilità dell'aggiornamento |
| riga di dettaglio costruita con `snprintf` in un buffer fisso (rimossa `humanSize()`) | elimina 5-6 allocazioni `std::string` ogni 120 ms durante il download, cioè nel momento in cui l'heap è più critico |
| `wifisetup::disconnect()` su tutte le uscite non riuscite/annullate | la radio non resta accesa dopo un errore o un annullamento (regola del brief) |
| timeout di stallo come costante `kStallTimeoutMs` | nessun cambio funzionale, sola leggibilità |

`Update.write`/`Update.end` già verificano magic e struttura dell'immagine a
livello Arduino; la verifica aggiunta è indipendente e avviene dopo la fine
della scrittura. Il download resta su TLS senza CA pinnata: vedi finding
**OTA-1**, non chiudibile senza una CA/hash pubblicati dal progetto.

### 1.3 Logger (`include/Logger.h`)

| Modifica | Perché |
| --- | --- |
| formattazione spostata in `src/utils/Logger.cpp` (una sola copia nel binario) | la prima versione inline faceva crescere il binario di ~5 KB; con la copia unica il delta complessivo torna negativo |
| una sola `vsnprintf` nel buffer `[FW][x] ...\n` e una sola `Serial.write` per riga | prima erano 3 scritture (prefisso, corpo, newline) e due passaggi di formattazione; con log da task diversi (loop e callback NimBLE della chat) le righe potevano interlacciarsi |
| argomenti per `const&` | niente copie degli argomenti (prima erano passati per valore) |
| troncamento a 255 byte per riga | limite esplicito: nessuna scrittura oltre il buffer |
| `__attribute__((format(printf, 2, 3)))` su `writeLine` | documenta il contratto printf; la verifica piena resta limitata perché il template passa un `fmt` non letterale (vedi LOG-2) |

Formato dell'output invariato (`[FW][I]`, `[FW][W]`, `[FW][E]` + newline).

### 1.4 Compass (`src/core/Compass.cpp`)

| Modifica | Perché |
| --- | --- |
| `dt` limitato a 250 ms | dopo una pausa lunga (menu aperto, scheduling) non si integra più tutto il tempo trascorso, che produceva salti di heading |
| normalizzazione con `std::fmod` + correzione del segno | i due `while` potevano iterare milioni di volte dopo un overflow di `millis()` o una pausa molto lunga, bloccando il task |
| costante `kMaxDtSec` documentata | rende esplicito il limite |

Bias, deadband, segno e scala restano invariati.

---

## 2. Build e misure

Comando: `python -m platformio run -e cardputer-adv-offensive`.

| Voce | Prima | Dopo | Delta |
| --- | --- | --- | --- |
| `firmware.bin` | 2.358.560 B | 2.355.984 B | **-2.576 B (-0,11%)** |
| Flash usata (PlatformIO) | 2.358.193 B / 2.621.440 B (90,0%) | 2.355.621 B / 2.621.440 B (89,9%) | **-2.572 B** |
| RAM statica (PlatformIO) | 115.520 B / 327.680 B (35,3%) | 115.520 B / 327.680 B (35,3%) | **0 B** |

Nota: la RAM riportata da PlatformIO è `data + bss`; il framebuffer (~64 KB) è
allocato a runtime e non compare in questa cifra.

---

## 3. Audit statico — findings

Severità: **C** critica, **A** alta, **M** media, **B** bassa, **I** informativa.

### Criticalità

| ID | Sev | File:riga | Findings | Impatto | Correzione proposta |
| --- | --- | --- | --- | --- | --- |
| OTA-1 | C | `src/utils/OtaUpdate.cpp:92-98` | `WiFiClientSecure::setInsecure()` + nessun hash/firma del `firmware.bin`; l'URL è `raw.githubusercontent.com` | un MITM (o un account/URL compromesso) può far flashare firmware arbitrario: esecuzione di codice e persistenza | CA pinnata (root ISRG o certificato del repo) + SHA-256 pubblicato e firmato (Ed25519) verificato prima di `Update.end(true)`; in alternativa Secure Boot con chiave del progetto |
| CHAT-1 | C | `src/modules/chat/Chat.cpp:131-145` | chiave AES-128 = SHA-256(`nickname mittente` + `\x1F` + `nickname destinatario`), IV derivato dal contatore; nessun segreto condiviso, nessun MAC (i 2 byte di "tag" sono un hash del nome) | chi conosce i due nickname decifra e può forgiare messaggi; CBC senza autenticazione | X25519/ECDH con fingerprint verificato + AES-GCM (o ChaCha20-Poly1305) con nonce casuale |
| WIFI-1 | C | `src/utils/WifiSetup.cpp:72-84` | SSID e password Wi-Fi salvati in chiaro nel namespace NVS `cpfw-net`; `CONFIG_SECURE_FLASH_ENC_ENABLED` non è attivo e non esiste una partizione `nvs_keys` | un dump flash (`esptool read_flash`) o un'app compromessa legge la password della rete | abilitare NVS encryption (`nvs_keys` + `nvs_flash_secure_init`) o cifrare il blob con una chiave in eFuse; in alternativa non persistere la password |

### Alta

| ID | Sev | File:riga | Findings | Impatto | Correzione proposta |
| --- | --- | --- | --- | --- | --- |
| CHAT-2 | A | `src/modules/chat/Chat.cpp:544` | ogni messaggio ricevuto viene scritto in chiaro su USB seriale (`logger::info("Chat BLE: %s: %s", ...)`) | chi è collegato via USB (o un log persistito) legge le conversazioni | rimuovere il log del testo o registrare solo mittente e lunghezza |
| WIFI-2 | A | `src/utils/WifiSetup.cpp:78-87` | `saveCredentials()` ignora il ritorno di `putString("ssid"/"pass")` e restituisce sempre `true` | la UI mostra "Credenziali salvate" anche se NVS è piena/fallita; al boot successivo la rete non c'è | controllare i due ritorni (0 = errore, salvo stringa vuota) e propagare `false`; il chiamante deve mostrare l'errore |
| CHAT-3 | A | `src/modules/chat/Chat.cpp:534-539` | `g_lastCounter[address] = counter`, `g_messages.push_back(std::move(message))` ed `erase()` avvengono dentro `portENTER_CRITICAL(&g_lock)` | allocazioni heap e deallocazioni con interrupt disabilitati: latenze imprevedibili, rischio watchdog con heap frammentato | costruire/riservare fuori dalla sezione critica e tenere nel lock solo copie in buffer fissi (ring buffer) |
| OTA-2 | A | `src/utils/OtaUpdate.cpp:221-232` | dopo `Update.end(true)` non c'era alcuna verifica indipendente dell'immagine scritta prima di `ESP.restart()` | un'immagine strutturalmente accettata ma con descrittore/app invalida poteva essere avviata | **applicato**: `esp_ota_get_partition_description()` + ripristino della partizione corrente in caso di errore |

### Media

| ID | Sev | File:riga | Findings | Impatto | Correzione proposta |
| --- | --- | --- | --- | --- | --- |
| CFG-1 | M | `src/utils/Config.cpp:439-460` | `applyJson()` chiamava i setter che salvano: fino a 14 commit NVS per un import | import lento e usura flash | **applicato**: un solo `save()` finale |
| CFG-2 | M | `src/utils/Config.cpp:141-147` | `String` temporanee e `String -> std::string` per nickname/destinatario a ogni boot | allocazioni heap e frammentazione all'avvio | **applicato**: `getString()` su buffer fisso |
| CFG-3 | M | `src/utils/Config.cpp:495-507` | `exportToFile()` scrive direttamente su `/config.json` con `FILE_WRITE` | interruzione di corrente durante il salvataggio = JSON troncato/corrotto | scrivere su `path.tmp` e rinominare (come `storage::writeText`) |
| COMPASS-1 | M | `src/core/Compass.cpp:80-85` | normalizzazione con due `while` su un `dt` non limitato | dopo overflow di `millis()`/pausa lunga il loop può iterare milioni di volte e bloccare il task | **applicato**: `dt` limitato + `std::fmod` |
| WIFI-3 | M | `src/modules/wifi/WifiMenu.cpp:604-628` | `snifferCb` (ISR, `IRAM_ATTR`) copia fino a 1600 byte dentro `portENTER_CRITICAL_ISR` per ogni frame promiscuo | interrupt disabilitati a lungo: perdita di pacchetti Wi-Fi/BLE e jitter | copiare fuori dalla sezione critica (flag `ready` per slot, doppio buffer) o ridurre `kSnifferMax` |
| WIFI-4 | M | `src/modules/wifi/WifiMenu.cpp:2036-2051` | `buildBeacon(uint8_t* out, ...)` non riceve la dimensione del buffer; il chiamante usa `kBeaconLen` e il caso SSID=32 riempie esattamente il buffer | qualunque nuovo chiamante con buffer più piccolo va in overflow | aggiungere `size_t outLen` e restituire 0 se `38 + ssidLen + tail > outLen`; usare `kBeaconMaxSsid` esplicito |
| GATT-1 | M | `src/modules/bleext/GattEnum.cpp:475-491` | allo scadere dei 25 s la UI esce ma il worker `ble_gatt` continua a usare NimBLE e `g_job` | il task sopravvive al menu e può interferire con i moduli BLE successivi (rientro bloccato da `g_jobActive`) | attendere il worker con timeout più lungo, oppure annullare la connessione e attendere `done` prima di uscire |
| LOG-1 | M | `include/Logger.h` + `src/utils/Logger.cpp:8-34` | tre `Serial` write per riga (prefisso, corpo, newline) e due formattazioni; l'implementazione inline duplicava il codice per ogni chiamata | righe interlacciate tra task (chat/NimBLE vs loop), più overhead e ~5 KB di flash | **applicato**: formattazione out-of-line, una sola `vsnprintf` + una sola `write` |
| LOG-2 | M | `include/Logger.h:19-22` | il template passa un `fmt` non letterale a `writeLine`: l'attributo `format(printf)` non può validare i tipi a ogni chiamata | un `%s` con tipo sbagliato resta UB non rilevato | loggare tramite macro (`#define LOG_I(fmt, ...)`) o overload che ricevono il letterale, così il compilatore controlla; in alternativa test con `-Wformat=2` |
| CFG-4 | M | `src/utils/Config.cpp:117,201` | `putUInt("magic", ...)` con ritorno ignorato e poi riscritto da `save()` | scritture inutili; nessun impatto se falliscono perché `save()` riprova | **applicato**: rimosse le scritture ridondanti |
| AUDIO-1 | M | `src/utils/AudioOut.cpp:43` | `new Audio()` senza `nothrow` e senza controllo; l'oggetto non viene mai distrutto | OOM = abort/reboot; leak intenzionale di una sola istanza | `new (std::nothrow) Audio()`, restituire `nullptr`/fallback e documentare il singleton |
| WIFI-5 | M | `src/modules/wifi/WifiMenu.cpp:1937,2152,2260` | ritorni di `esp_wifi_set_channel()`/`esp_wifi_set_promiscuous_rx_cb()` ignorati | il modulo mostra "in esecuzione" anche se il driver ha rifiutato il canale | controllare `esp_err_t` e mostrare un errore |

### Bassa / informativa

| ID | Sev | File:riga | Findings | Note |
| --- | --- | --- | --- | --- |
| NAV-1 | B | `src/core/Navigation.cpp:155-166` | `waitRelease()` attende fino a 3 s in passi da 10 ms | con un tasto bloccato la UI resta ferma 3 s; il limite è voluto, ma va documentato |
| COMPASS-2 | B | `src/core/Compass.cpp:36-50` | `begin()` blocca 400 ms per misurare il bias con `delay(10)` | accettabile all'avvio; se serve reattività si può campionare senza delay o in background |
| IR-1 | B | `src/modules/ir/IrMenu.cpp:622,915,1090,1121` | `delay(5..12)` in cicli con `navigation::poll()` | latenza ESC 5-12 ms, accettabile |
| PCAP-1 | B | `src/modules/pcap/PcapMenu.cpp:463,604,702` | `delay(1)` dentro i cicli di elaborazione | ~1000 wakeup/s; valutare `delay(2..5)` nei tratti senza I/O |
| UI-1 | I | `src/core/Display.cpp:949,1073` | `delay(30)` e `delay(120/260)` nelle animazioni | solo splash/transizioni, non loop interattivi |
| GAMES-1 | I | `src/modules/games/*.cpp` | molti `delay(8..20)` per il frame pacing | by design; non sono busy-wait |
| MAIN-1 | I | `src/main.cpp:177,205` | `delay(150)` dopo `Serial.begin`, `delay(1000)` nel `loop()` vuoto | bootstrap/loop inutilizzato |
| MEM-1 | I | `src/modules/games/Minecraft.cpp:672`, `src/modules/music/MusicMenu.cpp:211`, `src/modules/wifi/WifiMenu.cpp:654` | `new (std::nothrow)` con controllo e `delete[]` nei distruttori/percorsi di uscita | verificato: nessun leak ricorrente |
| MEM-2 | I | `src/utils/PcapTools.cpp:130,1530` | buffer su heap (`LineWriter` 4 KB, `LiveCollector` ~3 KB) con `unique_ptr` e `nothrow` | verificato: liberati alla distruzione |
| TASK-1 | I | `src/modules/bleext/GattEnum.cpp:449` | unico task applicativo: `xTaskCreate(..., kWorkerStack = 8192, ...)`, self-delete | nessuna misura di high-water nel modulo; `SettingsMenu` mostra solo quella del loop task (16 KB da `ARDUINO_LOOP_STACK_SIZE`) |
| STR-1 | I | tutto `src/` | **nessun** `strcpy`, `strcat`, `sprintf`, `gets`; solo 3 `strncpy` (RadarMenu 213-216, Doom 151) tutti con terminazione o buffer statico azzerato | positivo |
| STR-2 | I | `src/modules/bleext/BleCapture.cpp:151` | `char line[160]` per una riga CSV: con 31 byte di payload il massimo è ~114 caratteri | margine sufficiente; se `kMaxAdvData` cresce va ricalcolato |
| LOG-3 | I | `src/utils/Logger.h` | nessun timestamp/uptime nei log | utile per diagnosticare OTA e watchdog; aggiungibile senza cambiare il formato del prefisso |
| CFG-5 | I | `src/utils/WifiSetup.cpp:68-74` | `Preferences` aperta/chiusa a ogni `loadCredentials()` (anche in loop nella schermata impostazioni) | costo contenuto; si può tenere un'istanza statica |
| SD-1 | I | `src/utils/Storage.cpp:280-319` | `writeText()` usa file temporaneo + rename + `.bak` | positivo: scritture atomiche già disponibili per il fix CFG-3 |

### Credenziali e dati sensibili nei log

- Password Wi-Fi: **mai** loggata (positivo).
- SSID: loggato in `WifiSetup.cpp:86,354,368` — identificatore di rete, non segreto, ma è un dato di localizzazione. Se il dispositivo esce dalla disponibilità dell'utente, conviene ometterlo.
- Testo chat: loggato in chiaro (`Chat.cpp:544`) — finding **CHAT-2**.
- Credenziali del portale (feature offensiva): raccolte in RAM in `WifiMenu.cpp:2426-2438`, mai scritte su disco né loggate, azzerate all'uscita — positivo.
- Handshake/PMKID: scritti su SD per progetto (`PcapTools`/`WifiMenu`); sono materiale sensibile e vanno trattati come tali.

---

## 4. Patch proposte (fuori dal perimetro di questo branch)

1. **OTA firmato** (`src/utils/OtaUpdate.cpp`): scaricare in un buffer/SD, calcolare
   SHA-256 e confrontarlo con un manifest firmato Ed25519 incluso nel firmware;
   solo dopo `Update.begin`. Aggiungere una CA pinnata al `WiFiClientSecure`
   (`setCACert(...)`) al posto di `setInsecure()`.
2. **Rollback reale**: il framework ha `CONFIG_APP_ROLLBACK_ENABLE=y`, ma
   `verifyOta()` (weak, in `cores/esp32/esp32-hal-misc.c`) restituisce `true` e
   conferma l'immagine già in `initArduino()`. Per un rollback utile servono
   `bool verifyRollbackLater() { return true; }` e una chiamata a
   `esp_ota_mark_app_valid_cancel_rollback()` alla fine di `setup()` (main.cpp)
   dopo un self-test (display, SD, config). Richiede modifica di `main.cpp`.
3. **Log chat**: sostituire `logger::info("Chat BLE: %s: %s", ...)` con
   `logger::info("Chat BLE: messaggio da %s (%u byte)", sender, len)`.
4. **Sezione critica chat**: usare un ring buffer di struct a dimensione fissa
   per i messaggi in arrivo; costruire le `std::string` solo dopo il lock.
5. **Beacon**: `size_t buildBeacon(uint8_t* out, size_t outLen, ...)` con
   controllo di capacità.
6. **Logger**: far arrivare il formato letterale alla funzione con attributo
   `format(printf)` (macro o overload) per ottenere la validazione completa.
7. **`AudioOut`**: `new (std::nothrow) Audio()` e fallback silenzioso.
8. **`saveCredentials`**: controllare `putString` e restituire l'esito; il
   chiamante deve mostrare un errore se il salvataggio fallisce.
9. **NVS cifrata**: aggiungere una partizione `nvs_keys` (richiede modifica di
   `partitions.csv`, quindi flash USB una tantum) e inizializzare NVS con
   `nvs_flash_secure_init`; in alternativa cifrare solo il blob credenziali.
10. **ISR sniffer**: copiare il frame fuori dalla sezione critica, oppure
    limitare `kSnifferMax` a 512 byte per ridurre il tempo a interrupt off.

---

## 5. Rischi residui e test manuali

Rischi:

- Il firmware resta senza firma OTA e senza Secure Boot: **OTA-1 non è chiuso**
  da questo branch, solo documentato.
- Le modifiche a Config non cambiano chiavi/formato, ma un downgrade alla
  versione precedente deve continuare a leggere la stessa NVS (stesse chiavi:
  sì).
- Il logger ora tronca a 255 byte: eventuali righe molto lunghe (BadUSB, path
  SD) perdono la coda. Nessun log diventa più lungo di prima.
- La verifica OTA con `esp_ota_get_partition_description` dipende dal
  descrittore dell'app: un firmware di un altro progetto ma strutturalmente
  valido passerebbe (non c'è ancora una firma).
- `Compass`: il clamp a 250 ms migliora i salti dopo una pausa, ma rende
  l'integrazione leggermente meno precisa se `update()` viene chiamato a meno
  di 4 Hz. Il contratto resta ">= 20 Hz".

Test manuali suggeriti:

1. Boot pulito, modifica luminosità/tema/moduli, riavvio: valori persistiti.
2. `Impostazioni > Salva config`, poi `Importa config`: nessun errore, valori
   ripristinati; verificare che un JSON con `sleep_timeout_sec` fuori scala
   venga agganciato al valore ammesso.
3. OTA con WiFi reale: avanzamento, annullamento con ESC a metà download,
   riavvio; poi un secondo OTA per verificare il riavvio pulito.
4. Bussola: ruotare 360°, verificare che l'heading torni a 0 e che una pausa
   di 10 s nel menu non produca salti.
5. Log seriale: verificare che le righe non siano interlacciate con la chat
   BLE attiva.
