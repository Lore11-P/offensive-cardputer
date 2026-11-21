# Feature 01 — Database IR completo su microSD

## Cosa è stato fatto

- Importato **Flipper-IRDB** (`logickworkshop/Flipper-IRDB`, commit
  `d126fb1b6f1e114c52b4a8c19839ea65e3a9c24d`, licenza CC0 1.0) sulla microSD in
  `E:\ir`, senza toccare `E:\BruceIR`.
- I 307 file già presenti in `E:\ir` (albero piatto `/ir/<marca>`) **non sono
  stati cancellati né spostati**: restano come fallback per LittleFS e per le
  vecchie SD.
- Nuovo albero su SD: `/ir/<categoria>/<marca>/<file>.ir`. Nomi di cartelle e
  file sanificati (ASCII, `A-Z a-z 0-9 . _ - +`, lunghezza massima 48 caratteri)
  per FAT32. Collisioni di nome risolte con suffisso `_2`, `_3`; file identici
  già presenti saltati (hash SHA-256).
- `IrMenu.cpp` ora legge **LittleFS + SD insieme**:
  - le categorie vengono enumerate unendo i due filesystem;
  - le marche dentro `TV` uniscono `/ir/TV/<marca>` e il vecchio `/ir/<marca>`;
  - i file della stessa marca vengono fusi e deduplicati per nome
    (case-insensitive), quindi una voce sola per telecomando;
  - il path completo (`/ir/<categoria>/<marca>/<file>.ir` oppure
    `/ir/<marca>/<file>.ir`) è quello salvato in preferiti e ripresa.
- Categorie visibili: **TV, Clima, Audio, DVD/Blu-ray, Proiettore, Console,
  Universale, Altro**. Una categoria compare solo se ha almeno una marca.
- Ricerca, preferiti, ripresa dell'ultimo telecomando, tasti rapidi
  (`P M + - C X`), scanner per marca e scanner per categoria continuano a
  funzionare con il nuovo albero.
- Protocolli: accettate solo le varianti con lunghezza corretta
  (`NEC`/`NECext`, `Samsung`/`Samsung32`, `LG`/`LG2`, `Sony`/`SIRC12/15/20`,
  `RC5`, `RC5X`, `RC6`, `JVC`, `Kaseikyo`, `RCA`, raw). `NEC42`, `Samsung36` e
  `Pioneer` non vengono inviati come frame a 32 bit: compaiono come `[n/d]` con
  messaggio "Protocollo non supportato".
- Raw: massimo 512 valori per comando, file oltre 64 KB rifiutati con messaggio
  esplicito, nessuna copia di `rawData` nelle lambda del menu (riferimento al
  vettore dei comandi); il testo del file viene liberato dopo il parsing.
- `include/Storage.h` e `src/utils/Storage.cpp` **non sono stati modificati**:
  `storage::list`, `exists` e `readText` già uniscono LittleFS e SD.

## Conteggi

File `.ir` presenti su `E:\ir`: **2.255** (8,47 MB).

Voci visibili nel menu (dopo deduplica per marca+nome file): **2.015**.

| Categoria | Marche | Telecomandi visibili |
|---|---:|---:|
| TV | 133 | 464 |
| Clima | 228 | 403 |
| Audio | 128 | 371 |
| DVD/Blu-ray | 37 | 124 |
| Proiettore | 54 | 126 |
| Console | 4 | 8 |
| Universale | 5 | 5 |
| Altro | 343 | 514 |

- 240 duplicati di nome tra albero piatto e `TV` vengono nascosti dal menu.
- Con SD assente restano i **307** file LittleFS (`data/ir`), invariati.

Mappatura usata per l'import:

- TV: `TVs`.
- Clima: `ACs`, `Fans`, `Air_Purifiers`, `Heaters`, `Humidifiers`, `Fireplaces`,
  `Bidet`, `Window_cleaners`, `Dust_Collectors`.
- Audio: `Audio_and_Video_Receivers`, `SoundBars`, `Speakers`, `CD_Players`,
  `MiniDisc`, `Head_Units`, `Streaming_Devices`, `Multimedia`,
  `Car_Multimedia`.
- DVD/Blu-ray: `DVD_Players`, `Blu-Ray`, `Laserdisc`, `VCR`, `Converters`.
- Proiettore: `Projectors`.
- Console: `Consoles`.
- Universale: `Universal_TV_Remotes`.
- Altro: tutte le altre categorie organizzate (LED, ventilatori non clima,
  telecamere, decoder, giocattoli, ecc.). `_Converted_` è escluso perché
  duplica in altri formati gli stessi telecomandi.

## Comandi

- Menu principale → **Infrarossi** → **Telecomandi** → categoria → marca →
  telecomando.
- `ENTER` invia il comando selezionato / apre la voce.
- `P` power, `M` mute, `+`/`=` volume su, `-` volume giù, `C` canale su,
  `X` canale giù (invio immediato, `ENTER` ripete).
- `F` aggiunge/rimuove il telecomando dai preferiti.
- `ESC` torna indietro.
- **Cerca** cerca marca e modello in tutte le categorie.
- **Scanner** → categoria → marca invia il tasto Power di ogni telecomando.

## Firmware

- Build `cardputer-adv-offensive`: **SUCCESS**.
- Flash usata: **2.362.793 byte** su 2.621.440 (90,1%); `firmware.bin`
  2.363.152 byte (2,25 MB).
- Baseline stesso worktree senza le modifiche IR: 2.358.193 byte.
  Delta feature: **+4.600 byte (4,49 KiB)**, sotto il limite di ~5 KB.
- RAM statica invariata: 115.520 byte (35,3%).

## Test

- Build `cardputer-adv-offensive` eseguita con
  `python -m platformio run -e cardputer-adv-offensive`.
- Simulazione host della logica di enumerazione sui dati reali di `E:\ir`:
  2.015 voci visibili, 240 duplicati scartati, deduplica case-insensitive.
- Scansione protocolli su tutti i 2.255 file: NEC 11.196, NECext 9.922,
  Samsung32 4.172, Kaseikyo 2.175, RC5 2.103, SIRC 1.515, RC6 1.133,
  SIRC20 1.062, SIRC15 1.062, RCA 461, RC5X 232, Pioneer 180, NEC42 124.
  I climatizzatori usano solo NEC/NECext (già supportati).
- File più grande: 67.357 byte (`Clima/Hisense/Hisense_DG11J1-99_fahrenheit.ir`),
  rifiutato con messaggio esplicito; gli altri 2.254 file sono sotto 64 KB.

Test manuali suggeriti:

1. SD inserita: Infrarossi → Telecomandi → verificare le 8 categorie e aprire
   un clima (es. Samsung, LG, Daikin), un DVD e un proiettore.
2. SD assente: verificare che restino le 38 marche TV LittleFS e che preferiti
   e ripresa puntino ancora a `/ir/<marca>/<file>.ir`.
3. Aprire lo stesso telecomando da SD, aggiungerlo ai preferiti, riavviare e
   verificare la ripresa.
4. Cercare "samsung" e "split": i risultati devono includere TV e Clima.
5. Inviare Power/Vol su un dispositivo proprio e verificare che i comandi
   `[n/d]` (NEC42, Pioneer) mostrino l'avviso invece di trasmettere.

## Limiti

- Nessun test su hardware reale in questa sessione: build e simulazione host.
- 1 file oltre 64 KB non è apribile; 124 segnali NEC42 e 180 Pioneer sono
  marcati non supportati (nessun falso invio).
- `Altro` è una categoria aggiuntiva rispetto alle sette richieste, necessaria
  per includere il resto del database senza duplicare `_Converted_`.
- Le marche del vecchio albero piatto restano sempre sotto `TV`; non c'è
  rilevamento automatico del tipo di dispositivo per i file legacy.
- Nessuna modifica a `Storage`/`main`/`platformio.ini`/`ModuleFlags.h`.
