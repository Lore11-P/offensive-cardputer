#include "WifiExtMenu.h"

#include "Display.h"
#include "ModuleFlags.h"
#include "Ui.h"

#if ENABLE_MODULE_WIFIEXT

namespace wifiext {

void runMenu() {
    ui::message("WifiExt", {"Responder, Karma e Auto-Portal.", "Modulo in integrazione."}, "Menu principale");
}

}  // namespace wifiext

#endif  // ENABLE_MODULE_WIFIEXT
