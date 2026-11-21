#include "WifiLabMenu.h"

#include "Display.h"
#include "ModuleFlags.h"
#include "Ui.h"

#if ENABLE_MODULE_WIFILAB

namespace wifilab {

void runMenu() {
    ui::message("WifiLab", {"PMKID, client monitor, whitelist e spettro 2.4 GHz.", "Modulo in integrazione."}, "Menu principale");
}

}  // namespace wifilab

#endif  // ENABLE_MODULE_WIFILAB
