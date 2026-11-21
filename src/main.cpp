// ============================================================================
//  ORBIT OS v2.0.0 - entry point
//  M5Stack Cardputer ADV (ESP32-S3FN8, 8 MB flash)
//
//  Menu principale a hub: pochi gruppi coerenti, ogni modulo resta invariato.
//  Ogni modulo e' compilabile a parte con i flag ENABLE_MODULE_* definiti in
//  platformio.ini.
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
#include "modules/hub/HubMenus.h"

namespace {

void buildAndRunMainMenu() {
    MenuManager menu(fw::kTitle);
    menu.setHint(";.,/=scorri  ENTER=apri");
    menu.setCarousel();

#if ENABLE_MODULE_WIFI || ENABLE_MODULE_WIFILAB || ENABLE_MODULE_WIFIEXT || \
    ENABLE_MODULE_WPASEC || ENABLE_MODULE_ESPNOW
    menu.addItem("Rete", "globe", hub::networkMenu);
#endif
#if ENABLE_MODULE_BLE || ENABLE_MODULE_BLEHID || (ENABLE_MODULE_CHAT && ENABLE_MODULE_BLE)
    menu.addItem("Bluetooth", "ble", hub::bluetoothMenu);
#endif
#if ENABLE_MODULE_RADAR || ENABLE_MODULE_PCAPTOOLS
    menu.addItem("Analisi RF", "scan", hub::analysisMenu);
#endif
#if ENABLE_MODULE_RADIO || ENABLE_MODULE_MUSIC || ENABLE_MODULE_AUDIO
    menu.addItem("Media", "music", hub::mediaMenu);
#endif
#if ENABLE_MODULE_TOOLS || ENABLE_MODULE_SENSORS || ENABLE_MODULE_IR || ENABLE_MODULE_BADUSB
    menu.addItem("Strumenti", "tools", hub::toolsMenu);
#endif
#if ENABLE_MODULE_GAMES
    menu.addItem("Giochi", "games", hub::gamesEntry);
#endif
    menu.addItem("Sistema", "chip", hub::systemMenu);

    menu.run(false);
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(150);

    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);

    // Torcia spenta subito: dopo un reset o un panic il LED RGB non deve
    // restare acceso. M5.Led si inizializza alla prima chiamata.
    M5.Led.setBrightness(63);
    M5.Led.setAllColor(0, 0, 0);
    M5.Led.display();

    // Filesystem prima della configurazione (import opzionale di /config.json).
    storage::begin();
    config::begin();

    display::begin();
    display::setTheme(config::theme());
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
