# 08 — Sensori: livella, goniometro, pedometro, torcia

Stato: implementato, build `cardputer-adv-offensive` riuscita.

## Cosa è stato implementato

Tutto in `src/modules/sensors/` (nessun altro file toccato):

- **Livella** — bolla 2D su piano cartesiano, angoli X/Y con segno, filtro EMA,
  calibrazione dello zero con ENTER e ripristino con R. Fondo scala bolla ±25°,
  oltre la bolla resta sul bordo.
- **Goniometro** — angolo assoluto rispetto al piano orizzontale (0° = piatto,
  90° = verticale), quadrante/needle, lock con ENTER, reset con R.
- **Pedometro** — rilevamento passi dall'accelerometro, conteggio giornaliero in
  NVS (`orbit_sensors`: chiavi `steps`, `day`, `up`), reset con conferma,
  grafico degli ultimi 60 s (20 barre da 3 s) e passi/min.
- **Torcia** — `M5.Led` (LED RGB, GPIO 21) con bianco/rosso/verde, SOS Morse,
  luminosità 0–100%, spegnimento garantito su uscita schermata, uscita modulo e
  sospensione/timeout.

UI italiana, header/footer Orbit, framebuffer a doppio buffer tramite
`display::*`: nessun flicker. Frequenza: 30 Hz (livella/goniometro/pedometro),
25 Hz (torcia, sufficiente per SOS).

## Assi e formule

IMU BMI270 gestito da M5Unified (`M5.Imu`), assi dispositivo: X verso destra,
Y verso l'alto/indietro, Z verso l'alto (convenzione M5). Il vettore di gravità
viene normalizzato con Z positiva (`az < 0` → tutti gli assi invertiti), così la
livella funziona anche a faccia in giù; se su un esemplare i segni X/Y risultano
invertiti basta cambiare `nx` o `ny` nella livella.

- Livella: `roll = atan2(ax, az)`, `pitch = atan2(ay, az)` in gradi.
  Bolla schermo: `x = -roll`, `y = pitch` (la bolla va verso il lato rialzato).
- Goniometro: `angolo = atan2(sqrt(ax² + ay²), |az|)` in gradi, 0..90.
- Pedometro: `|a| = sqrt(ax² + ay² + az²)`, gravità stimata con passa-basso
  `g += 0.12 * (|a| - g)` a 30 Hz (circa 250 ms di costante di tempo),
  segnale di passo `dev = |a| - g`.

## Soglie e filtri

- Passo: `dev > 0.13 g` con rifrattario di 280 ms; riarmo a `dev < 0.03 g`.
  Un impulso di 0.25–0.50 g a 0.55 s viene contato una volta sola.
- Digitazione: la tastiera è solidale all'IMU. Ogni evento tastiera (o tasto
  tenuto) sospende il conteggio per 350 ms; dopo il rilascio si riarma solo con
  `dev < 0.03 g`.
- Livella: EMA 0.20 a 30 Hz. Goniometro: EMA 0.25 a 30 Hz.
- Grafico: 20 bucket da 3 s = ultimo minuto; altezza barra 5 px per passo,
  massimo 25 px.

## Conteggio giornaliero e NVS

Namespace `orbit_sensors`:

| chiave | tipo | significato |
|---|---|---|
| `steps` | uint32 | passi del giorno corrente |
| `day` | uint32 | giorno epoch (data/86400) se l'orologio è valido, altrimenti 0 |
| `up` | uint32 | secondi di uptime del pedometro accumulati (fallback senza data) |

Regole:
- Se `time()` è valido (dopo il 2023, quindi NTP impostato da un altro modulo o
  dal sistema), il giorno è `epoch/86400` e al cambio giorno i passi si azzerano.
- Se l'orologio non è valido si usa il fallback a 24 h di uptime accumulato del
  pedometro; a 86400 s i passi si azzerano.
- In mancanza di entrambi resta il reset manuale con R.
- La NVS non viene scritta a ogni passo: salvataggio ogni 60 s e all'uscita.
  Il reset manuale salva subito. `prefs.end()` chiude la sessione.

## Comandi

Menu Sensori: `;`/`.` muovono la selezione, ENTER apre, ESC torna.

| Schermata | Tasti |
|---|---|
| Livella | ENTER = calibra zero, R = azzera calibrazione, ESC = esci |
| Goniometro | ENTER = lock/unlock, R = reset misura, ESC = esci |
| Pedometro | R = azzera passi (con conferma), ESC = esci |
| Torcia | ENTER = SOS on/off, `,`/`/` = colore, `;`/`.` = luminosità ±10, ESC = esci |

La torcia spegne il LED:
1. all'uscita con ESC;
2. all'uscita dal menu Sensori (difesa in profondità);
3. quando il timeout di sospensione sta per scattare, 1 s prima, perché
   `powertimer::powerOff()` blocca il loop dentro `navigation::poll()`, e
   comunque appena la UI viene attenuata dal powertimer;
4. dopo la sospensione il LED resta spento finché non si preme un tasto
   (lo schermo mostra `STANDBY  PREMI UN TASTO`).

## IMU e LED allo stato neutro

- All'uscita di ogni schermata: `M5.Imu.clearOffsetData()`; nessun offset,
  calibrazione o lock software viene lasciato attivo.
- LED: `setBrightness(0)` + `setAllColor(0,0,0)` + `display()` su ogni uscita.
  Non viene ripristinata una luminosità precedente perché nessun altro modulo
  usa `M5.Led`.

## Test

### 10 passi simulati (offline)

L'algoritmo è stato riprodotto a 30 Hz con 10 impulsi half-sine (0.18 s) di
ampiezza 0.25/0.30/0.35/0.40/0.50 g a 0.55 s, rumore uniforme ±0.02 g:
tutte le esecuzioni danno **10 passi**. A 0.28 s di passo (circa 3,5 passi/s)
il rifrattario può perderne 1–2: il test manuale va fatto a ~2 Hz.

### Verifica manuale consigliata

1. Menu principale > Sensori (il display passa in orizzontale, 240x135).
2. Livella: appoggiare su un piano; X/Y ≈ 0, bolla al centro. Inclinare a
   destra/sinistra/avanti/indietro: la bolla va verso il lato rialzato.
3. Livella: ENTER su un piano inclinato noto (es. 10°); la lettura X/Y va a 0.
   R riporta alla lettura assoluta.
4. Goniometro: piatto ≈ 0°, in verticale ≈ 90°. ENTER congela il valore
   (badge LOCK), R lo sblocca.
5. Pedometro: camminare 10 passi a ~2 Hz; il contatore deve salire di 10 ±1 e
   comparire una barra nel grafico. Digitare sulla tastiera da fermo non deve
   aggiungere passi (finestra di soppressione 350 ms).
6. Pedometro: premere R, confermare ENTER; passi e grafico tornano a zero.
7. Uscire dal pedometro (ESC) e rientrare: il conteggio deve essere conservato.
8. Torcia: `,`/`/` cambiano BIANCO/ROSSO/VERDE; `;`/`.` cambiano la luminosità
   (0% = spento); ENTER avvia/ferma SOS. Il LED fisico deve seguire il colore.
9. Torcia: ESC, e poi uscire dal menu Sensori; il LED deve essere spento.
10. Torcia: lasciare fermo fino al timeout (default 120 s); il LED deve
    spegnersi 1 s prima dell'attenuazione/sospensione e restare spento dopo la
    ripresa finché non si preme un tasto.

## Dimensione firmware

Comando: `python -m platformio run -e cardputer-adv-offensive` (SUCCESS).

- `.pio/build/cardputer-adv-offensive/firmware.bin`: **2.370.656 byte**
  (2,26 MiB).
- Flash applicativa usata secondo PlatformIO: **2.370.293 byte su 2.621.440**
  (90,4%). Restano circa 251 KB nello slot OTA.
- RAM statica: **115.520 byte su 327.680** (35,3%). Il framebuffer 240x135
  (16 bpp, ~64,8 KB) viene allocato a runtime da `display::begin()`.
- Heap libero a runtime non misurato (nessun hardware collegato); il modulo non
  alloca buffer dinamici propri oltre a piccole `std::string` di formattazione.

## Limiti e rischi

- Nessun RTC/NTP in questo modulo: il "giorno" è affidabile solo se un altro
  modulo sincronizza l'orologio. Altrimenti si usa il fallback a 24 h di
  uptime o il reset manuale. Documentato sopra.
- Il pedometro è euristico: non è un dispositivo medico. Scossoni forti o
  camminate molto lente possono falsare il conteggio; la soppressione della
  tastiera riduce ma non elimina i falsi passi da digitazione.
- Il conteggio avanza solo mentre la schermata Pedometro è aperta: i menu
  bloccano e non esiste un servizio in background. Il totale resta in NVS, il
  grafico degli ultimi 60 s è solo di sessione.
- Durante la conferma modale del reset il campionamento IMU è sospeso per alcuni
  secondi: eventuali passi in quella finestra non vengono contati.
- Il verso della bolla dipende dagli assi del BMI270 montati sul Cardputer ADV;
  se su un esemplare appare invertito, invertire `nx` o `ny` in
  `drawLevelFrame()`.
- La UI forza il formato orizzontale e ripristina la rotazione utente all'uscita.
- Scritture NVS ogni 60 s: compromesso tra perdita di passi e usura flash.
- `M5.Led` è un singolo RGB (GPIO 21); non c'è un LED bianco dedicato, quindi
  "bianco" è RGB al massimo. La luminosità è mappata 0–100 → 0–255.
- La torcia non usa `powertimer::Hold`: il timeout resta attivo e il LED viene
  spento prima della sospensione. Dopo la ripresa serve un tasto per riaccendere.
- Non è stato possibile provare su hardware reale in questo worktree: build e
  simulazione offline sono le verifiche disponibili.
