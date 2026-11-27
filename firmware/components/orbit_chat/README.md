# orbit_chat

Chat BLE del firmware 2.x (`src/modules/chat/Chat.cpp`) portata su ESP-IDF 5.4
con il solo host NimBLE. Il protocollo e' **identico**: nessun GATT, nessuna
connessione, solo advertising manufacturer e scanning passivo.

## Protocollo (byte-compatibile col 2.x)

Tutto viaggia in un unico AD structure di manufacturer data (tipo `0xFF`),
lungo esattamente 31 byte (limite legacy):

```
[0x1E][0xFF][FF FF][contenuto 27 byte]
```

Il valore manufacturer inizia con il company ID `0xFFFF` (little endian),
quindi il pacchetto logico e' lungo 29 byte:

- messaggio (`magic 0xC8`):
  `FFFF C8 | nick[6] | tag[2] | counter u16 LE | cipher[16]`
- controllo (`magic 0xC9`):
  `FFFF C9 | type u8 | name[6] | queryId u32 LE | zeri fino a 29 byte`

Dettagli mantenuti dal 2.x:

- nick lungo 6 byte, riempito con spazi (`' '`, non NUL);
- `tag` = primi 2 byte di SHA-256 del nickname destinatario canonicalizzato
  (printable, trim a destra, minuscolo);
- chiave AES-128 = SHA-256(`sender + 0x1F + recipient`), sempre canonicalizzati;
- IV = primi 16 byte di SHA-256(`materiale + 0x1F + counter` in decimale);
- testo in chiaro di 16 byte: testo (max 15) + padding PKCS#7-like
  (byte di padding = quantita' di padding);
- control type: `1` query, `2` response, `3` presence;
- presence ogni 5 s, timeout peer 15 s, risposta alla query tenuta in onda
  600 ms, avviso peer 4 s, storico 8 messaggi, contatori per indirizzo;
- advertising non-connectable a 100 ms; scan passivo 50/50 ms senza filtro
  duplicati (il 2.x usava `wantDuplicates = true`).

## API

```cpp
#include "orbit_chat/Chat.h"

orbit_chat::runMenu();   // menu + chat; deinizializza il BT all'uscita
orbit_chat::shutdown();  // idempotente, chiamabile anche fuori dal menu
```

`runMenu()` apre il menu (Apri chat / Destinatario / Info). Alla chiusura della
chat e all'uscita dal menu chiama `shutdown()`: ferma scan e advertising,
`nimble_port_stop()` + `nimble_port_deinit()` e quindi disabilita/deinizializza
il controller BT. Dopo il ritorno il BT non e' inizializzato e l'heap e'
disponibile per i giochi Wren.

## sdkconfig richiesto

Il parent deve aggiungere (in `sdkconfig.defaults` o `sdkconfig`):

```
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_ROLE_OBSERVER=y
CONFIG_BT_NIMBLE_ROLE_BROADCASTER=y
CONFIG_BT_NIMBLE_ROLE_CENTRAL=n
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=n
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1
CONFIG_BT_CONTROLLER_ENABLED=y
```

Consigliati per ridurre RAM:

```
CONFIG_BT_NIMBLE_NVS_PERSIST=n
CONFIG_BT_NIMBLE_EXT_ADV=n
CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=4096
```

`CONFIG_BT_NIMBLE_MAX_CONNECTIONS` non ammette 0: 1 e' il minimo. Il componente
non crea connessioni, quindi observer + broadcaster bastano.

## Scelte e limiti noti

- **Nickname fisso `ORBIT`**: `config::chatNickname()` non e' stato portato.
  Il nome resta di 5 caratteri dentro il campo da 6; cambiarlo accorcia il
  padding ma non rompe il layout. Il destinatario predefinito e' pure `ORBIT`
  e si puo' modificare a runtime dal menu (solo RAM, nessun NVS).
- **Nessuna verifica di disponibilita' del nickname**: il 2.x la faceva
  (`kControlQuery` in uscita, timeout 1500 ms). La ricezione e la risposta alle
  query restano implementate, quindi un dispositivo 2.x vede `ORBIT` occupato.
- **Notifiche sempre attive**: il toggle `chatNotify` del 2.x non e' portato;
  presence e avviso peer sono sempre attivi (default 2.x).
- **Nessun beep**: lo speaker non e' usato; l'avviso peer e' solo a schermo.
- **API NimBLE, non Bluedroid**: in IDF 5.4 NimBLE non espone
  `esp_gap_ble_api.h`/`esp_ble_gap_*`. Sono usate le API host NimBLE
  equivalenti: `ble_gap_adv_set_data()` (raw AD), `ble_gap_adv_start()`,
  `ble_gap_disc()` + `BLE_GAP_EVENT_DISC`. `nimble_port_init()` fa gia'
  controller init/enable una sola volta; non viene chiamato
  `esp_bt_controller_enable()` a mano.
- **Init/deinit per sessione**: l'host viene inizializzato all'apertura della
  chat e deinizializzato alla chiusura; ogni nuova apertura rifa' il ciclo
  completo (supportato da IDF, vedi esempio `blecent` INIT_DEINIT_LOOP).
  Il componente possiede lo stack BT: nessun altro modulo deve tenerlo
  inizializzato mentre la chat e' in uso o chiamare `shutdown()` sotto un
  altro utente BLE.
- **Errore di stop**: se `nimble_port_stop()` fallisce (caso raro), il BT non
  viene deinizializzato a forza per non distruggere il task host ancora vivo;
  l'errore viene loggato.
- La chiave AES deriva solo dai nickname: chi li conosce puo' decifrare. Non
  usare per dati sensibili (stesso limite del 2.x).
