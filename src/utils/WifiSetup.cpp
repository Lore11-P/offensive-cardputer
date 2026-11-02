#include "WifiSetup.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>

#include <algorithm>
#include <string>
#include <vector>

#include "Display.h"
#include "Logger.h"
#include "Navigation.h"
#include "TextInput.h"
#include "Ui.h"

namespace wifisetup {
namespace {

// Numero massimo di reti mostrate nell'elenco (le piu' forti per RSSI).
constexpr size_t kMaxNetworks = 24;
constexpr uint32_t kConnectTimeoutMs = 15000;
constexpr const char* kNetNamespace = "cpfw-net";

std::string shortened(const std::string& text, size_t maxLen) {
    if (text.size() <= maxLen || maxLen < 2) {
        return text;
    }
    return text.substr(0, maxLen - 1) + "~";
}

uint16_t stateColor(bool connected) {
    return connected ? display::kAccent : display::kTextDim;
}

// --- Credenziali -----------------------------------------------------------
}  // namespace

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

bool loadCredentials(std::string& ssid, std::string& password) {
    Preferences prefs;
    if (!prefs.begin(kNetNamespace, true)) {
        return false;
    }
    ssid = prefs.getString("ssid", "").c_str();
    password = prefs.getString("pass", "").c_str();
    prefs.end();
    return !ssid.empty();
}

bool saveCredentials(const std::string& ssid, const std::string& password) {
    Preferences prefs;
    if (!prefs.begin(kNetNamespace, false)) {
        return false;
    }
    prefs.putString("ssid", ssid.c_str());
    prefs.putString("pass", password.c_str());
    prefs.end();
    logger::info("WiFi: credenziali salvate per %s", ssid.c_str());
    return true;
}

void forgetCredentials() {
    Preferences prefs;
    if (prefs.begin(kNetNamespace, false)) {
        prefs.remove("ssid");
        prefs.remove("pass");
        prefs.end();
        logger::info("WiFi: credenziali cancellate");
    }
}

bool hasCredentials() {
    std::string ssid;
    std::string password;
    return loadCredentials(ssid, password);
}

namespace {

// --- Scansione -------------------------------------------------------------
void drawScanProgress(uint32_t elapsedMs, const std::string& parent) {
    display::clear();
    display::header("Reti WiFi", parent);
    const int dots = static_cast<int>(elapsedMs / 350) % 4;
    display::textCentered(42, "Ricerca reti in corso" + std::string(static_cast<size_t>(dots), '.'),
                          display::kAccent, 2);
    display::progressBar(20, 66, 200, 8, static_cast<float>(elapsedMs) / 6000.0f,
                         display::kInfo);
    display::textCentered(84, "Attendi qualche secondo", display::kTextDim, 0);
    display::footer("` = annulla");
    display::push();
}

}  // namespace

bool scanNetworks(std::vector<Network>& networks, const std::string& parent) {
    networks.clear();
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(120);

    WiFi.scanNetworks(true, true);  // asincrona, mostra anche le reti nascoste

    const uint32_t started = millis();
    uint32_t lastDraw = 0;
    navigation::waitRelease();
    while (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            WiFi.scanDelete();
            return false;
        }
        if (millis() - lastDraw > 120) {
            lastDraw = millis();
            drawScanProgress(millis() - started, parent);
        }
        delay(40);
    }

    const int count = WiFi.scanComplete();
    if (count < 0) {
        WiFi.scanDelete();
        ui::message("Reti WiFi", {"!Scansione non riuscita.", "Riprova tra qualche secondo."},
                    parent);
        return false;
    }

    std::vector<Network> found;
    found.reserve(static_cast<size_t>(count));
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
        found.push_back(network);
    }
    WiFi.scanDelete();

    // Le reti con lo stesso SSID (mesh, repeater) vengono accorpate: per
    // connettersi conta solo il nome, si tiene il segnale piu' forte.
    for (const auto& network : found) {
        auto it = std::find_if(networks.begin(), networks.end(),
                               [&network](const Network& n) { return n.ssid == network.ssid; });
        if (it == networks.end()) {
            networks.push_back(network);
        } else if (network.rssi > it->rssi) {
            *it = network;
        }
    }

    std::sort(networks.begin(), networks.end(),
              [](const Network& a, const Network& b) { return a.rssi > b.rssi; });
    if (networks.size() > kMaxNetworks) {
        networks.resize(kMaxNetworks);
    }
    return true;
}

int chooseNetwork(const std::vector<Network>& networks, const std::string& parent) {
    if (networks.empty()) {
        return -1;
    }
    constexpr int kHeaderH = 22;
    constexpr int kFooterH = 12;
    constexpr int kRowH = 18;
    const int visible = std::max(1, (display::height() - kHeaderH - kFooterH) / kRowH);
    const int total = static_cast<int>(networks.size());
    int selected = 0;
    int top = 0;

    navigation::waitRelease();
    for (;;) {
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
            selected = (selected + total - 1) % total;
        } else if (ev.type == navigation::Event::Down) {
            selected = (selected + 1) % total;
        }
        if (selected < top) {
            top = selected;
        } else if (selected >= top + visible) {
            top = selected - visible + 1;
        }

        display::clear();
        display::header("Reti WiFi", parent);
        for (int row = 0; row < visible; ++row) {
            const int index = top + row;
            if (index >= total) {
                break;
            }
            const int y = kHeaderH + row * kRowH;
            const bool isSelected = (index == selected);
            if (isSelected) {
                display::g().fillRoundRect(2, y + 1, display::width() - 6, kRowH - 2, 3,
                                           display::kAccentDark);
                display::g().fillRect(2, y + 4, 2, kRowH - 8, display::kAccent);
            }
            const Network& network = networks[static_cast<size_t>(index)];
            const bool open = (network.auth == WIFI_AUTH_OPEN);
            const std::string label = shortened(network.ssid, 18) + "  " +
                                      std::to_string(network.rssi) + "dBm" + (open ? "  *" : "");
            display::text(10, y + 3, label, isSelected ? display::kWhite : display::kText, 2);
        }
        const int trackX = display::width() - 4;
        const int trackY = kHeaderH;
        const int trackH = display::height() - kHeaderH - kFooterH;
        display::g().fillRect(trackX, trackY, 3, trackH, display::kPanel);
        if (total > visible) {
            const int thumbH = std::max(10, trackH * visible / total);
            const int thumbY = trackY + (trackH - thumbH) * top / std::max(1, total - visible);
            display::g().fillRect(trackX, thumbY, 3, thumbH, display::kAccent);
        }
        display::footer("* = aperta   ENTER = seleziona   ` = annulla");
        display::push();
        delay(16);
    }
}

namespace {

bool promptPassword(const std::string& ssid, std::string& password) {
    std::vector<textinput::Field> fields = {
        {"Password", "", 63, true, false},
    };
    if (!textinput::edit(shortened(ssid, 22), "Reti WiFi", fields)) {
        return false;
    }
    password = fields[0].value;
    return true;
}

void drawConnectProgress(const std::string& ssid, uint32_t elapsedMs, const std::string& parent) {
    display::clear();
    display::header("WiFi", parent);
    display::textCentered(40, "Connessione a", display::kTextDim, 0);
    display::textCentered(54, shortened(ssid, 24), display::kAccent, 2);
    display::progressBar(20, 78, 200, 8,
                         static_cast<float>(elapsedMs) / static_cast<float>(kConnectTimeoutMs),
                         display::kInfo);
    display::textCentered(92, "se fallisce, controlla la password", display::kTextDim, 0);
    display::footer("` = annulla");
    display::push();
}

}  // namespace

bool connect(const std::string& ssid, const std::string& password,
             const std::string& parent) {
    if (ssid.empty()) {
        return false;
    }
    if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid.c_str()) {
        return true;
    }

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.disconnect(true);
    delay(100);
    WiFi.begin(ssid.c_str(), password.c_str());

    const uint32_t started = millis();
    uint32_t lastDraw = 0;
    navigation::waitRelease();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - started > kConnectTimeoutMs) {
            WiFi.disconnect(true);
            logger::warn("WiFi: timeout su %s", ssid.c_str());
            return false;
        }
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            WiFi.disconnect(true);
            return false;
        }
        if (millis() - lastDraw > 150) {
            lastDraw = millis();
            drawConnectProgress(ssid, millis() - started, parent);
        }
        delay(50);
    }
    logger::info("WiFi connesso: %s ip=%s heap=%u", ssid.c_str(),
                 WiFi.localIP().toString().c_str(), static_cast<unsigned>(ESP.getFreeHeap()));
    return true;
}

bool scanAndConnect(const std::string& parent) {
    std::vector<Network> networks;
    if (!scanNetworks(networks, parent)) {
        return false;
    }
    if (networks.empty()) {
        ui::message("Reti WiFi", {"!Nessuna rete trovata.", "Avvicinati al router e riprova."},
                    parent);
        return false;
    }
    const int choice = chooseNetwork(networks, parent);
    if (choice < 0) {
        return false;
    }
    const Network& network = networks[static_cast<size_t>(choice)];

    std::string password;
    if (network.auth != WIFI_AUTH_OPEN && !promptPassword(network.ssid, password)) {
        return false;
    }
    saveCredentials(network.ssid, password);

    if (connect(network.ssid, password, parent)) {
        std::string ip = WiFi.localIP().toString().c_str();
        ui::message("WiFi", {">Connesso a " + shortened(network.ssid, 20), "IP: " + ip,
                             "Credenziali salvate."},
                    parent);
        return true;
    }
    ui::message("WiFi",
                {"!Connessione non riuscita.", "Password errata o segnale debole.",
                 "Riprova o scegli un'altra rete."},
                parent);
    return false;
}

bool ensureConnected(bool interactive, const std::string& parent) {
    if (WiFi.status() == WL_CONNECTED) {
        return true;
    }

    std::string ssid;
    std::string password;
    if (!loadCredentials(ssid, password)) {
        if (!interactive) {
            return false;
        }
        return scanAndConnect(parent);
    }

    if (connect(ssid, password, parent)) {
        return true;
    }
    if (!interactive) {
        return false;
    }
    if (!ui::confirm("WiFi",
                     {"!Connessione a " + shortened(ssid, 18) + " non riuscita.",
                      "Configurare un'altra rete?"},
                     parent)) {
        return false;
    }
    return scanAndConnect(parent);
}

void disconnect() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

// --- Schermata di impostazione ---------------------------------------------
namespace {

void drawSettingsScreen(const std::string& parent, const std::vector<std::string>& actions,
                        int selected, const std::string& ssid, bool connected) {
    display::clear();
    display::header("Rete WiFi", parent);

    display::text(6, 24, ssid.empty() ? "Nessuna rete salvata" : ("Rete: " + shortened(ssid, 26)),
                  ssid.empty() ? display::kTextDim : display::kText, 0);
    std::string status = "Stato: ";
    if (connected) {
        status += "connesso ";
        status += WiFi.localIP().toString().c_str();
    } else {
        status += "non connesso";
    }
    display::text(6, 36, status, stateColor(connected), 0);

    for (size_t i = 0; i < actions.size(); ++i) {
        const int y = 52 + static_cast<int>(i) * 18;
        const bool isSelected = (static_cast<int>(i) == selected);
        if (isSelected) {
            display::g().fillRoundRect(2, y, display::width() - 6, 17, 3,
                                       display::kAccentDark);
            display::g().fillRect(2, y + 4, 2, 9, display::kAccent);
        }
        display::text(10, y + 3, actions[i], isSelected ? display::kWhite : display::kText, 2);
    }
    display::footer("; . = nav   ENTER = apri   ` = torna");
    display::push();
}

void enterManualNetwork(const std::string& parent) {
    std::vector<textinput::Field> fields = {
        {"SSID", "", 32, false, false},
        {"Password", "", 63, true, false},
    };
    if (!textinput::edit("Rete manuale", parent, fields)) {
        return;
    }
    if (fields[0].value.empty()) {
        ui::message("Rete manuale", {"!SSID vuoto."}, parent);
        return;
    }
    saveCredentials(fields[0].value, fields[1].value);
    if (connect(fields[0].value, fields[1].value, parent)) {
        ui::message("WiFi", {">Connesso.", "IP: " + std::string(WiFi.localIP().toString().c_str())},
                    parent);
    } else {
        ui::message("WiFi", {"!Connessione non riuscita.", "Verifica SSID e password."}, parent);
    }
}

void testConnection(const std::string& parent) {
    std::string ssid;
    std::string password;
    if (!loadCredentials(ssid, password)) {
        ui::message("Test connessione", {"!Nessuna rete salvata.", "Usa \"Scansiona e connetti\"."},
                    parent);
        return;
    }
    if (connect(ssid, password, parent)) {
        std::vector<std::string> lines;
        lines.push_back(">Connesso a " + shortened(ssid, 20));
        lines.push_back("IP: " + std::string(WiFi.localIP().toString().c_str()));
        lines.push_back("Gateway: " + std::string(WiFi.gatewayIP().toString().c_str()));
        lines.push_back("RSSI: " + std::to_string(WiFi.RSSI()) + " dBm");
        ui::message("Test connessione", lines, parent);
    } else {
        ui::message("Test connessione",
                    {"!Connessione non riuscita.", "Il router e' acceso?", "La password e' corretta?"},
                    parent);
    }
}

void confirmForget(const std::string& parent) {
    if (!ui::confirm("Dimentica rete",
                     {"Cancellare SSID e password", "salvati nella memoria del device?"}, parent)) {
        return;
    }
    forgetCredentials();
    disconnect();
    ui::message("WiFi", {">Credenziali cancellate."}, parent);
}

}  // namespace

void runSettings(const std::string& parent) {
    const std::vector<std::string> actions = {
        "Scansiona e connetti",
        "Inserisci SSID manualmente",
        "Test connessione",
        "Dimentica rete",
    };
    const int actionCount = static_cast<int>(actions.size());
    int selected = 0;
    bool anyAction = false;

    navigation::waitRelease();
    for (;;) {
        std::string ssid;
        std::string password;
        const bool stored = loadCredentials(ssid, password);
        const bool connected = (WiFi.status() == WL_CONNECTED);
        drawSettingsScreen(parent, actions, selected, stored ? ssid : std::string(), connected);

        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            break;
        }
        if (ev.type == navigation::Event::Up) {
            selected = (selected + actionCount - 1) % actionCount;
        } else if (ev.type == navigation::Event::Down) {
            selected = (selected + 1) % actionCount;
        } else if (ev.type == navigation::Event::Select) {
            anyAction = true;
            navigation::waitRelease();
            switch (selected) {
                case 0:
                    scanAndConnect(parent);
                    break;
                case 1:
                    enterManualNetwork(parent);
                    break;
                case 2:
                    testConnection(parent);
                    break;
                default:
                    confirmForget(parent);
                    break;
            }
            navigation::waitRelease();
        }
        delay(20);
    }
    navigation::waitRelease();

    // All'uscita la radio si spegne: i moduli che usano la rete riconnettono
    // da soli con le credenziali salvate.
    if (anyAction) {
        logger::info("WiFi: impostazioni chiuse, radio spenta");
    }
    disconnect();
}

}  // namespace wifisetup
