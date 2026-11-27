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
#include "modules/store/DiagConsole.h"
#include "modules/store/GameRuntime.h"
#include "modules/store/StoreMenu.h"

#if defined(ORBIT_DIAG_AUTOGAME) || defined(ORBIT_DIAG_BOOTGAME)
#include <WiFi.h>
#include <esp_heap_caps.h>

#include "WifiSetup.h"
#include "modules/store/StoreClient.h"
#endif
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
#if GAMES_ANY
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
    logger::info("STORE UPDATE 1.0");
    mascot::begin();

    display::begin();
    display::setTheme(config::theme());
    display::applyRotation(config::rotation());
    M5Cardputer.Display.setBrightness(config::brightness());

    navigation::begin();
    powertimer::begin();
    display::splash();
    diagconsole::begin();

#ifdef ORBIT_DIAG_BOOTGAME
    {
        // Percorso reale, senza menu: WiFi dello Store acceso e primo gioco su
        // SD. Serve a misurare heap/picco con gli stessi 61 KB di dati statici
        // del firmware di produzione.
        diag::log("PROBE boot free=%u blk=%u", static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
        const bool wifi = wifisetup::ensureConnected(false, "");
        diag::log("PROBE wifi=%d status=%d free=%u", wifi ? 1 : 0,
                  static_cast<int>(WiFi.status()), static_cast<unsigned>(ESP.getFreeHeap()));
        const std::vector<std::string> files = storage::list("/games", false, true);
        // Stato vivo del menu Store al momento del lancio: il catalogo resta in
        // memoria mentre il gioco gira (come nel percorso vero).
        std::vector<store::GameInfo> catalog;
        store::fetchCatalog(catalog, "Store");
        // Stato vivo del menu Store: oltre al catalogo, le voci con le lambda
        // che copiano GameInfo restano allocate mentre il gioco gira.
        MenuManager menu("Store", "Menu principale");
        {
            std::vector<MenuItem> items;
            for (const store::GameInfo& game : catalog) {
                items.emplace_back(game.name, "store", [game] { (void)game; });
            }
            menu.setItems(std::move(items));
        }
        menu.addItem("Giochi installati", "folder", [] {});
        diag::log("PROBE catalog=%u free=%u min=%u",
                  static_cast<unsigned>(catalog.size()),
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getMinFreeHeap()));
        if (files.empty()) {
            diag::log("PROBE no games on SD");
        }
        for (const std::string& file : files) {
            const std::string path = std::string("/games/") + file;
            diag::log("PROBE game=%s free=%u blk=%u", path.c_str(),
                      static_cast<unsigned>(ESP.getFreeHeap()),
                      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
            const bool ok = store::runGame(path);
            diag::log("PROBE runGame=%d free=%u min=%u", ok ? 1 : 0,
                      static_cast<unsigned>(ESP.getFreeHeap()),
                      static_cast<unsigned>(ESP.getMinFreeHeap()));
        }
    }
#endif
#ifdef ORBIT_DIAG_AUTOGAME
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
    // Framebuffer come nel firmware reale: release/restore del gioco misurano
    // gli stessi 63 KB che l'app vera libera.
    display::begin();
    diag::log("display ready heap=%u", static_cast<unsigned>(ESP.getFreeHeap()));
    logger::info("DIAG READY: heap=%u, 'r'=2048 'w'=wifi+2048 's'=primo gioco SD",
                 static_cast<unsigned>(ESP.getFreeHeap()));
    // Loop interattivo: heartbeat ogni 2 s (prova che l'app gira e la seriale
    // funziona) e comandi per lanciare il gioco sotto log completo.
    for (;;) {
        if (Serial.available() > 0) {
            const char c = static_cast<char>(Serial.read());
            if (c == 'r' || c == 'w' || c == 's') {
                std::string path = "/diag.wren";
                if (c == 'w') {
                    // Riproduce lo stato reale: WiFi dello Store acceso al
                    // momento del lancio, poi spento da runGame.
                    const bool wifi = wifisetup::ensureConnected(false, "");
                    diag::log("wifi=%d status=%d heap=%u", wifi ? 1 : 0,
                              static_cast<int>(WiFi.status()),
                              static_cast<unsigned>(ESP.getFreeHeap()));
                }
                if (c == 's') {
                    const std::vector<std::string> files =
                        storage::list("/games", false, true);
                    if (!files.empty()) {
                        path = std::string("/games/") + files.front();
                    }
                    diag::log("sd games=%u path=%s", static_cast<unsigned>(files.size()),
                              path.c_str());
                }
                diag::log("DIAG start game heap=%u", static_cast<unsigned>(ESP.getFreeHeap()));
                const bool ok = store::runGame(path);
                diag::log("DIAG runGame=%d heap=%u min=%u", ok ? 1 : 0,
                          static_cast<unsigned>(ESP.getFreeHeap()),
                          static_cast<unsigned>(ESP.getMinFreeHeap()));
            }
            continue;
        }
        static uint32_t lastHb = 0;
        if (millis() - lastHb > 2000) {
            lastHb = millis();
            Serial.printf("DIAG hb heap=%u min=%u\n",
                          static_cast<unsigned>(ESP.getFreeHeap()),
                          static_cast<unsigned>(ESP.getMinFreeHeap()));
        }
        delay(20);
    }
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
