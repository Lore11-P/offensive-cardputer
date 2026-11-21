# FIX — colori RGB565 nel dithering (`ditherSpan`)

Worktree `orbit-wt-fix-colors`, branch `fix/fix-colors`, base `553f604`.
Diagnosi completa: `docs/integration/DIAG_COLORS.md`.

## Modifica

`src/core/Orbit.cpp:73`, funzione `orbit::ditherSpan()`:

```cpp
// prima
d.pushImage(x + done, y, chunk, 1, s_row);

// dopo
// Il cast forza M5GFX a convertire da RGB565 nativo al depth reale
// della destinazione: il template pushImage(uint16_t*) assume invece
// un buffer gia' in ordine pannello (swap565) e invertirebbe i byte.
d.pushImage(x + done, y, chunk, 1,
            reinterpret_cast<const lgfx::rgb565_t*>(s_row));
```

`a`/`b` arrivano da `display::C(...)` e da `orbit::blend()`, quindi sono RGB565
nativi. Il template `pushImage(const uint16_t*)` di M5GFX, con
`_swapBytes = false` (default mai cambiato nel progetto), interpreta il buffer
come `swap565_t` e con `no_convert = true` lo copia così com’è: i due byte di
ogni pixel finiscono invertiti (es. NOVA `0x0022` → `0x2200`). Il cast a
`lgfx::rgb565_t` seleziona il ramo di conversione nativo→destinazione e corregge
sprite e pannello senza assumere il formato.

Scelta del cast invece del pre-swap con `lgfx::getSwap16`: il cast è chiaro,
non dipende da `_swapBytes` e resta corretto se in futuro cambia il depth della
destinazione. Costo misurato host-side: ~0,46 ns/px contro ~0,03 ns/px del
memcpy; per 24k px del cielo del carosello è una frazione di millisecondo su
PC e pochi decimi di ms su ESP32-S3, trascurabile rispetto alla SPI.

## File toccati

| File | Modifica |
|---|---|
| `src/core/Orbit.cpp` | cast `reinterpret_cast<const lgfx::rgb565_t*>(s_row)` + commento |
| `docs/integration/DIAG_COLORS.md` | aggiunta §6 “Esito del fix” |
| `docs/integration/FIX_COLORS.md` | questo documento |

Nessuna modifica a palette, `Display.cpp`, moduli, `platformio.ini` o API.

## Audit degli altri push

Cercati `pushImage`, `pushRect`, `pushPixels`, `pushColors` in `src/` e
`include/`:

- `src/core/Display.cpp:637`, `643` — `pushSprite` parziale, passa un
  `pixelcopy_t` costruito con i depth reali di sprite e pannello. Corretto:
  copia byte grezzi già in ordine pannello. Nessuna modifica.
- `src/core/Orbit.cpp:286` — blit della cache pianeta con `pixelcopy_t`,
  profondità reali e trasparenza `0x0000` (invariante allo swap). Corretto.
- Nessun `pushRect`/`pushPixels`/`pushColors` con buffer `uint16_t` non castato.
- `src/modules/music/MusicMenu.cpp:248-253` — già protetto con `drawPixel`.

## Test

### Build firmware

```powershell
python -m platformio run -e cardputer-adv-offensive
```

Esito: **SUCCESS**, 196,07 s.

| Voce | Valore |
|---|---:|
| Flash usata | 2.690.405 / 2.818.048 B (95,5%) |
| `firmware.bin` | 2.690.768 B |
| RAM statica | 147.280 / 327.680 B (44,9%) |

### Test host (logica di conversione con gli header M5GFX reali)

Compilato con MinGW g++ 16.2.0 e `-I` sui sorgenti M5GFX del worktree.
Il test include `lgfx/v1/misc/pixelcopy.hpp`, verifica con `static_assert` che
`get_depth<uint16_t>` sia `rgb565_2Byte` e `get_depth<rgb565_t>` sia
`rgb565_nonswapped`, poi esegue `copy_rgb_affine<swap565_t, rgb565_t>`.

Output:

```text
native 0x0022 -> raw swap565 0x2200 (atteso 0x2200)
native 0x0865 -> raw swap565 0x6508 (atteso 0x6508)
native 0x0843 -> raw swap565 0x4308 (atteso 0x4308)
native 0xFB4F -> raw swap565 0x4FFB (atteso 0x4FFB)
native 0x0000 -> raw swap565 0x0000 (atteso 0x0000)
senza cast: raw 0x0022, letto 0x2200 (bug 0x2200)
OK: cast RGB565 corretto, senza cast riproduce 0x0022 -> 0x2200
```

Il test è stato eseguito in `%TEMP%\opencode` (non committato): il repository
non ha una suite host per M5GFX e aggiungerne una richiederebbe una dipendenza
di test dedicata. I valori e il comando sono riportati qui per riproducibilità.

### Microbenchmark host

20000 iterazioni × 101 righe × 240 px:

```text
memcpy:      0.008 us/riga, 0.03 ns/px
cast rgb565: 0.111 us/riga, 0.46 ns/px
```

Misura host, non hardware: serve solo a escludere un costo percepibile. Il
frame del carosello usa ~24k px; il tempo vero è dominato dalla SPI.

### Non eseguito

- Nessun test su Cardputer ADV reale (vietato inventare risultati hardware).
- Nessuna misura dei frame sul dispositivo.
- Da verificare a occhio: splash e carosello in NOVA (cielo blu notte, non
  verde), anteprima tema in PAPER (crema, non verde scuro), header/footer
  invariati.

## Rischio residuo

1. **Nessuna validazione hardware**: la correttezza della conversione è
   verificata sui sorgenti M5GFX e con test host, non sul pannello reale.
2. **Costo CPU**: +~0,43 ns/px host-side rispetto al memcpy; su S3 stimato
   sotto il millisecondo per frame di cielo. Se il costo diventasse
   percepibile rispetto alla SPI, si può passare al pre-swap `getSwap16`
   (documentato in `DIAG_COLORS.md`), ma introduce l’assunzione `rgb565_2Byte`.
3. **Formato futuro**: il cast segue il depth reale, quindi è robusto a un
   cambio di `_swapBytes` o di profondità dello sprite; il resto del push
   parziale (`Display.cpp`) resta legato ai 16 bpp (rischio latente già noto,
   non affrontato qui per non cambiare logica).
4. **Nessun effetto su palette, layout o API**: il fix cambia solo il percorso
   di conversione del dithering.
