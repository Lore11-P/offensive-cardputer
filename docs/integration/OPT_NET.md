# OPT-NET — ottimizzazione WiFi / BLE / Radar / Chat

Ambito: solo RAM, frammentazione, CPU e chiusura risorse. Nessuna modifica a
funzioni, comandi, testi UI, layout o formati file (PCAP, hc22000, NVS).

File toccati:

- `src/modules/wifi/WifiMenu.cpp`
- `src/modules/radar/RadarMenu.cpp`
- `src/modules/ble/BleSpam.cpp`
- `src/modules/chat/Chat.cpp`
- `src/utils/PcapTools.cpp`
- `src/utils/WifiSetup.cpp`
- questo documento

Nessun header pubblico è cambiato.

## Misure

Build `cardputer-adv-offensive`, PlatformIO, Arduino-ESP32 2.0.16 / IDF 4.4.5.
Misure ottenute con la stessa toolchain: baseline da `HEAD` (c7456af), dopo le
modifiche, sorgenti identici a parte i sei file sopra.

| Voce | Prima | Dopo | Delta |
|---|---:|---:|---:|
| Flash usata (linker) | 2.358.193 B | 2.344.921 B | **−13.272 B (−0,56%)** |
| `firmware.bin` | 2.358.560 B | 2.345.280 B | **−13.280 B** |
| RAM statica (`.data`+`.bss`) | 115.520 B (35,3%) | 121.576 B (37,1%) | **+6.056 B** |
| Slot OTA residuo | 262.847 B | 277.119 B | +13.272 B |

La RAM statica cresce perché i pool prima erano heap (o stack) e ora sono in
BSS: +1,8 KB BleSpam, +1,5 KB Radar, +0,8 KB Chat, +1,8 KB WifiMenu,
+0,1 KB WifiSetup. In cambio l'heap non cresce più a ogni apertura/chiusura e
i callback non allocano. Con 327.680 B di SRAM restano ~206 KB di heap al boot.

Codice per oggetto (`size`, text / data+bss):

| Oggetto | Prima | Dopo |
|---|---:|---:|
| WifiMenu | 87.451 / 257 | 79.438 / 2.043 |
| RadarMenu | 37.556 / 1.405 | 33.157 / 2.932 |
| BleSpam | 21.742 / 36 | 19.706 / 1.844 |
| Chat | 27.490 / 145 | 22.061 / 913 |
| PcapTools | 15.495 / 0 | 15.519 / 0 |
| WifiSetup | 23.398 / 0 | 24.441 / 128 |

### Allocazioni per ciclo (stima da codice)

Non c'è hardware collegato: i conteggi sono per ispezione dei path, non da
heap profiler. Le `std::string` fino a 15 caratteri usano SSO e non allocano;
i numeri sotto contano solo le stringhe più lunghe e i contenitori.

| Path | Prima | Dopo |
|---|---|---|
| `chooseIndex` / `chooseList` (16–25 fps, per riga visibile) | 1 alloc/riga/frame | 0 |
| Finder BLE (25 fps) | ~2–4 alloc/frame | 0 |
| Radar BLE PPI (25 fps) | ~1–2 alloc/frame | 0 |
| Grafico banda (40 fps) | ~2–3 alloc/frame | 0 |
| Callback advertising NimBLE (per pacchetto) | 2–3 alloc | **0** |
| Chat: ricezione messaggio | map/vector + stringhe | 0 (slot fissi, SSO) |
| BLE spam: per pacchetto advertising | 1–2 alloc | **0** |
| Sniffer WiFi: apertura/chiusura | ~3,2 KB malloc/free + 12,8 KB ring | ring 12,8 KB, collector riusato |
| Impostazioni WiFi: schermata ferma | NVS + 2 `String` ogni 20 ms | solo su azione utente |
| Conversione PCAP: frame non EAPOL | doppia `parseEapol` | una sola classificazione |

## Modifiche

### WifiMenu

- Pool statico `g_networks` riusato da scanner, deauth, evil twin/portal: il
  vettore conserva la capacità tra le aperture.
- I dettagli della rete usano l'indice nel pool invece di copiare `Network`
  (due `std::string`) dentro ogni callback del menu.
- `stopPromiscuous()` unico per analyzer, jam detect, sniffer, deauth, beacon
  spam e deauth flood: promiscuo off, callback a `nullptr`, canale 1, radio off.
- Sniffer: `LiveCollector` statico (il buffer ~3 KB non viene più
  malloc/free a ogni sessione), slot di drain in BSS (1,6 KB in meno sullo
  stack del loop), stesso ring da 8×1600 B e stesso PCAP.
- `chooseIndex` riusa uno scratch per il troncamento delle righe.
- Testi di analyzer, jam detect, sniffer, port scan, host scan, netcut, deauth,
  beacon spam, deauth flood e AP fittizio formattati con un buffer statico:
  stringhe visualizzate identiche.
- NetCut: MAC in `char[18]` invece di `std::string` per dispositivo.
- TCP listener: anello fisso di 5 righe invece di `vector<string>` per frame.
- TCP client: `lastWord` riusa la capacità invece di essere ricostruito.
- Rimossa la copia locale di `securityName`, ora `wifisetup::securityName`.
- Eliminate le tre copie del loop che costruiva le etichette di rete.

### RadarMenu

- `BleDevice.address/name` fissi; l'indirizzo si formatta dai byte nativi
  (`getNative()`), il nome si estrae dal payload AD grezzo (`getPayload()`):
  il callback NimBLE non alloca più `toString()`/`getName()`.
- Bersaglio del finder confrontato sui 6 byte nativi con `memcmp`.
- Snapshot dispositivi in BSS (24 slot) invece che sullo stack a ogni frame.
- Un solo oggetto callback condiviso (prima quattro `static` distinti).
- Formattazione di finder, PPI, banda e CSI con scratch statico; troncamento
  nomi con scratch riusato. Footer del finder in stringa statica.
- Rimossi `measureWifiChannels`, `measureWifiBand`, `measureBleBand`,
  `drawBandMeasure`, `formatFloat` e i contatori `g_wifiWindowBytes`: erano
  codice morto (la vista banda usa la macchina a stati di `runBand`).
- Invariati dwell, filtri promiscui, TDM e deinit dello sniffer.

### BleSpam

- Risultati scanner in array fisso da 40 (`ScanResult` con `char[20]` e
  `char[18]`): niente `vector`/`string` nel callback; la lista viene ordinata
  in place dopo aver staccato i callback.
- AD dei payload precompilati una volta per sessione: nel loop di spam non si
  costruisce più una `std::string` per pacchetto.
- `allPayloads()` senza vector di vector.
- Testi di scan/spam con buffer statico.
- Restano invariati stop/reset dell'advertising e ripristino
  `BLE_OWN_ADDR_PUBLIC` all'uscita.

### Chat

- Storico in anello fisso da 8 messaggi con campi `char[]` (nessuna
  `std::string` dentro le sezioni critiche); peer e contatori in array fissi
  (16+16) con indirizzo nativo a 6 byte. Spariscono `std::map`, `std::vector`
  e la crescita illimitata dei peer.
- Callback: manufacturer data estratta dal payload grezzo, parsing su buffer
  fissi, nickname/indirizzo propri in cache. Nessuna allocazione per
  advertising; il messaggio viene copiato nel ring con `copyFixed()`.
- Derivazione chiave/IV e tag su `char[]` (stesso algoritmo, stesso
  separatore 0x1F, stessa canonicalizzazione/limite 6 caratteri).
- Snapshot di disegno su array statico; testi con scratch.
- Invariati: cifratura AES-CBC, formato pacchetto, presence/query/response,
  timeout peer e comportamento stealth.

#### Audit core (post-review)

- **Log seriale**: `Chat.cpp:759` registra solo `byte` e `ctr` del messaggio;
  il testo non viene più scritto su USB. Esito: applicato.
- **Sezione critica**: `ChatMessage` è POD (`Chat.cpp:53`, `copyFixed()` a
  `:61`); `pushMessageLocked` (`:649`) e lo snapshot di `drawChat` (`:788-796`)
  fanno solo copie/`memcpy` sotto `portENTER_CRITICAL`. Parsing, decifratura e
  costruzione stringhe restano fuori. Esito: applicato.
- **Errori NVS**: `wifisetup::saveCredentials` (`WifiSetup.cpp:102`) controlla
  `putString`, rilegge le chiavi e ritorna `false` senza dichiarare il
  salvataggio; `editNickname`/`editRecipient`/`toggleNotifications`
  (`Chat.cpp:981`, `:1003`, `:1136`) verificano `config::save()` e mostrano
  l'errore. Esito: applicato.

### PcapTools

- `convertToHc22000`: i frame non EAPOL vengono saltati subito usando
  `Reader::isEapol()` (già calcolato in `parsePayload`) invece di ripetere
  `parseEapol` su ogni frame. Formato e output identici.
- Il collector live non viene più allocato a ogni sniffer: vedi WifiMenu.

### WifiSetup

- `runSettings` legge le credenziali da NVS una volta e le ricarica solo dopo
  un'azione (prima apriva NVS e allocava due `String` ogni 20 ms).
- Etichette rete e stato con scratch statico, stesso testo.
- `shortenInto` e `textFormat` locali.

## Stack e chiusura risorse

- Loop task: 16 KB (invariato). Tolta dallo stack la copia da 1,6 KB per
  frame dello sniffer e gli array da 24 dispositivi del radar; i buffer
  equivalenti sono in BSS.
- Ogni uscita da promiscuo passa da `stopPromiscuous()` o `endSniffer()`:
  callback rimosso, canale 1, radio off.
- Sniffer: flush + close del PCAP prima di spegnere; hc22000 scritto dopo.
- Deauth/beacon/flood: stop promiscuo e radio off su ESC, timeout e errori.
- BLE spam: `stop()` + `reset()` + indirizzo pubblico.
- Chat/Radar: `scan->stop()` e callback staccati; NimBLE resta inizializzato
  (deinit/init ripetuti sono fragili, scelta preesistente documentata nei
  commenti).
- Evil Twin/Portal: server/DNS chiusi e credenziali RAM azzerate (invariato).

## Test manuali

1. **WiFi scanner**: aprire/chiudere 10 volte; al seriale l'heap dopo ogni
   chiusura deve tornare allo stesso valore (± poche centinaia di byte).
2. **Channel Analyzer / Jam Detect**: uscita con ESC e a timeout; verificare
   che il menu successivo possa rifare uno scan (radio non appesa).
3. **Sniffer**: cattura 30 s, ESC, controllare PCAP in Wireshark e hc22000 in
   hashcat; ripetere due volte e verificare che il file nuovo non sovrascriva
   il precedente.
4. **Deauth/Beacon/Flood** (solo build offensiva, rete propria): avvio, ESC,
   radio spenta; nessun frame dopo l'uscita.
5. **NetCut**: scan, VIP, cut, troll, restore; uscita e rientro.
6. **Evil Twin/Portal**: aprire, collegare un client di test, uscire; SSID e
   server non devono restare attivi.
7. **Radar Find BLE**: due dispositivi, selezione target, 60 s di inseguimento
   con bussola; ESC e riavvio.
8. **Radar BLE** e **Banda 2.4 GHz**: 5–10 minuti; grafico fluido, ESC
   reattivo, heap loggato stabile.
9. **BLE Scanner**: 4 round, dettagli, poi spam con ogni payload; verifica
   stop e popup assenti dopo l'uscita.
10. **Chat**: due Cardputer, nickname duplicato rifiutato, messaggio
    inviato/ricevuto, duplicato ignorato, presence, stealth, apertura/chiusura
    10 volte. Sul seriale devono comparire solo lunghezza e contatore, mai il
    testo; con NVS piena/quasi piena il salvataggio deve dichiarare l'errore.
11. **Impostazioni > Rete WiFi**: schermata ferma 60 s (niente traffico NVS),
    scan/connect/salva/dimentica.
12. **Capture**: conversione PCAP e estrazione EAPOL su una cattura di test.

## Rischi e limiti

- **RAM statica**: +6,5 KB permanenti. In cambio l'heap è deterministico;
  se in futuro serve più heap, i pool più grandi da ridurre sono
  `g_sniffSlots` (12,8 KB, già solo durante lo sniffer) e i 40 slot BleSpam.
- **Chat cross-version**: la crittografia è stata riscritta su buffer fissi
  mantenendo algoritmo e formato, ma va verificata tra due firmware
  (vecchio↔nuovo) prima del rilascio; un errore nel separatore/ordine byte
  renderebbe i messaggi non decifrabili.
- **Limiti fissi**: Chat tiene 16 peer e 16 contatori; in un ambiente molto
  affollato il peer più vecchio viene sostituito e potrebbe ricevere un
  secondo avviso di presenza. BleSpam resta a 40 dispositivi e Radar a 24,
  come prima.
- **Sniffer**: il ring resta su heap (12,8 KB) e viene liberato alla chiusura;
  il collector live ora resta allocato (~3 KB) per tutta la sessione.
- **NVS**: `saveCredentials` verifica con rilettura; le impostazioni Chat
  chiamano `config::save()` una seconda volta per conoscere l'esito (NVS viene
  scritta due volte, solo su azione utente). I testi UI restano invariati nel
  caso nominale; compaiono solo i messaggi di errore se il salvataggio fallisce.
- **PcapTools**: il salto dei frame non EAPOL si basa su `isEapol()` calcolato
  da `parsePayload`; se in futuro `parsePayload` cambiasse semantica, va
  riallineato.
- Non testato su hardware in questa sessione: build e analisi statica only.
