# orbit_wifi

Modulo WiFi di OrbitOS 3.0 per M5Stack Cardputer ADV (ESP32-S3, 8 MB flash,
nessuna PSRAM). Sostituisce `src/modules/wifi/WifiMenu.cpp` del firmware 2.x:
niente Arduino `WiFi.h`, solo ESP-IDF 5.4.

## Integrazione

Dipendenze del componente: `orbit_ui`, `orbit_hal`, `orbit_store`,
`orbit_mascot` (premi di fine attacco), `esp_wifi`, `esp_netif`, `esp_event`,
`nvs_flash`, `esp_http_server`.

Il componente espone solo:

```cpp
#include "orbit_wifi/Wifi.h"
orbit_wifi::runMenu();
```

Per aggiungerlo alla Home (fuori dallo scope di questo componente):

```cpp
// orbit_app/src/Home.cpp
menu.addItem("WiFi", "wifi", [] { orbit_wifi::runMenu(); });
```

e `orbit_wifi` nelle `REQUIRES` di `orbit_app/CMakeLists.txt`.

## Voci di menu

- **Scanner reti**: scansione attiva su tutti i canali 1-13, massimo 24 reti
  ordinate per RSSI. Etichetta con SSID, RSSI, canale, sicurezza abbreviata e
  `*` per le reti aperte. ENTER apre dettagli, deauth o evil twin sulla rete
  selezionata.
- **AP info**: SSID/BSSID, IP, gateway, canale, RSSI e MAC STA se la stazione e'
  connessa. Se non lo e', lo dice e non tenta una connessione (usa Store o
  Impostazioni).
- **WiFi Attacks** (sottomenu con accento rosso e riga
  `!SOLO SU RETI PROPRIE`):
  - **Deauth target**: scan, scelta AP, deauth broadcast reason 7 per 60 s,
    circa 20 frame/s.
  - **Deauth flood**: salta i canali 1-13 e invia deauth broadcast generico +
    deauth verso i BSSID osservati, per 60 s.
  - **Beacon spam**: beacon falsi con BSSID casuale e SSID ruotati, canali
    1-13, 100 ms per frame. Durata scelta da menu: 30/60/120 s.
  - **Evil twin + portal**: AP aperto con lo SSID clonato sul canale del
    target, server HTTP su porta 80 (`/`, `/post`, wildcard `/*` -> 302).
    Massimo 4 credenziali in RAM; nessuna scrittura su SD o NVS.
- **Analizzatore canali**: conteggio AP e barra RSSI migliore per canale,
  ENTER per riscansione.

## Gate legale

Ogni flusso offensivo chiama `ui::legalWarning(attivita', {riga}, "WiFi Attacks")`
e ritorna subito se l'utente nega. Il gate e' dentro le funzioni di attacco, non
solo nel menu, quindi vale anche per gli attacchi lanciati dallo scanner. Le
schermate di attacco e i messaggi di errore usano l'accento rosso.

## Limiti noti

- **Deauth e la patch IDF 5.4 (importante).** Il blob chiuso `libnet80211.a`
  chiama `ieee80211_raw_frame_sanity_check`, che accetta solo beacon, probe,
  action e data non-QoS: deauth (`0xC0`) e disassoc (`0xA0`) vengono rifiutati
  con `ESP_ERR_INVALID_ARG`. Ne' `--wrap` ne' un override forte funzionano
  perche' la chiamata e' interna all'oggetto. Il binario consegnato e' quindi
  patchato a livello di funzione con `tools/patch_raw_tx.py`:
  il prologo diventa `movi.n a2, 0; retw.n` (ritorno 0 con windowed ABI) e
  `esptool elf2image` rigenera checksum e SHA-256. Verificato su hardware:
  `deauth-tx=OK (ESP_OK)`. Ogni `idf.py build` normale rigenera un binario
  senza patch: usare `tools/build_offensive.ps1`.
  La patch disattiva un controllo per tutti i raw TX: solo dispositivo di
  laboratorio. Beacon spam, portal, scanner e analizzatore non dipendono.
- Nessuna cattura PMKID/EAPOL, nessun handshake, nessun PCAP/sniffer: fuori
  scope per questo componente.
- Deauth solo broadcast (DA `FF:FF:FF:FF:FF:FF`); non traccia i client
  associati per inviare deauth unicast.
- Il country viene forzato a `IT` (1-13 canali, 20 dBm) ogni volta che la
  radio parte, se il driver non e' gia' su IT/13. Il default IDF e' "world
  safe" 1-11. Per un altro dominio modificare `applyCountry()` in
  `src/Wifi.cpp`: il cambio ha effetto regolatorio, non usarlo fuori dall'Italia
  senza verificare le norme locali.
- Portale senza DNS hijack: il telefono puo' non aprire il portale in
  automatico. Aprire `http://192.168.4.1/` a mano. Il wildcard risponde 302 a
  `/` per le richieste captive.
- Massimo 4 credenziali, valori troncati a 30/28 caratteri nella schermata
  finale (il display e' 240x135). I buffer vengono azzerati dopo la visualizzazione.
- Massimo 24 AP in lista; la scansione e' bloccante per circa 1-2 secondi.
- Gli SSID non ASCII vengono ripuliti a `?` per il display e per il clone; un
  AP nascosto clonato usa `WiFi_Test`.
- A fine modulo il driver viene fermato e deinizializzato con
  `orbit_store::wifiShutdownDriver()`, cosi' lo Store puo' riconnettersi.
- I premi mascotte (`WifiDeauth`, `WifiDeauthFlood`, `WifiBeacon`, `EvilTwin`)
  partono solo se almeno un frame e' stato trasmesso (o dopo 10 s di AP), come
  nel firmware 2.x.
- La whitelist/NetGuard del firmware 2.x non e' stata portata: nessun blocco
  automatico basato su `/wifilab/whitelist.txt`.

## CONFIG richiesti

Gia' presenti in `firmware/sdkconfig`:

- `CONFIG_ESP_WIFI_ENABLED=y` (driver WiFi)
- `CONFIG_ESP_WIFI_SOFTAP_SUPPORT=y` (necessario per Evil twin; senza, il
  componente non compila)
- `CONFIG_HTTPD_MAX_REQ_HDR_LEN=512`
- `CONFIG_HTTPD_MAX_URI_LEN=512`
- `CONFIG_LWIP_MAX_SOCKETS=10` (il server HTTP usa fino a 7 socket)
- `CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384`

Nessun PSRAM: picco heap usato dalla scansione circa 2-3 KB (24 record AP),
server HTTP con stack 6 KB, credenziali 512 byte statici.

## File

- `include/orbit_wifi/Wifi.h` - API pubblica (`runMenu`).
- `src/WifiInternal.h` - tipi e helper condivisi.
- `src/Wifi.cpp` - menu, scanner, AP info, analizzatore, driver radio condiviso.
- `src/Attacks.cpp` - deauth, flood, beacon spam, evil twin + portale HTTP.

## Note di verifica

Build completo e test hardware via seriale (COM5): scansione 2 reti,
connessione STA, `deauth-tx=OK`, `beacon-tx=OK`, `deauth-ap=OK`. Il test
`orbit_wifi::selfTest()` e' compilato solo con `ORBIT_DIAG_SELFTEST=ON`.
