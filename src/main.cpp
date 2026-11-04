// ============================================================================
//  CardputerFirmware v1.0 - entry point
//  M5Stack Cardputer ADV (ESP32-S3FN8, 8 MB flash)
//
//  Menu principale -> moduli separati. Ogni modulo e' compilabile a parte con
//  i flag ENABLE_MODULE_* definiti in platformio.ini.
// ============================================================================

#include <Arduino.h>
#include <M5Cardputer.h>

#include "Config.h"
#include "Display.h"
#include "Logger.h"
#include "MenuManager.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "PowerTimer.h"
#include "Storage.h"
#include "Ui.h"

#if ENABLE_MODULE_BLE
#include "modules/ble/BleSpam.h"
#endif
#if ENABLE_MODULE_CHAT && ENABLE_MODULE_BLE
#include "modules/chat/Chat.h"
#endif
#if ENABLE_MODULE_WIFI
#include "modules/wifi/WifiMenu.h"
#endif
#if ENABLE_MODULE_IR
#include "modules/ir/IrMenu.h"
#endif
#if ENABLE_MODULE_GAMES
#include "modules/games/GamesMenu.h"
#endif
#if ENABLE_MODULE_SETTINGS
#include "modules/settings/SettingsMenu.h"
#endif
#if ENABLE_MODULE_SD
#include "modules/sd/SdMenu.h"
#endif
#if ENABLE_MODULE_RADAR
#include "modules/radar/RadarMenu.h"
#endif
#if ENABLE_MODULE_RADIO
#include "modules/radio/RadioMenu.h"
#endif

namespace {

std::function<void()> guardedModule(const char* name, config::Module module,
                                    void (*entry)()) {
    return [name, module, entry]() {
        if (!config::moduleEnabled(module)) {
            ui::message("Modulo disattivato",
                        {std::string(name) + " e' disattivato a runtime.",
                         "Attivalo da Impostazioni > Moduli attivi."},
                        "Menu principale");
            return;
        }
        entry();
    };
}

void buildAndRunMainMenu() {
    MenuManager menu(fw::kTitle);
    menu.setHint("`=indietro  </>=scorri  ENTER=apri");
    menu.setCarousel();

#if ENABLE_MODULE_BLE
    menu.addItem("BLE", "ble", guardedModule("BLE", config::Module::Ble, ble::runMenu));
#endif
#if ENABLE_MODULE_CHAT && ENABLE_MODULE_BLE
    menu.addItem("Chat", "chat", guardedModule("Chat", config::Module::Chat, chat::runMenu));
#endif
#if ENABLE_MODULE_WIFI
    menu.addItem("WiFi", "wifi", guardedModule("WiFi", config::Module::Wifi, wifi::runMenu));
#endif
#if ENABLE_MODULE_RADAR
    menu.addItem("Radar", "radar", guardedModule("Radar", config::Module::Radar, radar::runMenu));
#endif
#if ENABLE_MODULE_RADIO
    menu.addItem("Radio", "tv", guardedModule("Radio", config::Module::Radio, radio::runMenu));
#endif
#if ENABLE_MODULE_IR
    menu.addItem("Infrarossi", "ir", guardedModule("IR", config::Module::Ir, ir::runMenu));
#endif
#if ENABLE_MODULE_GAMES
    menu.addItem("Giochi", "games", guardedModule("Giochi", config::Module::Games, games::runMenu));
#endif
#if ENABLE_MODULE_SD
    menu.addItem("SD", "sd", guardedModule("SD", config::Module::Sd, sd_module::runMenu));
#endif
#if ENABLE_MODULE_SETTINGS
    menu.addItem("Impostazioni", "settings", settings_module::run);
#else
    menu.addItem("Impostazioni", "settings",
                 [] { ui::message("Impostazioni", {"Modulo non compilato."}, "Menu principale"); });
#endif

    menu.run(false);
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(150);

    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);

    // Filesystem prima della configurazione (import opzionale di /config.json).
    storage::begin();
    config::begin();

    display::begin();
    display::applyRotation(config::rotation());
    M5Cardputer.Display.setBrightness(config::brightness());

    navigation::begin();
    powertimer::begin();
    display::splash();

    logger::info("%s avviato", fw::kTitle);
    logger::info("flash=%uKB heap=%uKB offensive=%d", ESP.getFlashChipSize() / 1024,
                 ESP.getFreeHeap() / 1024, ENABLE_OFFENSIVE);

    buildAndRunMainMenu();
}

void loop() {
    // Il menu principale gira nel proprio ciclo bloccante in setup().
    // Questo loop resta come punto di ingresso per future estensioni.
    delay(1000);
}
