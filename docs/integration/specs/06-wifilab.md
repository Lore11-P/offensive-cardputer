# 06 — WiFi Lab: PMKID, client monitor, whitelist, waterfall 2.4

## Obiettivo
Portare in ORBIT le funzioni WiFi di PORKCHOP che non richiedono hardware.

## Requisiti
- Modulo `src/modules/wifilab/` con menu:
  - PMKID Hunt: scan AP, invio auth/assoc, cattura M1 con PMKID, salvataggio
    `/wifilab/pmkid/<bssid>.hc22000` e pcap; stato per target.
  - Client Monitor: selezione AP, elenco client (MAC, RSSI, OUI vendor),
    deauth del singolo client.
  - Whitelist: `/wifilab/whitelist.txt` (`BSSID|SSID`), esclusione da attacchi.
  - Spettro: barre + waterfall 2.4 GHz canali 1-13, PPS, peak hold.
- Gestire promiscuo e raw TX con le API esp_wifi; stop e deinit all'uscita.
- `ui::legalWarning` + `ENABLE_OFFENSIVE` per ogni TX; nessun TX se flag 0.
- Riusare idee/strutture da `WifiMenu.cpp` ma senza modificarlo.
- Heap: pool statici, nessun buffer da 64 KB; max 3 target PMKID in RAM.

## Accettazione
- Build OK.
- Lo spettro mostra attività reale.
- Il client monitor elenca client di un AP di test.
- Il PMKID hunt salva un file `.hc22000` valido (test su rete propria).
- `REPORT.md` con limiti, comandi, dimensione firmware.

## Attenzione
- Non toccare `main.cpp`, `platformio.ini`, `ModuleFlags.h`, `WifiMenu.cpp`.
- Solo 2.4 GHz: niente 5 GHz.
