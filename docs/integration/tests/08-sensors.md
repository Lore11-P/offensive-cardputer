# Test 08 — Sensori (livella, goniometro, pedometro, torcia)

- **Data:** 2026-11-21
- **Oggetto:** codice FINALE integrato, `C:\Users\utente\Desktop\opencode\offensive-cardputer`
- **File in esame:** `src/modules/sensors/SensorsMenu.cpp` (789 righe), `SensorsMenu.h`,
  `src/modules/sensors/REPORT.md`, più i file condivisi che il modulo usa
  (`src/core/Display.cpp`, `src/core/PowerTimer.cpp`, `src/core/Navigation.cpp`,
  `src/core/Ui.cpp`, `src/main.cpp`) e le librerie `M5Unified`/`M5Cardputer` in
  `.pio/libdeps/cardputer-adv-offensive`.
- **Spec:** `docs/integration/specs/08-sensors.md`; **BRIEF:** `docs/integration/BRIEF.md`;
  **piano di verifica:** `orbit-wt-qa/docs/integration/reviews/08-sensors.md`
  (nota: quel file è un piano scritto quando il modulo era ancora uno stub, quindi
  è stato usato solo come elenco di test, non come esito).
- **Metodo:** revisione statica del codice finale + riproduzione offline dell'algoritmo
  del pedometro. **Nessuna build, nessuna modifica, nessun hardware.** Dove serve
  l'evidenza, sono citate le righe delle librerie installate.
- **Verdetto in una riga:** nessun difetto **P0** trovato; 2 **P1** (LED condiviso con
  ESP-NOW, conteggio giornaliero senza RTC), 8 **P2**, 4 **P3**. I percorsi di
  spegnimento del LED della torcia sono coperti.

---

## 1. Sintesi dei finding

| ID | P | Titolo | File:riga | Stato |
|---|---|---|---|---|
| S-01 | **P1** | `ledOffHard()` azzera la luminosità **globale** di `M5.Led`: dopo una visita a Sensori il LED remoto di ESP-NOW non si accende più | `sensors/SensorsMenu.cpp:586-591, 615, 765, 783` + `espnow/EspNowMenu.cpp:1312-1316` | aperto |
| S-02 | **P1** | Pedometro "giornaliero": senza RTC/NTP mostra passi vecchi come "OGGI" e azzera i passi del giorno al primo sync; fallback 24 h irraggiungibile | `sensors/SensorsMenu.cpp:347-354, 365-381, 486-510, 531` | aperto |
| S-03 | P2 | `M5.Imu.clearOffsetData()` cancella la calibrazione offset dell'IMU (non un offset del modulo) e viene chiamato anche dopo la torcia, che l'IMU non lo usa | `sensors/SensorsMenu.cpp:84-86, 234, 342, 566, 766` | aperto |
| S-04 | P2 | LED RGB resta acceso dopo un reset con la torcia accesa: nessun frame nero al boot | `sensors/SensorsMenu.cpp:610-767` + `src/main.cpp:175-200` | aperto |
| S-05 | P2 | `powertimer::Hold` in pedometro (e livella/goniometro): nessun timeout, nessuna sospensione, ridisegno a 30 Hz → batteria | `sensors/SensorsMenu.cpp:177, 244, 361` | aperto |
| S-06 | P2 | Goniometro: linea di riferimento dal lato sbagliato ed etichetta "0" agli antipodi della scala | `sensors/SensorsMenu.cpp:315, 323` | aperto |
| S-07 | P2 | Timer di flash invertiti: il messaggio appare **dopo** la deadline (250/1200/1500 ms in ritardo) | `sensors/SensorsMenu.cpp:231, 533, 535` | aperto |
| S-08 | P2 | Rotazione: con rotazione utente 3 il modulo resta a 3 (UI capovolta e bolla invertita); con 0/2 prova due volte il candidato duplicato | `sensors/SensorsMenu.cpp:92-109` | aperto |
| S-09 | P2 | Dopo la sospensione servono **due** pressioni per riaccendere la torcia (il tasto di risveglio è consumato da `powerOff()`) | `sensors/SensorsMenu.cpp:680-694` + `core/PowerTimer.cpp:236-254` | aperto |
| S-10 | P2 | `prefs.putUInt()` non controllato: NVS piena → passi persi in silenzio, nessun `!NVS ERR` | `sensors/SensorsMenu.cpp:395-404` | aperto |
| S-11 | P3 | SOS: periodo `% 33` mentre la somma delle durate è 34 (ultimo gap accorciato) | `sensors/SensorsMenu.cpp:599` | aperto |
| S-12 | P3 | `ledOffHard()` all'uscita inizializza il driver RMT (heap) anche per chi non ha mai aperto la torcia, e spegne un LED acceso da un altro modulo | `sensors/SensorsMenu.cpp:783` | aperto |
| S-13 | P3 | Passi a 8+ cifre (font 4) invadono la colonna "PASSI/MIN" | `sensors/SensorsMenu.cpp:528-543` | aperto |
| S-14 | P3 | `enterLandscape()` non salta il candidato duplicato (2 `applyRotation` in più con rotazione portrait) | `sensors/SensorsMenu.cpp:95-101` | aperto |

---

## 2. Dettaglio

### S-01 (P1) — La torcia azzera la luminosità condivisa di `M5.Led` e rompe il LED di ESP-NOW

**Dove**
- `src/modules/sensors/SensorsMenu.cpp:586-591` (`ledOffHard`), chiamata a
  `:615` (ingresso torcia), `:765` (uscita torcia) e `:783` (**uscita dal menu
  Sensori, sempre**).
- `src/modules/espnow/EspNowMenu.cpp:1312-1316`: `applyLed()` chiama solo
  `M5.Led.setAllColor(r,g,b)` + `display()`, **mai** `setBrightness()`.
- `src/modules/sensors/REPORT.md:95-96`: «Non viene ripristinata una luminosità
  precedente perché nessun altro modulo usa `M5.Led`». **Falso.**

**Meccanica**: `M5.Led` è un singleton (`LED_Strip_Class`, default `_brightness = 63`,
vedi `.pio/.../M5Unified/src/utility/led/LED_Strip_Class.hpp:79`). `ledOffHard()` fa
`setBrightness(0)`; nel driver la scala è `br = (brightness+1)^2` e in uscita
`(canale * br) >> 16` (`.inl:246-249, 265-276`), quindi con brightness 0 ogni colore
diventa nero. `setAllColor()` da solo **non** ripristina la luminosità.

**Impatto**: basta aprire e chiudere il menu Sensori (senza nemmeno usare la torcia)
per lasciare `M5.Led` a luminosità 0 per tutto il resto del boot. Da quel momento:
- il comando remoto ESP-NOW `led r g b` (`EspNowMenu.cpp:1334-1336`) non produce
  alcuna luce;
- l'eventuale LED acceso via ESP-NOW viene spento all'uscita da Sensori.

Non si recupera finché non si riavvia il dispositivo (nessun altro punto del
firmware chiama `setBrightness`). Difetto invisibile ai test del solo modulo 08,
visibile solo sul firmware integrato.

**Fix minimo**
1. `ledOffHard()` non deve azzerare la luminosità condivisa: usare il solo
   `setAllColor(0,0,0)` (il nero basta a spegnere) e, se si vuole tornare al
   default M5, `setBrightness(63)`.
2. In `EspNowMenu::applyLed()` impostare la propria luminosità
   (`M5.Led.setBrightness(63)`) così il consumatore non dipende dallo stato lasciato
   da altri moduli.
3. Correggere `REPORT.md:95-96`.

**Test**: avviare, Sensori → ESC, poi ESP-NOW con un peer che invia `led 255 40 0`:
prima del fix nessun LED; dopo il fix LED rosso. Ripetere con `led off`.

---

### S-02 (P1) — Pedometro "giornaliero": passi vecchi mostrati come oggi e reset al primo sync NTP

**Dove**: `SensorsMenu.cpp:347-354` (`epochDayNow`), `:365-381` (caricamento e reset
d'ingresso), `:486-510` (rollover nel loop), `:531` (etichetta "OGGI" sempre).

**Contesto hardware/firmware**: la Cardputer ADV non ha RTC tamponato. `time()` è
valido solo se nella sessione corrente qualcuno ha fatto NTP (Radio `configTime`,
WPA-SEC) o ha impostato l'ora da Utility (`settimeofday`, `ToolsMenu.cpp:348`).
**Nessuno salva/ripristina l'ora in NVS**: dopo ogni reboot `time()` torna a 0.

**Meccanica e scenari**
1. Sessione A con NTP: `day=D`, `steps=5000` in NVS.
2. Reboot senza NTP: `epochDayNow()` restituisce 0; il ramo `today != 0` non gira,
   il ramo `else if (baseUp >= 86400)` non ha effetto (`up=0` perché con data valida
   viene salvato 0, riga 402). `day` resta `D` e i **5000 passi di ieri vengono
   mostrati come "OGGI"**.
3. L'utente cammina altri 2000 passi (totale 7000). Poi apre Radio e arriva l'NTP:
   il check ogni 30 s (righe 486-497) vede `currentDay != day` e **azzera** i 7000
   passi, compresi i 2000 di oggi. Se invece l'NTP restituisce lo stesso giorno `D`,
   i 5000 di ieri restano mescolati ai 2000 di oggi.
4. Il fallback "24 h di uptime" (`up`) si accumula **solo mentre la schermata
   Pedometro è aperta** (riga 399) e viene scritto 0 appena `day != 0` (riga 402):
   con qualunque sessione NTP precedente è definitivamente disabilitato; senza,
   servirebbe tenere la schermata accesa 24 h (con `Hold`, batteria insufficiente).
   In pratica è codice morto.

**Impatto**: la feature dichiarata "passi giornalieri" è inaffidabile nel caso
d'uso normale (nessun RTC); possibile **perdita dei passi del giorno** e valori
palesemente sbagliati etichettati "OGGI". È il finding che più si avvicina a un P0
se il criterio D-07 della review QA è considerato bloccante.

**Fix minimo**
- Persistire un flag `dayValid`/`lastSavedDay` e un contatore `stepsSinceBoot`.
- Al primo giorno valido dopo un boot **non azzerare** i passi accumulati da boot:
  adottare `day=currentDay` e salvare (i passi pre-boot sono già stati azzerati al
  passo 2 da una regola esplicita).
- Se `day == 0`, mostrare "DATA N.D." (o "SESSIONE") al posto di "OGGI".
- Alternativa più semplice e onesta: dichiarare nel REPORT che senza ora valida il
  conteggio non è giornaliero e mostrare l'etichetta corretta.

**Test**: D-06/D-07 della review QA; in particolare reboot senza NTP e primo sync
NTP durante la sessione.

---

### S-03 (P2) — `clearOffsetData()` cancella la calibrazione dell'IMU e non serve

**Dove**: `SensorsMenu.cpp:84-86` (`imuNeutral` → `M5.Imu.clearOffsetData()`),
chiamata a `:234` (livella), `:342` (goniometro), `:566` (pedometro) e `:766`
(uscita torcia, che l'IMU **non lo usa**).

**Evidenza**: in M5Unified `IMU_Class::begin()` esegue una calibrazione e scrive
`_offset_data` (`IMU_Class.inl:183-203`); `clearOffsetData()` azzera **quei** valori
(`inl:396-406`). Dopo `begin()` la calibrazione automatica è spenta
(`setCalibration(0,0,0)`), quindi gli offset cancellati **non vengono più
ricalcolati** fino al prossimo boot. Il modulo non imposta mai offset propri: la
frase nel REPORT («nessun offset ... viene lasciato attivo») descrive un problema
che non esiste.

**Impatto**: dal primo ritorno da una qualunque schermata Sensori (o dalla torcia)
le letture usano i dati grezzi con il bias del sensore: l'errore assoluto della
livella/goniometro può crescere di frazioni di grado fino a 1–2° a seconda
dell'esemplare e dell'orientamento. La calibrazione "ZERO" dell'utente maschera
solo l'orientamento in cui è stata fatta. È un peggioramento silenzioso e
permanente (fino al reboot) della feature principale, causato dal modulo stesso.

**Fix**: eliminare `imuNeutral()` e le sue chiamate (non c'è nulla da neutralizzare:
gli offset di zero e il lock sono variabili locali). Se in futuro servisse
ripristinare lo stato, l'API corretta è ri-eseguire `M5.Imu.begin()`, non cancellare
gli offset.

**Test**: A-02/A-03 con "R=reset" e letture assolute, prima e dopo una visita al
pedometro.

---

### S-04 (P2) — LED acceso dopo un reset con la torcia accesa

**Dove**: accensione in `SensorsMenu.cpp:634-657, 717-718`; spegnimento solo su
percorsi del modulo (`:765`, `:783`). Nel boot (`src/main.cpp:175-200`,
`display::begin()`) non c'è alcun frame nero per `M5.Led`.

**Meccanica**: il WS2812 (GPIO 21) mantiene il colore finché è alimentato; un reset
software (panic, watchdog, OTA, `esp_restart`) non toglie alimentazione. M5Unified
inizializza il driver LED in modo lazy (primo `setAllColor`/`setBrightness`), quindi
al boot nessun frame viene inviato.

**Impatto**: se il dispositivo si resetta con la torcia accesa (o con il LED acceso
da ESP-NOW), il LED resta acceso a tempo indefinito: consumo, calore e impossibilità
di spegnerlo dall'interfaccia (solo riaprendo Sensori e uscendo, o con un power
cycle). Viola il criterio E-05 della review.

**Fix**: inviare un frame nero all'avvio (in `display::begin()` o `setup()`, file
condivisi → serve il supervisore) oppure almeno all'ingresso di `runMenu()`; in
alternativa documentare il limite.

---

### S-05 (P2) — `Hold` in pedometro/livella/goniometro: niente sleep, batteria

**Dove**: `SensorsMenu.cpp:177, 244, 361`.

`powertimer::tick()` esce subito con `s_holdCount > 0` (`PowerTimer.cpp:131-134`):
con la schermata aperta non c'è attenuazione né sospensione. Per livella/goniometro
è la convenzione della casa (schermate di monitoraggio), ma il pedometro è pensato
per restare aperto a lungo e ridisegna a 30 Hz con push a schermo intero: la batteria
si esaurisce e il dispositivo non si spegne mai. Il REPORT lo sa (`REPORT.md:149-151`)
ma la combinazione con `Hold` non è documentata.

**Fix**: togliere `Hold` dal pedometro e chiamare `powertimer::activity()` a ogni
passo rilevato (così resta sveglio solo mentre si cammina e torna a dormire da
fermo); in alternativa un limite massimo esplicito.

**Nota**: il piano QA F-07 (dim/sospensione dentro livella/goniometro) **fallisce
by design** per via di `Hold`, non per un bug: va aggiornato il criterio.

---

### S-06 (P2) — Goniometro: riferimento e etichetta "0" dal lato sbagliato

**Dove**: `SensorsMenu.cpp:302-321` (arco e lancetta), `:315` (linea di riferimento),
`:323` ("0").

L'arco va da 360° a 270° (0° a **destra**, 90° in alto: `rad = (360-a)`), la lancetta
a 0° punta a `(cx+r, cy)`. Ma la linea di riferimento è disegnata con
`drawFastHLine(cx - r, cy, r)` → da `x=22` a `x=64`, cioè **a sinistra** del centro;
l'etichetta "0" è a `(7,72)`, agli antipodi della scala. Il quadrante risulta
incoerente: 0° sembra a sinistra ma la lancetta non ci arriva mai.

**Fix**: `drawFastHLine(cx, cy, r, ...)`; "0" vicino a `(cx+r-6, cy-10)`; "90" vicino
al punto `(cx, cy-r)`.

---

### S-07 (P2) — Timer di flash invertiti (feedback in ritardo)

**Dove**: `SensorsMenu.cpp:231` (`now - flashUntil < 1200`), `:533`
(`now - flashUntil < 250`), `:535` (`now - messageUntil < 1500`).

Le deadline sono `flashUntil = now + X`, ma il confronto è `now - deadline < X`:
finché `now < deadline` la sottrazione è negativa e, essendo `uint32_t`, diventa un
numero enorme → condizione falsa. Il messaggio compare **X ms dopo** la deadline e
resta per X ms (es.: "AZZERATO" appare 1,5 s dopo il reset; "PASSO" appare tra un
passo e l'altro). Con passi a 2 Hz il flash si accende proprio quando il passo
successivo lo spegne.

**Fix**: `now < flashUntil` / `now < messageUntil` (oppure `flashUntil > now`).

---

### S-08 (P2) — Rotazione: rotazione 3 → UI capovolta; candidato duplicato

**Dove**: `SensorsMenu.cpp:92-109`.

`enterLandscape()` salva `config::rotation()` e prova `{saved, 0, 1, 2, 3}`,
fermandosi al primo indice con `width() > height()`. Con rotazione utente **3**
(che è già landscape, `SettingsMenu.cpp:101-102`) il loop si ferma subito su 3: il
canvas è sempre 240x135, ma il layout è scritto per la rotazione 1, quindi per
l'utente l'immagine è **capovolta di 180°** e la bolla della livella si muove nel
verso opposto a quello fisico (A-04 fallisce). Con 0/2 il primo landscape è 1 (ok),
ma il candidato salvato viene provato due volte (2 `applyRotation` + ridisegni
inutili) perché manca il `continue` presente in `GamesMenu.cpp:72-74`.

**Fix**: forzare la rotazione 1 (o mappare `saved == 3 → 1`), saltare il duplicato;
`exitLandscape()` continua a ripristinare la rotazione utente.

**Nota**: se `applyRotation` non riesce ad allocare il framebuffer 240x135 (heap
frammentato dopo WiFi/TLS), il loop termina in portrait e la UI hardcoded 240 px
viene tagliata senza avviso. Probabilità bassa (stessa dimensione di allocazione),
ma il modulo non lo rileva.

---

### S-09 (P2) — Dopo la sospensione servono due tasti per riaccendere la torcia

**Dove**: `SensorsMenu.cpp:669-672` (uscita da standby solo su evento),
`:680-694` (standby); `PowerTimer.cpp:236-254` (il risveglio consuma il tasto e
chiama `navigation::begin()`).

`powerOff()` attende il rilascio del tasto di risveglio e azzera lo stato della
tastiera; `navigation::poll()` riparte senza eventi. Nel frattempo `standby` è
ancora `true`, quindi il LED resta spento e la UI mostra "STANDBY": serve una
**seconda** pressione. Il REPORT (`:87-88`) dice "un tasto", quindi c'è anche una
discrepanza documentale. Fix: al ritorno da `poll()`/`powerOff` riallineare
`lastActivity` e togliere `standby` (il tasto di risveglio è già stato usato per
riaccendere lo schermo).

---

### S-10 (P2) — Errori NVS non controllati

**Dove**: `SensorsMenu.cpp:395-404` (`prefs.putUInt` x3).

La NVS è una partizione da 20 KB condivisa (`partitions.csv:13`); se è piena o in
errore, `putUInt` restituisce 0 e i passi si perdono in silenzio. L'utente vede
`!NVS ERR` solo se fallisce `begin()`, non se fallisce la scrittura. Fix: controllare
i tre return e mostrare l'errore (o tenere un flag `nvsWriteOk`).

---

### S-11 (P3) — Periodo SOS 33 vs 34

**Dove**: `SensorsMenu.cpp:594-599`. Le durate sommano 34 unità ma `phase` è
`% 33`: l'ultimo gap (7 unità) viene troncato a 6 e il pattern si ripete 120 ms
prima. Il pattern resta riconoscibile. Fix: `% 34`.

### S-12 (P3) — Inizializzazione LED all'uscita del menu

**Dove**: `SensorsMenu.cpp:783`. `ledOffHard()` chiama `M5.Led.*` anche se la torcia
non è mai stata aperta: il primo uso inizializza il driver RMT (allocazioni heap +
canale RMT) e invia un frame. Inoltre spegne un LED eventualmente acceso da ESP-NOW.
Fix: chiamare solo se `M5.Led.isEnabled()`, oppure non toccare la luminosità (S-01).

### S-13 (P3) — Colonne che si sovrappongono con molti passi

**Dove**: `SensorsMenu.cpp:528-543`. Con 8+ cifre il valore a font 4 (12 px/cifra)
parte da `x=10` e supera `x=120` (colonna "PASSI/MIN" e suo valore). Realistico solo
oltre 10 milioni di passi/giorno; fix: font 2 oltre 999999 o clamp.

### S-14 (P3) — Candidato di rotazione duplicato

**Dove**: `SensorsMenu.cpp:95-101`. Vedi S-08: con rotazione 0/2 il candidato
salvato viene applicato due volte (due `clear()+push()` in più). Basso impatto.

---

## 3. Top 5 (i più gravi)

1. **S-01 (P1)** — `ledOffHard()` azzera la luminosità globale di `M5.Led`: dopo
   una visita a Sensori il LED comandato da ESP-NOW resta nero fino al reboot
   (`SensorsMenu.cpp:586-591/783`, `EspNowMenu.cpp:1312-1316`). Unica regressione
   cross-modulo trovata; il REPORT afferma il falso ("nessun altro modulo usa
   `M5.Led`").
2. **S-02 (P1)** — Pedometro giornaliero senza RTC: passi di ieri mostrati come
   "OGGI", e azzeramento dei passi del giorno al primo sync NTP; fallback 24 h
   irraggiungibile (`SensorsMenu.cpp:365-381, 486-510, 531`).
3. **S-03 (P2)** — `M5.Imu.clearOffsetData()` cancella la calibrazione IMU e non
   viene ricalcolata: accuratezza assoluta di livella/goniometro degradata dal primo
   ritorno, anche solo dopo la torcia (`:84-86, 234, 342, 566, 766`).
4. **S-04 (P2)** — LED RGB che resta acceso dopo un reset con la torcia accesa:
   nessun frame nero al boot, il WS2812 mantiene il colore (`:610-767` +
   `main.cpp:175-200`).
5. **S-05 (P2)** — `powertimer::Hold` su pedometro/livella/goniometro: nessuna
   sospensione, 30 Hz a schermo intero; batteria e nessun auto-spegnimento
   (`:177, 244, 361`).

A seguire: S-06 (goniometro), S-07 (flash invertiti), S-08 (rotazione 3),
S-09 (doppio tasto dopo la sospensione).

---

## 4. Verifiche eseguite e superate

- **LED spento nei percorsi nominali**: ESC torcia (`:765`), uscita modulo (`:783`),
  1 s prima del timeout (`:680-683`), su attenuazione/sospensione (`:684-694`).
  Nessun buco trovato nei percorsi della torcia; i problemi sono S-01/S-04/S-12.
- **Pedometro 10 passi (accettazione)**: algoritmo riprodotto offline a 30 Hz
  (impulsi half-sine 0,18 s, ampiezze 0,25–0,50 g, rumore ±0,02 g): 10/10 passi a
  1,6–2,2 Hz; 9/10 a 3,1 Hz; 7–9/10 a 3,6 Hz — coerente con il REPORT.
- **Soglie/refrattario**: `dev>0,13 g`, riarmo `dev<0,03 g`, refrattario 280 ms:
  nessun doppio conteggio nella simulazione.
- **Grafico minuto**: 20 bucket × 3 s, rotazione con `memmove`, nessun accesso fuori
  buffer; `perMinute` massimo 5100 (nessun overflow).
- **NVS**: nessuna scrittura per passo (60 s + uscita + reset, `:395-404, 513-515`);
  namespace e chiavi entro i limiti (`orbit_sensors`, `steps`, `day`, `up`).
- **Stack**: nessun VLA/array grande (max 20 B `buckets`, 16 B `snprintf`);
  `std::string` in SSO, nessuna allocazione per frame; `ARDUINO_LOOP_STACK_SIZE=16384`
  (`platformio.ini:40`) e catena di chiamata profonda ~2–3 KB. Nessun rischio.
- **Heap**: nessun accumulo per frame; `MenuManager`/`std::function` liberati
  all'uscita; unica riallocazione grossa `display::applyRotation` quando la rotazione
  utente è portrait (una tantum, stessa dimensione).
- **SOS**: sequenza 3 brevi / 3 lunghi / 3 brevi corretta (a parte S-11).
- **Assi/formule**: `roll=atan2(ax,az)`, `pitch=atan2(ay,az)`, `angolo=atan2(√(ax²+ay²),|az|)`
  e mapping bolla `(-roll, +pitch)` sono coerenti con la convenzione dichiarata
  (X destra, Y alto, Z uscente). Il verso fisico sul BMI270 non è verificabile senza
  hardware (A-01/A-03 restano da fare).
- **Rotazione ripristinata** all'uscita (`:104-109`) e su tutti i percorsi (anche
  IMU non disponibile, perché `exitLandscape()` è dopo `menu.run`).
- **Nessun `delay()` lungo**; frequenze 30 Hz / 25 Hz entro la spec 20–50 Hz.
- **Batteria**: il modulo non legge la batteria; l'header condiviso usa
  `batterySignature()` con cache 2 s (`Display.cpp:776, 784-792`). Nessun consumo
  anomalo oltre a S-05 e alla torcia (60 mA max del WS2812, non misurati).

---

## 5. Non verificabile in questo ambiente

- Verso fisico della bolla e dell'angolo su hardware reale (A-01/A-03/A-07).
- Latenza reale di ESC/ENTER a 30 Hz e flicker percepito (A-05/F-04).
- Corrente/runtime della torcia a 100% e durata del pedometro (E-06/F-02).
- Fallimento di `M5.Led.begin()` / `M5.Imu.begin()` su esemplari difettosi (A-08).
- Build: non eseguita per consegna. Il binario finale presente
  (`.pio/build/cardputer-adv-offensive/firmware.bin`, 2.618.256 B del 21/11 15:43)
  è dentro lo slot `app0` da 0x2B0000 e include `SensorsMenu.cpp.o`; il REPORT del
  modulo cita 2.370.656 B perché si riferisce al worktree del modulo, non al merge.

## 6. Note di integrazione

- `main.cpp:161` registra Sensori **senza** `guardedModule` e `config::Module` non
  ha una voce `Sensors`: il modulo non è disattivabile da *Impostazioni > Moduli
  attivi*. È una scelta del file già predisposto (non modificabile), nota nella
  review QA; non è un difetto del modulo.
- La review `orbit-wt-qa/docs/integration/reviews/08-sensors.md` descrive ancora lo
  stub: i suoi test A–F restano validi come piano, ma il verdetto "BLOCCATO" è
  superato dal codice finale.
