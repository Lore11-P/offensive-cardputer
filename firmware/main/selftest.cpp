// Auto-test seriale per il bring-up su hardware reale.
// Attivare con -DORBIT_DIAG_SELFTEST=ON; non entra nel firmware di produzione.
#ifdef ORBIT_DIAG_SELFTEST

#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>

#include <esp_chip_info.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_flash.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <nvs.h>

#include "Display.h"
#include "Navigation.h"
#include "orbit_compat.h"
#include "orbit_audio/Audio.h"
#include "orbit_ble/Ble.h"
#include "orbit_hal/battery.h"
#include "orbit_hal/display.h"
#include "orbit_hal/keyboard.h"
#include "orbit_settings/Settings.h"
#include "orbit_store/Store.h"
#include "orbit_wifi/Wifi.h"

namespace {

constexpr const char* kTag = "selftest";

void result(const char* name, bool ok, const char* detail = "") {
    ESP_LOGI(kTag, "RESULT %-18s %s %s", name, ok ? "PASS" : "FAIL", detail);
}

void findAudio(const std::string& path, int depth, std::string& out) {
    if (!out.empty() || depth < 0) {
        return;
    }
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
        return;
    }
    for (dirent* entry = readdir(dir); entry != nullptr; entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        if (entry->d_type == DT_DIR) {
            findAudio(path + "/" + name, depth - 1, out);
        } else {
            const size_t dot = name.rfind('.');
            if (dot != std::string::npos) {
                std::string ext = name.substr(dot);
                for (char& c : ext) {
                    c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                }
                if (ext == ".mp3" || ext == ".wav") {
                    out = path + "/" + name;
                }
            }
        }
        if (!out.empty()) {
            break;
        }
    }
    closedir(dir);
}

volatile bool g_audioStop = false;
volatile bool g_audioDone = false;
bool g_audioResult = false;
uint32_t g_audioPosition = 0;

void audioTestTask(void* arg) {
    const std::string* path = static_cast<const std::string*>(arg);
    g_audioResult = orbit_audio::playFile(*path, &g_audioStop);
    g_audioPosition = orbit_audio::positionMs();
    g_audioDone = true;
    vTaskDelete(nullptr);
}

int countFiles(const char* path, int maxDepth) {
    DIR* dir = opendir(path);
    if (dir == nullptr) {
        return -1;
    }
    int count = 0;
    for (dirent* entry = readdir(dir); entry != nullptr; entry = readdir(dir)) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (entry->d_type == DT_DIR && maxDepth > 0) {
            char child[384];
            std::snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
            const int sub = countFiles(child, maxDepth - 1);
            if (sub > 0) {
                count += sub;
            }
        } else if (entry->d_type == DT_REG) {
            ++count;
        }
    }
    closedir(dir);
    return count;
}

}  // namespace

void orbitSelfTest() {
    ESP_LOGI(kTag, "===== ORBIT SELF TEST BEGIN =====");
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    uint32_t flashSize = 0;
    esp_flash_get_size(nullptr, &flashSize);
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(kTag, "chip=%d cores=%d flash=%uMB heap=%uKB mac=%02X:%02X:%02X:%02X:%02X:%02X",
             chip.model, chip.cores, static_cast<unsigned>(flashSize / (1024 * 1024)),
             static_cast<unsigned>(esp_get_free_heap_size() / 1024), mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(kTag, "idf=%s", esp_get_idf_version());

    result("display", display::width() == 240 && display::height() == 135);
    result("framebuffer", display::framebufferActive());
    char kbd[128] = {};
    orbit_hal::keyboardDebug(kbd, sizeof(kbd));
    ESP_LOGI(kTag, "keyboard: %s", kbd);
    result("codec-es8311", orbit_hal::i2cPresent(0x18), "I2C 0x18");
    ESP_LOGI(kTag, "battery=%dmV %d%%", orbit_hal::batteryMillivolts(),
             orbit_hal::batteryPercent());

    nvs_handle_t handle = 0;
    const bool nvsOk = nvs_open("orbit-cfg", NVS_READONLY, &handle) == ESP_OK;
    if (nvsOk) {
        nvs_close(handle);
    }
    result("nvs", nvsOk);

    const bool sd = orbit_store::mountSd();
    result("sd", sd);
    if (sd) {
        const int music = countFiles("/sd/data/Music", 2);
        const int ir = countFiles("/sd/ir", 2);
        const int games = countFiles("/sd/games", 1);
        char detail[96];
        std::snprintf(detail, sizeof(detail), "music=%d ir=%d games=%d", music, ir, games);
        result("sd-content", true, detail);
    }

    const bool lfs = orbit_store::mountLittleFs();
    result("littlefs", lfs);
    if (lfs) {
        const int games = countFiles("/littlefs/games", 1);
        char detail[64];
        std::snprintf(detail, sizeof(detail), "games=%d", games);
        result("lfs-content", true, detail);
    }

    const bool audioOk = orbit_audio::begin();
    result("audio-begin", audioOk);
    if (audioOk) {
        std::string track;
        findAudio("/sd/data/Music", 2, track);
        if (track.empty()) {
            findAudio("/sd/music", 2, track);
        }
        if (!track.empty()) {
            ESP_LOGI(kTag, "audio file: %s", track.c_str());
            g_audioStop = false;
            g_audioDone = false;
            TaskHandle_t task = nullptr;
            xTaskCreate(audioTestTask, "selftest-audio", 32768, &track, 5, &task);
            if (task != nullptr) {
                delay(1500);
                g_audioStop = true;
                for (int i = 0; i < 30 && !g_audioDone; ++i) {
                    delay(100);
                }
                char detail[48];
                std::snprintf(detail, sizeof(detail), "pos=%ums stop=%d",
                              static_cast<unsigned>(g_audioPosition),
                              g_audioResult ? 1 : 0);
                result("audio-play", g_audioPosition > 500, detail);
            }
        } else {
            ESP_LOGW(kTag, "nessun mp3/wav in /sd/data/Music");
        }
        orbit_audio::end();
    }
    result("wifi-scan+raw", orbit_wifi::selfTest());
    result("ble-scan", orbit_ble::selfTest());
    ESP_LOGI(kTag, "heap after radios=%uKB",
             static_cast<unsigned>(esp_get_free_heap_size() / 1024));

    ESP_LOGI(kTag, "settings bright=%u theme=%u rot=%u nick=%s",
             static_cast<unsigned>(orbit_settings::brightness()),
             static_cast<unsigned>(orbit_settings::theme()),
             static_cast<unsigned>(orbit_settings::rotation()),
             orbit_settings::nickname().c_str());
    ESP_LOGI(kTag, "heap after mounts=%uKB", static_cast<unsigned>(esp_get_free_heap_size() / 1024));
    ESP_LOGI(kTag, "===== ORBIT SELF TEST END =====");
}

#endif  // ORBIT_DIAG_SELFTEST
