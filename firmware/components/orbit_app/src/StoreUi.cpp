#include "StoreUi.h"

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

void showStatus(const std::string& line) {
    display::clear(display::C(display::T_bg));
    display::header("Store", "Home");
    display::text(10, 42, line, display::C(display::T_text), 2);
    display::push();
}

void playOrInstall(const orbit_store::GameInfo& game) {
    const std::string path = orbit_store::gamePath(game.id);
    if (!orbit_store::gameInstalled(game)) {
        showStatus("Scarico " + game.name + "...");
        std::string error;
        if (!orbit_store::downloadGame(game, &error)) {
            ui::message("Store", {"!Download fallito.", error}, "Store");
            return;
        }
    }
    if (!orbit_wren::runGame(path)) {
        ui::message("Store", {"!Il gioco non e' partito.", game.name}, "Store");
    }
}

}  // namespace

void runStore() {
    if (!orbit_store::mountLittleFs()) {
        ui::message("Store", {"!Archivio giochi non disponibile.", "Inserisci la card e riprova."},
                    "Home");
        return;
    }

    showStatus("Connessione WiFi...");
    std::string error;
    if (!orbit_store::ensureWifi(&error)) {
        ui::message("Store", {"!WiFi non connesso.", error,
                              "Configura il WiFi in Impostazioni."},
                    "Home");
        return;
    }

    showStatus("Scarico il catalogo...");
    std::vector<orbit_store::GameInfo> catalog;
    if (!orbit_store::fetchCatalog(catalog, &error)) {
        ui::message("Store", {"!Catalogo non disponibile.", error}, "Home");
        return;
    }

    MenuManager menu("Store", "Home");
    for (const orbit_store::GameInfo& game : catalog) {
        const bool installed = orbit_store::gameInstalled(game);
        menu.addItem(installed ? game.name + " *" : game.name, "store",
                     [game] { playOrInstall(game); });
    }
    if (catalog.empty()) {
        menu.addItem("Nessun gioco", "store", [] {}, false);
    }
    menu.addItem("Giochi nativi", "games", [] { orbit_native_games::runMenu(); });
    menu.setHint("ENTER=installa/gioca  `=indietro");
    menu.run(true);
}

}  // namespace orbit_app
