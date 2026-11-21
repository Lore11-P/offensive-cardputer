#include "WpaSecMenu.h"

#include "Display.h"
#include "ModuleFlags.h"
#include "Ui.h"

#if ENABLE_MODULE_WPASEC

namespace wpasec {

void runMenu() {
    ui::message("WpaSec", {"Upload catture e potfile WPA-SEC.", "Modulo in integrazione."}, "Menu principale");
}

}  // namespace wpasec

#endif  // ENABLE_MODULE_WPASEC
