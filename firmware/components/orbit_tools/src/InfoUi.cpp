#include <cstdio>
#include <string>
#include <vector>

#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_mac.h>
#include <esp_system.h>

#include "ToolsInternal.h"
#include "Ui.h"

namespace orbit_tools {
namespace {

const char* chipModel(esp_chip_model_t model) {
    switch (model) {
        case CHIP_ESP32:
            return "ESP32";
        case CHIP_ESP32S2:
            return "ESP32-S2";
        case CHIP_ESP32S3:
            return "ESP32-S3";
        case CHIP_ESP32C3:
            return "ESP32-C3";
        case CHIP_ESP32C2:
            return "ESP32-C2";
        case CHIP_ESP32C6:
            return "ESP32-C6";
        case CHIP_ESP32H2:
            return "ESP32-H2";
        case CHIP_ESP32P4:
            return "ESP32-P4";
        case CHIP_ESP32C61:
            return "ESP32-C61";
        case CHIP_ESP32C5:
            return "ESP32-C5";
        case CHIP_POSIX_LINUX:
            return "POSIX";
        default:
            return "ESP";
    }
}

}  // namespace

void infoScreen() {
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);

    uint32_t flashSize = 0;
    esp_flash_get_size(nullptr, &flashSize);

    uint8_t mac[6] = {};

    char lineChip[64];
    char lineIdf[64];
    char lineHeap[64];
    char lineFlash[64];
    char lineMac[64];
    std::snprintf(lineChip, sizeof(lineChip), "Chip %s x%u r%u.%u", chipModel(chip.model),
                  static_cast<unsigned>(chip.cores), static_cast<unsigned>(chip.revision / 100),
                  static_cast<unsigned>(chip.revision % 100));
    std::snprintf(lineIdf, sizeof(lineIdf), "IDF %s", esp_get_idf_version());
    std::snprintf(lineHeap, sizeof(lineHeap), "Heap %u KB (min %u KB)",
                  static_cast<unsigned>(esp_get_free_heap_size() / 1024),
                  static_cast<unsigned>(esp_get_minimum_free_heap_size() / 1024));
    if (flashSize >= 1024 * 1024) {
        std::snprintf(lineFlash, sizeof(lineFlash), "Flash %u MB",
                      static_cast<unsigned>(flashSize / (1024 * 1024)));
    } else if (flashSize >= 1024) {
        std::snprintf(lineFlash, sizeof(lineFlash), "Flash %u KB",
                      static_cast<unsigned>(flashSize / 1024));
    } else {
        std::snprintf(lineFlash, sizeof(lineFlash), "Flash non disponibile");
    }
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        std::snprintf(lineMac, sizeof(lineMac), "MAC %02X:%02X:%02X:%02X:%02X:%02X", mac[0],
                      mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        std::snprintf(lineMac, sizeof(lineMac), "MAC non disponibile");
    }

    ui::message("Info",
                {"OrbitOS 3.0", lineChip, lineIdf, lineHeap, lineFlash, lineMac}, "Strumenti");
}

}  // namespace orbit_tools
