# DIAG — “colori un po’ sbagliati” su ORBIT OS 2.0.0

Worktree `orbit-wt-fix-colors`, branch `fix/fix-colors`, HEAD `553f604`.
Baseline di confronto: `c7456af` (“Specs dei 10 moduli”, pre-ottimizzazione).
Analisi statica: nessuna modifica al codice, nessuna compilazione, nessun commit.

La regressione è stata introdotta dal commit `c7e00af` (`perf(ui): rendering e
menu senza ridisegni inutili`). Il commit `53aeec1` (hub/icone) non cambia i
valori di colore.

---

## 1. Sintomi attesi (firma del difetto)

Se la causa è quella indicata al §2, il difetto ha una firma molto precisa:

- **Sbagliano solo le aree a dithering** prodotte da `orbit::sky()` /
  `orbit::ditherRect()`: cielo del carosello, splash di avvio, anteprima del
  tema, sfondo delle schermate WiFi / WiFi setup / OTA.
- **Tutto il resto è corretto**: testo, pannelli, bordi, header, footer, LED,
  pianeta, badge e icone (disegnati con `drawPixel`/`fillRect`/`drawLine`,
  quindi con conversione colore regolare).
- La tinta non è “leggermente diversa”: i byte dei 16 bit sono invertiti.
  In NOVA il cielo indaco diventa verde-oliva; in PAPER il cielo crema diventa
  verde scuro.
- Il difetto è **stabile** (non dipende da frame, rotazione o tema) e resta
  identico tra disegno completo e ridisegno dell’animazione del carosello,
  perché entrambi passano dallo stesso `ditherSpan`.

Valori concreti (RGB565 → RGB approssimato):

| Token | Valore atteso | Colore atteso | Dopo lo swap | Colore mostrato |
|---|---:|---|---:|---|
| NOVA `T_sky0` | `0x0022` | rgb(0,4,16) quasi nero-blu | `0x2200` | rgb(32,64,0) verde oliva |
| NOVA `T_sky1` | `0x0865` | rgb(8,12,41) blu notte | `0x6508` | rgb(98,161,65) verde |
| NOVA `T_bg` | `0x0843` | rgb(8,8,24) indaco | `0x4308` | rgb(65,97,65) grigio-verde |
| PAPER `T_sky0` | `0xE71A` | rgb(230,226,213) crema | `0x1AE7` | rgb(24,93,57) verde scuro |

Questa è la ragione per cui il difetto può essere percepito come “colori un po’
sbagliati” e non come “schermo rotto”: il 90% della UI usa i primitivi grafici
ed è corretto; sbaglia solo lo sfondo, che è però la superficie più grande.

---

## 2. Cause candidate, ordinate per probabilità

### Candidato 1 — `pushImage(uint16_t*)` in `ditherSpan` scrive RGB565 nativo in un framebuffer byte-swapped — probabilità ~90%

**Punto esatto:** `src/core/Orbit.cpp:66-76`, riga critica **`src/core/Orbit.cpp:73`**.

```cpp
// src/core/Orbit.cpp:66
void ditherSpan(lgfx::LGFXBase& d, int x, int y, int w, int level, uint16_t a, uint16_t b) {
    ...
    for (int i = 0; i < chunk; ++i) {
        s_row[i] = (bayer(x + done + i, y) < level) ? b : a;  // riga 71
    }
    d.pushImage(x + done, y, chunk, 1, s_row);                // riga 73  <-- BUG
}
```

`a`/`b` sono colori RGB565 **nativi** (es. `display::C(T_sky0)`), ma
`pushImage(const uint16_t*)` non li converte: tratta il buffer come se fosse
già nel formato del display.

Catena M5GFX 0.3.0 (la versione presente nei `.pio` dei worktree):

1. `LGFXBase.hpp:414` — il template `pushImage(const T*)` chiama `create_pc(data)`.
2. `LGFXBase.hpp:1196` — `create_pc(const uint16_t* data)` usa `_swapBytes`.
3. `_swapBytes` è **`false`** di default (`LGFXBase.hpp:1035`) e nel progetto non
   viene mai chiamato `setSwapBytes`.
4. `LGFXBase.hpp:1198-1201` — con `swap == false` il buffer viene interpretato
   come `swap565_t`, cioè **già byte-swapped**, qualunque sia il contenuto reale.
5. `pixelcopy.inl:44` — `no_convert = (src_depth == dst_depth)`: con sprite e
   pannello entrambi a 16 bpp (`rgb565_2Byte`) diventa `true`, quindi nessuna
   conversione: copia/memcpy dei byte così come sono.
6. `colortype.hpp:823` / `enum.hpp:469` — 16 bpp = `rgb565_2Byte` (`swap565_t`):
   il buffer dello sprite contiene i pixel in ordine di byte del pannello.
7. `Orbit.cpp:73` vi scrive invece valori nativi → il pannello riceve i due byte
   invertiti. `Panel_LCD::writeImage` prende il fast path `no_convert` e copia i
   byte così come sono: non c’è una seconda conversione che possa correggerli.

Prova che il progetto conosce già questo comportamento:
`src/modules/music/MusicMenu.cpp:248-253` evita deliberatamente `pushImage` per
le copertine:

```cpp
// drawPixel passa dalla conversione colore del display: pushImage
// riswappa i 16 bit e inverte i canali RGB della cover.
for (...) d.drawPixel(x + col, y + row, px[row * kCoverSide + col]);
```

Il commit `dd15a43` (“ORBIT OS 1.21.0: corregge i colori della copertina nel
player”) è nato esattamente per questo. L’ottimizzazione UI ha reintrodotto lo
stesso errore nel path del dithering.

**Perché è il candidato più probabile**

- È l’unico punto nuovo in cui un colore RGB565 nativo viene passato a
  `pushImage`.
- Il baseline `c7456af` disegnava lo stesso dither con `drawFastHLine()` (che
  converte il colore), quindi il confronto A/B isola la regressione.
- Spiega perfettamente la selettività: sbagliano le aree `sky`/`ditherRect`,
  non il resto della UI.
- Ha un precedente documentato nello stesso repository (copertine musica).

**Aree coinvolte** (tutte via `orbit::sky`/`orbit::ditherRect`):

- `src/core/Display.cpp:1120`, `1152`, `1219` — splash di avvio;
- `src/core/Display.cpp:1200` — fumo del lancio (`ditherRect`);
- `src/core/MenuManager.cpp:677` — corpo del carosello principale;
- `src/modules/settings/SettingsMenu.cpp:338` — anteprima tema;
- `src/modules/wifi/WifiMenu.cpp:143` — schermata WiFi;
- `src/utils/OtaUpdate.cpp:38` — schermata OTA;
- `src/utils/WifiSetup.cpp:150` — portale WiFi setup.

**Fix proposto (una riga):**

```cpp
// src/core/Orbit.cpp:73
d.pushImage(x + done, y, chunk, 1,
            reinterpret_cast<const lgfx::rgb565_t*>(s_row));
```

Il cast a `rgb565_t` attiva in M5GFX il ramo “sorgente RGB565 nativo”
(`LGFXBase.hpp:1186-1193`): `no_convert = false` e
`fp_copy = get_fp_copy_rgb_affine<rgb565_t>(_write_conv.depth)`, che converte
nel depth reale della destinazione (sprite o pannello). Corretto anche se in
futuro cambia il depth o `_swapBytes`.

**Alternativa più veloce** (stesso file, se il costo per pixel conta):

```cpp
const uint16_t ca = lgfx::getSwap16(a);   // orbita in ordine pannello
const uint16_t cb = lgfx::getSwap16(b);
...
s_row[i] = (bayer(x + done + i, y) < level) ? cb : ca;
```

Questa conserva il fast path memcpy, ma assume `rgb565_2Byte` sulla
destinazione (vero oggi per sprite 16 bpp e pannello Cardputer ADV). Va
commentata come assunzione, altrimenti si ricrea lo stesso tipo di bug latente.

**Rischio del fix:** nessun cambio di API né di layout. Il cast introduce una
conversione per pixel (240 px per riga, ~101 righe di cielo nel carosello,
~24k pixel): costo stimato basso su ESP32-S3, da misurare a occhio/`millis` se
si vuole essere certi. L’alternativa con `getSwap16` mantiene le prestazioni
attuali. Da non fare: `setSwapBytes(true)` globale come “fix”, perché cambia la
semantica di ogni futuro `pushImage(uint16_t*)` e va ricordato su entrambe le
istanze (sprite e display).

---

### Candidato 2 — push parziale `display::push(x,y,w,h)` e assunzione 16 bpp/2 byte — probabilità ~7%

**Punti esatti:** `src/core/Display.cpp:599-647`, in particolare:

- `628` `static uint16_t s_row[240]`
- `629` `const int stride = s_sprite.width() * 2;`
- `636`/`642` `pixelcopy_t pc(..., dstDepth, srcDepth)`
- `641` `memcpy(..., w * 2)`

Il codice **è corretto oggi**: legge i depth reali (`630-632`), copia i byte
grezzi dallo sprite (già in ordine pannello) e ricostruisce un `pixelcopy_t`
con `srcDepth == dstDepth == 16`, quindi il fast path `no_convert` copia gli
stessi byte senza riconvertirli. Il push pieno (`pushSprite`, riga 595) e
quello parziale usano lo stesso formato.

Il rischio è latente: `stride`, la dimensione del `memcpy` e `s_row` assumono
**16 bpp** anche se il codice legge il depth a runtime. Se in futuro:
- lo sprite venisse creato a 8/24 bpp (o il fallback `setBuffer(...,16)` a
  riga 173 cambiasse profondità),
- `_bitwidth != width` (non accade a 16 bpp; LovyanGFX può paddare solo i
  depth < 8),
- la destinazione avesse una palette: qui `pixelcopy_t` non riceve
  `dst.hasPalette()`/palette, a differenza del blit del pianeta,

allora **solo le strisce parziali** sarebbero sbagliate: header ridisegnato
dopo un cambio batteria (`MenuManager.cpp:510`, `Ui.cpp:21`), LED del footer
(`Display.cpp:1067`), corpo dell’animazione carosello
(`MenuManager.cpp:502-503`). Un ridisegno completo tornerebbe corretto, con un
effetto a “strisce” alternato. Sintomo diverso da quello osservato, per questo
la probabilità è bassa.

**Test di conferma:** far cambiare la firma batteria (collegare/scollegare USB)
e confrontare il colore dell’header prima/dopo; far lampeggiare il LED e
fotografare. Se le strisce restano identiche al resto, il push parziale è
innocente.

**Fix (solo se il test lo conferma):** rendere esplicito il formato sorgente
anche qui, es. `lgfx::pixelcopy_t pc(s_row, dstDepth, lgfx::rgb565_nonswapped)`
e `stride = s_sprite.width() * (srcDepth >> 3)`. M5GFX 0.3.0 non espone una
`pushSprite` parziale, quindi la strada restano i `pixelcopy_t` per riga. Non
serve ora.

---

### Candidato 3 — cache pianeta / tinta delle nuove icone hub — probabilità ~3%

**Cache pianeta:** `src/core/Orbit.cpp:280-289`.

```cpp
lgfx::pixelcopy_t pc(s_planetCanvas.getBuffer(), dst.getColorDepth(),
                     s_planetCanvas.getColorDepth(), dst.hasPalette(), nullptr,
                     kPlanetTransparent);
dst.pushImage(cx - side / 2, cy - side / 2, side, side, &pc);
```

Qui la conversione è **identità** (`swap565_t` → `swap565_t`, `color_convert`
generico restituisce il valore, `pixelcopy.hpp`), il colore trasparente
`0x0000` è invariante allo swap e il confronto avviene sui valori raw della
sprite. Se sbagliasse, l’effetto sarebbe locale: alone nero/quadrato intorno al
pianeta nel carosello e nell’anteprima tema, non un cambio di tinta globale.
La cache si invalida anche per tema (`Orbit.cpp:236-256`), quindi non è un
problema di palette stantia.

**Nuove icone hub:** `src/core/MenuManager.cpp:170-173` passa un `tint` per le
voci non selezionate del carosello; `Pen` in `src/core/OrbitIcons.cpp:22-24`
collassa `main`/`alt`/`hot` su quel singolo colore, mentre i riempimenti
espliciti `C(T_bg3)`/`C(T_line)` restano a piena tinta. Nelle icone nuove
(`hid`, `tools`, `sensors`, `espnow`, `wifilab`…) si possono quindi vedere
dettagli chiari su un corpo spento: è un’incoerenza **solo delle icone**, non
dei colori globali. Se la lamentela riguarda esclusivamente le icone del
carosello, questa è la spiegazione; le palette e i token non c’entrano.

**Rilievo minore correlato:** `Pen` usa sempre una griglia 16×16
(`OrbitIcons.cpp:17-21`); con `size = 12` (es. `EspNowMenu.cpp:2638`) l’icona
sborda di 2 px per lato e può coprire bordi/testo. Non è un errore di colore,
ma può sembrare “pixel colorati fuori posto”.

---

## 3. Piste verificate e scartate

| Pista | Esito |
|---|---|
| Tabella palette (`include/OrbitPalette.h`) | **Nessuna differenza** tra `c7456af` e HEAD; i 7 temi e i 46 token sono identici. |
| `display::C()` vs variabili `k*` | `C(t)` restituisce `pal[t]`; `applyPalette()` assegna i `k*` dagli stessi token, nello stesso ordine del baseline (`Display.cpp:56-86`). Nessuna regressione. |
| Nuovi colori hub (`HubMenus.cpp`) | Il file non contiene costanti colore: usa solo `MenuManager` e le icone. |
| `setSwapBytes` | **Mai chiamato** in `src/`/`include/`. Il default M5GFX è `false`, che è proprio la premessa del bug del §2. |
| `pushRect` / `readRect` | **Nessun uso** nel firmware. `readRect` è usato solo internamente da M5GFX. |
| `createSprite` / `setColorDepth` | Solo `Display.cpp:158-160` (sprite UI) e `Orbit.cpp:247-257` (cache pianeta), entrambi 16 bpp. Nessun 8/24 bpp. |
| `pushImage` con colori nativi | **Due soli punti** in tutto il progetto: `Orbit.cpp:73` (bug) e `Orbit.cpp:286` (planeta, con `pixelcopy_t` corretto). |
| Header/footer/LED | Disegnati con primitivi nello sprite; il push parziale è byte-corretto oggi. Nessuna evidenza di errore. |
| Copertine musica | Già protette dal workaround `drawPixel` (`MusicMenu.cpp:248-253`), non coinvolte. |

---

## 4. Test per confermare (in ordine di costo)

1. **Tinte piatte (isolamento in un colpo).**
   Su una schermata di test disegnare, una sotto l’altra:
   - `d.fillRect(x, y0, w, 1, display::C(T_sky0));`  → atteso `0x0022`;
   - `orbit::ditherRect(x, y1, w, 1, display::C(T_sky0), display::C(T_sky0), 8);`
   - `orbit::ditherRect(x, y2, w, 1, display::C(T_sky1), display::C(T_sky1), 8);`
   Con `a == b` il dither è irrilevante: tutte e tre le righe devono avere lo
   stesso colore. Se la prima è blu e la seconda/terza sono verdi, il bug è
   `ditherSpan`/`pushImage`.

2. **Lettura RGB dal framebuffer (senza fotocamera).**
   Dopo `ditherRect` chiamare `display::g().readPixel(x, y)` e stampare il
   valore su seriale. `readPixel` riconverte il raw in RGB565 nativo: se
   restituisce `0x2200` invece di `0x0022`, lo sprite contiene già i byte
   invertiti. Ripetere su un pixel scritto con `fillRect`: deve restituire
   `0x0022`.

3. **Screenshot BMP.**
   `display::saveScreenshot("/diag.bmp")` dal menu principale e campionare un
   pixel di cielo (es. `(20, 30)`). Se il BMP ha il cielo verde ma i pannelli
   corretti, la corruzione è nel contenuto dello sprite, non nel push al
   pannello.

4. **Confronto A/B.**
   Fotografare lo splash del build `c7456af` e del build HEAD con la stessa
   palette NOVA. Il baseline deve avere il cielo blu notte; HEAD verde oliva.
   In alternativa, ripristinare temporaneamente solo `ditherSpan` a
   `drawFastHLine` e osservare se il cielo torna corretto (è il test decisivo).

5. **Sweep temi.**
   Aprire Impostazioni > Tema colore su NOVA, PAPER e MONO. Se tutte le aree
   `sky` cambiano tinta nello stesso modo (byte swap) mentre pianeta, testo e
   barra dei temi restano corretti, è confermato il §2. In PAPER il salto
   crema → verde scuro è il più evidente.

6. **Escludere i push parziali.**
   Con il menu carosello fermo, collegare/scollegare USB per far cambiare la
   batteria nell’header e attendere il lampeggio del LED. Se header e footer
   mantengono gli stessi colori del resto, il candidato 2 è escluso.

7. **Escludere le icone.**
   Aprire i 7 hub e confrontare le icone in lista (tinta piena) e nel
   carosello (tinta blend). Se il problema è solo “alcune parti delle icone
   sono spente/chiare”, è il candidato 3, non la palette.

---

## 5. Fix proposto — riepilogo

| Priorità | File:riga | Modifica | Rischio |
|---|---|---|---|
| **P0** | `src/core/Orbit.cpp:73` | `pushImage(..., reinterpret_cast<const lgfx::rgb565_t*>(s_row))` | Basso: converte correttamente; costo per pixel da misurare. |
| P1 (solo se serve performance) | `src/core/Orbit.cpp:66-73` | pre-swappare `a`/`b` con `lgfx::getSwap16` e mantenere il fast path | Basso ma assumption `rgb565_2Byte` da commentare. |
| P2 (latente) | `src/core/Display.cpp:628-641` | rendere espliciti `srcDepth`/stride invece di `width*2` | Nessun effetto oggi; irrobustisce i push parziali. |
| P3 (solo cosmetico) | `src/core/OrbitIcons.cpp:17-24` | gestire `size < 16` e la tinta mista delle icone hub | Nessun impatto sui colori globali. |

**Verdetto:** la causa più probabile in assoluto è il **candidato 1**, con
confidenza alta: è l’unico punto in cui la regressione `c7e00af` passa colori
RGB565 nativi a `pushImage`, spiega esattamente la selettività del difetto
(sfondo/dither sbagliati, resto corretto) e ha un precedente documentato nello
stesso repository (`MusicMenu.cpp:248-249`).

Il fix P0 è una riga, non tocca API, layout, palette o moduli. Da verificare su
hardware con i test 1-5 e con un controllo del tempo frame del carosello.

---

## 6. Esito del fix (aggiornamento)

**Applicato** in `src/core/Orbit.cpp:73`:

```cpp
d.pushImage(x + done, y, chunk, 1,
            reinterpret_cast<const lgfx::rgb565_t*>(s_row));
```

Il cast è stato scelto al posto del pre-swap con `getSwap16` perché è la
soluzione corretta rispetto al depth reale della destinazione (sprite o
pannello) e non dipende da `_swapBytes` o dal formato `rgb565_2Byte`.
Il commento nel codice spiega perché il cast è necessario.

**Audit degli altri push:** in `src/`/`include` non esistono altri
`pushImage`/`pushRect` con buffer `uint16_t` non castati. Restano solo:

- `src/core/Display.cpp:637/643` — passano un `pixelcopy_t` con i depth reali
  (corretto, nessuna modifica);
- `src/core/Orbit.cpp:286` — blit del pianeta con `pixelcopy_t` e trasparenza
  (corretto, nessuna modifica).

**Build:** `python -m platformio run -e cardputer-adv-offensive` → **SUCCESS**
(196,07 s).

| Voce | Valore |
|---|---:|
| Flash usata | 2.690.405 / 2.818.048 B (95,5%) |
| `firmware.bin` | 2.690.768 B |
| RAM statica | 147.280 / 327.680 B (44,9%) |

**Test host** (MinGW g++ 16.2.0, header M5GFX reali del worktree):

- `get_depth<uint16_t>` = `rgb565_2Byte` e `get_depth<rgb565_t>` =
  `rgb565_nonswapped` verificati con `static_assert`;
- conversione `copy_rgb_affine<swap565_t, rgb565_t>`: `0x0022 → 0x2200`,
  `0x0865 → 0x6508`, `0x0843 → 0x4308`, `0xFB4F → 0x4FFB`, `0x0000 → 0x0000`;
- percorso senza cast: il buffer resta `0x0022`, riletto come `0x2200` →
  riproduce esattamente il bug descritto.

**Costo misurato (host, non hardware):** 240 px/riga, conversione
`rgb565_t → swap565_t` circa 0,46 ns/px contro 0,03 ns/px del memcpy; per il
cielo del carosello (240×101 ≈ 24k px) resta una frazione di millisecondo su
PC e dell’ordine di pochi decimi di ms su ESP32-S3. Il collo di bottiglia vero
resta la SPI del display. La scelta del cast è quindi confermata.

**Non fatto:** nessun test su Cardputer reale; nessuna misura dei tempi frame
sul dispositivo.

