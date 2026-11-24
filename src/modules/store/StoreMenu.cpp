#include "StoreMenu.h"

#include <string>
#include <vector>

#include "GameRuntime.h"
#include "MenuManager.h"
#include "Storage.h"
#include "StoreClient.h"
#include "StoreConfig.h"
#include "Ui.h"
#include "WifiSetup.h"

namespace store {
namespace {

std::vector<MenuItem> gameItems(const std::vector<GameInfo>& games) {
    std::vector<MenuItem> items;
    for (const GameInfo& game : games) {
        std::string label = game.name;
        if (!game.version.empty()) {
            label += " " + game.version;
        }
        if (gameInstalled(game)) {
            label += " *";
        }
        // Copia per valore: il catalogo locale puo' essere ricostruito.
        items.emplace_back(label, "store", [game] {
            if (!gameInstalled(game) && !downloadGame(game, "Store")) {
                return;
            }
            const std::string path = gamePath(game.id);
            if (!path.empty()) {
                runGame(path);
            }
        });
    }
    return items;
}

void openInstalled() {
    const std::vector<std::string> files = storage::list(kGamesDir, false, true);
    MenuManager installed("Giochi installati", "Store");
    for (const std::string& file : files) {
        constexpr size_t kExtLen = 5;  // ".wren"
        if (file.size() <= kExtLen ||
            file.compare(file.size() - kExtLen, kExtLen, ".wren") != 0) {
            continue;
        }
        const std::string name = file.substr(0, file.size() - kExtLen);
        installed.addItem(name, "games", [file] {
            runGame(std::string(kGamesDir) + "/" + file);
        });
    }
    if (installed.items().empty()) {
        ui::message("Giochi installati",
                    {"Nessun gioco in /games.", "Scarica un gioco dal catalogo."}, "Store");
        return;
    }
    installed.setHint("ENTER=gioca  `=indietro");
    installed.run(true);
}

}  // namespace

void runMenu() {
    if (!storage::hasSD()) {
        ui::message("Store",
                    {"!Serve la microSD.", "Inserisci la card e riprova."},
                    "Menu principale");
        return;
    }
    if (!wifisetup::ensureConnected(true, "Store")) {
        ui::message("Store",
                    {"!Serve una connessione WiFi.", "Configura la rete da",
                     "Impostazioni > Rete WiFi."},
                    "Menu principale");
        return;
    }

    std::vector<GameInfo> games;
    const bool catalogOk = fetchCatalog(games, "Store");

    MenuManager menu("Store", "Menu principale");
    menu.setItems(gameItems(games));
    menu.addItem("Giochi installati", "folder", [] { openInstalled(); });
    if (!catalogOk) {
        // Riprova ricostruisce il menu in place: fetchCatalog mostra gia'
        // l'errore, qui si resta sulla lista dei giochi installati.
        menu.addItem("Riprova", "rotate", [&menu, &games] {
            if (!fetchCatalog(games, "Store")) {
                return;
            }
            menu.setItems(gameItems(games));
            menu.addItem("Giochi installati", "folder", [] { openInstalled(); });
        });
    }
    menu.setHint("ENTER=installa/gioca  `=indietro");
    menu.run(true);
}

}  // namespace store
