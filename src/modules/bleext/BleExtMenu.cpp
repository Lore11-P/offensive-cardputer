#include "BleExtMenu.h"

#include "ModuleFlags.h"

#if ENABLE_MODULE_BLEEXT

#include "BleExtInternal.h"
#include "MenuManager.h"

namespace bleext {

void runMenu() {
  MenuManager menu("BLE+", "BLE");
  menu.addItem("Trova AirTag", "ble", [] { runAirTagFinder(); });
#if ENABLE_OFFENSIVE
  menu.addItem("Falsifica AirTag", "ble", [] { runAirTagSpoof(); });
#endif
  menu.addItem("Trova Flipper", "ble", [] { runFlipperFinder(); });
  menu.addItem("Servizi GATT", "ble", [] { runGattEnum(); });
  menu.addItem("Rileva skimmer", "ble", [] { runSkimmerDetect(); });
  menu.addSeparator("Registrazione");
  menu.addItem("Wardriving", "sd", [] { runWardrivingMenu(); });
  menu.addItem("Cattura BLE", "sd", [] { runBleCapture(); });
  menu.setHint("`=indietro  ;.=nav  ENTER=apri");
  menu.run(true);
}

}  // namespace bleext

#endif  // ENABLE_MODULE_BLEEXT
