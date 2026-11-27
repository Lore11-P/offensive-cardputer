#include "orbit_wifi/Wifi.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_wifi.h>

#include "Display.h"
#include "MenuManager.h"
#include "Navigation.h"
#include "Ui.h"
#include "WifiInternal.h"
#include "orbit_compat.h"
#include "orbit_store/Store.h"

namespace orbit_wifi {
namespace {

size_t ssidLength(const uint8_t ssid[33]) {
    size_t len = 0;
    while (len < 32 && ssid[len] != 0) {
        ++len;
    }
    return len;
}

// I nomi rete arrivano dall'aria: niente byte di controllo sul display.
std::string printableSsid(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = data[i];
        out.push_back((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?');
    }
    return out;
}

const char* authShort(wifi_auth_mode_t auth) {
    switch (auth) {
        case WIFI_AUTH_OPEN:
            return "OPEN";
        case WIFI_AUTH_WEP:
            return "WEP";
        case WIFI_AUTH_WPA_PSK:
            return "WPA";
        case WIFI_AUTH_WPA2_PSK:
            return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:
            return "WPA2";
        case WIFI_AUTH_ENTERPRISE:
            return "ENT";
        case WIFI_AUTH_WPA3_PSK:
            return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:
            return "WPA3";
        case WIFI_AUTH_WAPI_PSK:
            return "WAPI";
        case WIFI_AUTH_OWE:
            return "OWE";
        case WIFI_AUTH_DPP:
            return "DPP";
        default:
            return "SEC";
    }
}

const char* authLong(wifi_auth_mode_t auth) {
    switch (auth) {
        case WIFI_AUTH_OPEN:
            return "Aperta";
        case WIFI_AUTH_WEP:
            return "WEP";
        case WIFI_AUTH_WPA_PSK:
            return "WPA-PSK";
        case WIFI_AUTH_WPA2_PSK:
            return "WPA2-PSK";
        case WIFI_AUTH_WPA_WPA2_PSK:
            return "WPA/WPA2";
        case WIFI_AUTH_ENTERPRISE:
            return "Enterprise";
        case WIFI_AUTH_WPA3_PSK:
            return "WPA3-PSK";
        case WIFI_AUTH_WPA2_WPA3_PSK:
            return "WPA2/WPA3";
        case WIFI_AUTH_WAPI_PSK:
            return "WAPI";
        case WIFI_AUTH_OWE:
            return "OWE";
        case WIFI_AUTH_DPP:
            return "DPP";
        default:
            return "Altro";
    }
}

// Il driver WiFi del firmware 2.x usava un country IT su 13 canali. Lo
// imposto una volta per sessione: i canali 12/13 dipendono dal dominio radio.
void applyCountry() {
    wifi_country_t current = {};
    if (esp_wifi_get_country(&current) == ESP_OK && current.nchan >= kChannelCount &&
        current.cc[0] == 'I' && current.cc[1] == 'T') {
        return;
    }
    wifi_country_t country = {};
    country.cc[0] = 'I';
    country.cc[1] = 'T';
    country.cc[2] = '\0';
    country.schan = 1;
    country.nchan = kChannelCount;
    country.max_tx_power = 80;  // 20 dBm
    country.policy = WIFI_COUNTRY_POLICY_MANUAL;
    esp_wifi_set_country(&country);
}

void drawChannelBars(const uint16_t counts[kChannelCount + 1],
                     const int8_t best[kChannelCount + 1], int total) {
    display::clear(display::C(display::T_bg));
    display::header("Canali (" + std::to_string(total) + " AP)", "WiFi");
    auto& d = display::g();
    for (int channel = 1; channel <= kChannelCount; ++channel) {
        const int y = 24 + (channel - 1) * 7;
        char number[4];
        std::snprintf(number, sizeof(number), "%2d", channel);
        display::text(6, y, number, display::C(display::T_dim), 0);

        char countText[8];
        std::snprintf(countText, sizeof(countText), "%2u",
                      static_cast<unsigned>(counts[channel]));
        display::text(22, y, countText,
                      counts[channel] ? display::C(display::T_text)
                                      : display::C(display::T_line),
                      0);

        d.fillRect(42, y + 2, 148, 3, display::C(display::T_line2));
        if (counts[channel] > 0) {
            const int level = best[channel];
            int length = (level + 90) * 2;
            if (length < 4) {
                length = 4;
            }
            if (length > 148) {
                length = 148;
            }
            const uint16_t color = level >= -55   ? display::C(display::T_ok)
                                   : level >= -72 ? display::C(display::T_warn)
                                                  : display::C(display::T_err);
            d.fillRect(42, y + 2, length, 3, color);
        }

        char dbText[8];
        if (counts[channel] > 0) {
            std::snprintf(dbText, sizeof(dbText), "%4d", static_cast<int>(best[channel]));
        } else {
            std::snprintf(dbText, sizeof(dbText), "  --");
        }
        display::text(194, y, dbText,
                      counts[channel] ? display::C(display::T_text)
                                      : display::C(display::T_dim),
                      0);
    }
    display::footer("num AP / barra RSSI  ESC=indietro");
    display::push();
}

void runScanner() {
    ui::drawScreen("Radar WiFi", "WiFi",
                   {"Scansione attiva canali 1-13", "Attendi 1-2 secondi."},
                   "attendi");
    std::vector<Network> networks;
    if (!scanNetworks(networks)) {
        ui::message("Scanner", {"!Scansione non riuscita.", "Radio occupata o canale non valido."},
                    "WiFi");
        return;
    }
    if (networks.empty()) {
        ui::message("Scanner", {"Nessuna rete trovata."}, "WiFi");
        return;
    }

    MenuManager menu("Reti WiFi", "WiFi");
    for (size_t i = 0; i < networks.size(); ++i) {
        const Network network = networks[i];
        menu.addItem(networkLabel(network), network.auth == WIFI_AUTH_OPEN ? "wifi" : "scan",
                     [network] {
                         MenuManager net("Rete", "WiFi");
                         net.addItem("Dettagli", "scan", [network] {
                             ui::message("Dettagli rete",
                                         {"SSID: " + (network.ssid.empty()
                                                          ? std::string("(nascosta)")
                                                          : shortText(network.ssid, 28)),
                                          "BSSID: " + bssidText(network.bssid),
                                          "Canale: " + std::to_string(network.channel),
                                          "Sicurezza: " + std::string(authLong(network.auth)),
                                          "RSSI: " + std::to_string(network.rssi) + " dBm"},
                                         "WiFi");
                         });
                         net.addItem("Deauth target", "jammer", [network] { attackDeauth(network); });
                         net.addItem("Evil twin + portal", "portal",
                                     [network] { attackEvilTwinPortal(network); });
                         net.setHint("ENTER=apri  `=indietro");
                         net.run(true);
                     });
    }
    menu.setHint("ENTER=apri  * = aperta  `=indietro");
    menu.run(true);
}

void runApInfo() {
    if (!orbit_store::wifiEnsureDriver()) {
        ui::message("AP info", {"!Driver WiFi non disponibile."}, "WiFi");
        return;
    }
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        ui::message("AP info",
                    {"WiFi non connesso.", "Connetti da Impostazioni", "o apri lo Store."},
                    "WiFi");
        return;
    }

    esp_netif_ip_info_t ip = {};
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != nullptr) {
        esp_netif_get_ip_info(netif, &ip);
    }
    char ipText[16] = "0.0.0.0";
    char gatewayText[16] = "0.0.0.0";
    esp_ip4addr_ntoa(&ip.ip, ipText, sizeof(ipText));
    esp_ip4addr_ntoa(&ip.gw, gatewayText, sizeof(gatewayText));

    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macText[18];
    std::snprintf(macText, sizeof(macText), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
                  mac[2], mac[3], mac[4], mac[5]);

    const std::string ssid = printableSsid(ap.ssid, ssidLength(ap.ssid));
    char radio[40];
    std::snprintf(radio, sizeof(radio), "Canale %u  RSSI %d dBm",
                  static_cast<unsigned>(ap.primary), static_cast<int>(ap.rssi));

    ui::message("AP info",
                {"SSID: " + shortText(ssid.empty() ? std::string("(nascosta)") : ssid, 26),
                 "BSSID: " + bssidText(ap.bssid),
                 "IP: " + std::string(ipText),
                 "Gateway: " + std::string(gatewayText),
                 radio,
                 "MAC STA: " + std::string(macText)},
                "WiFi");
}

void runChannelAnalyzer() {
    for (;;) {
        ui::drawScreen("Analizzatore", "WiFi",
                       {"Scansione canali 1-13...", "Conteggio AP e RSSI."}, "attendi");
        std::vector<Network> networks;
        if (!scanNetworks(networks)) {
            ui::message("Analizzatore", {"!Scansione non riuscita."}, "WiFi");
            return;
        }
        uint16_t counts[kChannelCount + 1] = {};
        int8_t best[kChannelCount + 1];
        for (int i = 0; i <= kChannelCount; ++i) {
            best[i] = -128;
        }
        for (const Network& network : networks) {
            const uint8_t channel = clampChannel(network.channel);
            ++counts[channel];
            if (network.rssi > best[channel]) {
                best[channel] = network.rssi;
            }
        }
        drawChannelBars(counts, best, static_cast<int>(networks.size()));

        bool rescan = false;
        for (;;) {
            const navigation::EventData ev = navigation::poll();
            if (ev.type == navigation::Event::Back) {
                return;
            }
            if (ev.type == navigation::Event::Select) {
                rescan = true;
                break;
            }
            delay(25);
        }
        if (!rescan) {
            return;
        }
    }
}

void runAttacksMenu() {
    // MenuManager non espone un colore di accento: per il sottomenu offensivo
    // cambio solo i token d'accento e li ripristino all'uscita.
    const uint16_t accent = display::pal[display::T_acc];
    const uint16_t accentDark = display::pal[display::T_accD];
    const uint16_t accentLight = display::pal[display::T_accL];
    display::pal[display::T_acc] = display::pal[display::T_err];
    display::pal[display::T_accD] = display::pal[display::T_errD];
    display::pal[display::T_accL] = display::pal[display::T_errL];

    MenuManager menu("WiFi Attacks", "WiFi");
    menu.addSeparator("!SOLO SU RETI PROPRIE");
    menu.addItem("Deauth target", "jammer", [] { runDeauthTarget(); });
    menu.addItem("Deauth flood", "jammer", [] { runDeauthFlood(); });
    menu.addItem("Beacon spam", "jammer", [] { runBeaconSpam(); });
    menu.addItem("Evil twin + portal", "portal", [] { runEvilTwinPortal(); });
    menu.setHint("ENTER=apri  `=indietro");
    menu.run(true);

    display::pal[display::T_acc] = accent;
    display::pal[display::T_accD] = accentDark;
    display::pal[display::T_accL] = accentLight;
}

}  // namespace

void configureCountry() {
    applyCountry();
}

bool radioBeginSta() {
    if (!orbit_store::wifiEnsureDriver()) {
        return false;
    }
    // La config STA resta in RAM: la copia su NVS dello Store non viene
    // toccata e il driver non si riconnette durante scan o TX.
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_stop();
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
        radioStop();
        return false;
    }
    configureCountry();

    wifi_config_t empty = {};
    if (esp_wifi_set_config(WIFI_IF_STA, &empty) != ESP_OK) {
        radioStop();
        return false;
    }
    if (esp_wifi_start() != ESP_OK) {
        radioStop();
        return false;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);
    // L'handler dello Store reagisce a STA_START con esp_wifi_connect(): la
    // config vuota lo fa fallire subito, prima della prima scansione.
    delay(120);
    return true;
}

void radioStop() {
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_NULL);
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
}

bool scanNetworks(std::vector<Network>& out) {
    out.clear();
    if (!radioBeginSta()) {
        return false;
    }

    wifi_scan_config_t config = {};
    config.show_hidden = true;
    config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    config.scan_time.active.min = 80;
    config.scan_time.active.max = 140;

    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; ++attempt) {
        err = esp_wifi_scan_start(&config, true);
        if (err == ESP_OK) {
            break;
        }
        delay(150);
    }
    if (err != ESP_OK) {
        return false;
    }

    uint16_t total = 0;
    if (esp_wifi_scan_get_ap_num(&total) != ESP_OK) {
        esp_wifi_clear_ap_list();
        return false;
    }
    if (total == 0) {
        return true;
    }

    const uint16_t wanted = total > kMaxNetworks ? kMaxNetworks : total;
    std::vector<wifi_ap_record_t> records(wanted);
    uint16_t got = wanted;
    if (esp_wifi_scan_get_ap_records(&got, records.data()) != ESP_OK) {
        esp_wifi_clear_ap_list();
        return false;
    }
    records.resize(got);

    std::sort(records.begin(), records.end(),
              [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) { return a.rssi > b.rssi; });

    out.reserve(got);
    for (const wifi_ap_record_t& record : records) {
        Network network;
        network.ssid = printableSsid(record.ssid, ssidLength(record.ssid));
        std::memcpy(network.bssid, record.bssid, sizeof(network.bssid));
        network.rssi = record.rssi;
        network.channel = clampChannel(record.primary);
        network.auth = record.authmode;
        out.push_back(std::move(network));
    }
    return true;
}

std::string bssidText(const uint8_t bssid[6]) {
    char text[18];
    std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", bssid[0], bssid[1],
                  bssid[2], bssid[3], bssid[4], bssid[5]);
    return std::string(text);
}

std::string networkLabel(const Network& network) {
    const std::string name =
        network.ssid.empty() ? std::string("(nascosta)") : shortText(network.ssid, 10);
    char text[64];
    std::snprintf(text, sizeof(text), "%-10s %4d c%u %s%s", name.c_str(),
                  static_cast<int>(network.rssi), static_cast<unsigned>(network.channel),
                  authShort(network.auth), network.auth == WIFI_AUTH_OPEN ? "*" : "");
    return std::string(text);
}

std::string shortText(const std::string& text, size_t maxLen) {
    if (maxLen == 0) {
        return std::string();
    }
    if (text.size() <= maxLen) {
        return text;
    }
    return text.substr(0, maxLen - 1) + "~";
}

void runMenu() {
    if (!orbit_store::wifiEnsureDriver()) {
        ui::message("WiFi", {"!Driver WiFi non disponibile.", "Riprova dopo un riavvio."},
                    "Home");
        return;
    }

    MenuManager menu("WiFi", "Home");
    menu.addItem("Scanner reti", "scan", [] { runScanner(); });
    menu.addItem("AP info", "wifi", [] { runApInfo(); });
    menu.addItem("WiFi Attacks", "jammer", [] { runAttacksMenu(); });
    menu.addItem("Analizzatore canali", "radar", [] { runChannelAnalyzer(); });
    menu.setHint("ENTER=apri  `=indietro");
    menu.run(true);

    // Uscita dal modulo: ferma la radio e lascia il driver pulito allo Store.
    esp_wifi_stop();
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    esp_wifi_set_mode(WIFI_MODE_NULL);
    orbit_store::wifiShutdownDriver();
}

bool selfTest() {
    if (!orbit_store::wifiEnsureDriver()) {
        ESP_LOGE("wifi-selftest", "driver non disponibile");
        return false;
    }
    std::vector<Network> networks;
    const bool scanOk = scanNetworks(networks);
    ESP_LOGI("wifi-selftest", "scan=%d reti=%u", scanOk ? 1 : 0,
             static_cast<unsigned>(networks.size()));

    bool rawOk = false;
    if (radioBeginSta()) {
        esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
        uint8_t frame[26] = {0xC0, 0x00, 0x00, 0x00,
                             0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                             0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
                             0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                             0x00, 0x00, 0x07, 0x00};
        frame[10] = static_cast<uint8_t>(esp_random());
        frame[11] = static_cast<uint8_t>(esp_random());
        const esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), true);
        ESP_LOGI("wifi-selftest", "deauth-tx=%s (%s)", err == ESP_OK ? "OK" : "BLOCCATO",
                 esp_err_to_name(err));
        rawOk = err == ESP_OK;

        // Controllo di confronto: un beacon deve passare il filtro IDF.
        uint8_t beacon[38] = {0x80, 0x00, 0x00, 0x00,
                              0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                              0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
                              0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                              0x64, 0x00, 0x01, 0x00,
                              0x00, 0x01, 0x54, 0x00};
        const esp_err_t beaconErr =
            esp_wifi_80211_tx(WIFI_IF_STA, beacon, sizeof(beacon), true);
        ESP_LOGI("wifi-selftest", "beacon-tx=%s (%s)",
                 beaconErr == ESP_OK ? "OK" : "BLOCCATO", esp_err_to_name(beaconErr));

        // Prova in modalita' AP: alcuni driver applicano il filtro solo a STA.
        esp_wifi_stop();
        wifi_config_t apConfig = {};
        std::memcpy(apConfig.ap.ssid, "ORBIT-TEST", 10);
        apConfig.ap.ssid_len = 10;
        apConfig.ap.channel = 1;
        apConfig.ap.authmode = WIFI_AUTH_OPEN;
        apConfig.ap.max_connection = 1;
        if (esp_wifi_set_mode(WIFI_MODE_AP) == ESP_OK &&
            esp_wifi_set_config(WIFI_IF_AP, &apConfig) == ESP_OK &&
            esp_wifi_start() == ESP_OK) {
            const esp_err_t apErr =
                esp_wifi_80211_tx(WIFI_IF_AP, frame, sizeof(frame), true);
            ESP_LOGI("wifi-selftest", "deauth-ap=%s (%s)",
                     apErr == ESP_OK ? "OK" : "BLOCCATO", esp_err_to_name(apErr));
        }
        radioStop();
    }
    orbit_store::wifiShutdownDriver();
    return scanOk && rawOk;
}

}  // namespace orbit_wifi
