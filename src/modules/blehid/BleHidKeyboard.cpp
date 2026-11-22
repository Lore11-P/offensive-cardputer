#include "BleHidKeyboard.h"

#include "ModuleFlags.h"

#if ENABLE_MODULE_BLEHID

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <Preferences.h>

#include <cstdio>
#include <cstring>

#include "Logger.h"

namespace blehid {
namespace hid {
namespace {

constexpr uint8_t kShift = 0x02;
constexpr uint8_t kAltGr = 0x40;  // tasto Alt di destra
// Ritmo minimo tra due report HID: un host a 15-30 ms di connection interval
// non regge i ~222 report/s del vecchio press+release a 9 ms.
constexpr uint32_t kReportGapMs = 15;

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

constexpr uint32_t kMinFreeHeap = 32 * 1024;
constexpr uint16_t kNoHandle = 0xFFFF;
// Just Works + conferma esplicita sulla Cardputer: non richiediamo MITM.
// Portare a true per pretendere anche sec_state.authenticated.
constexpr bool kRequireAuthenticated = false;

bool g_begun = false;
volatile bool g_connected = false;
volatile bool g_secure = false;
volatile bool g_authenticated = false;
volatile bool g_authorized = false;
volatile bool g_bootProtocol = false;
volatile uint16_t g_peerHandle = kNoHandle;
char g_peerAddress[18] = "--";
portMUX_TYPE g_peerMux = portMUX_INITIALIZER_UNLOCKED;
StartError g_lastError = StartError::None;
std::string g_name;
NimBLEHIDDevice* g_hid = nullptr;
NimBLECharacteristic* g_input = nullptr;
NimBLECharacteristic* g_bootInput = nullptr;
NimBLECharacteristic* g_output = nullptr;
NimBLECharacteristic* g_bootOutput = nullptr;
volatile uint8_t g_leds = 0;
uint32_t g_sentKeys = 0;
volatile uint32_t g_reportDrops = 0;
Layout g_layout = Layout::It;
bool g_layoutLoaded = false;

// Un solo central autorizzato: il primo che si connette. Gli altri vengono
// disconnessi subito. L'autorizzazione e' esplicita sulla Cardputer.
class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
        if (server == nullptr || desc == nullptr) {
            return;
        }
        bool busy = false;
        portENTER_CRITICAL(&g_peerMux);
        busy = g_peerHandle != kNoHandle;
        portEXIT_CRITICAL(&g_peerMux);
        if (busy) {
            logger::warn("BLE HID: seconda connessione rifiutata (handle %u)",
                         static_cast<unsigned>(desc->conn_handle));
            server->disconnect(desc->conn_handle);
            return;
        }

        const std::string address =
            NimBLEAddress(desc->peer_id_addr).toString();
        portENTER_CRITICAL(&g_peerMux);
        std::snprintf(g_peerAddress, sizeof(g_peerAddress), "%s", address.c_str());
        g_peerHandle = desc->conn_handle;
        g_connected = true;
        g_secure = desc->sec_state.encrypted &&
                   (!kRequireAuthenticated || desc->sec_state.authenticated);
        g_authenticated = desc->sec_state.authenticated;
        g_authorized = false;
        g_leds = 0;
        portEXIT_CRITICAL(&g_peerMux);

        logger::info("BLE HID: host connesso %s, avvio sicurezza", address.c_str());
        const int rc = NimBLEDevice::startSecurity(desc->conn_handle);
        if (rc != 0) {
            logger::warn("BLE HID: startSecurity rc=%d", rc);
        }
    }

    void onDisconnect(NimBLEServer*, ble_gap_conn_desc* desc) override {
        if (desc == nullptr) {
            return;
        }
        bool mine = false;
        portENTER_CRITICAL(&g_peerMux);
        if (desc->conn_handle == g_peerHandle) {
            mine = true;
            g_peerHandle = kNoHandle;
            g_connected = false;
            g_secure = false;
            g_authenticated = false;
            g_authorized = false;
            g_bootProtocol = false;
            g_leds = 0;
            std::snprintf(g_peerAddress, sizeof(g_peerAddress), "--");
        }
        portEXIT_CRITICAL(&g_peerMux);
        if (mine) {
            logger::info("BLE HID: host autorizzato disconnesso");
        }
    }

    void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
        if (desc == nullptr) {
            return;
        }
        const bool secure = desc->sec_state.encrypted &&
                            (!kRequireAuthenticated || desc->sec_state.authenticated);
        bool mine = false;
        portENTER_CRITICAL(&g_peerMux);
        mine = desc->conn_handle == g_peerHandle;
        if (mine) {
            g_secure = secure;
            g_authenticated = desc->sec_state.authenticated;
            if (!secure) {
                g_authorized = false;
            }
        }
        portEXIT_CRITICAL(&g_peerMux);
        if (!mine) {
            return;
        }
        if (!secure) {
            logger::warn("BLE HID: link non cifrato/autenticato, disconnessione");
            NimBLEServer* server = NimBLEDevice::getServer();
            if (server != nullptr) {
                server->disconnect(desc->conn_handle);
            }
        } else {
            logger::info("BLE HID: link cifrato (authenticated=%s)",
                         desc->sec_state.authenticated ? "si" : "no");
        }
    }
};

class OutputCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* characteristic) override {
        if (characteristic == nullptr || !g_secure || !g_authorized) {
            return;
        }
        const NimBLEAttValue value = characteristic->getValue();
        if (value.length() > 0) {
            g_leds = value.data()[0];
        }
    }
};

// Protocol Mode: 0x00 = Boot Protocol, 0x01 = Report Protocol.
// In boot protocol si usa solo 0x2A22, mai 0x2A4D e 0x2A22 insieme.
class ProtocolCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* characteristic) override {
        if (characteristic == nullptr) {
            return;
        }
        const NimBLEAttValue value = characteristic->getValue();
        if (value.length() > 0) {
            g_bootProtocol = (value.data()[0] == 0x00);
            logger::info("BLE HID: protocol mode %s",
                         g_bootProtocol ? "boot" : "report");
        }
    }
};

ServerCallbacks g_serverCallbacks;
OutputCallbacks g_outputCallbacks;
ProtocolCallbacks g_protocolCallbacks;

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

bool inputSubscribed() {
    return g_input != nullptr && g_input->getSubscribedCount() > 0;
}

bool bootInputSubscribed() {
    return g_bootInput != nullptr && g_bootInput->getSubscribedCount() > 0;
}

// Notifica una sola caratteristica, solo se sottoscritta e solo su un link
// cifrato e autorizzato. 0x2A22 esiste solo in Boot Protocol.
//
// NimBLECharacteristic::notify() e' void e scarta in silenzio: qui si usa la
// chiamata di basso livello per sapere se il report e' stato accodato.
bool notifyReport(NimBLECharacteristic* characteristic, uint16_t connHandle,
                  const uint8_t report[8]) {
    if (characteristic == nullptr || connHandle == kNoHandle ||
        characteristic->getHandle() == 0xFFFF) {
        ++g_reportDrops;
        return false;
    }
    characteristic->setValue(report, 8);
    os_mbuf* om = ble_hs_mbuf_from_flat(report, 8);
    if (om == nullptr) {
        if (g_reportDrops == 0) {
            logger::warn("BLE HID: mbuf esaurito, report scartato");
        }
        ++g_reportDrops;
        return false;
    }
    // Il mbuf viene consumato dalla chiamata in ogni caso.
    const int rc = ble_gatts_notify_custom(connHandle, characteristic->getHandle(), om);
    if (rc != 0) {
        if (g_reportDrops == 0) {
            logger::warn("BLE HID: notify rc=%d, report scartato", rc);
        }
        ++g_reportDrops;
        return false;
    }
    delay(kReportGapMs);
    return true;
}

bool sendReport(const uint8_t report[8]) {
    if (!g_begun || !g_connected || !g_secure || !g_authorized) {
        return false;
    }
    uint16_t handle = kNoHandle;
    portENTER_CRITICAL(&g_peerMux);
    handle = g_peerHandle;
    portEXIT_CRITICAL(&g_peerMux);

    if (g_bootProtocol) {
        if (bootInputSubscribed()) {
            return notifyReport(g_bootInput, handle, report);
        }
        return false;
    }
    if (inputSubscribed()) {
        return notifyReport(g_input, handle, report);
    }
    return false;
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
    portENTER_CRITICAL(&g_peerMux);
    g_connected = false;
    g_secure = false;
    g_authenticated = false;
    g_authorized = false;
    g_bootProtocol = false;
    g_peerHandle = kNoHandle;
    std::snprintf(g_peerAddress, sizeof(g_peerAddress), "--");
    portEXIT_CRITICAL(&g_peerMux);
    g_begun = false;
}

// True se un modulo precedente (Chat, BLE+, Radar) ha lasciato un host
// connesso. In quel caso deinit ucciderebbe una sessione legittima.
bool otherHostConnected() {
    NimBLEServer* server = NimBLEDevice::getServer();
    if (server != nullptr && server->getConnectedCount() > 0) {
        return true;
    }
#if defined(CONFIG_BT_NIMBLE_ROLE_CENTRAL)
    std::list<NimBLEClient*>* clients = NimBLEDevice::getClientList();
    if (clients != nullptr) {
        for (NimBLEClient* client : *clients) {
            if (client != nullptr && client->isConnected()) {
                return true;
            }
        }
    }
#endif
    return false;
}

}  // namespace

StartError lastError() {
    return g_lastError;
}

bool begin(const std::string& name) {
    if (g_begun) {
        return true;
    }
    g_lastError = StartError::None;
    loadLayout();

    // Lo stack puo' essere gia' attivo (Chat, BLE, Radar). Se un host e'
    // connesso non lo tocciamo: chiuderebbe una sessione legittima. Senza
    // peer lo fermiamo e lo ricreiamo in modo esclusivo col nostro nome.
    if (NimBLEDevice::getInitialized()) {
        if (otherHostConnected()) {
            g_lastError = StartError::PeerConnected;
            logger::warn("BLE HID: stack BLE occupato da un host connesso");
            return false;
        }
        logger::info("BLE HID: stack BLE gia' attivo, riavvio esclusivo");
        NimBLEDevice::stopAdvertising();
        NimBLEScan* scan = NimBLEDevice::getScan();
        if (scan != nullptr) {
            scan->stop();
        }
        NimBLEDevice::deinit(true);
        delay(30);
    }

    if (ESP.getFreeHeap() < kMinFreeHeap) {
        g_lastError = StartError::LowHeap;
        logger::error("BLE HID: heap insufficiente (%u byte)",
                      static_cast<unsigned>(ESP.getFreeHeap()));
        return false;
    }

    NimBLEDevice::init(name);
    if (!NimBLEDevice::getInitialized()) {
        g_lastError = StartError::Stack;
        logger::error("BLE HID: init NimBLE fallita");
        return false;
    }
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC, false);
    NimBLEDevice::setSecurityAuth(true, false, true);
    // Just Works: l'autorizzazione vera e' la conferma esplicita sulla
    // Cardputer, dopo il controllo dell'indirizzo del central.
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    NimBLEServer* server = NimBLEDevice::createServer();
    if (server == nullptr) {
        g_lastError = StartError::Stack;
        NimBLEDevice::deinit(true);
        return false;
    }
    server->setCallbacks(&g_serverCallbacks, false);
    server->advertiseOnDisconnect(true);

    g_hid = new NimBLEHIDDevice(server);
    if (g_hid == nullptr) {
        g_lastError = StartError::Stack;
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
    NimBLECharacteristic* protocolMode = g_hid->protocolMode();
    if (g_input == nullptr || g_output == nullptr || g_bootInput == nullptr ||
        g_bootOutput == nullptr || protocolMode == nullptr) {
        cleanup();
        NimBLEDevice::deinit(true);
        g_lastError = StartError::Stack;
        return false;
    }
    g_output->setCallbacks(&g_outputCallbacks);
    g_bootOutput->setCallbacks(&g_outputCallbacks);
    protocolMode->setCallbacks(&g_protocolCallbacks);
    g_hid->startServices();

    g_name = name;
    g_leds = 0;
    g_sentKeys = 0;
    g_reportDrops = 0;
    portENTER_CRITICAL(&g_peerMux);
    g_connected = false;
    g_secure = false;
    g_authenticated = false;
    g_authorized = false;
    g_bootProtocol = false;
    g_peerHandle = kNoHandle;
    std::snprintf(g_peerAddress, sizeof(g_peerAddress), "--");
    portEXIT_CRITICAL(&g_peerMux);

    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    if (advertising == nullptr) {
        cleanup();
        NimBLEDevice::deinit(true);
        g_lastError = StartError::Stack;
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
        g_lastError = StartError::Stack;
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

bool linkUp() {
    return g_begun && g_connected;
}

bool linkSecure() {
    return linkUp() && g_secure;
}

bool authorized() {
    return linkSecure() && g_authorized;
}

bool subscribed() {
    // Solo il report input 0x2A4D: il boot input 0x2A22 non rende la
    // sessione pronta.
    return g_begun && inputSubscribed();
}

bool ready() {
    return authorized() && subscribed();
}

bool connected() {
    return ready();
}

bool bootProtocol() {
    return g_bootProtocol;
}

uint16_t peerCount() {
    if (!NimBLEDevice::getInitialized()) {
        return 0;
    }
    NimBLEServer* server = NimBLEDevice::getServer();
    return server != nullptr ? server->getConnectedCount() : 0;
}

std::string peerAddress() {
    char buffer[18];
    portENTER_CRITICAL(&g_peerMux);
    std::memcpy(buffer, g_peerAddress, sizeof(buffer));
    buffer[sizeof(buffer) - 1] = '\0';
    portEXIT_CRITICAL(&g_peerMux);
    return std::string(buffer);
}

bool peerIdentity(PeerIdentity& out) {
    char buffer[18];
    bool up = false;
    portENTER_CRITICAL(&g_peerMux);
    up = g_connected && g_peerHandle != kNoHandle;
    if (up) {
        out.handle = g_peerHandle;
        std::memcpy(buffer, g_peerAddress, sizeof(buffer));
        buffer[sizeof(buffer) - 1] = '\0';
    }
    portEXIT_CRITICAL(&g_peerMux);
    if (!up) {
        return false;
    }
    // Fuori dalla sezione critica: std::string puo' allocare.
    out.address.assign(buffer);
    return true;
}

bool authorizePeer(uint16_t expectedHandle, const std::string& expectedAddress) {
    if (expectedAddress.size() >= sizeof(g_peerAddress)) {
        return false;
    }
    bool ok = false;
    portENTER_CRITICAL(&g_peerMux);
    // Confronto sotto lo stesso mutex che onConnect/onDisconnect usano per
    // aggiornare handle e indirizzo: il peer confermato deve essere ancora
    // quello collegato e cifrato.
    if (expectedHandle != kNoHandle && g_peerHandle == expectedHandle &&
        std::strcmp(g_peerAddress, expectedAddress.c_str()) == 0 && g_connected &&
        g_secure) {
        g_authorized = true;
        ok = true;
    }
    portEXIT_CRITICAL(&g_peerMux);
    if (ok) {
        logger::info("BLE HID: host autorizzato dall'utente");
    } else {
        logger::warn("BLE HID: autorizzazione negata, peer cambiato nel frattempo");
    }
    return ok;
}

void deauthorizePeer() {
    g_authorized = false;
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

uint32_t reportDrops() {
    return g_reportDrops;
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
    if (!authorized()) {
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
    if (!sendReport(report)) {
        return false;
    }
    delay(holdMs);
    uint8_t release[8] = {};
    if (!sendReport(release)) {
        return false;
    }
    // Conta solo il tasto i cui due report sono stati accodati davvero.
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
