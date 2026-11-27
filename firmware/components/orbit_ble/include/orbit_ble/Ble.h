#pragma once

namespace orbit_ble {

// Menu "Bluetooth": scanner BLE, spam di advertising (con avviso legale
// obbligatorio) e info sullo stack. Inizializza NimBLE all'ingresso e lo
// deinizializza all'uscita, per restituire l'heap ai giochi Wren / WiFi.
void runMenu();

// Solo diagnostica seriale: scan passivo di ~2,5 s, senza UI.
bool selfTest();

}  // namespace orbit_ble
