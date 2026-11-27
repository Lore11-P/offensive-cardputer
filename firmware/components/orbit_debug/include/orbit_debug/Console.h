#pragma once

// Console di debug sulla USB-Serial-JTAG: comandi di riga e risposte "[DBG] ".
namespace orbit_debug {

// Avvia il task di console (core 0, 4 KB). Idempotente.
void begin();

}  // namespace orbit_debug
