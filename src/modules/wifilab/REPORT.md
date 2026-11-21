# 06 — WiFi Lab: REPORT

Modulo: `src/modules/wifilab/` (solo `WifiLabMenu.cpp`; `WifiLabMenu.h` invariato).
Nessun file condiviso modificato (`main.cpp`, `platformio.ini`, `ModuleFlags.h`,
`Config.cpp`, `WifiMenu.cpp` intatti).

## 1. Cosa è stato implementato

- **Menu WiFi Lab** con quattro voci: PMKID Hunt, Client Monitor, Whitelist,
  Spettro 2.4 GHz. Header/footer ORBIT, UI italiana, nessun `delay()` lungo
  (il massimo sono finestre di ascolto da 200 ms con poll della tastiera ogni
  4 ms).
- **PMKID Hunt attivo**
  - scansione AP sui canali 1-13 (country IT forzato, radio in STA + promiscuo);
  - selezione multipla di massimo **3 target** in RAM;
  - invio di auth open system e assoc request (RSN WPA2-PSK/CCMP) con MAC STA
    locale casuale per target;
  - cattura dei frame data con un anello statico di 6 slot da 512 byte
    (nessun buffer da 64 KB), parsing EAPOL sul task principale;
  - accettazione del solo M1 (`pairwise && ack && !MIC`) che contenga un IE
    vendor `00:0F:AC:04` con 16 byte di PMKID reali;
  - salvataggio `/wifilab/pmkid/<bssid>.hc22000` (riga `WPA*01*...`) e
    `/wifilab/pmkid/<bssid>.pcap` (linktype 105, beacon/probe response del
    target + frame EAPOL);
  - stato per target: `cerco`, `M1`, `senza ESSID`, `trovato`, `n/d`;
  - stop/drenaggio del promiscuo e chiusura file all'uscita.
- **Client Monitor**
  - scelta dell'AP, canale fisso sull'AP;
  - raccolta client dai frame data (toDS/fromDS, QoS escluso il vettore di
    indirizzi non valido), tabella statica di 16 client con MAC, RSSI, numero
    frame e ultimo avvistamento;
  - lookup OUI locale per i vendor più comuni (Apple, Samsung, Intel,
    Espressif, Huawei, Xiaomi, TP-Link, Cisco, Google, Amazon, Microsoft,
    Raspberry, Sonos, Nintendo, Sony, LG, Netgear, Asus, Ubiquiti, Philips,
    D-Link); MAC locally-administered mostrato come `random`;
  - deauth del **solo client selezionato** (DA = client, SA/BSSID = AP),
    rifiutata se il client non è più in tabella o non è stato visto negli
    ultimi 10 s. Nessun frame broadcast: gli altri client non vengono toccati.
- **Whitelist**
  - file `/wifilab/whitelist.txt`, una voce per riga: `BSSID|SSID`, `BSSID|`,
    `|SSID`; righe `#` ignorate;
  - normalizzazione BSSID (maiuscole/minuscole, `:`, `-`, spazi, 12 hex senza
    separatori), SSID con trim di spazi/CR e rimozione di virgolette; il
    confronto SSID ignora maiuscole/minuscole (scelta conservativa: meglio
    escludere una rete in più che attaccarne una protetta);
  - massimo 24 voci, file massimo 4 KB;
  - **fail-closed**: file assente, > 4 KB, con righe malformate o con più di
    24 voci valide ⇒ `g_whiteReady = false` e PMKID/deauth bloccati, perché
    non è possibile garantire l'esclusione;
  - un AP whitelisted (per BSSID o per SSID) non viene mai selezionato per
    PMKID e non riceve auth/assoc/deauth; il controllo è ripetuto anche
    all'avvio del target e in `deauthClient`.
  - aggiunta di un AP dalla UI (crea il file) e creazione esplicita di un file
    vuoto con conferma (`N`).
- **Spettro 2.4 GHz**
  - promiscuo su tutti i tipi di frame, dwell 60 ms per canale, sweep 1-13;
  - barre live con scala automatica, canale corrente evidenziato;
  - **peak hold** per canale (linea gialla);
  - **PPS** calcolato sulla finestra reale dello sweep + totale frame;
  - **waterfall** 13 colonne × 16 righe (2 px per riga), intensità in 4
    livelli, aggiornata a ogni sweep;
  - uscita con ESC e stop del promiscuo.
- **Legal gate**
  - `ui::legalWarning` prima della scansione/attività PMKID e prima di ogni
    deauth;
  - `esp_wifi_80211_tx` presente solo nel blocco `#if ENABLE_OFFENSIVE`; con
    flag 0 il menu PMKID e il deauth mostrano un messaggio e non trasmettono
    nulla (auth/assoc non sono nemmeno compilate).
- **Heap / radio**
  - pool statici: 32 AP, 24 voci whitelist, 3 target PMKID, 16 client, 6 slot
    RX da 512 byte (~3 KB), waterfall 208 byte, tabella OUI in flash;
  - nessuna allocazione dinamica nel percorso radio; `File` aperti solo per
    i target PMKID e chiusi sempre;
  - `RxGuard` garantisce `esp_wifi_set_promiscuous(false)`, callback a null,
    filtro a null, canale 1 e `WiFi.mode(WIFI_OFF)` su ogni uscita.

## 2. Comandi / tasti

| Schermata | Tasti |
|---|---|
| Menu WiFi Lab | `ENTER` apre, `` ` `` (ESC fisico) torna |
| PMKID: scelta target | `;`/`.` su/giù, `ENTER` seleziona/deseleziona (max 3), `S` avvia, `` ` `` annulla |
| PMKID: caccia | `` ` `` interrompe (max 120 s) |
| Client Monitor: scelta AP | `;`/`.` , `ENTER`, `` ` `` |
| Client Monitor | `;`/`.` sceglie il client, `D` deauth del selezionato, `` ` `` esce |
| Whitelist | `A` aggiunge un AP scansionato, `N` crea file vuoto (con conferma), `` ` `` esce |
| Spettro 2.4 | `` ` `` esce |

Legenda: `;` = freccia su, `.` = freccia giù (layout Cardputer).

## 3. Formati su microSD

### `/wifilab/whitelist.txt`

```text
# commento
AA:BB:CC:DD:EE:FF|MiaRete
AA:BB:CC:DD:EE:FF|
|SoloPerNome
```

- BSSID: 12 cifre hex, separatori `:`/`-`/spazi opzionali.
- SSID: testo fino a 32 byte, spazi esterni rimossi, virgolette esterne
  rimosse.
- Una voce con BSSID protegge quel BSSID; una voce con solo SSID protegge
  tutte le reti con quel nome; una voce con entrambi protegge BSSID e SSID.
- File assente / > 4 KB / riga malformata / > 24 voci: attacchi bloccati.

### `/wifilab/pmkid/<bssid>.hc22000`

Base nome: BSSID in minuscolo senza separatori, es. `aabbccddeeff.hc22000`.
Riga prodotta (formato hashcat 22000):

```text
WPA*01*<pmkid 32 hex>*<bssid 12 hex>*<sta 12 hex>*<essid hex>***
```

- `<sta>` è il MAC locale casuale usato per l'associazione: è quello per cui
  l'AP ha calcolato il PMKID.
- Il file viene scritto solo se l'ESSID è noto e la riga supera il controllo
  sintattico interno (`validHc22000Line`) e la dimensione su SD.
- Rete nascosta / beacon senza SSID ⇒ **nessun** `.hc22000` (resta il pcap per
  analisi successiva). Un PMKID senza ESSID non viene conteggiato come
  successo.

### `/wifilab/pmkid/<bssid>.pcap`

- Global header PCAP little-endian, linktype **105** (802.11), snaplen 512.
- Frame senza FCS (4 byte rimossi da `sig_len`, come in `WifiMenu.cpp`).
- Contiene i beacon/probe response del solo BSSID target (per ricavare
  l'ESSID) e i frame EAPOL del target.
- Il file viene troncato a ogni sessione e chiuso all'uscita.

## 4. Cosa non è stato possibile / limiti

- **Nessun test su hardware**: in questo ambiente non c'è una Cardputer ADV
  collegata, quindi non è stato possibile verificare PMKID reali, client
  monitor o spectrum su aria. La build è verde, ma i test RF sono manuali.
- **hashcat non installato** nell'ambiente di lavoro: la riga hc22000 è stata
  validata con il controllo sintattico interno (`validHc22000Line`) e con un
  parser Python che applica la stessa struttura (`WPA*01*` + 32/12/12 hex +
  ESSID hex + `***`); non è stato possibile lanciare
  `hashcat -m 22000 --identify` né un crack con PSK di test. Il test con PSK
  nota è descritto sotto e va eseguito su un PC con hashcat.
- **PMKID dipende dall'AP**: molti AP includono il PMKID nell'M1 solo per
  client in roaming o non lo includono affatto; WPA3-only, Enterprise e PMF
  possono rifiutare l'associazione. In quei casi il target finisce `n/d`.
- **Rete nascosta**: senza ESSID il file `.hc22000` non viene creato di
  proposito; il pcap può essere convertito/analizzato a parte.
- **Un solo PMKID per target**: il nome file è fissato dal BSSID (come da
  specifica) e viene sovrascritto alla sessione successiva. Il pcap viene
  troncato a ogni sessione.
- **Whitelist fail-closed**: con file assente non si può attaccare. Per una
  rete di test bisogna creare la whitelist (vuota con `N`, oppure con una
  voce che non sia l'AP di test).
- **Solo 2.4 GHz**, canali 1-13; nessun 5 GHz (hardware e specifica).
- **PPS/spettro**: un solo radio, quindi il valore è aggregato sullo sweep
  (circa 780 ms), non un analizzatore a banda intera simultanea.
- **FCS**: si assume che `sig_len` includa i 4 byte di FCS, come nel codice
  esistente `WifiMenu.cpp`. Se una versione del driver non li includesse, il
  pcap perderebbe 4 byte in coda (l'analisi EAPOL resta corretta perché usa
  le lunghezze dichiarate).
- **Deauth client**: funziona solo se il client è stato visto negli ultimi
  10 s e se l'AP non è in whitelist; non è un deauth flood e non tocca gli
  altri client.

## 5. Dimensione firmware

Build `cardputer-adv-offensive` (PlatformIO 6.7.0, Arduino-ESP32 2.0.16,
`-Os`), ultima build verde:

- `firmware.bin`: **2.398.048 byte**.
- Flash usata da PlatformIO: **2.397.689 / 2.621.440 byte (91,5%)** dello slot
  applicativo OTA da 2,5 MB; restano circa **223 KB** liberi.
- RAM statica: **122.072 / 327.680 byte (37,3%)**.
- Firmware ELF: `.pio/build/cardputer-adv-offensive/firmware.elf`.
- `firmware.bin`: `.pio/build/cardputer-adv-offensive/firmware.bin`.

## 6. Test manuali suggeriti (solo rete propria)

1. **Whitelist / fail-closed**
   - con SD senza `/wifilab/whitelist.txt`, aprire PMKID Hunt: deve comparire
     il blocco e nessun frame deve partire;
   - creare il file vuoto da `WiFi Lab > Whitelist > N` (conferma) e verificare
     che PMKID riparta;
   - aggiungere il BSSID dell'AP di test e verificare che PMKID lo rifiuti e
     che il deauth sia bloccato.
2. **PMKID su AP di test**
   - AP WPA2-PSK proprio, canale 1-13, SSID noto, PSK di test (es.
     `orbit-test-123`, mai reale);
   - PMKID Hunt, selezionare l'AP, `S`, attendere `trovato` o `n/d`;
   - controllare `aabbccddeeff.hc22000` e il pcap; copiare su PC e lanciare
     `hashcat -m 22000 <file>.hc22000 wordlist.txt` con la PSK di test in
     wordlist. Verificare anche `hashcat -m 22000 --identify <file>`.
   - Se l'AP non manda PMKID, provare con un secondo AP; il limite è dell'AP.
3. **Client Monitor**
   - collegare due client (A e B) all'AP di test, generare traffico;
   - verificare MAC, RSSI e vendor; selezionare A e premere `D`;
   - controllare che A si disconnetta/riconnetta e che **B resti connesso**;
   - verificare che dopo 10 s senza traffico il deauth di A venga rifiutato
     come stale.
4. **Spettro**
   - aprire Spettro 2.4: barre e waterfall devono muoversi con traffico
     reale (download su un client 2.4 GHz), PPS > 0, peak hold che resta;
   - all'uscita il WiFi del Cardputer deve essere spento e il menu principale
     deve continuare a funzionare.

## 7. Rischi noti

- Le funzioni attive sono compilate solo in `cardputer-adv-offensive`
  (`ENABLE_OFFENSIVE=1`). La build pubblica `cardputer-adv` non contiene
  auth/assoc/deauth.
- L'uso su reti non proprie è illegale; l'avviso legale è obbligatorio e il
  codice non esegue TX se il flag è 0.
- Il deauth agisce su un solo client e non è pensato per interrompere intere
  reti; resta comunque una trasmissione 802.11 non autorizzata se usata fuori
  da un ambiente di test.
