# ORBIT OS — UI finale: menu a hub, icone e coerenza

Branch: `feat/ui-final` — worktree `orbit-wt-ui-final`.
Perimetro: solo UI (menu principale, breadcrumb, icone, hint). Nessuna modifica
a moduli, comandi, schermate interne, `Config.cpp`, `partitions.csv`,
`flasher/`, `platformio.ini`, `ModuleFlags.h`.

## Struttura menu prima

Carosello principale con 20 voci in sequenza:

`BLE · Chat · BLE HID · WiFi · WiFi Lab · WiFi+ · WPA-SEC · ESP-NOW ·
Radar · Radio · Musica · Audio · Capture · BadUSB · Infrarossi · Giochi ·
SD · Utility · Sensori · Impostazioni`

Problemi: voci WiFi/BLE mescolate a media, strumenti e sistema; icone ripetute
(`ble` per BLE/BLE HID, `wifi` per tre voci, `chat` per Chat/ESP-NOW, `music`
per Musica/Audio, `radar` per Radar/Sensori, `settings` per Utility/Impostazioni).

## Struttura menu dopo

Carosello principale con 7 gruppi (accesso a tutte le 20 funzioni):

| Voce | Icona | Contenuto |
|---|---|---|
| Rete | `globe` | WiFi · WiFi Lab · WiFi+ · WPA-SEC · ESP-NOW |
| Bluetooth | `ble` | BLE · BLE HID · Chat |
| Analisi RF | `scan` | Radar · Capture |
| Media | `music` | Radio · Musica · Audio |
| Strumenti | `tools` | Utility · Sensori · Infrarossi · BadUSB |
| Giochi | `games` | apre direttamente il menu Giochi |
| Sistema | `chip` | SD · Impostazioni |

- I hub sono menu lista standard (`MenuManager`), con footer e indicatore
  `n/totale`; il menu principale resta a carosello orbitale.
- `Impostazioni` resta sempre raggiungibile: se `ENABLE_MODULE_SETTINGS=0`
  il hub Sistema mostra il messaggio "Modulo non compilato" come prima.
- I hub vengono aggiunti solo se almeno un modulo del gruppo è compilato
  (eccezione: Sistema, sempre presente). I moduli con toggle runtime
  conservano il messaggio "Modulo disattivato" e il percorso
  `Impostazioni > Moduli attivi`.

## Breadcrumb

I menu radice dei moduli dichiarano storicamente `"Menu principale"` (o
`"ORBIT"` per Audio). `MenuManager::setRootContext()` sostituisce quel padre
con il hub attivo:

- `Rete > WiFi`
- `Media > Audio`
- `Sistema > Impostazioni`
- `Strumenti > Infrarossi`

I sottomenu interni ai moduli non cambiano: continuano a mostrare il loro padre
immediato (es. `WiFi > Reti WiFi`, `ESP-NOW > Inbox`). Il contesto è un solo
puntatore statico impostato con RAII dal hub e azzerato all'uscita.

## Icone procedurali

Aggiunte in `src/core/OrbitIcons.cpp`, griglia 16x16, stessi token palette,
nessun asset esterno:

| Modulo | Prima | Dopo |
|---|---|---|
| BLE HID | `ble` (condivisa) | `hid` — tastiera con antenna |
| WiFi Lab | `wifi` (condivisa) | `wifilab` — beuta da laboratorio |
| WiFi+ | `wifi` (condivisa) | `wifiplus` — onde WiFi + badge `+` |
| WPA-SEC | `lock` | `wpasec` — scudo con toppa |
| ESP-NOW | `chat` (condivisa) | `espnow` — mesh di tre nodi |
| Audio | `music` (condivisa) | `audio` — forma d'onda |
| Utility | `settings` (condivisa) | `tools` — cassetta attrezzi |
| Sensori | `radar` (condivisa) | `sensors` — livella a bolla |

Icone hub: `globe`, `ble`, `scan`, `music`, `tools`, `games`, `chip`.
Nessuna voce del menu principale condivide più l'icona con un'altra.

## Testi e hint

- Menu principale: `;.,/=scorri  ENTER=apri` (niente `ESC`, che sul menu
  radice non ha effetto).
- Hub: `ESC=indietro  ;.=nav  ENTER=apri`.
- Etichette invariate nelle schermate dei moduli; titoli hub in italiano.
- Sette palette e stile Orbit invariati; carosello, lista e griglia invariati.

## File toccati

- `src/main.cpp` — menu principale a 7 hub, rimosse le inclusioni dei moduli,
  LED RGB spento in `setup()`.
- `src/modules/hub/HubMenus.h` — interfaccia dei hub (nuovo).
- `src/modules/hub/HubMenus.cpp` — hub, guardie runtime, contesto breadcrumb
  (nuovo).
- `include/MenuManager.h` — `setRootContext()` (nuovo).
- `src/core/MenuManager.cpp` — sostituzione del padre radice; hint di default
  invariato nel comportamento.
- `src/core/OrbitIcons.cpp` — 8 icone procedurali.
- `docs/integration/UI_FINAL.md` — questo documento.

Non modificati: moduli delle feature, `Config.cpp`, `ModuleFlags.h`,
`platformio.ini`, `partitions.csv`, `flasher/`.

## Build e dimensioni

Comando: `python -m platformio run -e cardputer-adv-offensive` → **SUCCESS**.

| Voce | Valore |
|---|---:|
| `firmware.bin` | **2.652.192 B** |
| Flash usata (PlatformIO) | **2.651.829 / 2.818.048 B (94,1%)** |
| Margine sotto il limite richiesto | **165.856 B** |
| RAM statica | 146.384 / 327.680 B (44,7%) |

Il limite di 2.818.048 B è rispettato.

## Correzione tester Sensori (P2): torcia spenta all'avvio

In `setup()`, subito dopo `M5Cardputer.begin`, il LED RGB viene inizializzato e
portato a nero:

```cpp
M5.Led.setBrightness(63);
M5.Led.setAllColor(0, 0, 0);
M5.Led.display();
```

Un reset o un panic mentre la torcia è accesa non lascia più il LED RGB acceso
fino al prossimo comando: lo stato viene azzerato prima di filesystem, display
e menu. `M5.Led` si inizializza alla prima chiamata, quindi non serve altro.
Nessun altro comportamento è stato modificato.

## Rischi

1. **Contesto breadcrumb globale**: un futuro menu radice che dichiari
   `"Menu principale"`/`"ORBIT"` mentre un hub è aperto erediterebbe il nome
   del hub. Oggi solo i root dei moduli usano quei padri e il contesto è
   azzerato con RAII.
2. **Icone non viste su LCD**: le nuove icone 16x16 compilano e usano gli
   stessi primitivi delle esistenti, ma la resa a 16 px (e nel carosello a
   20–24 px) va verificata a occhio su tutte e 7 le palette.
3. **Un livello in più**: Impostazioni e SD ora sono in `Sistema` (due tap).
   Nessun comando è cambiato, ma l'accesso frequente è meno diretto.
4. **Budget flash**: 94,1% dello slot, 165,9 KB liberi. Altri moduli o asset
   possono superare il limite di 2.818.048 B.
5. **Nessun test su hardware** in questa sessione: build e verifica statica
   della struttura; restano da provare carosello a 7 voci, ritorno da ogni
   modulo, breadcrumb e tutte le palette.

## Test manuali suggeriti

1. Carosello principale: scorrere i 7 hub, verificare icone, etichette e
   `07/07`; aprire e chiudere ogni hub con ESC.
2. Breadcrumb: `Rete > WiFi`, `Media > Audio`, `Sistema > Impostazioni`,
   poi entrare in un sottomenu e verificare che il padre resti quello interno.
3. Moduli con toggle: disattivare WiFi da Impostazioni e aprire
   `Rete > WiFi`: deve comparire "Modulo disattivato"; riattivarlo.
4. Tutte le palette: verificare contrasto delle nuove icone e degli hub.
5. Rientro: aprire un modulo da ogni hub e tornare al menu principale 3 volte,
   controllando che il breadcrumb non resti bloccato sul hub precedente.
6. Torcia: accendere il LED da Sensori > Torcia, resettare la Cardputer e
   verificare che all'avvio il LED RGB sia spento.
