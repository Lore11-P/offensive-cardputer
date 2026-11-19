#include "BadUsbMenu.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "ModuleFlags.h"

#if ENABLE_MODULE_BADUSB

#include "BadUsbScript.h"
#include "Logger.h"
#include "MenuManager.h"
#include "Storage.h"
#include "Ui.h"

namespace badusb {
namespace {

constexpr const char* kFolder = "/badusb";
constexpr size_t kMaxScripts = 100;

struct ScriptEntry {
    std::string name;  // nome con estensione
    std::string path;  // "/badusb/nome.txt"
    bool onSd = false;
};

std::string lowerCopy(const std::string& value) {
    std::string out = value;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool isTxtFile(const std::string& name) {
    if (name.size() <= 4) {
        return false;
    }
    return lowerCopy(name.substr(name.size() - 4)) == ".txt";
}

// La microSD ha la precedenza; se non contiene script si passa a LittleFS.
std::vector<ScriptEntry> scanScripts() {
    std::vector<ScriptEntry> scripts;
    auto collect = [&scripts](bool onSd) {
        const auto entries = storage::listEntries(kFolder, onSd);
        for (const auto& entry : entries) {
            if (entry.isDir || !isTxtFile(entry.name)) {
                continue;
            }
            ScriptEntry script;
            script.name = entry.name;
            script.path = std::string(kFolder) + "/" + entry.name;
            script.onSd = onSd;
            scripts.push_back(std::move(script));
            if (scripts.size() >= kMaxScripts) {
                return;
            }
        }
    };

    if (storage::hasSD()) {
        collect(true);
    }
    if (scripts.empty()) {
        // Lista unita: se la SD non aveva .txt restano solo i file LittleFS.
        collect(false);
    }

    std::sort(scripts.begin(), scripts.end(), [](const ScriptEntry& a, const ScriptEntry& b) {
        const std::string left = lowerCopy(a.name);
        const std::string right = lowerCopy(b.name);
        if (left != right) {
            return left < right;
        }
        return a.name < b.name;
    });
    return scripts;
}

void showHelp() {
    ui::message("Guida rapida",
                {"Script .txt in /badusb (microSD o LittleFS).",
                 "Comandi: STRING, DELAY, ENTER, TAB, frecce,",
                 "CTRL/ALT/SHIFT/GUI + tasto.",
                 "REPEAT n ripete l'ultima riga.",
                 "` interrompe l'esecuzione.",
                 "Collega il cavo USB al computer."},
                "BadUSB", "ENTER/ESC = torna");
}

void runScriptEntry(const ScriptEntry& script) {
    logger::info("BadUSB: eseguo %s", script.path.c_str());
    runScript(script.path, script.name, script.onSd);
}

void runList(bool allowReload) {
    const std::vector<ScriptEntry> scripts = scanScripts();

    MenuManager menu("BadUSB", "Menu principale");
    for (const auto& script : scripts) {
        menu.addItem(script.name, "usb", [script] { runScriptEntry(script); });
    }
    if (scripts.empty()) {
        menu.addSeparator("Nessuno script .txt in /badusb");
    } else if (scripts.size() >= kMaxScripts) {
        menu.addSeparator("Mostrati i primi 100 script");
    }

    menu.addSeparator();
    if (allowReload) {
        // Un solo livello di annidamento: la lista ricaricata non ripete la
        // voce, cosi' non si accumulano menu stantii.
        menu.addItem("Ricarica elenco", "settings", [] { runList(false); });
    }
    menu.addItem("Guida rapida", "file", showHelp);
    menu.setHint("ESC=indietro  ;.=nav  ENTER=avvia");
    menu.run(true);
}

}  // namespace

void runMenu() {
    runList(true);
}

}  // namespace badusb

#endif  // ENABLE_MODULE_BADUSB
