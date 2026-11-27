#pragma once

namespace orbit_chat {

// Menu della chat BLE basata su advertising (nessun GATT).
void runMenu();

// Ferma scan/advertising e deinizializza host NimBLE e controller BT, per
// liberare heap. Idempotente: runMenu() la chiama sempre prima di uscire.
// Va chiamata dal task applicativo, mai da un callback NimBLE.
void shutdown();

}  // namespace orbit_chat
