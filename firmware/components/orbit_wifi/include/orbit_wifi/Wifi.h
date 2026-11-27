#pragma once

// Modulo WiFi di OrbitOS 3.0: scanner, AP info, attacchi 802.11 e
// analizzatore canali. Il menu va aperto dal hub "Home".
namespace orbit_wifi {

void runMenu();

// Solo diagnostica seriale: scansione + un singolo frame raw di prova.
// Non usato dal firmware di produzione (ORBIT_DIAG_SELFTEST).
bool selfTest();

}  // namespace orbit_wifi
