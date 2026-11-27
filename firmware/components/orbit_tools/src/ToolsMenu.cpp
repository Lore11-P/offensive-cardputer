#include "orbit_tools/Tools.h"

#include "MenuManager.h"
#include "ToolsInternal.h"

namespace orbit_tools {

void runMenu() {
    MenuManager menu("Strumenti", "Home");
    menu.addItem("Orologio", "planet", [] { clockScreen(); });
    menu.addItem("Note", "file", [] { notesMenu(); });
    menu.addItem("Calcolatrice", "chip", [] { calculatorScreen(); });
    menu.addItem("TOTP", "key", [] { totpMenu(); });
    menu.addItem("QR code", "scan", [] { qrScreen(); });
    menu.addItem("Info", "info", [] { infoScreen(); });
    menu.addItem("microSD", "sd", [] { sdBrowser("/sd"); });
    menu.setHint("ENTER=apri  `=indietro");
    menu.run(true);
}

}  // namespace orbit_tools
