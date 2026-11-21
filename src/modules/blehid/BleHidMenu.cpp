#include "BleHidMenu.h"

#include "Display.h"
#include "ModuleFlags.h"
#include "Ui.h"

#if ENABLE_MODULE_BLEHID

namespace blehid {

void runMenu() {
    ui::message("BleHid", {"BLE HID: tastiera BLE e Bad BLE.", "Modulo in integrazione."}, "Menu principale");
}

}  // namespace blehid

#endif  // ENABLE_MODULE_BLEHID
