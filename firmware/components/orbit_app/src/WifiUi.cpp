#include "StoreUi.h"

#include <string>

#include "Display.h"
#include "InputUi.h"
#include "Navigation.h"
#include "Ui.h"
#include "orbit_store/Store.h"

namespace orbit_app {
namespace {

void drawLine(const std::string& title, const std::string& line) {
    display::clear(display::C(display::T_bg));
    display::header(title, "Impostazioni");
    display::text(8, 38, line, display::C(display::T_text), 2);
    display::push();
}

}  // namespace

void runWifiSetup() {
    std::string ssid;
    if (!orbit_store::currentSsid(ssid)) {
        ssid = "Vodafone-A40244362";
    }

    display::clear(display::C(display::T_bg));
    display::header("WiFi", "Impostazioni");
    display::text(8, 34, "Rete:", display::C(display::T_dim), 0);
    display::text(8, 46, ssid, display::C(display::T_text), 2);
    display::text(8, 68, "INVIO=password  `=indietro", display::C(display::T_dim), 0);
    display::push();

    for (;;) {
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Select) {
            break;
        }
        if (ev.type == navigation::Event::Back) {
            return;
        }
        delay(20);
    }

    std::string password;
    if (!textInput("Password WiFi", "Impostazioni", password, 63)) {
        return;
    }

    if (!orbit_store::saveWifi(ssid, password)) {
        ui::message("WiFi", {"!Salvataggio non riuscito.", "NVS non disponibile."},
                    "Impostazioni");
        return;
    }

    drawLine("WiFi", "Connessione...");
    std::string error;
    if (!orbit_store::ensureWifi(&error)) {
        ui::message("WiFi", {"!Connessione fallita.", error}, "Impostazioni");
        return;
    }
    ui::message("WiFi", {"Connesso a", ssid}, "Impostazioni");
}

}  // namespace orbit_app
