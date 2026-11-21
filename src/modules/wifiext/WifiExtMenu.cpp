#include "WifiExtMenu.h"

#include <string>
#include <vector>

#include "AutoPortal.h"
#include "Display.h"
#include "Karma.h"
#include "MenuManager.h"
#include "ModuleFlags.h"
#include "Responder.h"
#include "Storage.h"
#include "Ui.h"
#include "WifiExtCommon.h"

#if ENABLE_MODULE_WIFIEXT

namespace wifiext {
namespace {

std::string fileLine(const char* path) {
    if (!storage::exists(path, true)) {
        return std::string(path) + ": assente";
    }
    return std::string(path) + ": " + std::to_string(storage::fileSize(path, true)) +
           " byte";
}

void showStatus() {
    std::vector<std::string> lines;
    lines.push_back("SD: " + sdFreeText());
    lines.push_back(fileLine("/wifiext/ntlm.txt"));
    lines.push_back(fileLine("/wifiext/portal.csv"));
    lines.push_back(storage::exists("/wifiext/karma.txt")
                        ? "karma.txt: presente"
                        : "karma.txt: assente (SSID noti)");
    lines.push_back(storage::exists("/wifiext/portal.html", true)
                        ? "portal.html: presente"
                        : "portal.html: pagina interna");
    ui::message("WiFi+ dati", lines, "WiFi+");
}

void deleteFile(const char* path, const char* label) {
    if (!storage::exists(path, true)) {
        ui::message(label, {"File gia' assente."}, "WiFi+");
        return;
    }
    if (!ui::confirm(label,
                     {"Cancellare " + std::string(path) + "?",
                      "L'operazione non e' reversibile."},
                     "WiFi+")) {
        return;
    }
    if (storage::removeFile(path, true)) {
        ui::message(label, {">File cancellato."}, "WiFi+");
    } else {
        ui::message(label, {"!Cancellazione non riuscita."}, "WiFi+");
    }
}

void notBuilt() {
    ui::message("WiFi+",
                {"Funzione non inclusa in questa build.",
                 "Usa l'ambiente cardputer-adv-offensive."},
                "WiFi+");
}

}  // namespace

void runMenu() {
    MenuManager menu("WiFi+", "Menu principale");
    menu.addSeparator("Strumenti");
#if ENABLE_OFFENSIVE
    menu.addItem("Responder", "scan", [] { runResponder(); });
    menu.addItem("Karma AP", "wifi", [] { runKarma(); });
    menu.addItem("Auto-Portal", "portal", [] { runAutoPortal(); });
#else
    menu.addItem("Responder", "scan", notBuilt);
    menu.addItem("Karma AP", "wifi", notBuilt);
    menu.addItem("Auto-Portal", "portal", notBuilt);
#endif
    menu.addSeparator("Dati");
    menu.addItem("Stato e spazio SD", "sd", showStatus);
    menu.addItem("Cancella ntlm.txt", "file",
                 [] { deleteFile("/wifiext/ntlm.txt", "Cancella hash"); });
    menu.addItem("Cancella portal.csv", "file",
                 [] { deleteFile("/wifiext/portal.csv", "Cancella credenziali"); });
    menu.setHint(";.=nav   ENTER=apri   ESC=indietro");
    menu.run(true);
}

}  // namespace wifiext

#endif  // ENABLE_MODULE_WIFIEXT
