#include "BleHidKeyboard.h"

#include "ModuleFlags.h"

#if ENABLE_MODULE_BLEHID

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <Preferences.h>

#include <cstring>

#include "Logger.h"

namespace blehid {
namespace hid {
namespace {

constexpr uint8_t kShift = 0x02;
constexpr uint8_t kAltGr = 0x40;  // tasto Alt di destra
constexpr uint32_t kKeyGapMs = 3;

// Report Map HID: tastiera boot (8 byte) con Report ID 1 + LED output.
const uint8_t kReportMap[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Key Codes)
    0x19, 0xE0,        //   Usage Minimum (224)
    0x29, 0xE7,        //   Usage Maximum (231)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data, Variable, Absolute): modificatori
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Constant): byte riservato
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x01,        //   Report Size (1)
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (1)
    0x29, 0x05,        //   Usage Maximum (5)
    0x91, 0x02,        //   Output (Data, Variable, Absolute): LED
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x03,        //   Report Size (3)
    0x91, 0x01,        //   Output (Constant): padding LED
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Key Codes)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0x65,        //   Usage Maximum (101)
    0x81, 0x00,        //   Input (Data, Array): 6 tasti
    0xC0               // End Collection
};

bool g_begun = false;
volatile bool g_connected = false;
std::string g_name;
NimBLEHIDDevice* g_hid = nullptr;
NimBLECharacteristic* g_input = nullptr;
NimBLECharacteristic* g_bootInput = nullptr;
NimBLECharacteristic* g_output = nullptr;
NimBLECharacteristic* g_bootOutput = nullptr;
uint8_t g_leds = 0;
uint32_t g_sentKeys = 0;
Layout g_layout = Layout::It;
bool g_layoutLoaded = false;

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, ble_gap_conn_desc*) override {
        g_connected = true;
        logger::info("BLE HID: host connesso");
    }
    void onDisconnect(NimBLEServer*) override {
        g_connected = false;
        g_leds = 0;
        logger::info("BLE HID: host disconnesso");
    }
};

class OutputCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* characteristic) override {
        if (characteristic == nullptr) {
            return;
        }
        const NimBLEAttValue value = characteristic->getValue();
        if (value.length() > 0) {
            g_leds = value.data()[0];
        }
    }
};

ServerCallbacks g_serverCallbacks;
OutputCallbacks g_outputCallbacks;

void loadLayout() {
    if (g_layoutLoaded) {
        return;
    }
    g_layoutLoaded = true;
    Preferences prefs;
    if (prefs.begin("orbit_blehid", true)) {
        const uint8_t saved = prefs.getUChar("layout", 0);
        g_layout = saved == 1 ? Layout::Us : Layout::It;
        prefs.end();
    }
}

void sendReport(const uint8_t report[8]) {
    if (g_input != nullptr) {
        g_input->setValue(report, 8);
        g_input->notify(report, 8);
    }
    if (g_bootInput != nullptr) {
        g_bootInput->setValue(report, 8);
        g_bootInput->notify(report, 8);
    }
}

bool mapAscii(uint8_t c, Layout layout, uint8_t& usage, uint8_t& mods) {
    usage = 0;
    mods = 0;
    if (c >= 'a' && c <= 'z') {
        usage = static_cast<uint8_t>(0x04 + (c - 'a'));
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        usage = static_cast<uint8_t>(0x04 + (c - 'A'));
        mods = kShift;
        return true;
    }
    if (c >= '1' && c <= '9') {
        usage = static_cast<uint8_t>(0x1E + (c - '1'));
        return true;
    }
    if (c == '0') {
        usage = 0x27;
        return true;
    }

    if (layout == Layout::Us) {
        switch (c) {
            case ' ': usage = 0x2C; return true;
            case '!': usage = 0x1E; mods = kShift; return true;
            case '"': usage = 0x1F; mods = kShift; return true;
            case '#': usage = 0x20; mods = kShift; return true;
            case '$': usage = 0x21; mods = kShift; return true;
            case '%': usage = 0x22; mods = kShift; return true;
            case '&': usage = 0x23; mods = kShift; return true;
            case '\'': usage = 0x34; return true;
            case '(': usage = 0x26; mods = kShift; return true;
            case ')': usage = 0x27; mods = kShift; return true;
            case '*': usage = 0x25; mods = kShift; return true;
            case '+': usage = 0x2E; mods = kShift; return true;
            case ',': usage = 0x36; return true;
            case '-': usage = 0x2D; return true;
            case '.': usage = 0x37; return true;
            case '/': usage = 0x38; return true;
            case ':': usage = 0x33; mods = kShift; return true;
            case ';': usage = 0x33; return true;
            case '<': usage = 0x36; mods = kShift; return true;
            case '=': usage = 0x2E; return true;
            case '>': usage = 0x37; mods = kShift; return true;
            case '?': usage = 0x38; mods = kShift; return true;
            case '@': usage = 0x1F; mods = kShift; return true;
            case '[': usage = 0x2F; return true;
            case '\\': usage = 0x31; return true;
            case ']': usage = 0x30; return true;
            case '^': usage = 0x23; mods = kShift; return true;
            case '_': usage = 0x2D; mods = kShift; return true;
            case '`': usage = 0x35; return true;
            case '{': usage = 0x2F; mods = kShift; return true;
            case '|': usage = 0x31; mods = kShift; return true;
            case '}': usage = 0x30; mods = kShift; return true;
            case '~': usage = 0x35; mods = kShift; return true;
            default: return false;
        }
    }

    // Layout italiano (tasti fisici del PC configurato in italiano).
    switch (c) {
        case ' ': usage = 0x2C; return true;
        case '!': usage = 0x1E; mods = kShift; return true;
        case '"': usage = 0x1F; mods = kShift; return true;
        case '#': usage = 0x34; mods = kAltGr; return true;
        case '$': usage = 0x21; mods = kShift; return true;
        case '%': usage = 0x22; mods = kShift; return true;
        case '&': usage = 0x23; mods = kShift; return true;
        case '\'': usage = 0x2D; return true;
        case '(': usage = 0x25; mods = kShift; return true;
        case ')': usage = 0x26; mods = kShift; return true;
        case '*': usage = 0x30; mods = kShift; return true;
        case '+': usage = 0x30; return true;
        case ',': usage = 0x36; return true;
        case '-': usage = 0x38; return true;
        case '.': usage = 0x37; return true;
        case '/': usage = 0x24; mods = kShift; return true;
        case ':': usage = 0x37; mods = kShift; return true;
        case ';': usage = 0x36; mods = kShift; return true;
        case '<': usage = 0x64; return true;
        case '=': usage = 0x27; mods = kShift; return true;
        case '>': usage = 0x64; mods = kShift; return true;
        case '?': usage = 0x2D; mods = kShift; return true;
        case '@': usage = 0x33; mods = kAltGr; return true;
        case '[': usage = 0x2F; mods = kAltGr; return true;
        case '\\': usage = 0x35; return true;
        case ']': usage = 0x30; mods = kAltGr; return true;
        case '^': usage = 0x2E; mods = kShift; return true;
        case '_': usage = 0x38; mods = kShift; return true;
        case '{': usage = 0x2F; mods = kAltGr | kShift; return true;
        case '|': usage = 0x35; mods = kShift; return true;
        case '}': usage = 0x30; mods = kAltGr | kShift; return true;
        default: return false;
    }
}

bool mapItAccent(uint32_t codepoint, uint8_t& usage, uint8_t& mods) {
    mods = 0;
    switch (codepoint) {
        case 0x00E0: usage = 0x34; return true;             // a con accento grave
        case 0x00E8: usage = 0x2F; return true;             // e grave
        case 0x00E9: usage = 0x2F; mods = kShift; return true;  // e acuta
        case 0x00EC: usage = 0x2E; return true;             // i grave
        case 0x00F2: usage = 0x33; return true;             // o grave
        case 0x00F9: usage = 0x31; return true;             // u grave
        case 0x00E7: usage = 0x33; mods = kShift; return true;  // c cediglia
        case 0x00A7: usage = 0x31; mods = kShift; return true;  // section
        case 0x00B0: usage = 0x34; mods = kShift; return true;  // grado
        case 0x00A3: usage = 0x20; mods = kShift; return true;  // sterlina
        case 0x20AC: usage = 0x08; mods = kAltGr; return true;  // euro
        default: return false;
    }
}

bool specialUsage(uint8_t code, uint8_t& usage) {
    switch (code) {
        case kEnter: usage = 0x28; return true;
        case kTab: usage = 0x2B; return true;
        case kEsc: usage = 0x29; return true;
        case kBackspace: usage = 0x2A; return true;
        case kDelete: usage = 0x4C; return true;
        case kCapsLock: usage = 0x39; return true;
        case kInsert: usage = 0x49; return true;
        case kHome: usage = 0x4A; return true;
        case kEnd: usage = 0x4D; return true;
        case kPageUp: usage = 0x4B; return true;
        case kPageDown: usage = 0x4E; return true;
        case kUp: usage = 0x52; return true;
        case kDown: usage = 0x51; return true;
        case kLeft: usage = 0x50; return true;
        case kRight: usage = 0x4F; return true;
        case kF1: usage = 0x3A; return true;
        case kF2: usage = 0x3B; return true;
        case kF3: usage = 0x3C; return true;
        case kF4: usage = 0x3D; return true;
        case kF5: usage = 0x3E; return true;
        case kF6: usage = 0x3F; return true;
        case kF7: usage = 0x40; return true;
        case kF8: usage = 0x41; return true;
        case kF9: usage = 0x42; return true;
        case kF10: usage = 0x43; return true;
        case kF11: usage = 0x44; return true;
        case kF12: usage = 0x45; return true;
        default: return false;
    }
}

void cleanup() {
    g_input = nullptr;
    g_bootInput = nullptr;
    g_output = nullptr;
    g_bootOutput = nullptr;
    delete g_hid;  // i servizi appartengono al server, liberato da deinit(true)
    g_hid = nullptr;
    g_connected = false;
    g_begun = false;
}

}  // namespace

bool begin(const std::string& name) {
    if (g_begun) {
        return true;
    }
    loadLayout();

    // Gli altri moduli BLE (Chat, BLE Spam) lasciano lo stack acceso: qui il
    // modulo e' esclusivo, quindi si riparte da zero con il nostro nome.
    if (NimBLEDevice::getInitialized()) {
        NimBLEDevice::stopAdvertising();
        NimBLEScan* scan = NimBLEDevice::getScan();
        if (scan != nullptr) {
            scan->stop();
        }
        NimBLEDevice::deinit(true);
        delay(30);
    }
    NimBLEDevice::init(name);
    if (!NimBLEDevice::getInitialized()) {
        logger::error("BLE HID: init NimBLE fallita");
        return false;
    }
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC, false);
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    NimBLEServer* server = NimBLEDevice::createServer();
    if (server == nullptr) {
        NimBLEDevice::deinit(true);
        return false;
    }
    server->setCallbacks(&g_serverCallbacks, false);
    server->advertiseOnDisconnect(true);

    g_hid = new NimBLEHIDDevice(server);
    if (g_hid == nullptr) {
        NimBLEDevice::deinit(true);
        return false;
    }
    g_hid->manufacturer("ORBIT OS");
    g_hid->pnp(0x02, 0x303A, 0x0001, 0x0100);
    g_hid->hidInfo(0, 0x02);
    g_hid->reportMap(const_cast<uint8_t*>(kReportMap),
                     static_cast<uint16_t>(sizeof(kReportMap)));

    g_input = g_hid->inputReport(1);
    g_output = g_hid->outputReport(1);
    g_bootInput = g_hid->bootInput();
    g_bootOutput = g_hid->bootOutput();
    if (g_input == nullptr || g_output == nullptr || g_bootInput == nullptr ||
        g_bootOutput == nullptr) {
        cleanup();
        NimBLEDevice::deinit(true);
        return false;
    }
    g_output->setCallbacks(&g_outputCallbacks);
    g_bootOutput->setCallbacks(&g_outputCallbacks);
    g_hid->startServices();

    g_name = name;
    g_leds = 0;
    g_sentKeys = 0;

    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    if (advertising == nullptr) {
        cleanup();
        NimBLEDevice::deinit(true);
        return false;
    }
    advertising->reset();
    advertising->setName(name);
    advertising->setAppearance(HID_KEYBOARD);
    advertising->addServiceUUID(g_hid->hidService()->getUUID());
    advertising->setMinInterval(32);
    advertising->setMaxInterval(64);
    if (!advertising->start()) {
        logger::error("BLE HID: advertising non avviato");
        cleanup();
        NimBLEDevice::deinit(true);
        return false;
    }

    g_begun = true;
    logger::info("BLE HID: pronto come %s, heap=%u", name.c_str(),
                 static_cast<unsigned>(ESP.getFreeHeap()));
    return true;
}

void end() {
    if (!g_begun) {
        return;
    }
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    if (advertising != nullptr) {
        advertising->stop();
    }
    cleanup();
    NimBLEDevice::deinit(true);
    delay(20);
    logger::info("BLE HID: stack rilasciato, heap=%u",
                 static_cast<unsigned>(ESP.getFreeHeap()));
}

bool active() {
    return g_begun;
}

bool connected() {
    return g_begun && g_connected;
}

bool subscribed() {
    if (!g_begun) {
        return false;
    }
    const bool input = g_input != nullptr && g_input->getSubscribedCount() > 0;
    const bool boot = g_bootInput != nullptr && g_bootInput->getSubscribedCount() > 0;
    return input || boot;
}

const std::string& deviceName() {
    loadLayout();
    return g_name;
}

Layout layout() {
    loadLayout();
    return g_layout;
}

void setLayout(Layout value) {
    g_layout = value;
    g_layoutLoaded = true;
    Preferences prefs;
    if (prefs.begin("orbit_blehid", false)) {
        prefs.putUChar("layout", value == Layout::Us ? 1 : 0);
        prefs.end();
    }
}

uint8_t leds() {
    return g_leds;
}

uint32_t sentKeys() {
    return g_sentKeys;
}

bool mapCodepoint(uint32_t codepoint, uint8_t& usage, uint8_t& mods) {
    loadLayout();
    if (codepoint < 0x80) {
        if (mapAscii(static_cast<uint8_t>(codepoint), g_layout, usage, mods)) {
            return true;
        }
        return false;
    }
    if (g_layout == Layout::It) {
        return mapItAccent(codepoint, usage, mods);
    }
    return false;
}

bool tapUsage(uint8_t usage, uint8_t mods, uint32_t holdMs) {
    if (!g_begun || !g_connected) {
        return false;
    }
    uint8_t report[8] = {mods, 0, 0, 0, 0, 0, 0, 0};
    if (usage >= 0xE0 && usage <= 0xE7) {
        report[0] |= static_cast<uint8_t>(1U << (usage - 0xE0));
    } else if (usage != 0) {
        report[2] = usage;
    } else {
        return false;
    }
    sendReport(report);
    delay(holdMs);
    uint8_t release[8] = {};
    sendReport(release);
    delay(kKeyGapMs);
    ++g_sentKeys;
    return true;
}

bool tapSpecial(uint8_t code) {
    uint8_t usage = 0;
    if (!specialUsage(code, usage)) {
        return false;
    }
    return tapUsage(usage, 0);
}

bool tapChar(char c) {
    return tapCodepoint(static_cast<uint8_t>(c));
}

bool tapCodepoint(uint32_t codepoint) {
    if (codepoint == '\n') {
        return tapSpecial(kEnter);
    }
    if (codepoint == '\t') {
        return tapSpecial(kTab);
    }
    if (codepoint == '\b') {
        return tapSpecial(kBackspace);
    }
    uint8_t usage = 0;
    uint8_t mods = 0;
    if (!mapCodepoint(codepoint, usage, mods)) {
        return false;
    }
    return tapUsage(usage, mods);
}

bool tapScriptKey(uint8_t code, uint8_t mods) {
    if (code < 0x80) {
        uint8_t usage = 0;
        uint8_t charMods = 0;
        if (!mapCodepoint(code, usage, charMods)) {
            return false;
        }
        return tapUsage(usage, static_cast<uint8_t>(mods | charMods));
    }
    uint8_t usage = 0;
    if (!specialUsage(code, usage)) {
        return false;
    }
    return tapUsage(usage, mods);
}

size_t typeText(const std::string& text) {
    size_t sent = 0;
    for (size_t i = 0; i < text.size();) {
        const uint8_t c = static_cast<uint8_t>(text[i]);
        uint32_t codepoint = c;
        size_t length = 1;
        if (c == '\\' && i + 1 < text.size()) {
            const char next = text[i + 1];
            if (next == 'n' || next == 'N') {
                codepoint = '\n';
                length = 2;
            } else if (next == 't' || next == 'T') {
                codepoint = '\t';
                length = 2;
            } else if (next == '\\') {
                codepoint = '\\';
                length = 2;
            }
        } else if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
            codepoint = ((c & 0x1FU) << 6) | (static_cast<uint8_t>(text[i + 1]) & 0x3FU);
            length = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
            codepoint = ((c & 0x0FU) << 12) |
                        ((static_cast<uint8_t>(text[i + 1]) & 0x3FU) << 6) |
                        (static_cast<uint8_t>(text[i + 2]) & 0x3FU);
            length = 3;
        }
        i += length;
        if (tapCodepoint(codepoint)) {
            ++sent;
        }
    }
    return sent;
}
void releaseAll() {
    if (!g_begun) {
        return;
    }
    uint8_t release[8] = {};
    sendReport(release);
}

}  // namespace hid
}  // namespace blehid

#endif  // ENABLE_MODULE_BLEHID
