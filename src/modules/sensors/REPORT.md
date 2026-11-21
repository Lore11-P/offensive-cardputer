# 08 — Sensori: livella, goniometro, pedometro, torcia

Stato: implementato, fix post-review applicati, build `cardputer-adv-offensive`
riuscita.

## Cosa è stato implementato

Tutto in `src/modules/sensors/` (nessun altro file toccato):

- **Livella** — bolla 2D su piano cartesiano, angoli X/Y con segno, filtro EMA,
  calibrazione dello zero con ENTER e ripristino con R. Fondo scala bolla ±25°,
  oltre la bolla resta sul bordo.
- **Goniometro** — angolo assoluto rispetto al piano orizzontale (0° = piatto,
  90° = verticale), lancetta e arco 0–90°, lock con ENTER, reset con R.
- **Pedometro** — rilevamento passi dall'accelerometro, conteggio giornaliero in
  NVS (`orbit_sensors`), reset con conferma, grafico degli ultimi 60 s
  (20 barre da 3 s) e passi/min.
- **Torcia** — `M5.Led` (LED RGB, GPIO 21) con bianco/rosso/verde, SOS Morse,
  luminosità 0–100%, spegnimento su uscita schermata, uscita modulo e
  sospensione/timeout.

UI italiana, header/footer Orbit, framebuffer a doppio buffer tramite
`display::*`: nessun flicker. Frequenza: 30 Hz (livella/goniometro/pedometro),
25 Hz (torcia, sufficiente per SOS). Nessun `powertimer::Hold`: il timeout di
sospensione resta attivo in tutte le schermate; il pedometro chiama
`powertimer::activity()` a ogni passo, così resta sveglio solo mentre si cammina.

## Assi e formule

IMU BMI270 gestito da M5Unified (`M5.Imu`), assi dispositivo: X verso destra,
Y verso l'alto/indietro, Z verso l'alto (convenzione M5). Il vettore di gravità
viene normalizzato con Z positiva (`az < 0` → tutti gli assi invertiti), così la
livella funziona anche a faccia in giù; se su un esemplare i segni X/Y risultano
invertiti basta cambiare `nx` o `ny` nella livella.

- Livella: `roll = atan2(ax, az)`, `pitch = atan2(ay, az)` in gradi.
  Bolla schermo: `x = -roll`, `y = pitch` (la bolla va verso il lato rialzato).
- Goniometro: `angolo = atan2(sqrt(ax² + ay²), |az|)` in gradi, 0..90.
  La scala ha lo 0 a destra (fine della linea di riferimento) e 90° in alto;
  la lancetta segue la stessa convenzione.
- Pedometro: `|a| = sqrt(ax² + ay² + az²)`, gravità stimata con passa-basso
  `g += 0.12 * (|a| - g)` a 30 Hz (circa 250 ms di costante di tempo),
  segnale di passo `dev = |a| - g`.

## Soglie e filtri

- Passo: `dev > 0.13 g` con rifrattario di 280 ms; riarmo a `dev < 0.03 g`.
  Un impulso di 0.25–0.50 g a 0.55 s viene contato una volta sola.
- Digitazione: la tastiera è solidale all'IMU. Ogni evento tastiera (o tasto
  tenuto) sospende il conteggio per 350 ms; dopo il rilascio si riarma solo con
  `dev < 0.03 g`.
- Dopo una pausa di campionamento > 1 s (sospensione o finestra modale) il
  filtro della gravità si riallinea al primo campione e non conta quel frame:
  evita un falso passo al risveglio.
- Livella: EMA 0.20 a 30 Hz. Goniometro: EMA 0.25 a 30 Hz.
- Grafico: 20 bucket da 3 s = ultimo minuto; altezza barra 5 px per passo,
  massimo 25 px.

## Conteggio giornaliero, NVS e fallback senza RTC

Namespace `orbit_sensors`:

| chiave | tipo | significato |
|---|---|---|
| `steps` | uint32 | passi del giorno noto (ultimo giorno valido) |
| `pending` | uint32 | passi accumulati mentre il giorno non è noto |
| `day` | uint32 | giorno epoch (data/86400) dell'ultimo giorno valido |
| `dayValid` | uint8 | 1 se `day` è una data reale, 0 se sconosciuta |
| `up` | uint32 | secondi di uptime accumulati per il fallback 24 h |

Regole:
- Se `time()` è valido (dopo il 2023, quindi NTP impostato da Radio/WPA-SEC/
  Utility o dal sistema), il giorno è `epoch/86400`.
- Al boot con data valida: se `dayValid` è 1 e il giorno salvato è diverso, i
  passi di `steps` sono di un altro giorno e vengono sostituiti da `pending`;
  se il giorno è lo stesso, `pending` viene sommato a `steps`.
- Se la data non è valida, la UI mostra **`DATA N.D.`** e il conteggio visibile è
  `pending`: i passi non vengono spacciati per "oggi" e non si perdono a ogni
  boot. `steps` resta in NVS come ultimo totale del giorno noto.
- Al **primo sync NTP** (la data diventa valida durante la sessione) `pending`
  non viene azzerato: viene attribuito a oggi (`steps += pending` se l'ultimo
  giorno noto è già oggi, altrimenti `steps = pending`).
- A mezzanotte, con data valida, `steps` e `pending` si azzerano e
  `day` passa al nuovo giorno.
- Fallback 24 h: senza data valida, `up` conta l'uptime accumulato (anche tra
  riaperture della schermata; il tempo a device spento non è contato). A 86400 s
  `steps` e `pending` si azzerano e il conteggio riparte. Il primo ingresso di
  ogni boot aggiunge il tempo già trascorso dall'accensione.
- Il reset manuale (R + conferma) azzera `steps` e `pending` e salva subito.
- La NVS non viene scritta a ogni passo: salvataggio ogni 60 s e all'uscita.
  I return di `putUInt`/`putUChar` sono controllati; se una scrittura fallisce
  la UI mostra `!NVS ERR` (anche con NVS piena, non solo con `begin()` fallito).
  `prefs.end()` chiude la sessione.

## LED condiviso con ESP-NOW

`M5.Led` è un singleton (`LED_Strip_Class`, luminosità default **63**) usato
anche da ESP-NOW, che chiama solo `setAllColor()` + `display()` e non imposta la
luminosità. Il modulo quindi:

- non azzera mai la luminosità globale;
- alla chiusura della torcia lascia il colore **nero** e riporta la luminosità a
  **63** (`ledTorchOff()`), così i comandi LED successivi di ESP-NOW tornano a
  funzionare;
- se la torcia non viene mai aperta, non tocca `M5.Led` (nessuna inizializzazione
  RMT e nessun frame inviato).

Il frame nero al boot per il caso "reset con torcia accesa" è nella patch
condivisa di `src/main.cpp` (applicata dal supervisore al merge); questo modulo
non la duplica.

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
2. all'uscita dal menu Sensori (solo se la torcia è stata aperta);
3. quando il timeout di sospensione sta per scattare, 1 s prima, perché
   `powertimer::powerOff()` blocca il loop dentro `navigation::poll()`, e
   comunque appena la UI viene attenuata dal powertimer;
4. dopo la sospensione il LED resta spento; il tasto che ha svegliato il device
   vale anche da riaccensione (non serve una seconda pressione), con timeout
   riavviato.

## IMU e stato all'uscita

- Nessuna chiamata a `M5.Imu.clearOffsetData()`: cancellerebbe la calibrazione
  interna di M5Unified, che non viene ricalcolata fino al boot successivo.
- Lo zero della livella e il lock del goniometro sono variabili locali: non
  restano offset, lock o configurazioni IMU dopo l'uscita.
- La rotazione utente viene salvata e ripristinata; dentro il modulo il layout è
  sempre in rotazione 1 (landscape standard, 240x135), anche se l'utente aveva
  scelto 3. Questo evita la UI capovolta e la bolla invertita.

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
4. Goniometro: piatto ≈ 0° (lancetta a destra), in verticale ≈ 90° (in alto).
   ENTER congela il valore (badge LOCK), R lo sblocca.
5. Pedometro: camminare 10 passi a ~2 Hz; il contatore deve salire di 10 ±1 e
   comparire una barra nel grafico. Digitare sulla tastiera da fermo non deve
   aggiungere passi (finestra di soppressione 350 ms).
6. Pedometro: premere R, confermare ENTER; passi e grafico tornano a zero.
7. Uscire dal pedometro (ESC) e rientrare: il conteggio deve essere conservato.
8. **LED condiviso**: avviare, uscire da Sensori senza aprire la torcia, poi
   usare ESP-NOW con un peer che invia `led 255 40 0`: il LED deve accendersi
   rosso. Ripetere aprendo e chiudendo la torcia prima del comando ESP-NOW.
9. **Senza RTC**: cancellare l'ora (reboot senza NTP), aprire il pedometro:
   deve mostrare `DATA N.D.` e il conteggio della sessione, non i passi di ieri
   come "OGGI". Camminare, poi sincronizzare l'ora: i passi accumulati non
   devono azzerarsi.
10. **Fallback 24 h**: con data non valida e `up` ≥ 86400 (o forzando il valore
    in NVS), riaprire il pedometro: `pending` e `steps` devono azzerarsi con
    avviso `24H RESET`.
11. Torcia: `,`/`/` cambiano BIANCO/ROSSO/VERDE; `;`/`.` cambiano la luminosità
    (0% = spento); ENTER avvia/ferma SOS. Il LED fisico deve seguire il colore.
12. Torcia: ESC, e poi uscire dal menu Sensori; il LED deve essere spento.
13. Torcia: lasciare fermo fino al timeout (default 120 s); il LED deve
    spegnersi 1 s prima dell'attenuazione/sospensione. Al risveglio, con una
    sola pressione, la torcia deve riaccendersi con il timeout riavviato.
14. Rotazione utente 3: aprire Sensori, verificare che la UI sia diritta
    (rotazione 1) e che all'uscita torni la rotazione 3.
15. NVS piena / errore di scrittura: la UI deve mostrare `!NVS ERR` e non
    perdere silenziosamente i passi.

## Dimensione firmware

Comando: `python -m platformio run -e cardputer-adv-offensive` (SUCCESS).

- `.pio/build/cardputer-adv-offensive/firmware.bin`: **2.370.960 byte**
  (2,26 MiB).
- Flash applicativa usata secondo PlatformIO: **2.370.597 byte su 2.621.440**
  (90,4%). Restano circa 251 KB nello slot OTA.
- RAM statica: **115.528 byte su 327.680** (35,3%). Il framebuffer 240x135
  (16 bpp, ~64,8 KB) viene allocato a runtime da `display::begin()`.
- Heap libero a runtime non misurato (nessun hardware collegato); il modulo non
  alloca buffer dinamici propri oltre a piccole `std::string` di formattazione.

## Limiti e rischi

- Nessun RTC/NTP in questo modulo: il "giorno" è affidabile solo se un altro
  modulo sincronizza l'orologio. Senza data la UI mostra `DATA N.D.`; il
  conteggio visibile è `pending`, il fallback è a 24 h di uptime (non tempo
  reale) e il reset manuale resta disponibile.
- Il primo sync NTP attribuisce a oggi tutti i passi accumulati senza data: se
  il cambio di giorno è avvenuto durante quel periodo, la divisione non è
  perfetta. È il compromesso dichiarato per non azzerare i passi della sessione.
- Il pedometro è euristico: non è un dispositivo medico. Scossoni forti o
  camminate molto lente possono falsare il conteggio; la soppressione della
  tastiera riduce ma non elimina i falsi passi da digitazione.
- Il conteggio avanza solo mentre la schermata Pedometro è aperta: i menu
  bloccano e non esiste un servizio in background. Il totale e `pending`
  restano in NVS; il grafico degli ultimi 60 s è solo di sessione.
- Durante la conferma modale del reset il campionamento IMU è sospeso per alcuni
  secondi: eventuali passi in quella finestra non vengono contati.
- Il verso della bolla dipende dagli assi del BMI270 montati sul Cardputer ADV;
  se su un esemplare appare invertito, invertire `nx` o `ny` in
  `drawLevelFrame()`.
- La UI forza la rotazione 1 e ripristina la rotazione utente all'uscita. Se
  l'allocazione del framebuffer 240x135 fallisce (heap molto frammentato), la
  UI può restare nella rotazione precedente.
- Scritture NVS ogni 60 s: compromesso tra perdita di passi e usura flash.
- `M5.Led` è un singolo RGB (GPIO 21); non c'è un LED bianco dedicato, quindi
  "bianco" è RGB al massimo. La luminosità della torcia è mappata 0–100 → 0–255;
  all'uscita la luminosità globale torna al default 63 e il colore è nero.
- Il colore precedente eventualmente impostato da ESP-NOW non viene ripristinato
  dopo la torcia: il LED resta nero a luminosità 63, e il modulo ESP-NOW può
  impostare il proprio colore al comando successivo.
- Il frame nero al boot contro il reset con torcia accesa è in `src/main.cpp`
  (patch condivisa del supervisore), non in questo modulo.
- Non è stato possibile provare su hardware reale in questo worktree: build e
  simulazione offline sono le verifiche disponibili.
