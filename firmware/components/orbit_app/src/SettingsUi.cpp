#include "SettingsUi.h"

#include <cstdio>
#include <string>
#include <vector>

#include "Display.h"
#include "InputUi.h"
#include "MenuManager.h"
#include "SdUi.h"
#include "StoreUi.h"
#include "Ui.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "orbit_hal/display.h"
#include "orbit_settings/Settings.h"
#include "orbit_store/Store.h"

namespace orbit_app {
namespace {

void themeMenu() {
    MenuManager menu("Tema", "Impostazioni");
    for (uint8_t i = 0; i < display::kThemeCount; ++i) {
        const bool current = display::themeId() == i;
        menu.addItem(std::string(display::themeName(i)) + (current ? " *" : ""),
                     "settings", [i] {
                         display::setTheme(i);
                         orbit_settings::setTheme(i);
                     });
    }
    menu.setHint("ENTER=applica  `=indietro");
    menu.run(true);
}

void brightnessMenu() {
    struct Level {
        uint8_t value;
        const char* label;
    };
    constexpr Level kLevels[] = {{64, "25%"}, {128, "50%"}, {192, "75%"}, {255, "100%"}};
    MenuManager menu("Luminosita'", "Impostazioni");
    for (const Level& level : kLevels) {
        const bool current = orbit_settings::brightness() == level.value;
        menu.addItem(std::string(level.label) + (current ? " *" : ""), "settings",
                     [value = level.value] {
                         orbit_hal::displayBrightness(value);
                         orbit_settings::setBrightness(value);
                     });
    }
    menu.setHint("ENTER=applica  `=indietro");
    menu.run(true);
}

void rotationMenu() {
    constexpr const char* kLabels[] = {"0 gradi", "90 gradi", "180 gradi", "270 gradi"};
    MenuManager menu("Rotazione", "Impostazioni");
    const uint8_t current = orbit_settings::rotation();
    for (uint8_t i = 0; i < 4; ++i) {
        menu.addItem(std::string(kLabels[i]) + (current == i ? " *" : ""), "rotate",
                     [i] {
                         display::applyRotation(i);
                         if (display::rotation() == i) {
                             orbit_settings::setRotation(i);
                         }
                     });
    }
    menu.setHint("ENTER=applica  `=indietro");
    menu.run(true);
}

void nicknameMenu() {
    std::string nick = orbit_settings::nickname();
    if (!textInput("Nickname chat", "Impostazioni", nick, 6)) {
        return;
    }
    if (nick.empty()) {
        return;
    }
    orbit_settings::setNickname(nick);
    ui::message("Nickname chat",
                {"Salvato: " + orbit_settings::nickname(), "Max 6 caratteri."},
                "Impostazioni");
}

void storageInfo() {
    if (!orbit_store::mountSd()) {
        ui::message("Storage", {"!microSD non montata.", "Inserisci la card."},
                    "Impostazioni");
        return;
    }
    uint64_t total = 0;
    uint64_t freeBytes = 0;
    esp_vfs_fat_info("/sd", &total, &freeBytes);
    char line1[64];
    char line2[64];
    std::snprintf(line1, sizeof(line1), "Liberi: %llu MB",
                  static_cast<unsigned long long>(freeBytes / (1024 * 1024)));
    std::snprintf(line2, sizeof(line2), "Totali: %llu MB",
                  static_cast<unsigned long long>(total / (1024 * 1024)));
    ui::message("Storage", {"microSD", line1, line2}, "Impostazioni");
}

void legalNotice() {
    ui::message("Avviso legale",
                {"!Solo reti e dispositivi propri.",
                 "Deauth, beacon spam, evil twin,",
                 "BLE spam e jammer IR sono reati",
                 "se usati su terzi o reti altrui.",
                 "Ambiente RF isolato e autorizzato.",
                 "Nessuna garanzia. Uso a tuo rischio.",
                 ">Leggi il README del progetto."},
                "Impostazioni");
}

void info() {
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    uint32_t flashSize = 0;
    esp_flash_get_size(nullptr, &flashSize);
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char lineIdf[64];
    char lineHeap[64];
    char lineFlash[64];
    char lineMac[64];
    std::snprintf(lineIdf, sizeof(lineIdf), "IDF %s", esp_get_idf_version());
    std::snprintf(lineHeap, sizeof(lineHeap), "Heap: %u KB",
                  static_cast<unsigned>(esp_get_free_heap_size() / 1024));
    std::snprintf(lineFlash, sizeof(lineFlash), "Flash: %u MB",
                  static_cast<unsigned>(flashSize / (1024 * 1024)));
    std::snprintf(lineMac, sizeof(lineMac), "MAC %02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ui::message("Info", {"OrbitOS 3.0", lineIdf, lineHeap, lineFlash, lineMac},
                "Impostazioni");
}

}  // namespace

void runSettings() {
    MenuManager menu("Impostazioni", "Home");
    menu.addItem("WiFi", "wifi", [] { runWifiSetup(); });
    menu.addItem("Nickname chat", "chat", [] { nicknameMenu(); });
    menu.addItem("Tema", "settings", [] { themeMenu(); });
    menu.addItem("Luminosita'", "settings", [] { brightnessMenu(); });
    menu.addItem("Rotazione", "rotate", [] { rotationMenu(); });
    menu.addItem("Storage", "folder", [] { storageInfo(); });
    menu.addItem("Scheda SD", "folder", [] { runSdMenu(); });
    menu.addItem("Avviso legale", "warn", [] { legalNotice(); });
    menu.addItem("Info", "chip", [] { info(); });
    menu.addItem("Aggiornamenti", "rotate", [] {
        ui::message("Aggiornamenti", {"OTA firmware in M4."}, "Impostazioni");
    });
    menu.setHint("ENTER=apri  `=indietro");
    menu.run(true);
}

}  // namespace orbit_app
