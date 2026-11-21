# ORBIT OS — ottimizzazione rendering e menu (core UI)

Branch: `feat/opt-ui-core`. Nessuna modifica a moduli, `main.cpp`,
`platformio.ini`, `ModuleFlags.h`, `Config.cpp`, `SettingsMenu.cpp`.
API pubbliche preservate; solo aggiunte. Sette palette invariate.

## Misure

| Voce | Prima | Dopo | Delta |
|---|---|---|---|
| `firmware.bin` | 2.358.560 B | 2.349.536 B | **-9.024 B (-0,38%)** |
| Flash usata (PlatformIO) | 2.358.193 B (90,0%) | 2.349.173 B (89,6%) | -9.020 B |
| RAM statica (`data`+`bss`) | 115.520 B (35,3%) | 116.872 B (35,7%) | +1.352 B |
| Framebuffer heap (240x135x2) | 64.800 B | 64.800 B | invariato |
| Cache pianeta (heap, r=22) | — | 5.202 B (51x51x2) | solo al primo frame del carosello |

La RAM statica cresce di ~1,3 KB per i buffer di riga (2x480 B), lo stato del
LED/planet cache e le code tastiera fisse; in cambio spariscono le allocazioni
per frame. La cache pianeta e' heap e fallisce in modo sicuro (disegno
procedurale identico) se non c'e' spazio.

### Ridisegni (conteggio deterministico, non millisecondi misurati)

Schermo intero = 240x135x2 = **64.800 B** di push; body = 48.480 B;
header = 10.560 B; LED = 7x7x2 = **98 B**.

| Scenario | Prima | Dopo |
|---|---|---|
| Menu carosello fermo | 1 full/s (60/min, ~3,9 MB/min) | 0 full; solo LED quando cambia (~30 push da 98 B/min) + header se cambia la batteria |
| Animazione carosello (6 frame per spostamento) | 6 full (388,8 KB) | 6 body (290,9 KB, **-25%**) con pianeta in cache |
| Menu lista/griglia fermo | 1 full/s | 0 full; header solo al cambio firma batteria |
| `ui::message` / `ui::confirm` / `waitAnyKey` | 2 full/s (120/min, ~7,8 MB/min) | 1 full all'apertura; poi solo LED (~43 push da 98 B/min) e header al cambio batteria |
| Sospensione software, non in carica | 1 schermata piena/s per 8 s | solo al cambio percentuale |

## Modifiche

### `src/core/Display.cpp`, `include/Display.h`
- Overload `const char*` per `text`, `textCentered`, `textWidth`: i letterali
  non costruiscono piu' `std::string` temporanee (heap) a ogni chiamata.
- `push(x,y,w,h)`: aggiornamento parziale del framebuffer, una `pushImage` per
  riga; nessuna allocazione (buffer statico solo per le strisce non a piena
  larghezza). Il colore e' convertito con `pixelcopy_t` usando i color depth
  reali di sprite e pannello.
- `footerTick()`: ridisegna e pubblica solo il LED di stato (7x7), e solo
  quando lo stato di lampeggio cambia. No-op se il footer mostra un trailing.
- `batterySignature()`: percentuale + flag ricarica in un `uint16_t`, usata per
  ridisegnare l'header solo quando serve.
- `header`/`footer`: eliminati i `std::string` temporanei (`parent + " > " +
  title`, `crumb`, `clipped` con concatenazione nel loop). Il troncamento e'
  calcolato sulla larghezza lineare del font e disegnato da buffer di stack;
  il risultato pixel e' identico (verificato con equivalenza esaustiva della
  formula su font 0/2/4, lunghezze 0..120, larghezze 0..320).
- Letture batteria gia' cache 2 s: invariate.

### `src/core/Orbit.cpp`
- `sky`/`ditherRect`: una `pushImage` per riga da un buffer statico da 240
  pixel, invece di centinaia di `drawFastHLine`; stessi pixel (stesso Bayer).
- `planet`: rasterizzato una volta in una `M5Canvas` trasparente (chiave:
  posizione, raggio, luce, city lights, tema) e poi blittato. Attivo solo per
  `r <= 32` (carosello r=22 e anteprima temi r=16); lo splash r=38 resta
  procedurale per non allocare ~12 KB. Se la sprite non e' allocabile o un
  pixel coincide con il colore trasparente, si torna al disegno procedurale.
- `planetInto` e' `noinline` per non duplicare la rasterizzazione.

### `src/core/MenuManager.cpp`, `include/MenuManager.h`
- Loop a sporcizia graduata:
  - interazione/cambio selezione -> full draw (come prima);
  - frame di animazione carosello -> solo body + push body;
  - tick di 1 s -> nessun redraw; `footerTick()` per il LED e header solo se
    `batterySignature()` cambia.
- `draw()` diviso in `drawBody()` + `drawStatus()` + `trailingText()`; il clear
  a schermo intero e' sostituito dal riempimento della sola area body (header
  e footer coprono il resto).
- `Text` handler: redraw solo se il callback ritorna `true` (prima anche un
  tasto ignorato forzava un frame).
- Etichetta del carosello maiuscola/troncata in un buffer di stack (niente
  `std::string` per frame).
- `ellipsize` senza ciclo O(n^2) e con `reserve`; semantica identica.

### `src/core/Navigation.cpp`
- Rimossi `std::vector<char>` per ultima parola e coda eventi: array fissi
  (16+16) e `memmove`. Un poll gira ogni ~2 ms nei menu, quindi sparisce il
  churn di heap su ogni pressione tasto. Priorita' eventi invariata.

### `src/core/Ui.cpp`
- `drawScreen` disegnato una volta all'apertura (prima ogni 500 ms): durante
  l'attesa si aggiornano solo il LED (`footerTick`) e l'header al cambio
  batteria. Eliminata la copia `std::string` di ogni riga.
- `message`, `confirm`, `waitAnyKey` usano lo stesso aggiornamento parziale.

### `src/core/PowerTimer.cpp`
- La percentuale della schermata di sospensione viene dalla cache display
  (una lettura PMIC ogni 2 s invece di una al secondo).
- Senza ricarica la schermata di sospensione e' statica: si ridisegna solo se
  cambia la percentuale; in ricarica resta il lampeggio del fulmine.

## Rischi

1. **Cache pianeta**: 5,2 KB di heap al primo frame del carosello. Se
   l'allocazione fallisce (heap frammentato dopo Wi-Fi/BLE) il disegno torna
   procedurale: nessuna differenza visiva, frame leggermente piu' lento.
2. **Push parziali**: usano `pixelcopy_t` sul buffer dello sprite con il color
   depth reale. Un futuro cambio di formato/endianness in M5GFX potrebbe
   invertire i colori solo nelle strisce parziali; il push pieno resta sano.
   Il codice legge i depth a runtime, non li assume.
3. **Aggiornamenti guidati dallo stato**: header/LED non vengono piu'
   ridisegnati ogni secondo. Codice esterno che sovrascrive l'area header o
   footer senza chiamare `display::header/footer` puo' lasciare quelle strisce
   stantie fino al prossimo ridisegno completo.
4. **Buffer tastiera fissi**: 16 caratteri per parola/coda. La matrice fisica
   non consente piu' di 16 tasti simultanei; un inject hardware diverso
   perderebbe caratteri oltre il limite (documentato come `shortcut:`).
5. **RAM statica +1,3 KB**: accettabile (35,7%), ma da tenere presente se in
   futuro si aggiungono altri buffer statici.

## Test manuali suggeriti

1. Menu principale (carosello): scorrere tutte le voci, verificare pianeta,
   orbita, badge, etichette e animazione in tutte e 7 le palette.
2. Titoli lunghi/breadcrumb: entrare in SD fino a path profondi, verificare
   troncamento con `..` identico a prima.
3. Lista e griglia: separatori, voci disabilitate, indicatore `P1/2`, scrollbar,
   hint troncato a destra del trailing.
4. `ui::message`/`ui::confirm`/avviso legale: lasciare aperto >15 s, il LED
   lampeggia, ENTER/ESC funzionano, la batteria nell'header si aggiorna.
5. Impostazioni > Tema colore: anteprima pianeta per ogni tema, cambio
   rotazione, ritorno al menu (la cache pianeta si ricostruisce).
6. Batteria/carica: avviare con e senza USB, verificare percentuale e fulmine.
7. Spegnimento automatico: attendere attenuazione e sospensione; senza
   caricatore la schermata non deve ridisegnarsi; con caricatore il fulmine
   lampeggia; un tasto riprende e ripubblica il framebuffer.
8. Tastiera: frecce, ENTER, ESC, lettere, scorciatoie (`F` preferiti ecc.),
   input testo; nessun evento perso o duplicato.
9. Nessun flicker percepibile su LED/header/body e nessuna riga residua dopo
   l'animazione del carosello.
10. Screenshot su SD: il BMP deve essere identico al contenuto a schermo.

## Verifica

- Build: `python -m platformio run -e cardputer-adv-offensive` -> SUCCESS.
- Equivalenza troncamento testo: 0 differenze su 3 font x 121 lunghezze x 321
  larghezze, sia con semantica footer (min 0) sia menu (min 2 caratteri).
- Nessun test su hardware disponibile in questa sessione: i conteggi di
  ridisegno sono deterministici; i tempi frame non sono misurati.
