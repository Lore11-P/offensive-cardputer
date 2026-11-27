# orbit_mascot

Zorbo, la mascotte procedurale del firmware 2.x, portata su ESP-IDF.

- Stato persistente in NVS namespace `orbit-mascot` (`score`, `aff`, `evt`, `pts`).
- `runMenu()`: schermata interattiva, ENTER = coccola, ESC = esci.
- `award(Event)`: punti fissi per azione offensiva completata, da chiamare solo
  dopo `ui::legalWarning` e a fine azione. Nessun punto per pacchetto.
- Disegno procedurale identico: nessun asset, nessuna allocazione nel frame.

Limiti: i moduli WiFi/BLE/IR devono chiamare `mascot::award()` esplicitamente;
il collegamento e' fatto dove il port lo consente.
