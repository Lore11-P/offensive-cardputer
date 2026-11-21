#include "SensorsMenu.h"

#include "Display.h"
#include "ModuleFlags.h"
#include "Ui.h"

#if ENABLE_MODULE_SENSORS

namespace sensors {

void runMenu() {
    ui::message("Sensors", {"Livella, goniometro, pedometro e torcia LED.", "Modulo in integrazione."}, "Menu principale");
}

}  // namespace sensors

#endif  // ENABLE_MODULE_SENSORS
