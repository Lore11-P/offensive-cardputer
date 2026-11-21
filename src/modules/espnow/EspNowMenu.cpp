#include "EspNowMenu.h"

#include "Display.h"
#include "ModuleFlags.h"
#include "Ui.h"

#if ENABLE_MODULE_ESPNOW

namespace espnow {

void runMenu() {
    ui::message("EspNow", {"ESP-NOW: file, comandi e clipboard mesh.", "Modulo in integrazione."}, "Menu principale");
}

}  // namespace espnow

#endif  // ENABLE_MODULE_ESPNOW
