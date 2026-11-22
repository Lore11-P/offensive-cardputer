# FIX_CORE — callback menu, VBUS, carosello portrait, coda tastiera

Worktree: `orbit-wt-fix-core`, branch `review/fix-core`, base `ec5a601`
(ORBIT OS 2.0.2). Riferimento: `docs/integration/review/01-core.md`.
Hardware di riferimento: M5Stack Cardputer ADV, ESP32-S3FN8, 320 KB SRAM,
nessuna PSRAM.

Applicati i Must fix M1/M2 e gli Should fix S1/S2/S3 nella parte piccola e
sicura. Nessun hardware collegato in questo ambiente: build e test host sono
misurati, il comportamento su device resta da verificare (§5).

---

## 1. Modifiche

### 1.1 M1 — `MenuManager`: callback copiata prima di invocarla

`src/core/MenuManager.cpp`, ramo `Event::Select`:

- `auto action = items_[selected_].action;` viene copiata **prima** di
  `navigation::waitRelease()` e dell'invocazione. Se l'azione chiama
  `setItems()`, `addItem()` o `addSeparator()`, la ricostruzione di `items_`
  non distrugge piu' il `std::function` in esecuzione (niente use-after-free).
- Stessa cosa per `textHandler_`: l'handler viene copiato prima della
  chiamata, cosi' un `setTextHandler()` fatto dall'handler non invalida la
  funzione che sta girando.
- Costo: una copia di `std::function` per attivazione (e per tasto di testo),
  solo quando la callback esiste.

### 1.2 M2 — Ricarica: VBUS prima dell'euristica

`src/core/Display.cpp`, `chargingLikely()`:

- Dopo il controllo esplicito `isCharging()`, viene letta
  `M5Cardputer.Power.getVBUSVoltage()`.
- Se il PMIC espone VBUS (`>= 0`), lo stato e' `vbus > 4000` mV e l'euristica
  sulla tensione batteria non viene eseguita.
- L'euristica a gradini resta solo come fallback quando `getVBUSVoltage()`
  ritorna `-1` (modello senza monitor VBUS).

`src/core/PowerTimer.cpp`, `powerOff()`:

- La prima schermata di sospensione usa `display::chargingLikely()` invece di
  `false`; cosi' su una board con VBUS la scritta **IN RICARICA** compare
  subito, senza il secondo ridisegno dopo un secondo.

**Limite hardware accertato (ADV v1.0):** il fix VBUS e' inerte su questa
revisione. M5Unified 0.3.0 (versione risolta dalla build) mappa
`board_M5CardputerADV` su `pmic_adc`
(`Power_Class.inl:984-988`) e `getVBUSVoltage()` ritorna `-1` per i modelli
senza PMIC (`Power_Class.inl:2965-3010`). Lo schematic K132-Adv v1.0 usa un
TP4057: i pin `CHRG`/`STDBY` non sono collegati a nessun GPIO e la batteria
e' letta solo dall'ADC su G10. Non esiste quindi un segnale USB leggibile dal
firmware su questa revisione: il caso "device gia' collegato al caricatore
all'avvio/sospensione" resta affidato all'euristica e **non e' risolto**.
Il ramo VBUS e' pronto per una revisione hardware o una libreria che lo
espongano.

### 1.3 S1 — Carosello in portrait

`src/core/MenuManager.cpp`, `drawCarousel()`:

- Se `display::width() < 200` il carosello non viene disegnato: si usa
  `drawList()` (lista verticale gia' esistente).
- `top_` viene ricalcolato qui (in modalita' carosello `ensureVisible()` non
  lo fa) centrando la selezione e clampando all'ultima pagina.
- `carouselAnim_` viene azzerato: senza, il loop continuerebbe a ridisegnare
  il corpo ogni 2 ms per un'animazione che in portrait non esiste.
- Nessun cambiamento alla geometria landscape.

### 1.4 S2 — Coda tastiera senza perdere tasti

`src/core/Navigation.cpp` + nuovo `include/NavCore.h`:

- La logica di differenza (`fresh`, `newEnter`, `newBack`, coda) e' in
  `navigation::stepKeys()`, senza dipendenze hardware.
- I caratteri non-freccia nuovi vengono accodati **prima** dei rami
  ENTER/ESC; un carattere nello stesso campione di ENTER/ESC non viene perso.
- Il tasto ESC fisico (`` ` ``) non viene accodato come testo.
- Le frecce in eccesso nello stesso campione vengono accodate e consegnate ai
  poll successivi: `;`+`.` non scarta piu' la seconda freccia.
- `waitRelease()` riallinea `KeyState` e svuota la coda, come prima.

### 1.5 S3 — Test host

Nuova cartella `test/test_core/test_main.cpp`, framework Unity gia'
configurato nell'env `native`:

1. carattere + ENTER nello stesso campione (ENTER ora, carattere al poll dopo);
2. due frecce nello stesso campione (`;` poi `.`);
3. wrap di pagina della griglia (stessa colonna);
4. ultima pagina parziale (clamp sull'ultima voce);
5. tutte le voci disabilitate (selezione invariata);
6. ESC non accodato come `Text`.

La matematica di griglia e' stata estratta in `include/GridMath.h`
(`gridmath::move`) e `MenuManager::moveGrid()` ora usa quella, cosi' il test
copre la stessa implementazione del firmware.

---

## 2. File toccati

| File | Modifica |
|---|---|
| `src/core/MenuManager.cpp` | M1, S1, `moveGrid` su `GridMath.h` |
| `src/core/Navigation.cpp` | S2, usa `NavCore.h` |
| `src/core/Display.cpp` | M2, ramo VBUS |
| `include/Display.h` | commento `chargingLikely` |
| `src/core/PowerTimer.cpp` | M2, stato iniziale schermata sospensione |
| `include/NavCore.h` | nuovo, logica tastiera pura |
| `include/GridMath.h` | nuovo, matematica griglia pura |
| `test/test_core/test_main.cpp` | nuovo, 6 test host |

Non toccati: `platformio.ini`, `partitions.csv`, `flasher/`, moduli delle
altre aree.

---

## 3. Build

Env `cardputer-adv-offensive`, timeout 900.000 ms.

- Build completa (cache vuota): `SUCCESS`, 576,8 s.
- Build incrementale dopo l'ultima modifica: `SUCCESS`, 83,0 s.
- Baseline: stessi numeri di `FIX_MUSIC.md` sullo stesso HEAD `ec5a601`
  (firmware 2.697.712 B, flash 2.697.341 B, RAM 147.392 B).

| Voce | Prima | Dopo | Delta |
|---|---:|---:|---:|
| `firmware.bin` | 2.697.712 B | **2.698.384 B** | **+672 B (+0,02%)** |
| Flash `text+data` | 2.697.341 B (95,7%) | **2.698.017 B (95,74%)** | **+676 B** |
| RAM statica `.data+.bss` | 147.392 B (45,0%) | **147.392 B (45,0%)** | **0 B** |
| Libero nello slot app 2.0 | 120.707 B | **120.031 B** | −676 B |

---

## 4. Test host

```
$env:PATH = "C:\msys64\mingw64\bin;$env:PATH"
python -m platformio test -e native --filter test_core
```

Risultato: **6 test, 6 passati** (`native:test_core [PASSED]`). Nessun test
hardware-in-the-loop.

---

## 5. Rischi e cose da verificare

1. **M2 su ADV resta aperto.** La revisione v1.0 non ha VBUS leggibile
   (TP4057 senza linee di stato verso la MCU). Serve un test su hardware con
   USB e senza USB per confermare che il fallback sia almeno invariato; il
   caso "avvio/sospensione gia' su USB" non e' risolvibile via software su
   questa board. Da rivalutare se una revisione futura espone VBUS/PMIC.
2. **API `getVBUSVoltage()`**: presente in M5Unified 0.3.0, la versione
   risolta dalla build. Le dipendenze non sono pinnate: una versione molto
   vecchia di M5Unified non compilerebbe. Il progetto usa gia' 0.3.0.
3. **Portrait non visto a schermo.** La lista e' la stessa `drawList()` del
   menu verticale, ma la resa (etichette, icone, righe visibili) va guardata
   su device in rotazione 0/2.
4. **Test host parziali.** Coprono solo le due funzioni pure; non coprono
   `MenuManager::run()` ne' il percorso hardware della tastiera.
5. **Copia `std::function`** per ogni attivazione: se una callback futura
   cattura buffer grandi, la copia alloca. Oggi le callback sono lambda
   piccole; costo trascurabile.
