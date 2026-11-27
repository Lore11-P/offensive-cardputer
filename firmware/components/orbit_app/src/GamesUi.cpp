#include "StoreUi.h"

#include <dirent.h>
#include <string>
#include <vector>

#include "Display.h"
#include "MenuManager.h"
#include "Ui.h"
#include "orbit_native_games/NativeGames.h"
#include "orbit_store/Store.h"
#include "orbit_wren/GameRuntime.h"

namespace orbit_app {
namespace {

// I .wren scaricati dal firmware 2.x stanno su SD in /games: restano
// eseguibili anche senza reinstallarli dallo Store.
std::vector<std::string> sdGames() {
    std::vector<std::string> names;
    if (!orbit_store::mountSd()) {
        return names;
    }
    DIR* dir = opendir("/sd/games");
    if (dir == nullptr) {
        return names;
    }
    for (dirent* entry = readdir(dir); entry != nullptr; entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".wren") == 0) {
            names.push_back(name);
        }
    }
    closedir(dir);
    return names;
}

void playWren(const std::string& path, const std::string& name) {
    if (!orbit_wren::runGame(path)) {
        ui::message("Giochi",
                    {"!Gioco non avviato.", name,
                     "Memoria insufficiente o file non valido."},
                    "Giochi");
    }
}

}  // namespace

void runGames() {
    MenuManager menu("Giochi", "Home");
    menu.addItem("Giochi nativi", "games", [] { orbit_native_games::runMenu(); });

    if (orbit_store::mountLittleFs()) {
        const std::vector<std::string> files = orbit_store::installedGames();
        for (const std::string& file : files) {
            const std::string label = file.substr(0, file.size() - 5);
            menu.addItem(label, "games", [file, label] {
                playWren(std::string(orbit_store::kGamesDir) + "/" + file, label);
            });
        }
    }
    for (const std::string& file : sdGames()) {
        const std::string label = file.substr(0, file.size() - 5) + " (SD)";
        menu.addItem(label, "games", [file, label] {
            playWren("/sd/games/" + file, label);
        });
    }
    if (menu.items().size() == 1) {
        menu.addItem("Nessun gioco scaricato", "store", [] {}, false);
    }

    menu.setHint("ENTER=gioca  `=indietro");
    menu.run(true);
}

}  // namespace orbit_app
