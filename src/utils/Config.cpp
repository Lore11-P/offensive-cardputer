#include "Config.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <algorithm>

#include "Logger.h"
#include "Storage.h"

namespace config {
namespace {
Preferences g_prefs;
Settings g_settings;
bool g_ready = false;
constexpr const char* kNamespace = "cardputer-fw";
constexpr uint32_t kConfigMagic = 0x43504631;  // "CPF1"

uint8_t clampBrightness(int value) {
    return static_cast<uint8_t>(std::max(10, std::min(255, value)));
}

uint8_t clampRotation(int value) {
    return static_cast<uint8_t>(std::max(0, std::min(3, value)));
}

uint16_t clampU16(int value, int lo, int hi) {
    return static_cast<uint16_t>(std::max(lo, std::min(hi, value)));
}

bool bitFor(Module module, uint16_t& mask) {
    if (module >= Module::Count) {
        return false;
    }
    mask = static_cast<uint16_t>(1u << static_cast<uint8_t>(module));
    return true;
}

void applyDefaults() {
    g_settings = Settings();
}

void refreshCompiledModules() {
    // Un modulo non compilato non deve mai risultare attivo a runtime.
    const Module modules[] = {Module::Ble,   Module::Wifi, Module::Ir,  Module::Games,
                              Module::Settings, Module::Sd, Module::Radar, Module::Radio};
    for (Module m : modules) {
        if (!moduleCompiled(m)) {
            uint16_t bit = 0;
            if (bitFor(m, bit)) {
                g_settings.modules &= static_cast<uint16_t>(~bit);
            }
        }
    }
}
}  // namespace

void begin() {
    applyDefaults();
    g_ready = g_prefs.begin(kNamespace, false);
    if (!g_ready) {
        logger::error("NVS non disponibile: impostazioni non persistenti");
        refreshCompiledModules();
        return;
    }

    if (g_prefs.getUInt("magic", 0) != kConfigMagic) {
        // Primo avvio: prova a importare /config.json, poi salva i default.
        g_prefs.putUInt("magic", kConfigMagic);
        if (storage::exists("/config.json")) {
            importFromFile();
        }
        refreshCompiledModules();
        save();
    } else {
        g_settings.brightness = clampBrightness(g_prefs.getUChar("bright", 128));
        g_settings.rotation = clampRotation(g_prefs.getUChar("rot", 1));
        g_settings.modules = g_prefs.getUShort("mods", 0xFFFF);
        g_settings.bleDurationSec =
            clampU16(g_prefs.getUShort("ble_dur", 30), 5, 300);
        g_settings.bleIntervalMs =
            clampU16(g_prefs.getUShort("ble_itv", 100), 20, 1000);
        g_settings.irRepeat = static_cast<uint8_t>(
            std::max(0, std::min(5, static_cast<int>(g_prefs.getUChar("ir_rep", 2)))));
        g_settings.irTvbgDelayMs =
            clampU16(g_prefs.getUShort("ir_tvd", 120), 20, 2000);
        g_settings.sleepTimeoutSec = g_prefs.getUShort("sleep_s", 120);
        g_settings.radarSensitivity =
            static_cast<uint8_t>(std::max(1, std::min(5, static_cast<int>(g_prefs.getUChar("radar_s", 3)))));
        refreshCompiledModules();
    }
}

const Settings& get() {
    return g_settings;
}

void save() {
    if (!g_ready) {
        return;
    }
    g_prefs.putUChar("bright", g_settings.brightness);
    g_prefs.putUChar("rot", g_settings.rotation);
    g_prefs.putUShort("mods", g_settings.modules);
    g_prefs.putUShort("ble_dur", g_settings.bleDurationSec);
    g_prefs.putUShort("ble_itv", g_settings.bleIntervalMs);
    g_prefs.putUChar("ir_rep", g_settings.irRepeat);
    g_prefs.putUShort("ir_tvd", g_settings.irTvbgDelayMs);
    g_prefs.putUShort("sleep_s", g_settings.sleepTimeoutSec);
    g_prefs.putUChar("radar_s", g_settings.radarSensitivity);
    g_prefs.putUInt("magic", kConfigMagic);
}

void reset() {
    applyDefaults();
    refreshCompiledModules();
    if (g_ready) {
        g_prefs.clear();
        g_prefs.putUInt("magic", kConfigMagic);
        save();
    }
}

uint8_t brightness() {
    return g_settings.brightness;
}

void setBrightness(uint8_t value) {
    g_settings.brightness = clampBrightness(value);
    save();
}

uint8_t rotation() {
    return g_settings.rotation;
}

void setRotation(uint8_t index) {
    g_settings.rotation = clampRotation(index);
    save();
}

bool moduleCompiled(Module module) {
    switch (module) {
        case Module::Ble:
            return ENABLE_MODULE_BLE;
        case Module::Wifi:
            return ENABLE_MODULE_WIFI;
        case Module::Ir:
            return ENABLE_MODULE_IR;
        case Module::Games:
            return ENABLE_MODULE_GAMES;
        case Module::Settings:
            return ENABLE_MODULE_SETTINGS;
        case Module::Sd:
            return ENABLE_MODULE_SD;
        case Module::Radar:
            return ENABLE_MODULE_RADAR;
        case Module::Radio:
            return ENABLE_MODULE_RADIO;
        default:
            return false;
    }
}

bool moduleEnabled(Module module) {
    if (!moduleCompiled(module)) {
        return false;
    }
    uint16_t bit = 0;
    return bitFor(module, bit) && (g_settings.modules & bit);
}

void setModuleEnabled(Module module, bool enabled) {
    uint16_t bit = 0;
    if (!bitFor(module, bit) || !moduleCompiled(module)) {
        return;
    }
    if (enabled) {
        g_settings.modules |= bit;
    } else {
        g_settings.modules &= static_cast<uint16_t>(~bit);
    }
    save();
}

uint16_t bleDurationSec() {
    return g_settings.bleDurationSec;
}

void setBleDurationSec(uint16_t seconds) {
    g_settings.bleDurationSec = clampU16(seconds, 5, 300);
    save();
}

uint16_t bleIntervalMs() {
    return g_settings.bleIntervalMs;
}

void setBleIntervalMs(uint16_t ms) {
    g_settings.bleIntervalMs = clampU16(ms, 20, 1000);
    save();
}

uint8_t irRepeat() {
    return g_settings.irRepeat;
}

void setIrRepeat(uint8_t repeats) {
    g_settings.irRepeat = static_cast<uint8_t>(std::max(0, std::min(5, static_cast<int>(repeats))));
    save();
}

uint16_t irTvbgDelayMs() {
    return g_settings.irTvbgDelayMs;
}

void setIrTvbgDelayMs(uint16_t ms) {
    g_settings.irTvbgDelayMs = clampU16(ms, 20, 2000);
    save();
}

uint16_t sleepTimeoutSec() {
    return g_settings.sleepTimeoutSec;
}

void setSleepTimeoutSec(uint16_t seconds) {
    // 0 = disattivato; gli altri valori sono quelli proposti in Impostazioni.
    static const uint16_t kAllowed[] = {0, 30, 60, 120, 300, 600, 900, 1800};
    uint16_t chosen = 0;
    for (uint16_t value : kAllowed) {
        if (seconds <= value) {
            chosen = value;
            break;
        }
    }
    g_settings.sleepTimeoutSec = chosen;
    save();
}

uint8_t radarSensitivity() {
    return g_settings.radarSensitivity;
}

void setRadarSensitivity(uint8_t level) {
    g_settings.radarSensitivity = static_cast<uint8_t>(std::max(1, std::min(5, static_cast<int>(level))));
    save();
}

std::string toJson() {
    JsonDocument doc;
    doc["version"] = 1;
    doc["brightness"] = g_settings.brightness;
    doc["rotation"] = g_settings.rotation;
    doc["sleep_timeout_sec"] = g_settings.sleepTimeoutSec;

    JsonObject modules = doc["modules"].to<JsonObject>();
    modules["ble"] = moduleEnabled(Module::Ble);
    modules["wifi"] = moduleEnabled(Module::Wifi);
    modules["ir"] = moduleEnabled(Module::Ir);
    modules["games"] = moduleEnabled(Module::Games);
    modules["settings"] = moduleEnabled(Module::Settings);
    modules["sd"] = moduleEnabled(Module::Sd);
    modules["radar"] = moduleEnabled(Module::Radar);
    modules["radio"] = moduleEnabled(Module::Radio);

    JsonObject ble = doc["ble"].to<JsonObject>();
    ble["duration_sec"] = g_settings.bleDurationSec;
    ble["interval_ms"] = g_settings.bleIntervalMs;

    JsonObject ir = doc["ir"].to<JsonObject>();
    ir["repeat"] = g_settings.irRepeat;
    ir["tvbgone_delay_ms"] = g_settings.irTvbgDelayMs;

    JsonObject radarCfg = doc["radar"].to<JsonObject>();
    radarCfg["sensitivity"] = g_settings.radarSensitivity;

    std::string out;
    out.reserve(384);
    serializeJson(doc, out);
    return out;
}

bool applyJson(const char* json, size_t len) {
    if (!json || len == 0) {
        return false;
    }
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, json, len);
    if (err) {
        logger::warn("Config JSON non valido: %s", err.c_str());
        return false;
    }

    g_settings.brightness = clampBrightness(doc["brightness"] | g_settings.brightness);
    g_settings.rotation = clampRotation(doc["rotation"] | g_settings.rotation);
    g_settings.sleepTimeoutSec = static_cast<uint16_t>(
        doc["sleep_timeout_sec"] | g_settings.sleepTimeoutSec);

    if (doc["modules"].is<JsonObject>()) {
        JsonObject modules = doc["modules"].as<JsonObject>();
        if (modules["ble"].is<bool>()) setModuleEnabled(Module::Ble, modules["ble"].as<bool>());
        if (modules["wifi"].is<bool>()) setModuleEnabled(Module::Wifi, modules["wifi"].as<bool>());
        if (modules["ir"].is<bool>()) setModuleEnabled(Module::Ir, modules["ir"].as<bool>());
        if (modules["games"].is<bool>()) setModuleEnabled(Module::Games, modules["games"].as<bool>());
        if (modules["sd"].is<bool>()) setModuleEnabled(Module::Sd, modules["sd"].as<bool>());
        if (modules["radar"].is<bool>()) setModuleEnabled(Module::Radar, modules["radar"].as<bool>());
        if (modules["radio"].is<bool>()) setModuleEnabled(Module::Radio, modules["radio"].as<bool>());
    }

    if (doc["ble"].is<JsonObject>()) {
        g_settings.bleDurationSec = clampU16(doc["ble"]["duration_sec"] | g_settings.bleDurationSec, 5, 300);
        g_settings.bleIntervalMs = clampU16(doc["ble"]["interval_ms"] | g_settings.bleIntervalMs, 20, 1000);
    }
    if (doc["ir"].is<JsonObject>()) {
        g_settings.irRepeat = static_cast<uint8_t>(
            std::max(0, std::min(5, doc["ir"]["repeat"] | static_cast<int>(g_settings.irRepeat))));
        g_settings.irTvbgDelayMs =
            clampU16(doc["ir"]["tvbgone_delay_ms"] | g_settings.irTvbgDelayMs, 20, 2000);
    }
    if (doc["radar"].is<JsonObject>()) {
        g_settings.radarSensitivity = static_cast<uint8_t>(std::max(
            1, std::min(5, static_cast<int>(doc["radar"]["sensitivity"] | g_settings.radarSensitivity))));
    }

    refreshCompiledModules();
    save();
    return true;
}

bool exportToFile(const char* path) {
    if (!storage::hasLittleFS()) {
        return false;
    }
    File f = LittleFS.open(path, FILE_WRITE);
    if (!f) {
        return false;
    }
    const std::string json = toJson();
    const size_t written = f.write(reinterpret_cast<const uint8_t*>(json.data()), json.size());
    f.close();
    return written == json.size();
}

bool importFromFile(const char* path) {
    std::string json;
    if (!storage::readText(path, json, 4096) || json.empty()) {
        return false;
    }
    return applyJson(json.c_str(), json.size());
}

}  // namespace config
