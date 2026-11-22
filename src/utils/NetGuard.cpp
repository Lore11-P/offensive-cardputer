#include "NetGuard.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <strings.h>

#include "Storage.h"

namespace netguard {
namespace {

Entry g_entries[kMaxEntries];
int g_count = 0;
bool g_ready = false;

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

bool parseMac(const std::string& text, uint8_t out[6]) {
    char hex[13];
    int count = 0;
    for (char c : text) {
        if (c == ':' || c == '-' || c == ' ' || c == '.') {
            continue;
        }
        if (count >= 12) {
            return false;
        }
        hex[count++] = c;
    }
    if (count != 12) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        const int hi = hexValue(hex[i * 2]);
        const int lo = hexValue(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

bool sameMac(const uint8_t a[6], const uint8_t b[6]) {
    return std::memcmp(a, b, 6) == 0;
}

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

void macText(const uint8_t mac[6], char out[18]) {
    std::snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
                  mac[2], mac[3], mac[4], mac[5]);
}

void trim(std::string& text) {
    size_t begin = 0;
    while (begin < text.size() &&
           (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                           text[end - 1] == '\r')) {
        --end;
    }
    text = text.substr(begin, end - begin);
}

}  // namespace

void reload() {
    g_count = 0;
    g_ready = false;
    if (!storage::hasSD() || !storage::exists(kPath, true)) {
        return;
    }
    if (storage::fileSize(kPath, true) > 4096) {
        return;
    }
    std::string text;
    if (!storage::readText(kPath, text, 4096, true)) {
        return;
    }
    size_t start = 0;
    bool invalid = false;
    bool overflow = false;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(start, end - start);
        start = end + 1;
        trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const size_t bar = line.find('|');
        std::string macPart = bar == std::string::npos ? line : line.substr(0, bar);
        std::string ssidPart =
            bar == std::string::npos ? std::string() : line.substr(bar + 1);
        trim(macPart);
        trim(ssidPart);
        if (ssidPart.size() >= 2 && ssidPart.front() == '"' &&
            ssidPart.back() == '"') {
            ssidPart = ssidPart.substr(1, ssidPart.size() - 2);
        }
        Entry item;
        const bool hasMacText = !macPart.empty();
        const bool macOk = parseMac(macPart, item.bssid);
        if (hasMacText && (!macOk || !validBssid(item.bssid))) {
            invalid = true;
            continue;
        }
        item.hasBssid = macOk;
        if (ssidPart.size() > 32) {
            ssidPart.resize(32);
        }
        item.hasSsid = !ssidPart.empty();
        if (!item.hasSsid && !item.hasBssid) {
            invalid = true;
            continue;
        }
        if (item.hasSsid) {
            std::memcpy(item.ssid, ssidPart.data(), ssidPart.size());
            item.ssid[ssidPart.size()] = 0;
        }
        if (g_count < kMaxEntries) {
            g_entries[g_count++] = item;
        } else {
            overflow = true;
        }
    }
    g_ready = !invalid && !overflow;
}

bool ready() { return g_ready; }

int count() { return g_count; }

const Entry& entry(int index) {
    static const Entry kEmpty;
    if (index < 0 || index >= g_count) {
        return kEmpty;
    }
    return g_entries[index];
}

bool isProtected(const uint8_t bssid[6], const char* ssid, uint8_t ssidLen) {
    for (int i = 0; i < g_count; ++i) {
        const Entry& item = g_entries[i];
        if (item.hasBssid && bssid != nullptr && sameMac(item.bssid, bssid)) {
            return true;
        }
        if (item.hasSsid) {
            if (ssid == nullptr || ssidLen == 0) {
                // Voce solo-SSID contro un AP nascosto: fail-closed. Se la voce
                // ha un BSSID, il confronto sopra e' gia' sufficiente.
                if (!item.hasBssid) {
                    return true;
                }
                continue;
            }
            const size_t length = ssidLen > 32 ? 32 : ssidLen;
            if (std::strlen(item.ssid) == length &&
                strncasecmp(item.ssid, ssid, length) == 0) {
                return true;
            }
        }
    }
    return false;
}

bool isProtectedBssid(const uint8_t bssid[6]) {
    for (int i = 0; i < g_count; ++i) {
        if (g_entries[i].hasBssid && sameMac(g_entries[i].bssid, bssid)) {
            return true;
        }
    }
    return false;
}

bool add(const uint8_t bssid[6], const char* ssid, uint8_t ssidLen) {
    if (g_count >= kMaxEntries) {
        return false;
    }
    for (int i = 0; i < g_count; ++i) {
        if (g_entries[i].hasBssid && sameMac(g_entries[i].bssid, bssid)) {
            return false;
        }
    }
    std::string text;
    if (storage::exists(kPath, true)) {
        // Fail-closed: un file esistente ma non leggibile per intero non deve
        // essere riscritto con la sola nuova voce (perderebbe le protezioni).
        if (!storage::readText(kPath, text, 4096, true)) {
            return false;
        }
    }
    char mac[18];
    macText(bssid, mac);
    std::string line = mac;
    line.push_back('|');
    if (ssid != nullptr && ssidLen > 0) {
        line.append(ssid, ssidLen > 32 ? 32 : ssidLen);
    }
    line.push_back('\n');
    if (text.size() + line.size() > 4096) {
        return false;
    }
    if (!text.empty() && text.back() != '\n') {
        text.push_back('\n');
    }
    text += line;
    storage::mkdir("/wifilab", true);
    if (!storage::writeText(kPath, text, true)) {
        return false;
    }
    reload();
    return true;
}

}  // namespace netguard
