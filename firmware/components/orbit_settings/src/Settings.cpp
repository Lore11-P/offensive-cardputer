#include "orbit_settings/Settings.h"

#include <cctype>
#include <string>

#include <esp_log.h>
#include <nvs.h>

namespace orbit_settings {
namespace {

constexpr const char* kTag = "settings";
constexpr const char* kNamespace = "orbit-cfg";
constexpr uint8_t kDefaultBrightness = 128;
constexpr uint8_t kDefaultTheme = 0;
constexpr uint8_t kDefaultRotation = 1;
constexpr const char* kDefaultNickname = "ORBIT";

uint8_t s_brightness = kDefaultBrightness;
uint8_t s_theme = kDefaultTheme;
uint8_t s_rotation = kDefaultRotation;
std::string s_nickname = kDefaultNickname;

void save() {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(kTag, "NVS non disponibile");
        return;
    }
    nvs_set_u8(handle, "bright", s_brightness);
    nvs_set_u8(handle, "theme", s_theme);
    nvs_set_u8(handle, "rot", s_rotation);
    nvs_set_str(handle, "nick", s_nickname.c_str());
    nvs_commit(handle);
    nvs_close(handle);
}

}  // namespace

void begin() {
    // Namespace nuovo di OrbitOS 3.0.
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) == ESP_OK) {
        uint8_t value = 0;
        if (nvs_get_u8(handle, "bright", &value) == ESP_OK) {
            s_brightness = value;
        }
        if (nvs_get_u8(handle, "theme", &value) == ESP_OK && value < 16) {
            s_theme = value;
        }
        if (nvs_get_u8(handle, "rot", &value) == ESP_OK && value < 4) {
            s_rotation = value;
        }
        char nick[16] = {};
        size_t nickLen = sizeof(nick);
        if (nvs_get_str(handle, "nick", nick, &nickLen) == ESP_OK && nick[0] != '\0') {
            s_nickname = nick;
        }
        nvs_close(handle);
        return;
    }

    // Migrazione dal 2.x: namespace "cardputer-fw", magic "CPF1".
    nvs_handle_t legacy = 0;
    if (nvs_open("cardputer-fw", NVS_READONLY, &legacy) != ESP_OK) {
        return;
    }
    uint32_t magic = 0;
    if (nvs_get_u32(legacy, "magic", &magic) == ESP_OK && magic == 0x43504631) {
        uint8_t value = 0;
        if (nvs_get_u8(legacy, "bright", &value) == ESP_OK) {
            s_brightness = value;
        }
        if (nvs_get_u8(legacy, "theme", &value) == ESP_OK && value < 16) {
            s_theme = value;
        }
        if (nvs_get_u8(legacy, "rot", &value) == ESP_OK && value < 4) {
            s_rotation = value;
        }
        char nick[16] = {};
        size_t nickLen = sizeof(nick);
        if (nvs_get_str(legacy, "chat_nick", nick, &nickLen) == ESP_OK && nick[0] != '\0') {
            s_nickname = nick;
        }
        ESP_LOGI(kTag, "impostazioni importate dal firmware 2.x");
        save();
    }
    nvs_close(legacy);
}

uint8_t brightness() {
    return s_brightness;
}

uint8_t theme() {
    return s_theme;
}

uint8_t rotation() {
    return s_rotation;
}

std::string nickname() {
    return s_nickname;
}

void setBrightness(uint8_t value) {
    s_brightness = value;
    save();
}

void setTheme(uint8_t id) {
    if (id >= 16) {
        return;
    }
    s_theme = id;
    save();
}

void setRotation(uint8_t index) {
    if (index >= 4) {
        return;
    }
    s_rotation = index;
    save();
}

void setNickname(const std::string& value) {
    std::string clean;
    for (char c : value) {
        if (c >= 0x20 && c < 0x7F && clean.size() < 6) {
            clean.push_back(c);
        }
    }
    while (!clean.empty() && clean.back() == ' ') {
        clean.pop_back();
    }
    if (clean.empty()) {
        return;
    }
    s_nickname = clean;
    save();
}

}  // namespace orbit_settings
