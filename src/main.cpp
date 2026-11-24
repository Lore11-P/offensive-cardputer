// ============================================================================
//  ORBIT OS v2.3.3 - entry point
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
#include "modules/mascot/Mascot.h"
#include "modules/store/DiagLog.h"
#include "modules/store/GameRuntime.h"
#include "modules/store/StoreMenu.h"

#ifdef ORBIT_DIAG_AUTOGAME
#include <LittleFS.h>

#include "modules/store/DiagGame.h"
#endif

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
    menu.addItem("Zorbo", "alien", mascot::runMenu);
    menu.addItem("Store", "store", store::runMenu);
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
    diag::begin();
    diag::log("BOOT reset=%d heap=%u", static_cast<int>(esp_reset_reason()),
              static_cast<unsigned>(ESP.getFreeHeap()));
    mascot::begin();

    display::begin();
    display::setTheme(config::theme());
    display::applyRotation(config::rotation());
    M5Cardputer.Display.setBrightness(config::brightness());

    navigation::begin();
    powertimer::begin();
    display::splash();

#ifdef ORBIT_DIAG_AUTOGAME
    logger::info("DIAG: heap=%u, invia 'r' per lanciare 2048",
                 static_cast<unsigned>(ESP.getFreeHeap()));
    diag::log("DIAG wait heap=%u", static_cast<unsigned>(ESP.getFreeHeap()));
    {
        File gameFile = LittleFS.open("/diag.wren", FILE_WRITE);
        if (gameFile) {
            gameFile.print(kDiagGame);
            gameFile.close();
            diag::log("diag file written");
        } else {
            diag::log("diag file write FAILED");
        }
    }
    const uint32_t waitStart = millis();
    while (millis() - waitStart < 8000) {
        if (Serial.available() > 0 && Serial.read() == 'r') {
            break;
        }
        delay(50);
    }
    diag::log("DIAG start game heap=%u", static_cast<unsigned>(ESP.getFreeHeap()));
    const bool ok = store::runGame("/diag.wren");
    diag::log("DIAG runGame=%d heap=%u", ok ? 1 : 0,
              static_cast<unsigned>(ESP.getFreeHeap()));
#endif

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
