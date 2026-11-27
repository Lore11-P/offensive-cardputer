# orbit_ble

Porta su ESP-IDF 5.4 / NimBLE del modulo BLE del firmware 2.x
(`src/modules/ble/`): scanner, spam di advertising e info sullo stack.
Nessun GATT, nessuna connessione, nessun BLE HID/BadUSB/BLE+.

## API

```cpp
#include "orbit_ble/Ble.h"

orbit_ble::runMenu();   // menu Bluetooth; deinizializza il BT all'uscita
```

Il menu si dichiara figlio di `Home`. Va agganciato dal parent (fuori da
questo componente), ad esempio in `orbit_app`:

```cpp
#include "orbit_ble/Ble.h"
menu.addItem("Bluetooth", "bluetooth", [] { orbit_ble::runMenu(); });
```

e `orbit_ble` va aggiunto ai `REQUIRES` di chi lo chiama. Il componente non
modifica `main/`, `orbit_chat/` o la sdkconfig.

## Contenuto

### Scanner BLE

- Scansione **attiva** (SCAN_REQ) a 50/50 ms: i nomi arrivano spesso solo
  nella SCAN_RESPONSE. Per il passivo basta `params.passive = 1`.
- Lista a 32 slot fissi, deduplicata per MAC (nessuna crescita heap nel
  callback; quando è piena sostituisce la voce vista meno di recente).
- Le voci non viste da 15 s vengono eliminate: la lista resta una fotografia
  del presente.
- Vengono mostrati fino a 20 dispositivi, ordinati per RSSI (l'ultimo
  ricevuto, non il massimo storico). Refresh dello schermo ogni 2 s.
- Nome da AD `0x09` (completo) o `0x08` (abbreviato), solo ASCII stampabile.
- `;` / `.` scorrono, ENTER apre i dettagli: nome, MAC, tipo di indirizzo,
  RSSI, ultimo avvistamento e hex dell'advertisement principale (31 byte).
- ESC ferma la scansione (`ble_gap_disc_cancel`) e torna al menu.

### BLE Spam

Sottomenu con i set `Random`, `Apple (iOS/AirPods)`, `Samsung`,
`Google/Android`, `Windows SwiftPair`, `All`. Scelto il set si sceglie la
durata (30 o 60 s); **ogni trasmissione passa da `ui::legalWarning()`** con
attività `BLE spam <set>` e, se l'utente rifiuta, non parte nulla.

I payload sono quelli del 2.x (`BlePayloads.h`), passati a
`ble_gap_adv_set_data()` così come sono: sono già AD structure complete
(`[len][type][dati]`), non vanno incapsulati in un wrapper manufacturer.
Sono verificati a compile time: `<= 31` byte e catena AD valida.

Durante lo spam:

- rotazione del payload ogni ~140 ms (100-200 ms), advertising
  non-connectable a 60 ms di intervallo;
- schermo con set, payload corrente, conteggio TX (slot di advertising
  avviati con successo) e barra del tempo residuo;
- ESC interrompe entro un ciclo del task UI (~20-50 ms) e ferma subito
  l'advertising; la durata massima è 30/60 s.

### BLE info

Indirizzo locale e tipo, stato del controller (`esp_bt_controller_get_status`),
stato del sync NimBLE, heap libero e heap minimo. Nessuna trasmissione.

## Ciclo di vita del BT

Come `orbit_chat`: `runMenu()` chiama `nimble_port_init()`, configura
`ble_hs_cfg.reset_cb` / `sync_cb` e avvia il task host con
`nimble_port_freertos_init()`. All'uscita ferma scan/advertising,
`nimble_port_stop()`, `nimble_port_deinit()` (che disabilita e deinizializza
anche il controller) e libera la lista di scansione. Dopo il ritorno il BT è
spento e l'heap è disponibile per Wren/WiFi.

Se l'init fallisce viene mostrato un `ui::message` e il menu non parte: nessun
crash. Se `nimble_port_stop()` fallisce (caso raro) il BT non viene
deinizializzato a forza, come nella chat.

**Un solo proprietario dello stack**: nessun altro modulo deve tenere il BT
acceso mentre questo menu è aperto. Il menu tiene lo stack inizializzato
anche quando è fermo su una voce, e lo spegne solo all'uscita.

## sdkconfig

La sdkconfig attuale è già sufficiente:

```
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_CONTROLLER_ENABLED=y
CONFIG_BT_NIMBLE_ROLE_BROADCASTER=y
CONFIG_BT_NIMBLE_ROLE_OBSERVER=y
CONFIG_BT_NIMBLE_EXT_ADV=n
CONFIG_BT_NIMBLE_NVS_PERSIST=n
CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=4096
```

`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=3` va bene (il componente non apre
connessioni). Non serve `nvs_flash` nel componente. Le API usate sono quelle
host NimBLE (`host/ble_gap.h`, `host/ble_hs_id.h`), non `esp_gap_ble_api.h`.

## Limiti noti / rischi

- **Nessuna rotazione del MAC**: si usa l'indirizzo del controller (pubblico
  se disponibile, altrimenti quello inferito da `ble_hs_id_infer_auto`). Un OS
  vicino può deduplicare/rate-limitare i popup. La rotazione con
  `ble_hs_id_set_rnd` è possibile ma richiede un ciclo stop/set/start in più e
  non è stata portata per tenerlo semplice.
- Il conteggio TX conta gli slot di advertising riavviati, non i singoli
  pacchetti in aria (con intervallo 60 ms ogni slot trasmette più volte).
- I payload Apple/Fast Pair/Swift Pair sono quelli del 2.x: gli ID dei modelli
  cambiano nel tempo e alcuni OS recenti potrebbero non mostrare più il popup.
- Advertising legacy (31 byte). Con `CONFIG_BT_NIMBLE_EXT_ADV=y` il codice
  continua a funzionare ma non usa l'extended advertising.
- Lo scanner è attivo di default (invia SCAN_REQ). Non è un'attività
  offensiva e non è dietro avviso legale; se serve un ascolto puramente
  passivo, impostare `params.passive = 1` in `startScan()`.
- Memoria statica: circa 2,3 KB per la lista scan (32 voci) più le tabelle
  payload; il callback non alloca e non usa `std::vector`.

## Uso responsabile

Strumento per laboratorio personale autorizzato. Lo spam trasmette
advertising falsificati che possono generare popup su dispositivi vicini:
usarlo solo su dispositivi propri o in un ambiente RF isolato e autorizzato.
L'avviso legale è obbligatorio e non va rimosso.
