#include "orbit_store/Store.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <esp_err.h>
#include <esp_littlefs.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <driver/sdspi_host.h>
#include <driver/spi_common.h>

namespace orbit_store {
namespace {

constexpr const char* kTag = "store-sd";
constexpr int kPinCs = 12;
constexpr int kPinMosi = 14;
constexpr int kPinSck = 40;
constexpr int kPinMiso = 39;

bool s_mounted = false;
sdmmc_card_t* s_card = nullptr;

}  // namespace

bool mountSd() {
    if (s_mounted) {
        return true;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount = {};
    mount.format_if_mount_failed = false;
    mount.max_files = 6;
    mount.allocation_unit_size = 16 * 1024;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    spi_bus_config_t bus = {};
    bus.mosi_io_num = kPinMosi;
    bus.miso_io_num = kPinMiso;
    bus.sclk_io_num = kPinSck;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4000;
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(kTag, "SPI2 non inizializzabile");
        return false;
    }

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = static_cast<gpio_num_t>(kPinCs);
    slot.host_id = SPI2_HOST;

    const esp_err_t err =
        esp_vfs_fat_sdspi_mount("/sd", &host, &slot, &mount, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "microSD non montata: %s", esp_err_to_name(err));
        return false;
    }
    s_mounted = true;
    ESP_LOGI(kTag, "microSD montata");
    return true;
}

bool mountLittleFs() {
    static bool mounted = false;
    if (mounted) {
        return true;
    }
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = "/littlefs";
    conf.partition_label = "spiffs";
    // Mai formattare in automatico: la LittleFS contiene i dati 2.x.
    // La vecchia immagine e' salvata in backup/2026-11-26-pre-idf/littlefs.bin.
    conf.format_if_mount_failed = false;
    conf.dont_mount = false;
    const esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "LittleFS non montata: %s", esp_err_to_name(err));
        return false;
    }
    mkdir(kGamesDir, 0777);
    mounted = true;
    ESP_LOGI(kTag, "LittleFS montata");
    return true;
}

std::string gamePath(const std::string& id) {
    return std::string(kGamesDir) + "/" + id + ".wren";
}

bool gameInstalled(const GameInfo& game) {
    const std::string path = gamePath(game.id);
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0) {
        return false;
    }
    return game.size == 0 || static_cast<uint32_t>(st.st_size) == game.size;
}

std::vector<std::string> installedGames() {
    std::vector<std::string> names;
    DIR* dir = opendir(kGamesDir);
    if (dir == nullptr) {
        return names;
    }
    for (dirent* entry = readdir(dir); entry != nullptr; entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.size() <= 5 || name.compare(name.size() - 5, 5, ".wren") != 0) {
            continue;
        }
        names.push_back(name);
    }
    closedir(dir);
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace orbit_store
