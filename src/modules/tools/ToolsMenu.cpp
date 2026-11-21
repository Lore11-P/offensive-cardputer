#include "ToolsMenu.h"

#include "Display.h"
#include "ModuleFlags.h"
#include "Ui.h"

#if ENABLE_MODULE_TOOLS

namespace tools {

void runMenu() {
    ui::message("Tools", {"Clock, calendario, note, calcolatrice, QR e TOTP.", "Modulo in integrazione."}, "Menu principale");
}

}  // namespace tools

#endif  // ENABLE_MODULE_TOOLS
