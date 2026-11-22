#pragma once

#include <cstdint>

// Whitelist condivisa da tutti i moduli che trasmettono frame offensivi
// (WiFi Lab e WiFi Atks). Fail-closed: se il file manca o non e' leggibile per
// intero, ready() e' false e i chiamanti devono bloccare la trasmissione.
namespace netguard {

constexpr const char* kPath = "/wifilab/whitelist.txt";
constexpr int kMaxEntries = 24;

struct Entry {
    uint8_t bssid[6] = {};
    bool hasBssid = false;
    char ssid[33] = {};
    bool hasSsid = false;
};

// Rilegge il file. Idempotente; da chiamare prima di ogni uso offensivo.
void reload();
bool ready();
int count();
const Entry& entry(int index);

// True se il BSSID/SSID corrisponde a una voce protetta. Un AP nascosto e'
// protetto da una voce solo-SSID: non si puo' dimostrare che non sia la rete
// dichiarata.
bool isProtected(const uint8_t bssid[6], const char* ssid, uint8_t ssidLen);
bool isProtectedBssid(const uint8_t bssid[6]);

// Aggiunge una voce e riscrive il file. False se il file esistente non e'
// leggibile o la voce non entra: non si perdono le protezioni gia' presenti.
bool add(const uint8_t bssid[6], const char* ssid, uint8_t ssidLen);

}  // namespace netguard
