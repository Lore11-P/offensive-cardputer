# MERGE B — gruppo B (04, 06, 07, 09, 10)

- Worktree: `orbit-wt-merge-b` (branch `feat/merge-b`)
- Base: `feat/10-modules` (`c7456af`, "Specs dei 10 moduli")
- Data: 2026-11-21
- Build di riferimento: `python -m platformio run -e cardputer-adv-offensive`
- Nessun push eseguito.

## Esito per feature

| # | Feature | Branch / commit | Merge commit | Esito | Conflitti |
|---|---|---|---|---|---|
| 04 | BLE HID + Bad BLE | `feat/blehid` / `99ed8c8` | `32d13ab` | unita | nessuno |
| 06 | WiFi Lab (PMKID, client, whitelist, spettro) | `feat/wifilab` / `183477c` | `3ea1c4b` | unita | nessuno |
| 07 | WPA-SEC (upload, potfile, key) | `feat/wpasec` / `a2050aa` | `dfea93d` | unita | nessuno |
| 09 | WiFi+ (Responder, Karma, Auto-Portal) | `feat/wifiext` / `dc1bfde` | `02073f7` | unita | nessuno |
| 10 | ESP-NOW (file, comandi, clipboard) | `feat/espnow` / `5eb78d6` | `5aa06f3` | unita | nessuno |

ESP-NOW soddisfaceva la condizione richiesta: REPORT.md presente, build
dichiarata SUCCESS (`2.426.096` byte). La versione unita (`5eb78d6`) include
anche la correzione `isCrcSidecar` che era rimasta non committata nel worktree
originale; nessun file estraneo al modulo.

Ogni merge è stato fatto con `git merge --no-ff` e un merge commit separato.
Non sono stati uniti ottimizzatori, Piano B partizioni o UI.

## Build per step

| Step | Contenuto | Firmware.bin | Flash (ELF) / 2.621.440 | RAM statica / 327.680 |
|---|---|---|---|---|
| 1 | + BLE HID | 2.393.344 B | 2.392.981 B (91,3%) | 115.616 B (35,3%) |
| 2 | + WiFi Lab | 2.432.912 B | 2.432.545 B (92,8%) | 122.168 B (37,3%) |
| 3 | + WPA-SEC | 2.487.184 B | 2.486.821 B (94,9%) | 122.208 B (37,3%) |
| 4 | + WiFi+ | 2.519.488 B | 2.519.121 B (96,1%) | 125.680 B (38,4%) |
| 5 | + ESP-NOW (finale) | 2.586.768 B | 2.586.401 B (98,7%) | 126.952 B (38,7%) |

Tutte le build sono `SUCCESS`; nessun errore o warning bloccante.

## Conflitti

Nessun conflitto testuale o semantico: i cinque branch toccavano solo la
propria cartella `src/modules/<nome>/` (più i rispettivi `REPORT.md`).
I file condivisi sono rimasti invariati rispetto a `feat/10-modules`:

```text
git diff --stat feat/10-modules..HEAD -- src/main.cpp platformio.ini \
  include/ModuleFlags.h src/utils/Config.cpp src/modules/settings/SettingsMenu.cpp
(nessun output)
```

## Dimensione finale

- `firmware.bin`: **2.586.768 byte** (2,47 MiB)
- Flash applicativa: **2.586.401 / 2.621.440 byte (98,7%)**
- Spazio residuo nello slot OTA: **35.039 byte (~34 KiB)**
- RAM statica: **126.952 / 327.680 byte (38,7%)**, ~200 KB liberi
- Heap runtime non misurato in questa sede (serve hardware).

## Cosa manca

- Test su hardware: PMKID/deauth/spettro, upload WPA-SEC reale, Responder/
  Karma/Auto-Portal e trasferimenti ESP-NOW tra due Cardputer non sono stati
  eseguiti (solo build).
- Le review in `orbit-wt-qa/docs/integration/reviews` sono state scritte sullo
  scaffold `c7456af`; non esiste una review post-implementazione né una verifica
  della convivenza runtime dei cinque moduli.
- Margine flash quasi esaurito (34 KiB): qualsiasi altra feature o correzione
  richiede riduzioni, esclusione di moduli o Piano B partizioni.
- ESP-NOW è in chiaro e senza autenticazione (limite dichiarato nel REPORT);
  whitelist comandi presente ma `beep`/`led`/`msg` sono attivabili da un peer.
- Nessun push, come da mandato.
