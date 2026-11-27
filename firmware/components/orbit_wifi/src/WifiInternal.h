#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <esp_wifi.h>

// Tipi e helper condivisi tra il menu WiFi e i flussi offensivi.
namespace orbit_wifi {

constexpr uint8_t kMaxNetworks = 24;
constexpr uint8_t kChannelCount = 13;

struct Network {
    std::string ssid;  // ASCII stampabile, max 32 byte
    uint8_t bssid[6] = {};
    int8_t rssi = 0;
    uint8_t channel = 1;
    wifi_auth_mode_t auth = WIFI_AUTH_OPEN;
};

inline uint8_t clampChannel(int channel) {
    if (channel < 1) {
        return 1;
    }
    if (channel > kChannelCount) {
        return kChannelCount;
    }
    return static_cast<uint8_t>(channel);
}

// Radio in STA, nessuna connessione: usa la config vuota in RAM cosi' il
// driver non si riconnette durante scan e TX. Ferma sempre con radioStop().
bool radioBeginSta();
void radioStop();
// Forza il country IT (1-13 canali) se il driver non lo ha gia'.
void configureCountry();

// Scansione attiva 1..13, massimo kMaxNetworks reti ordinate per RSSI.
bool scanNetworks(std::vector<Network>& out);

std::string bssidText(const uint8_t bssid[6]);
std::string networkLabel(const Network& network);
std::string shortText(const std::string& text, size_t maxLen);

// Flussi offensivi: ognuno include ui::legalWarning e ritorna se negato.
void attackDeauth(const Network& target);
void attackEvilTwinPortal(const Network& target);

void runDeauthTarget();
void runDeauthFlood();
void runBeaconSpam();
void runEvilTwinPortal();

}  // namespace orbit_wifi
