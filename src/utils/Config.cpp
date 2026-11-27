#include "Config.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <algorithm>
#include <cstdio>

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

uint16_t snapSleepTimeout(uint16_t seconds) {
    // 0 = disattivato; gli altri valori sono quelli proposti in Impostazioni.
    static const uint16_t kAllowed[] = {0, 30, 60, 120, 300, 600, 900, 1800};
    if (seconds > 1800) {
        seconds = 1800;  // fuori scala: massimo, mai 0
    }
    for (uint16_t value : kAllowed) {
        if (seconds <= value) {
            return value;
        }
    }
    return 1800;
}

bool bitFor(Module module, uint16_t& mask) {
    if (module >= Module::Count) {
        return false;
    }
    mask = static_cast<uint16_t>(1u << static_cast<uint8_t>(module));
    return true;
}

std::string defaultChatNickname() {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "CP%04X",
                  static_cast<unsigned>(ESP.getEfuseMac() & 0xFFFF));
    return buf;
}

std::string sanitizeChatName(const std::string& value,
                             const std::string& fallback) {
    std::string out;
    out.reserve(kChatNicknameMax);
    for (char c : value) {
        if (out.size() >= kChatNicknameMax) {
            break;
        }
        if (c >= 0x20 && c < 0x7F) {
            out.push_back(c);
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out.empty() ? fallback : out;
}

std::string sanitizeChatNickname(const std::string& value) {
    return sanitizeChatName(value, defaultChatNickname());
}

void applyDefaults() {
    g_settings = Settings();
    g_settings.chatNickname = defaultChatNickname();
    g_settings.chatRecipient = g_settings.chatNickname;
}

void refreshCompiledModules() {
    // Un modulo non compilato non deve mai risultare attivo a runtime.
    const Module modules[] = {Module::Ble,   Module::Wifi, Module::Ir,  Module::Games,
                              Module::Settings, Module::Sd, Module::Radar, Module::Radio,
                              Module::Chat, Module::Music, Module::BadUsb, Module::PcapTools,
                              Module::BleExt};
    for (Module m : modules) {
        if (!moduleCompiled(m)) {
            uint16_t bit = 0;
            if (bitFor(m, bit)) {
                g_settings.modules &= static_cast<uint16_t>(~bit);
            }
        }
    }
}

// Variante senza save(): usata da setModuleEnabled e da applyJson per fare un
// solo commit NVS invece di uno per chiave.
void applyModuleEnabled(Module module, bool enabled) {
    uint16_t bit = 0;
    if (module == Module::Settings) {
        return;
    }
    if (!bitFor(module, bit) || !moduleCompiled(module)) {
        return;
    }
    if (enabled) {
        g_settings.modules |= bit;
    } else {
        g_settings.modules &= static_cast<uint16_t>(~bit);
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
        // Primo avvio: prova a importare /config.json, poi salva i default
        // (save() scrive anche la magic, niente scritture ridondanti).
        if (storage::exists("/config.json")) {
            importFromFile();
        }
        refreshCompiledModules();
        save();
    } else {
        g_settings.brightness = clampBrightness(g_prefs.getUChar("bright", 128));
        g_settings.rotation = clampRotation(g_prefs.getUChar("rot", 1));
        g_settings.theme = static_cast<uint8_t>(
            std::min(6, static_cast<int>(g_prefs.getUChar("theme", 0))));
        g_settings.modules = g_prefs.getUShort("mods", 0xFFFF);
        g_settings.bleDurationSec =
            clampU16(g_prefs.getUShort("ble_dur", 30), 5, 300);
        g_settings.bleIntervalMs =
            clampU16(g_prefs.getUShort("ble_itv", 100), 20, 1000);
        g_settings.irRepeat = static_cast<uint8_t>(
            std::max(0, std::min(5, static_cast<int>(g_prefs.getUChar("ir_rep", 2)))));
        g_settings.irTvbgDelayMs =
            clampU16(g_prefs.getUShort("ir_tvd", 120), 20, 2000);
        g_settings.sleepTimeoutSec =
            snapSleepTimeout(g_prefs.getUShort("sleep_s", 120));
        g_settings.radarSensitivity =
            static_cast<uint8_t>(std::max(1, std::min(5, static_cast<int>(g_prefs.getUChar("radar_s", 3)))));
        // Buffer fisso abbastanza largo per qualsiasi valore creato dalla UI
        // (la sanificazione taglia poi a kChatNicknameMax). Una chiave
        // mancante lascia il buffer vuoto e il sanificatore applica il default.
        char storedNickname[64] = {};
        g_prefs.getString("chat_nick", storedNickname, sizeof(storedNickname));
        g_settings.chatNickname = sanitizeChatNickname(storedNickname);
        char storedRecipient[64] = {};
        g_prefs.getString("chat_to", storedRecipient, sizeof(storedRecipient));
        g_settings.chatRecipient =
            sanitizeChatName(storedRecipient, g_settings.chatNickname);
        g_settings.chatNotify = g_prefs.getBool("chat_notify", true);
        refreshCompiledModules();
    }
}

const Settings& get() {
    return g_settings;
}

bool save() {
    if (!g_ready) {
        return false;
    }
    bool ok = true;
    auto check = [&ok](size_t wrote) {
        if (wrote == 0) {
            ok = false;
        }
    };
    check(g_prefs.putUChar("bright", g_settings.brightness));
    check(g_prefs.putUChar("rot", g_settings.rotation));
    check(g_prefs.putUChar("theme", g_settings.theme));
    check(g_prefs.putUShort("mods", g_settings.modules));
    check(g_prefs.putUShort("ble_dur", g_settings.bleDurationSec));
    check(g_prefs.putUShort("ble_itv", g_settings.bleIntervalMs));
    check(g_prefs.putUChar("ir_rep", g_settings.irRepeat));
    check(g_prefs.putUShort("ir_tvd", g_settings.irTvbgDelayMs));
    check(g_prefs.putUShort("sleep_s", g_settings.sleepTimeoutSec));
    check(g_prefs.putUChar("radar_s", g_settings.radarSensitivity));
    // putString() ritorna strlen(): 0 e' anche il valore legittimo di una
    // stringa vuota, quindi l'esito si puo' valutare solo se non e' vuota.
    auto checkString = [&ok](size_t wrote, const std::string& value) {
        if (wrote == 0 && !value.empty()) {
            ok = false;
        }
    };
    checkString(g_prefs.putString("chat_nick", g_settings.chatNickname.c_str()),
                g_settings.chatNickname);
    checkString(g_prefs.putString("chat_to", g_settings.chatRecipient.c_str()),
                g_settings.chatRecipient);
    check(g_prefs.putBool("chat_notify", g_settings.chatNotify));
    check(g_prefs.putUInt("magic", kConfigMagic));
    if (!ok) {
        logger::error("NVS: salvataggio impostazioni incompleto");
    }
    return ok;
}

void reset() {
    applyDefaults();
    refreshCompiledModules();
    if (g_ready) {
        g_prefs.clear();
        // save() riscrive tutte le chiavi, magic compresa.
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

uint8_t theme() {
    return g_settings.theme;
}

void setTheme(uint8_t index) {
    g_settings.theme = static_cast<uint8_t>(std::min(6, static_cast<int>(index)));
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
            return GAMES_ANY;
        case Module::Settings:
            return ENABLE_MODULE_SETTINGS;
        case Module::Sd:
            return ENABLE_MODULE_SD;
        case Module::Radar:
            return ENABLE_MODULE_RADAR;
        case Module::Radio:
            return ENABLE_MODULE_RADIO;
        case Module::Chat:
            return ENABLE_MODULE_CHAT && ENABLE_MODULE_BLE;
        case Module::Music:
            return ENABLE_MODULE_MUSIC;
        case Module::BadUsb:
            return ENABLE_MODULE_BADUSB;
        case Module::PcapTools:
            return ENABLE_MODULE_PCAPTOOLS;
        case Module::BleExt:
            return ENABLE_MODULE_BLEEXT && ENABLE_MODULE_BLE;
        default:
            return false;
    }
}

bool moduleEnabled(Module module) {
    // Impostazioni non deve mai poter essere disattivato (ne' da NVS ne' da
    // JSON): sarebbe l'unico modo per rendere il menu irraggiungibile.
    if (module == Module::Settings) {
        return moduleCompiled(module);
    }
    if (!moduleCompiled(module)) {
        return false;
    }
    uint16_t bit = 0;
    return bitFor(module, bit) && (g_settings.modules & bit);
}

void setModuleEnabled(Module module, bool enabled) {
    applyModuleEnabled(module, enabled);
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
    g_settings.sleepTimeoutSec = snapSleepTimeout(seconds);
    save();
}

uint8_t radarSensitivity() {
    return g_settings.radarSensitivity;
}

void setRadarSensitivity(uint8_t level) {
    g_settings.radarSensitivity = static_cast<uint8_t>(std::max(1, std::min(5, static_cast<int>(level))));
    save();
}

const std::string& chatNickname() {
    return g_settings.chatNickname;
}

void setChatNickname(const std::string& nickname) {
    g_settings.chatNickname = sanitizeChatNickname(nickname);
    save();
}

const std::string& chatRecipient() {
    return g_settings.chatRecipient;
}

void setChatRecipient(const std::string& recipient) {
    g_settings.chatRecipient =
        sanitizeChatName(recipient, g_settings.chatNickname);
    save();
}

bool chatNotifyEnabled() {
    return g_settings.chatNotify;
}

void setChatNotifyEnabled(bool enabled) {
    g_settings.chatNotify = enabled;
    save();
}

std::string toJson() {
    JsonDocument doc;
    doc["version"] = 1;
    doc["brightness"] = g_settings.brightness;
    doc["rotation"] = g_settings.rotation;
    doc["theme"] = g_settings.theme;
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
    modules["chat"] = moduleEnabled(Module::Chat);
    modules["music"] = moduleEnabled(Module::Music);
    modules["badusb"] = moduleEnabled(Module::BadUsb);
    modules["pcaptools"] = moduleEnabled(Module::PcapTools);
    modules["bleext"] = moduleEnabled(Module::BleExt);

    JsonObject chat = doc["chat"].to<JsonObject>();
    chat["nickname"] = g_settings.chatNickname;
    chat["recipient"] = g_settings.chatRecipient;
    chat["notify"] = g_settings.chatNotify;

    JsonObject ble = doc["ble"].to<JsonObject>();
    ble["duration_sec"] = g_settings.bleDurationSec;
    ble["interval_ms"] = g_settings.bleIntervalMs;

    JsonObject ir = doc["ir"].to<JsonObject>();
    ir["repeat"] = g_settings.irRepeat;
    ir["tvbgone_delay_ms"] = g_settings.irTvbgDelayMs;

    JsonObject radarCfg = doc["radar"].to<JsonObject>();
    radarCfg["sensitivity"] = g_settings.radarSensitivity;

    std::string out;
    out.reserve(512);
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
    g_settings.theme = static_cast<uint8_t>(
        std::min(6, static_cast<int>(doc["theme"] | g_settings.theme)));
    // Stesso aggancio al set di timeout ammessi del setter, ma senza il
    // commit NVS: il save() finale copre tutte le chiavi modificate.
    g_settings.sleepTimeoutSec = snapSleepTimeout(static_cast<uint16_t>(
        doc["sleep_timeout_sec"] | g_settings.sleepTimeoutSec));

    if (doc["modules"].is<JsonObject>()) {
        JsonObject modules = doc["modules"].as<JsonObject>();
        if (modules["ble"].is<bool>()) applyModuleEnabled(Module::Ble, modules["ble"].as<bool>());
        if (modules["wifi"].is<bool>()) applyModuleEnabled(Module::Wifi, modules["wifi"].as<bool>());
        if (modules["ir"].is<bool>()) applyModuleEnabled(Module::Ir, modules["ir"].as<bool>());
        if (modules["games"].is<bool>()) applyModuleEnabled(Module::Games, modules["games"].as<bool>());
        if (modules["settings"].is<bool>())
            applyModuleEnabled(Module::Settings, modules["settings"].as<bool>());
        if (modules["sd"].is<bool>()) applyModuleEnabled(Module::Sd, modules["sd"].as<bool>());
        if (modules["radar"].is<bool>()) applyModuleEnabled(Module::Radar, modules["radar"].as<bool>());
        if (modules["radio"].is<bool>()) applyModuleEnabled(Module::Radio, modules["radio"].as<bool>());
        if (modules["chat"].is<bool>()) applyModuleEnabled(Module::Chat, modules["chat"].as<bool>());
        if (modules["music"].is<bool>()) applyModuleEnabled(Module::Music, modules["music"].as<bool>());
        if (modules["badusb"].is<bool>())
            applyModuleEnabled(Module::BadUsb, modules["badusb"].as<bool>());
        if (modules["pcaptools"].is<bool>())
            applyModuleEnabled(Module::PcapTools, modules["pcaptools"].as<bool>());
        if (modules["bleext"].is<bool>())
            applyModuleEnabled(Module::BleExt, modules["bleext"].as<bool>());
    }

    if (doc["chat"].is<JsonObject>()) {
        JsonObject chat = doc["chat"].as<JsonObject>();
        if (chat["nickname"].is<const char*>()) {
            g_settings.chatNickname = sanitizeChatNickname(chat["nickname"].as<std::string>());
        }
        if (chat["recipient"].is<const char*>()) {
            g_settings.chatRecipient =
                sanitizeChatName(chat["recipient"].as<std::string>(), g_settings.chatNickname);
        }
        g_settings.chatNotify = chat["notify"] | g_settings.chatNotify;
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
