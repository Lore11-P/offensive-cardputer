#include "WpaSecMenu.h"

#include "ModuleFlags.h"

#if ENABLE_MODULE_WPASEC

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_system.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "Display.h"
#include "Logger.h"
#include "MenuManager.h"
#include "Navigation.h"
#include "PcapTools.h"
#include "PowerTimer.h"
#include "Storage.h"
#include "TextInput.h"
#include "Ui.h"
#include "WifiSetup.h"

namespace wpasec {
namespace {

// ---------------------------------------------------------------------------
//  Costanti
// ---------------------------------------------------------------------------

constexpr const char* kHost = "wpa-sec.stanev.org";
constexpr uint16_t kPort = 443;
constexpr const char* kModuleName = "WPA-SEC";
constexpr const char* kDir = "/wpasec";
constexpr const char* kKeyPath = "/wpasec/key.txt";
constexpr const char* kUploadedPath = "/wpasec/uploaded.txt";
constexpr const char* kPotfilePath = "/wpasec/potfile.txt";
constexpr const char* kPotfileNewPath = "/wpasec/potfile.new";

// Gate TLS: mbedTLS usa ~20 KB di buffer interni piu' lo handshake.
constexpr uint32_t kMinFreeHeap = 40 * 1024;
constexpr uint32_t kMinMaxBlock = 32 * 1024;

constexpr uint32_t kNetTimeoutMs = 20000;
constexpr uint32_t kHandshakeTimeoutMs = 12000;
constexpr size_t kUploadChunk = 1024;
constexpr size_t kUploadResponseMax = 16 * 1024;
constexpr size_t kMaxUploadedBytes = 16 * 1024;
constexpr size_t kMaxPotfileBytes = 4 * 1024 * 1024;
constexpr size_t kMaxPotfileScan = 256 * 1024;
constexpr size_t kMaxCaptureHashLines = 128;
constexpr size_t kCrackSlots = 2048;
constexpr size_t kMaxUploadedEntries = 200;

// CA pinnata: GTS Root R4 (Google Trust Services), valida fino al 2036.
// Il certificato del servizio e' firmato da WE1 -> GTS Root R4.
const char kRootCa[] = R"(-----BEGIN CERTIFICATE-----
MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD
VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG
A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw
WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz
IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi
AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi
QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR
HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW
BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D
9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8
p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD
-----END CERTIFICATE-----
)";

bool g_caFailed = false;     // ultima verifica CA fallita (solo per la UI)
bool g_clockFailed = false;  // NTP non disponibile: certificato non verificabile
bool g_clockAttempted = false;  // NTP gia' tentato in questa sessione
bool g_insecureAck = false;  // l'utente ha accettato TLS senza verifica
bool g_networkUsed = false;  // il modulo ha aperto/avviato la radio

// ---------------------------------------------------------------------------
//  Utility stringhe
// ---------------------------------------------------------------------------

bool isSpaceChar(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && isSpaceChar(text[begin])) {
        ++begin;
    }
    while (end > begin && isSpaceChar(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

char lowerChar(char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c + ('a' - 'A'));
    }
    return c;
}

std::string lowerCopy(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        c = lowerChar(c);
    }
    return out;
}

bool startsWithNoCase(const std::string& text, const char* prefix) {
    const size_t len = std::strlen(prefix);
    if (text.size() < len) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (lowerChar(text[i]) != lowerChar(prefix[i])) {
            return false;
        }
    }
    return true;
}

bool containsNoCase(const std::string& text, const char* needle) {
    const std::string haystack = lowerCopy(text);
    return haystack.find(lowerCopy(needle)) != std::string::npos;
}

bool equalsNoCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (lowerChar(a[i]) != lowerChar(b[i])) {
            return false;
        }
    }
    return true;
}

bool hasControlChars(const std::string& text) {
    for (char c : text) {
        if (static_cast<unsigned char>(c) < 0x20) {
            return true;
        }
    }
    return false;
}

std::string shorten(const std::string& text, size_t maxChars) {
    if (maxChars == 0) {
        return "";
    }
    if (text.size() <= maxChars) {
        return text;
    }
    if (maxChars == 1) {
        return "~";
    }
    return text.substr(0, maxChars - 1) + "~";
}

std::string formatSize(size_t size) {
    char buffer[20];
    if (size < 1024) {
        std::snprintf(buffer, sizeof(buffer), "%uB", static_cast<unsigned>(size));
    } else if (size < 1024UL * 1024UL) {
        std::snprintf(buffer, sizeof(buffer), "%uK",
                      static_cast<unsigned>((size + 1023) / 1024));
    } else {
        const unsigned whole = static_cast<unsigned>(size / (1024UL * 1024UL));
        const unsigned tenth = static_cast<unsigned>(((size / 1024UL) % 1024UL) * 10 / 1024);
        std::snprintf(buffer, sizeof(buffer), "%u.%uM", whole, tenth);
    }
    return buffer;
}

std::string formatHeap(uint32_t bytes) {
    return std::to_string(static_cast<unsigned>(bytes / 1024)) + "K";
}

// Converte testo libero in righe <= width caratteri per ui::message.
std::vector<std::string> wrapForUi(const std::string& text, size_t width, size_t maxLines) {
    std::vector<std::string> lines;
    if (width == 0 || maxLines == 0) {
        return lines;
    }
    std::string current;
    for (char c : text) {
        if (c == '\n' || c == '\r') {
            if (!current.empty()) {
                lines.push_back(current);
                current.clear();
                if (lines.size() >= maxLines) {
                    return lines;
                }
            }
            continue;
        }
        current.push_back(c);
        if (current.size() >= width) {
            lines.push_back(current);
            current.clear();
            if (lines.size() >= maxLines) {
                return lines;
            }
        }
    }
    if (!current.empty() && lines.size() < maxLines) {
        lines.push_back(current);
    }
    return lines;
}

// Rimuove l'eventuale chiave da una risposta prima di mostrarla o loggarla.
std::string redactKey(const std::string& text, const std::string& key) {
    if (key.empty()) {
        return text;
    }
    std::string out = text;
    size_t pos = 0;
    while ((pos = out.find(key, pos)) != std::string::npos) {
        out.replace(pos, key.size(), "[key]");
        pos += 5;
    }
    return out;
}

std::string cleanResponse(const std::string& raw, const std::string& key) {
    std::string out;
    bool inTag = false;
    for (char c : raw) {
        if (c == '<') {
            inTag = true;
            continue;
        }
        if (c == '>') {
            inTag = false;
            continue;
        }
        if (inTag) {
            continue;
        }
        if (c == '\r' || c == '\n' || c == '\t') {
            c = ' ';
        }
        if (static_cast<unsigned char>(c) >= 0x20 &&
            static_cast<unsigned char>(c) < 0x7F) {
            out.push_back(c);
        }
    }
    std::string collapsed;
    bool space = false;
    for (char c : out) {
        if (c == ' ') {
            if (!space && !collapsed.empty()) {
                collapsed.push_back(' ');
            }
            space = true;
        } else {
            collapsed.push_back(c);
            space = false;
        }
    }
    collapsed = trim(redactKey(collapsed, key));
    if (collapsed.size() > 240) {
        collapsed = collapsed.substr(0, 239) + "~";
    }
    return collapsed;
}

// ---------------------------------------------------------------------------
//  Percorsi e chiave
// ---------------------------------------------------------------------------

bool useSD() {
    return storage::hasSD();
}

std::string capturePath(const std::string& name) {
    return pcaptools::joinPath(pcaptools::kCaptureDir, name);
}

bool normalizeKey(std::string& key) {
    key = trim(key);
    if (key.size() != 32) {
        return false;
    }
    for (char& c : key) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!std::isxdigit(u)) {
            return false;
        }
        c = lowerChar(c);
    }
    return true;
}

bool loadKey(std::string& key) {
    if (!storage::readText(kKeyPath, key, 128, useSD())) {
        return false;
    }
    return normalizeKey(key);
}

bool saveKey(const std::string& key) {
    storage::mkdir(kDir, useSD());
    return storage::writeText(kKeyPath, key + "\n", useSD());
}

// ---------------------------------------------------------------------------
//  Lettura righe da file (senza String, un buffer riusato)
// ---------------------------------------------------------------------------

class LineReader {
public:
    explicit LineReader(File& file) : file_(file) {}

    bool next(std::string& out, size_t maxLen) {
        out.clear();
        bool any = false;
        for (;;) {
            if (pos_ >= len_) {
                const int got = file_.read(buffer_, sizeof(buffer_));
                len_ = got > 0 ? static_cast<size_t>(got) : 0;
                pos_ = 0;
                if (len_ == 0) {
                    if (any && !out.empty() && out.back() == '\r') {
                        out.pop_back();
                    }
                    return any;
                }
            }
            const char c = static_cast<char>(buffer_[pos_++]);
            any = true;
            if (c == '\n') {
                if (!out.empty() && out.back() == '\r') {
                    out.pop_back();
                }
                return true;
            }
            if (out.size() < maxLen) {
                out.push_back(c);
            }
        }
    }

private:
    File& file_;
    uint8_t buffer_[256] = {};
    size_t pos_ = 0;
    size_t len_ = 0;
};

// ---------------------------------------------------------------------------
//  Potfile: indice leggero delle password trovate
// ---------------------------------------------------------------------------

// Estrae un MAC da 12 cifre hex, con o senza ':' / '-'.
bool parseMacToken(const std::string& token, uint64_t& mac) {
    std::string hex;
    hex.reserve(12);
    for (char c : token) {
        if (c == ':' || c == '-' || c == '.') {
            continue;
        }
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
        hex.push_back(lowerChar(c));
    }
    if (hex.size() != 12) {
        return false;
    }
    mac = std::strtoull(hex.c_str(), nullptr, 16);
    return true;
}

// MAC all'inizio della riga: accetta 12 hex, aa:bb:cc:dd:ee:ff o
// aabb.ccdd.eeff e restituisce quanti caratteri ha consumato.
bool macAtStart(const std::string& text, uint64_t& mac, size_t& consumed) {
    if (text.size() >= 17 &&
        (text[2] == ':' || text[2] == '-') &&
        (text[5] == ':' || text[5] == '-') &&
        (text[8] == ':' || text[8] == '-') &&
        (text[11] == ':' || text[11] == '-') &&
        (text[14] == ':' || text[14] == '-') &&
        parseMacToken(text.substr(0, 17), mac)) {
        consumed = 17;
        return true;
    }
    if (text.size() >= 14 && text[4] == '.' && text[9] == '.' &&
        parseMacToken(text.substr(0, 14), mac)) {
        consumed = 14;
        return true;
    }
    if (text.size() >= 12 && parseMacToken(text.substr(0, 12), mac)) {
        consumed = 12;
        return true;
    }
    return false;
}

// BSSID del record: primo campo nel formato reale
// "bssid:station:ssid:password", campo 3 nelle righe WPA*.
bool bssidFromPotLine(const std::string& line, uint64_t& bssid) {
    const std::string text = trim(line);
    if (text.empty() || text[0] == '#') {
        return false;
    }
    if (text.rfind("WPA*", 0) == 0) {
        size_t start = 0;
        for (int field = 0; field <= 3; ++field) {
            const size_t end = text.find('*', start);
            if (field == 3) {
                const std::string token =
                    text.substr(start, end == std::string::npos ? std::string::npos
                                                                : end - start);
                return parseMacToken(token, bssid);
            }
            if (end == std::string::npos) {
                return false;
            }
            start = end + 1;
        }
        return false;
    }
    size_t consumed = 0;
    if (!macAtStart(text, bssid, consumed) || consumed >= text.size() ||
        text[consumed] != ':') {
        return false;
    }
    return true;
}

// Password: ultimo campo del formato bssid:station:ssid:password, oppure
// tutto dopo il primo ':' nelle righe WPA* (l'hash non contiene ':').
bool potfileLinePassword(const std::string& line, std::string& out) {
    const std::string text = trim(line);
    if (text.rfind("WPA*", 0) == 0) {
        const size_t colon = text.find(':');
        if (colon == std::string::npos || colon + 1 > text.size()) {
            return false;
        }
        out = text.substr(colon + 1);
        return true;
    }
    // Formato reale: BSSID (12 hex) ':' station (12 hex) ':' ssid ':' password.
    // L'SSID puo' contenere ':', quindi la password e' l'ultimo campo.
    uint64_t bssid = 0;
    uint64_t station = 0;
    if (text.size() > 26 && text[12] == ':' && text[25] == ':' &&
        parseMacToken(text.substr(0, 12), bssid) &&
        parseMacToken(text.substr(13, 12), station)) {
        const size_t last = text.rfind(':');
        if (last != std::string::npos && last >= 26) {
            out = text.substr(last + 1);
            return true;
        }
    }

    // Variante con BSSID/station in notazione MAC (aa:bb:cc:dd:ee:ff).
    if (text.size() > 36 && text[17] == ':' && text[35] == ':' &&
        parseMacToken(text.substr(0, 17), bssid) &&
        parseMacToken(text.substr(18, 17), station)) {
        const size_t last = text.rfind(':');
        if (last != std::string::npos && last >= 36) {
            out = text.substr(last + 1);
            return true;
        }
    }

    const size_t first = text.find(':');
    if (first == std::string::npos) {
        return false;
    }
    const size_t second = text.find(':', first + 1);
    if (second == std::string::npos) {
        return false;
    }
    const size_t third = text.find(':', second + 1);
    if (third == std::string::npos) {
        return false;
    }
    out = text.substr(third + 1);
    return true;
}

bool looksLikeHtmlDocument(const std::string& text) {
    const std::string lower = lowerCopy(text.substr(0, 1024));
    return lower.find("<!doctype") != std::string::npos ||
           lower.find("<html") != std::string::npos ||
           lower.find("<head") != std::string::npos ||
           lower.find("<body") != std::string::npos;
}

bool hasErrorWords(const std::string& lower) {
    return lower.find("not a valid") != std::string::npos ||
           lower.find("no valid") != std::string::npos ||
           lower.find("invalid") != std::string::npos ||
           lower.find("error") != std::string::npos ||
           lower.find("rejected") != std::string::npos ||
           lower.find("bad capture") != std::string::npos ||
           lower.find("not authorized") != std::string::npos ||
           lower.find("no capture") != std::string::npos ||
           lower.find("failed") != std::string::npos ||
           lower.find("too large") != std::string::npos ||
           lower.find("forbidden") != std::string::npos ||
           lower.find("not submitted") != std::string::npos ||
           lower.find("not accepted") != std::string::npos ||
           lower.find("not uploaded") != std::string::npos;
}

bool hasSuccessWords(const std::string& lower) {
    if (lower == "ok" || lower.rfind("ok ", 0) == 0 || lower.rfind("ok.", 0) == 0) {
        return true;
    }
    return lower.find("accepted") != std::string::npos ||
           lower.find("queued") != std::string::npos ||
           lower.find("submitted") != std::string::npos ||
           lower.find("uploaded") != std::string::npos ||
           lower.find("success") != std::string::npos ||
           lower.find("received") != std::string::npos ||
           lower.find("thank") != std::string::npos ||
           lower.find("processing") != std::string::npos ||
           lower.find("stored") != std::string::npos ||
           lower.find("added") != std::string::npos ||
           lower.find("completed") != std::string::npos;
}

struct CrackIndex {
    std::vector<uint64_t> bssids;
    uint32_t lines = 0;
    bool available = false;
    bool truncated = false;

    bool build() {
        bssids.clear();
        lines = 0;
        available = false;
        truncated = false;
        if (!storage::exists(kPotfilePath, useSD())) {
            return false;
        }
        File file = storage::openRead(kPotfilePath, useSD());
        if (!file) {
            return false;
        }
        bssids.reserve(256);
        LineReader reader(file);
        std::string line;
        size_t scanned = 0;
        while (reader.next(line, 512)) {
            scanned += line.size() + 1;
            if (!line.empty() && line[0] != '#') {
                uint64_t bssid = 0;
                std::string password;
                if (bssidFromPotLine(line, bssid) &&
                    potfileLinePassword(line, password)) {
                    ++lines;
                    if (bssids.size() < kCrackSlots) {
                        bssids.push_back(bssid);
                    } else {
                        truncated = true;
                        break;
                    }
                }
            }
            if (scanned > kMaxPotfileScan) {
                truncated = true;
                break;
            }
        }
        file.close();
        std::sort(bssids.begin(), bssids.end());
        bssids.erase(std::unique(bssids.begin(), bssids.end()), bssids.end());
        available = true;
        return true;
    }

    bool contains(uint64_t bssid) const {
        return std::binary_search(bssids.begin(), bssids.end(), bssid);
    }
};

// Cerca la password associata al BSSID di una riga hash nel potfile.
bool potfilePassword(const std::string& hashLine, std::string& out) {
    uint64_t wanted = 0;
    if (!bssidFromPotLine(hashLine, wanted)) {
        return false;
    }
    if (!storage::exists(kPotfilePath, useSD())) {
        return false;
    }
    File file = storage::openRead(kPotfilePath, useSD());
    if (!file) {
        return false;
    }
    LineReader reader(file);
    std::string line;
    size_t scanned = 0;
    bool found = false;
    while (reader.next(line, 512)) {
        scanned += line.size() + 1;
        uint64_t bssid = 0;
        if (bssidFromPotLine(line, bssid) && bssid == wanted &&
            potfileLinePassword(line, out)) {
            found = true;
            break;
        }
        if (scanned > kMaxPotfileScan) {
            break;
        }
    }
    file.close();
    return found;
}

bool potfileLooksValid(const std::string& sample) {
    if (sample.empty()) {
        return true;
    }
    if (looksLikeHtmlDocument(sample)) {
        return false;
    }
    bool sawData = false;
    size_t pos = 0;
    while (pos < sample.size()) {
        const size_t end = sample.find('\n', pos);
        const std::string line =
            trim(sample.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
        if (!line.empty() && line[0] != '#') {
            sawData = true;
            uint64_t bssid = 0;
            std::string password;
            if (bssidFromPotLine(line, bssid) &&
                potfileLinePassword(line, password)) {
                if (line.rfind("WPA*", 0) == 0 ||
                    std::count(line.begin(), line.end(), ':') >= 3) {
                    return true;
                }
            }
            if (hasErrorWords(lowerCopy(line))) {
                return false;
            }
        }
        if (end == std::string::npos) {
            break;
        }
        pos = end + 1;
    }
    // Solo righe vuote/commenti: potfile valido senza risultati.
    return !sawData;
}

// ---------------------------------------------------------------------------
//  Registro upload
// ---------------------------------------------------------------------------

bool parseUploadedLine(const std::string& line, std::string& name, size_t& size) {
    const size_t bar = line.find('|');
    if (bar == std::string::npos || bar == 0) {
        return false;
    }
    name = line.substr(0, bar);
    size = static_cast<size_t>(std::strtoul(line.c_str() + bar + 1, nullptr, 10));
    return true;
}

bool isUploaded(const std::string& name, size_t size, const std::string& registry) {
    size_t pos = 0;
    while (pos < registry.size()) {
        const size_t end = registry.find('\n', pos);
        const std::string line =
            trim(registry.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
        std::string entryName;
        size_t entrySize = 0;
        if (parseUploadedLine(line, entryName, entrySize) && entryName == name &&
            entrySize == size) {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        pos = end + 1;
    }
    return false;
}

bool markUploaded(const std::string& name, size_t size) {
    const bool sd = useSD();
    std::string registry;
    if (storage::exists(kUploadedPath, sd) &&
        !storage::readText(kUploadedPath, registry, kMaxUploadedBytes, sd)) {
        return false;  // registro illeggibile: non lo si sovrascrive
    }

    std::vector<std::string> kept;
    kept.reserve(16);
    size_t pos = 0;
    while (pos < registry.size() && kept.size() < kMaxUploadedEntries) {
        const size_t end = registry.find('\n', pos);
        const std::string line =
            trim(registry.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
        std::string entryName;
        size_t entrySize = 0;
        if (!line.empty() && parseUploadedLine(line, entryName, entrySize) &&
            entryName != name) {
            kept.push_back(line);
        }
        if (end == std::string::npos) {
            break;
        }
        pos = end + 1;
    }

    std::string out;
    for (const auto& line : kept) {
        out += line;
        out += '\n';
    }
    out += name;
    out += '|';
    out += std::to_string(static_cast<unsigned long>(size));
    out += '\n';
    storage::mkdir(kDir, sd);
    return storage::writeText(kUploadedPath, out, sd);
}

// ---------------------------------------------------------------------------
//  Catture
// ---------------------------------------------------------------------------

struct Capture {
    std::string name;
    size_t size = 0;
    bool hc = false;
    bool uploaded = false;
    bool cracked = false;
};

bool captureCracked(const std::string& name, const CrackIndex& index, std::string* password) {
    if (!index.available || index.bssids.empty()) {
        return false;
    }
    std::string target = name;
    if (!pcaptools::isHc22000Name(name)) {
        target = pcaptools::hc22000Path(name);
        if (!storage::exists(capturePath(target), useSD())) {
            return false;
        }
    }
    File file = storage::openRead(capturePath(target), useSD());
    if (!file) {
        return false;
    }
    LineReader reader(file);
    std::string line;
    std::string match;
    uint32_t count = 0;
    while (count < kMaxCaptureHashLines && reader.next(line, 512)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty()) {
            continue;
        }
        ++count;
        uint64_t bssid = 0;
        if (bssidFromPotLine(trimmed, bssid) && index.contains(bssid)) {
            match = trimmed;
            break;
        }
    }
    file.close();
    if (match.empty()) {
        return false;
    }
    if (password != nullptr) {
        potfilePassword(match, *password);
    }
    return true;
}

bool loadCaptures(std::vector<Capture>& items, bool scanPasswords) {
    items.clear();
    if (!storage::hasSD() && !storage::hasLittleFS()) {
        return false;
    }
    const bool sd = useSD();
    const std::vector<storage::Entry> entries =
        storage::listEntries(pcaptools::kCaptureDir, sd);
    std::string registry;
    storage::readText(kUploadedPath, registry, kMaxUploadedBytes, sd);

    CrackIndex index;
    if (scanPasswords) {
        index.build();
    }

    for (const auto& entry : entries) {
        if (entry.isDir) {
            continue;
        }
        const bool hc = pcaptools::isHc22000Name(entry.name);
        if (!hc && !pcaptools::isPcapName(entry.name)) {
            continue;
        }
        Capture item;
        item.name = entry.name;
        item.size = entry.size;
        item.hc = hc;
        item.uploaded = isUploaded(item.name, item.size, registry);
        item.cracked = scanPasswords && captureCracked(item.name, index, nullptr);
        items.push_back(std::move(item));
    }

    std::sort(items.begin(), items.end(), [](const Capture& a, const Capture& b) {
        if (a.hc != b.hc) {
            return a.hc;
        }
        return a.name < b.name;
    });
    return true;
}

uint32_t countHashLines(const std::string& path) {
    File file = storage::openRead(path, useSD());
    if (!file) {
        return 0;
    }
    LineReader reader(file);
    std::string line;
    uint32_t lines = 0;
    while (lines < 2048 && reader.next(line, 512)) {
        if (!trim(line).empty()) {
            ++lines;
        }
    }
    file.close();
    return lines;
}

// ---------------------------------------------------------------------------
//  Rete: TLS, HTTP/1.1 manuale in streaming
// ---------------------------------------------------------------------------

class NetReader {
public:
    NetReader(WiFiClient& client, uint32_t timeoutMs)
        : client_(client), timeoutMs_(timeoutMs), last_(millis()) {}

    bool readByte(uint8_t& out) {
        for (;;) {
            if (client_.available() > 0) {
                const int value = client_.read();
                if (value >= 0) {
                    out = static_cast<uint8_t>(value);
                    last_ = millis();
                    return true;
                }
            }
            if (!client_.connected()) {
                return false;
            }
            if (millis() - last_ > timeoutMs_) {
                timedOut_ = true;
                return false;
            }
            delay(2);
        }
    }

    bool readLine(std::string& out, size_t maxLen) {
        out.clear();
        for (;;) {
            uint8_t c = 0;
            if (!readByte(c)) {
                return false;
            }
            if (c == '\n') {
                if (!out.empty() && out.back() == '\r') {
                    out.pop_back();
                }
                return true;
            }
            if (out.size() >= maxLen) {
                return false;
            }
            out.push_back(static_cast<char>(c));
        }
    }

    bool readExact(uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            if (!readByte(data[i])) {
                return false;
            }
        }
        return true;
    }

    size_t readSome(uint8_t* data, size_t cap) {
        while (client_.available() == 0) {
            if (!client_.connected()) {
                return 0;
            }
            if (millis() - last_ > timeoutMs_) {
                timedOut_ = true;
                return 0;
            }
            delay(2);
        }
        const size_t available = static_cast<size_t>(client_.available());
        const size_t want = std::min(available, cap);
        const int got = client_.read(data, want);
        if (got > 0) {
            last_ = millis();
            return static_cast<size_t>(got);
        }
        return 0;
    }

    bool timedOut() const { return timedOut_; }

private:
    WiFiClient& client_;
    uint32_t timeoutMs_;
    uint32_t last_;
    bool timedOut_ = false;
};

struct ResponseHead {
    int status = 0;
    long contentLength = -1;
    bool chunked = false;
    std::string location;
    std::string contentType;
};

bool readResponseHead(NetReader& reader, ResponseHead& head) {
    head = ResponseHead{};
    std::string line;
    if (!reader.readLine(line, 256) || line.rfind("HTTP/", 0) != 0) {
        return false;
    }
    const size_t space = line.find(' ');
    if (space == std::string::npos) {
        return false;
    }
    head.status = std::atoi(line.c_str() + space + 1);
    if (head.status < 100 || head.status > 599) {
        return false;
    }
    for (int i = 0; i < 48; ++i) {
        if (!reader.readLine(line, 512)) {
            return false;
        }
        if (line.empty()) {
            return true;
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string name = trim(line.substr(0, colon));
        const std::string value = trim(line.substr(colon + 1));
        if (equalsNoCase(name, "Content-Length")) {
            head.contentLength = std::atol(value.c_str());
        } else if (equalsNoCase(name, "Transfer-Encoding")) {
            if (containsNoCase(value, "chunked")) {
                head.chunked = true;
            }
        } else if (equalsNoCase(name, "Location")) {
            head.location = value;
        } else if (equalsNoCase(name, "Content-Type")) {
            head.contentType = value;
        }
    }
    return false;
}

template <typename Sink>
bool readBodyDecoded(NetReader& reader, const ResponseHead& head, size_t maxBytes,
                     Sink sink, size_t& total) {
    total = 0;
    uint8_t buffer[512];
    auto emit = [&](const uint8_t* data, size_t len) -> bool {
        if (len > maxBytes || total + len > maxBytes) {
            return false;
        }
        if (!sink(data, len)) {
            return false;
        }
        total += len;
        return true;
    };

    if (head.chunked) {
        for (;;) {
            std::string line;
            if (!reader.readLine(line, 128)) {
                return false;
            }
            const size_t semi = line.find(';');
            const std::string token =
                trim(line.substr(0, semi == std::string::npos ? std::string::npos : semi));
            if (token.empty()) {
                return false;
            }
            char* end = nullptr;
            const unsigned long chunk = std::strtoul(token.c_str(), &end, 16);
            if (end == token.c_str()) {
                return false;
            }
            if (chunk == 0) {
                for (int i = 0; i < 16; ++i) {
                    if (!reader.readLine(line, 256) || line.empty()) {
                        break;
                    }
                }
                return true;
            }
            if (chunk > maxBytes || total + chunk > maxBytes) {
                return false;
            }
            size_t left = static_cast<size_t>(chunk);
            while (left > 0) {
                const size_t want = std::min(left, sizeof(buffer));
                if (!reader.readExact(buffer, want)) {
                    return false;
                }
                if (!emit(buffer, want)) {
                    return false;
                }
                left -= want;
            }
            if (!reader.readLine(line, 8)) {
                return false;
            }
        }
    }

    if (head.contentLength >= 0) {
        size_t left = static_cast<size_t>(head.contentLength);
        if (left > maxBytes) {
            return false;
        }
        while (left > 0) {
            const size_t got = reader.readSome(buffer, std::min(left, sizeof(buffer)));
            if (got == 0) {
                return false;
            }
            if (!emit(buffer, got)) {
                return false;
            }
            left -= got;
        }
        return true;
    }

    for (;;) {
        const size_t got = reader.readSome(buffer, sizeof(buffer));
        if (got == 0) {
            return !reader.timedOut();
        }
        if (!emit(buffer, got)) {
            return false;
        }
    }
}

struct BodySample {
    std::string text;
    size_t total = 0;
    bool complete = false;
};

BodySample readBodySample(WiFiClient& client, const ResponseHead& head, size_t maxBytes) {
    BodySample sample;
    NetReader reader(client, 8000);
    sample.complete = readBodyDecoded(
        reader, head, maxBytes,
        [&](const uint8_t* data, size_t len) -> bool {
            sample.text.append(reinterpret_cast<const char*>(data), len);
            return true;
        },
        sample.total);
    return sample;
}

std::string readSnippet(WiFiClient& client, const ResponseHead& head, size_t maxBytes) {
    return readBodySample(client, head, maxBytes).text;
}

bool sendAll(WiFiClient& client, const uint8_t* data, size_t len) {
    size_t sent = 0;
    uint32_t last = millis();
    while (sent < len) {
        const size_t want = std::min<size_t>(len - sent, 2048);
        const size_t written = client.write(data + sent, want);
        if (written > 0) {
            sent += written;
            last = millis();
            continue;
        }
        if (!client.connected()) {
            return false;
        }
        if (millis() - last > 15000) {
            return false;
        }
        delay(2);
    }
    return true;
}

bool sendString(WiFiClient& client, const std::string& text) {
    return sendAll(client, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

enum class OpenStatus { Ok, Cancelled, Error };

// Solo errori X.509 / data: DNS, TCP, timeout e OOM non devono mai portare a
// setInsecure(). L'allocazione X.509 e gli errori SSL di memoria sono esclusi.
bool isCertificateError(int code) {
    if (code == 0) {
        return false;
    }
    if (code == MBEDTLS_ERR_X509_ALLOC_FAILED) {
        return false;
    }
    if (code == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED ||
        code == MBEDTLS_ERR_SSL_BAD_HS_CERTIFICATE) {
        return true;
    }
    return code <= -0x2700 && code > -0x4000;
}

// Apre il TLS. La CA viene ritentata a ogni operazione; il fallback insicuro
// scatta solo per un errore di certificato/data, con conferma esplicita.
OpenStatus openTls(WiFiClientSecure& client, bool& verified, std::string& error) {
    verified = false;
    error.clear();
    client.setHandshakeTimeout(kHandshakeTimeoutMs);
    client.setTimeout(20);
    client.setNoDelay(true);

    client.setCACert(kRootCa);
    if (client.connect(kHost, kPort) == 1) {
        g_caFailed = false;
        verified = true;
        return OpenStatus::Ok;
    }

    char detailBuffer[96] = {};
    const int code = client.lastError(detailBuffer, sizeof(detailBuffer));
    client.stop();
    const std::string detail =
        code != 0 && detailBuffer[0] != '\0' ? std::string(detailBuffer)
                                             : std::string("DNS/TCP o timeout");
    if (!isCertificateError(code)) {
        g_caFailed = false;
        error = detail;
        logger::warn("WPA-SEC: TLS non disponibile: %s", error.c_str());
        return OpenStatus::Error;
    }

    g_caFailed = true;
    logger::warn("WPA-SEC: CA fallita (%d): %s", code, detail.c_str());
    if (!g_insecureAck) {
        std::vector<std::string> lines;
        lines.push_back("!TLS senza verifica certificato");
        if (g_clockFailed) {
            lines.push_back("Orologio non sincronizzato:");
            lines.push_back("certificato non verificabile.");
        } else {
            lines.push_back("Certificato non valido:");
            lines.push_back(shorten(detail, 34));
        }
        lines.push_back("Il traffico resta cifrato ma non");
        lines.push_back("autenticato: un MITM puo' leggere");
        lines.push_back("la chiave API. Continuare?");
        if (!ui::confirm("TLS senza verifica", lines, kModuleName)) {
            error = "TLS non verificato: annullato.";
            return OpenStatus::Cancelled;
        }
        g_insecureAck = true;
    }

    client.setInsecure();
    if (client.connect(kHost, kPort) == 1) {
        verified = false;
        return OpenStatus::Ok;
    }
    char fallbackBuffer[96] = {};
    const int fallbackCode = client.lastError(fallbackBuffer, sizeof(fallbackBuffer));
    if (fallbackCode != 0 && fallbackCode != code && fallbackBuffer[0] != '\0') {
        error = fallbackBuffer;
    } else {
        error = "TLS: connessione non riuscita (DNS/TCP/timeout).";
    }
    client.stop();
    return OpenStatus::Error;
}

// Costruisce il path da un Location, accettando solo lo stesso host in HTTPS.
bool redirectPath(const std::string& location, std::string& path) {
    const std::string loc = trim(location);
    if (loc.empty() || hasControlChars(loc)) {
        return false;
    }
    if (startsWithNoCase(loc, "https://")) {
        const size_t authorityStart = 8;
        const size_t slash = loc.find('/', authorityStart);
        std::string authority = loc.substr(authorityStart,
                                           slash == std::string::npos
                                               ? std::string::npos
                                               : slash - authorityStart);
        std::string host = authority;
        const size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            host = authority.substr(0, colon);
            const int port = std::atoi(authority.c_str() + colon + 1);
            if (port != kPort) {
                return false;
            }
        }
        if (!equalsNoCase(host, kHost)) {
            return false;
        }
        path = slash == std::string::npos ? "/" : loc.substr(slash);
        return true;
    }
    if (loc[0] == '/') {
        path = loc;
        return true;
    }
    return false;
}

bool isRedirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

std::string makeBoundary() {
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "----ORBITwpaSec%08lx%08lx",
                  static_cast<unsigned long>(esp_random()),
                  static_cast<unsigned long>(esp_random()));
    return buffer;
}

std::string safeFilename(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (c == '"' || c == '\r' || c == '\n' || c == '\\') {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
    }
    if (out.empty()) {
        out = "capture.hc22000";
    }
    return shorten(out, 96);
}

// ---------------------------------------------------------------------------
//  UI
// ---------------------------------------------------------------------------

void drawWork(const std::string& title, const std::string& subject,
              const std::string& phase, float fraction, const std::string& detail,
              bool insecure = false) {
    display::clear(display::kPaper);
    display::header(title, kModuleName);
    display::text(4, 24, shorten(subject, 38), display::kInk, 0);
    display::text(4, 38, shorten(phase, 38), display::kMuted, 0);
    const float clamped = std::max(0.0f, std::min(1.0f, fraction));
    display::progressBar(12, 58, display::width() - 24, 8, clamped, display::kAccent);
    char percent[8];
    std::snprintf(percent, sizeof(percent), "%u%%",
                  static_cast<unsigned>(clamped * 100.0f + 0.5f));
    display::textCentered(72, percent, display::kInk, 2);
    if (!detail.empty()) {
        display::textCentered(88, shorten(detail, 38), display::kMuted, 0);
    }
    display::footer(insecure ? "TLS NON VERIFICATO  ESC=annulla" : "ESC = annulla");
    display::push();
}

void drawClock(uint32_t elapsedMs) {
    const float fraction = std::min(1.0f, static_cast<float>(elapsedMs) / 8000.0f);
    display::clear(display::kPaper);
    display::header("Orologio", kModuleName);
    display::textCentered(32, "Sincronizzazione NTP", display::kInk, 0);
    display::textCentered(48, "per verificare il certificato", display::kMuted, 0);
    display::progressBar(24, 70, display::width() - 48, 8, fraction, display::kAccent);
    display::textCentered(88, std::to_string(static_cast<unsigned>(fraction * 100)) + "%",
                          display::kMuted, 0);
    display::footer("ESC = salta (TLS senza verifica)");
    display::push();
}

bool ensureClock() {
    if (time(nullptr) > 1700000000) {
        return true;
    }
    if (g_clockAttempted) {
        return false;
    }
    g_clockAttempted = true;
    drawClock(0);
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    const uint32_t start = millis();
    uint32_t lastDraw = 0;
    navigation::waitRelease();
    while (millis() - start < 8000) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }
        if (time(nullptr) > 1700000000) {
            logger::info("WPA-SEC: orologio sincronizzato");
            return true;
        }
        if (millis() - lastDraw > 150) {
            lastDraw = millis();
            drawClock(millis() - start);
        }
        delay(50);
    }
    logger::warn("WPA-SEC: NTP non disponibile, TLS senza verifica");
    g_clockFailed = true;
    return false;
}

bool prepareNetwork() {
    g_networkUsed = true;
    if (!wifisetup::ensureConnected(true, kModuleName)) {
        ui::message(kModuleName,
                    {"!Serve una connessione WiFi.", "Configura la rete da",
                     "Impostazioni > Rete WiFi."},
                    kModuleName);
        return false;
    }

    const uint32_t freeHeap = ESP.getFreeHeap();
    const uint32_t maxBlock = ESP.getMaxAllocHeap();
    if (freeHeap < kMinFreeHeap || maxBlock < kMinMaxBlock) {
        std::vector<std::string> lines;
        lines.push_back("!Memoria insufficiente per TLS.");
        lines.push_back("Libera: " + formatHeap(freeHeap));
        lines.push_back("Blocco max: " + formatHeap(maxBlock));
        lines.push_back("Servono almeno 40K liberi e");
        lines.push_back("un blocco contiguo da 32K.");
        lines.push_back("Riavvia il device e riprova.");
        ui::message("Heap insufficiente", lines, kModuleName);
        return false;
    }
    ensureClock();
    return true;
}

// ---------------------------------------------------------------------------
//  Upload multipart
// ---------------------------------------------------------------------------

struct UploadResult {
    bool ok = false;
    bool cancelled = false;
    bool tlsVerified = false;
    int status = 0;
    std::string note;
    std::string error;
    std::string response;
};

UploadResult uploadFile(const Capture& item, const std::string& key) {
    UploadResult result;
    const bool sd = useSD();
    File file = storage::openRead(capturePath(item.name), sd);
    if (!file) {
        result.note = "File non leggibile.";
        return result;
    }
    const size_t fileSize = static_cast<size_t>(file.size());
    if (fileSize == 0) {
        file.close();
        result.note = "File vuoto.";
        return result;
    }

    std::string remotePath = std::string("/?submit&key=") + key;
    bool usePost = true;
    powertimer::Hold hold;

    for (int hop = 0; hop < 3; ++hop) {
        drawWork("Upload", item.name, "Connessione TLS", 0.0f, "", g_caFailed);
        WiFiClientSecure client;
        bool verified = false;
        std::string tlsError;
        const OpenStatus state = openTls(client, verified, tlsError);
        result.tlsVerified = verified;
        if (state == OpenStatus::Cancelled) {
            result.cancelled = true;
            result.note = "TLS non verificato: annullato.";
            result.error = tlsError;
            file.close();
            return result;
        }
        if (state == OpenStatus::Error) {
            result.note = "Connessione TLS non riuscita.";
            result.error = tlsError;
            file.close();
            return result;
        }

        std::string request;
        std::string prefix;
        std::string suffix;
        size_t bodyLength = 0;
        if (usePost) {
            if (!file.seek(0)) {
                client.stop();
                file.close();
                result.note = "File non riavvolgibile.";
                return result;
            }
            const std::string boundary = makeBoundary();
            prefix = "--" + boundary + "\r\n";
            prefix += "Content-Disposition: form-data; name=\"file\"; filename=\"";
            prefix += safeFilename(item.name);
            prefix += "\"\r\nContent-Type: application/octet-stream\r\n\r\n";
            suffix = "\r\n--" + boundary + "--\r\n";
            bodyLength = prefix.size() + fileSize + suffix.size();

            request = "POST " + remotePath + " HTTP/1.1\r\n";
            request += std::string("Host: ") + kHost + "\r\n";
            request += "User-Agent: ORBIT-OS-WPASEC\r\n";
            request += "Accept: */*\r\n";
            request += "Connection: close\r\n";
            request += "Cookie: key=" + key + "\r\n";
            request += "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n";
            request += "Content-Length: " +
                       std::to_string(static_cast<unsigned long>(bodyLength)) + "\r\n\r\n";
        } else {
            request = "GET " + remotePath + " HTTP/1.1\r\n";
            request += std::string("Host: ") + kHost + "\r\n";
            request += "User-Agent: ORBIT-OS-WPASEC\r\n";
            request += "Accept: */*\r\n";
            request += "Connection: close\r\n";
            request += "Cookie: key=" + key + "\r\n\r\n";
        }

        bool failed = false;
        if (!sendString(client, request)) {
            failed = true;
            result.note = "Invio richiesta non riuscito.";
        }
        if (!failed && usePost) {
            if (!sendString(client, prefix)) {
                failed = true;
                result.note = "Invio multipart non riuscito.";
            }
            size_t sent = 0;
            uint8_t buffer[kUploadChunk];
            uint32_t lastDraw = 0;
            while (!failed && sent < fileSize) {
                const int got = file.read(buffer, sizeof(buffer));
                if (got <= 0) {
                    failed = true;
                    result.note = "Lettura file non riuscita.";
                    break;
                }
                if (!sendAll(client, buffer, static_cast<size_t>(got))) {
                    failed = true;
                    result.note = "Connessione persa durante l'invio.";
                    break;
                }
                sent += static_cast<size_t>(got);
                navigation::update();
                if (navigation::poll().type == navigation::Event::Back) {
                    result.cancelled = true;
                    result.note = "Upload annullato.";
                    failed = true;
                    break;
                }
                if (millis() - lastDraw > 250 || sent == fileSize) {
                    lastDraw = millis();
                    drawWork("Upload", item.name, "Invio dati",
                             static_cast<float>(sent) / static_cast<float>(fileSize),
                             formatSize(sent) + " / " + formatSize(fileSize), g_caFailed);
                }
            }
            if (!failed && !sendString(client, suffix)) {
                failed = true;
                result.note = "Chiusura multipart non riuscita.";
            }
        }

        if (failed) {
            client.stop();
            file.close();
            return result;
        }

        drawWork("Upload", item.name, "Risposta del server", 1.0f, "", g_caFailed);
        NetReader reader(client, kNetTimeoutMs);
        ResponseHead head;
        if (!readResponseHead(reader, head)) {
            client.stop();
            file.close();
            result.note = "Risposta HTTP non valida.";
            return result;
        }
        result.status = head.status;

        if (isRedirect(head.status)) {
            std::string next;
            if (!redirectPath(head.location, next)) {
                client.stop();
                file.close();
                result.note = "Redirect non consentito.";
                return result;
            }
            client.stop();
            remotePath = next;
            usePost = (head.status == 307 || head.status == 308);
            continue;
        }

        const bool tooLarge =
            head.contentLength > static_cast<long>(kUploadResponseMax);
        const BodySample sample = readBodySample(client, head, kUploadResponseMax);
        client.stop();
        result.response = cleanResponse(sample.text, key);
        const std::string lower = lowerCopy(result.response);
        const bool httpOk = head.status == 200 || head.status == 201 || head.status == 202;

        if (!sample.complete) {
            result.note = tooLarge ? "Risposta troppo grande."
                                   : "Risposta incompleta o troppo grande.";
        } else if (looksLikeHtmlDocument(sample.text)) {
            result.note = "Il server ha restituito HTML: file lasciato locale.";
        } else if (!httpOk) {
            result.note = "HTTP " + std::to_string(head.status) + ".";
        } else if (hasErrorWords(lower)) {
            result.note = "Il server ha rifiutato la cattura.";
        } else if (!hasSuccessWords(lower)) {
            result.note = "Esito non riconosciuto: file lasciato locale.";
        } else {
            result.ok = true;
        }
        file.close();
        return result;
    }

    file.close();
    result.note = "Troppi redirect.";
    return result;
}

// ---------------------------------------------------------------------------
//  Download potfile
// ---------------------------------------------------------------------------

struct DownloadResult {
    bool ok = false;
    bool cancelled = false;
    bool tlsVerified = false;
    size_t size = 0;
    uint32_t lines = 0;
    uint32_t hashes = 0;
    bool truncated = false;
    std::string note;
    std::string error;
    std::string response;
};

bool backendRename(const std::string& from, const std::string& to, bool sd) {
    if (sd) {
        return storage::hasSD() && SD.rename(from.c_str(), to.c_str());
    }
    return storage::hasLittleFS() && LittleFS.rename(from.c_str(), to.c_str());
}

// Rename diretto (atomico dove il filesystem lo supporta). Se il rename su
// file esistente viene rifiutato, si usa un .bak e si ripristina in caso di
// errore, senza mai lasciare il potfile vecchio sovrascritto a meta'.
bool replaceFileAtomic(const std::string& from, const std::string& to, bool sd) {
    if (backendRename(from, to, sd)) {
        return true;
    }
    const std::string backup = to + ".bak";
    storage::removeFile(backup, sd);
    if (!backendRename(to, backup, sd)) {
        return false;
    }
    if (backendRename(from, to, sd)) {
        storage::removeFile(backup, sd);
        return true;
    }
    backendRename(backup, to, sd);
    return false;
}

DownloadResult downloadPotfile(const std::string& key) {
    DownloadResult result;
    const bool sd = useSD();
    std::string remotePath = std::string("/?api&dl=1&key=") + key;
    powertimer::Hold hold;

    for (int hop = 0; hop < 3; ++hop) {
        drawWork("Potfile", kHost, "Connessione TLS", 0.0f, "", g_caFailed);
        WiFiClientSecure client;
        bool verified = false;
        std::string tlsError;
        const OpenStatus state = openTls(client, verified, tlsError);
        result.tlsVerified = verified;
        if (state == OpenStatus::Cancelled) {
            result.cancelled = true;
            result.note = "TLS non verificato: annullato.";
            result.error = tlsError;
            return result;
        }
        if (state == OpenStatus::Error) {
            result.note = "Connessione TLS non riuscita.";
            result.error = tlsError;
            return result;
        }

        std::string request = "GET " + remotePath + " HTTP/1.1\r\n";
        request += std::string("Host: ") + kHost + "\r\n";
        request += "User-Agent: ORBIT-OS-WPASEC\r\n";
        request += "Accept: text/plain, */*\r\n";
        request += "Connection: close\r\n";
        request += "Cookie: key=" + key + "\r\n\r\n";

        if (!sendString(client, request)) {
            client.stop();
            result.note = "Invio richiesta non riuscito.";
            return result;
        }

        NetReader reader(client, kNetTimeoutMs);
        ResponseHead head;
        if (!readResponseHead(reader, head)) {
            client.stop();
            result.note = "Risposta HTTP non valida.";
            return result;
        }

        if (isRedirect(head.status)) {
            std::string next;
            if (!redirectPath(head.location, next)) {
                client.stop();
                result.note = "Redirect non consentito.";
                return result;
            }
            client.stop();
            remotePath = next;
            continue;
        }

        if (head.status != 200) {
            const std::string raw = readSnippet(client, head, 300);
            client.stop();
            result.response = cleanResponse(raw, key);
            result.note = "HTTP " + std::to_string(head.status) + ".";
            return result;
        }

        if (containsNoCase(head.contentType, "text/html")) {
            const std::string raw = readSnippet(client, head, 300);
            client.stop();
            result.response = cleanResponse(raw, key);
            result.note = "Risposta HTML: chiave non valida o servizio.";
            return result;
        }

        if (head.contentLength > static_cast<long>(kMaxPotfileBytes)) {
            client.stop();
            result.note = "Potfile troppo grande (limite 4 MB).";
            return result;
        }

        storage::mkdir(kDir, sd);
        File out = storage::openWrite(kPotfileNewPath, sd);
        if (!out) {
            client.stop();
            result.note = "Scrittura su SD non riuscita.";
            return result;
        }

        size_t written = 0;
        std::string sample;
        uint32_t lastDraw = 0;
        auto sink = [&](const uint8_t* data, size_t len) -> bool {
            if (out.write(data, len) != len) {
                return false;
            }
            if (sample.size() < 2048) {
                const size_t want = std::min(len, 2048 - sample.size());
                sample.append(reinterpret_cast<const char*>(data), want);
            }
            if (millis() - lastDraw > 200) {
                lastDraw = millis();
                const float fraction =
                    head.contentLength > 0
                        ? static_cast<float>(written) /
                              static_cast<float>(head.contentLength)
                        : 0.0f;
                drawWork("Potfile", kHost, "Download in corso", fraction,
                         formatSize(written) +
                             (head.contentLength > 0
                                  ? " / " + formatSize(static_cast<size_t>(head.contentLength))
                                  : std::string()),
                         g_caFailed);
            }
            navigation::update();
            if (navigation::poll().type == navigation::Event::Back) {
                result.cancelled = true;
                return false;
            }
            return true;
        };

        const bool decoded = readBodyDecoded(reader, head, kMaxPotfileBytes, sink, written);
        out.flush();
        const int writeError = out.getWriteError();
        out.close();
        client.stop();
        const size_t newSize = storage::fileSize(kPotfileNewPath, sd);

        if (result.cancelled) {
            storage::removeFile(kPotfileNewPath, sd);
            result.note = "Download annullato.";
            return result;
        }
        if (!decoded || writeError != 0 || newSize != written) {
            storage::removeFile(kPotfileNewPath, sd);
            result.note = writeError != 0 || newSize != written
                              ? "Scrittura potfile non riuscita."
                              : (reader.timedOut() ? "Timeout del server."
                                                   : "Download incompleto.");
            return result;
        }
        if (!potfileLooksValid(sample)) {
            storage::removeFile(kPotfileNewPath, sd);
            result.response = cleanResponse(sample, key);
            result.note = "Risposta non valida: chiave o servizio.";
            return result;
        }
        if (written == 0) {
            if (!ui::confirm("Potfile vuoto",
                             {"Il server ha restituito 0 byte.",
                              "Salvare un potfile vuoto?", "Procedere?"},
                             kModuleName)) {
                storage::removeFile(kPotfileNewPath, sd);
                result.note = "Potfile vuoto non salvato.";
                return result;
            }
        }
        if (!replaceFileAtomic(kPotfileNewPath, kPotfilePath, sd)) {
            storage::removeFile(kPotfileNewPath, sd);
            result.note = "Salvataggio potfile non riuscito.";
            return result;
        }

        result.ok = true;
        result.size = storage::fileSize(kPotfilePath, sd);
        CrackIndex index;
        if (index.build()) {
            result.lines = index.lines;
            result.hashes = static_cast<uint32_t>(index.bssids.size());
            result.truncated = index.truncated;
        }
        return result;
    }

    result.note = "Troppi redirect.";
    return result;
}

// ---------------------------------------------------------------------------
//  Elenco e dettaglio catture
// ---------------------------------------------------------------------------

void drawList(const std::vector<Capture>& items, int selected, int top, int rows,
              bool forUpload) {
    display::clear(display::kPaper);
    display::header(forUpload ? "Carica catture" : "Catture", kModuleName);

    for (int i = 0; i < rows; ++i) {
        const int index = top + i;
        if (index >= static_cast<int>(items.size())) {
            break;
        }
        const int y = 23 + i * 10;
        const Capture& item = items[static_cast<size_t>(index)];
        const bool isSelected = index == selected;
        if (isSelected) {
            display::g().fillRect(1, y, display::width() - 2, 10, display::kPaperLight);
            display::g().fillRect(1, y, 2, 10, display::kAccent);
        }

        const std::string size = formatSize(item.size);
        std::string tail = size;
        tail += item.uploaded ? " UP" : " --";
        tail += item.cracked ? " PW" : "";
        const int tailWidth = display::textWidth(tail, 0);
        const size_t maxChars = static_cast<size_t>(
            std::max(6, (display::width() - 12 - tailWidth) / 6));
        const std::string label = shorten(item.name, maxChars);

        uint16_t statusColor = display::kMuted;
        if (item.cracked) {
            statusColor = display::kDanger;
        } else if (item.uploaded) {
            statusColor = display::C(display::T_ok);
        }
        const uint16_t nameColor =
            isSelected ? display::kInk : (item.hc ? display::kInk : display::kMuted);
        display::text(4, y + 1, label, nameColor, 0);
        display::text(display::width() - tailWidth - 3, y + 1, tail, statusColor, 0);
    }

    display::footer(forUpload ? "ESC=torna  ;.=nav  ENTER=carica"
                              : "ESC=torna  ;.=nav  ENTER=dettagli");
    display::push();
}

int pickCapture(const std::vector<Capture>& items, bool forUpload) {
    if (items.empty()) {
        return -1;
    }
    constexpr int kRowH = 10;
    const int rows = std::max(1, (display::height() - 21 - 12) / kRowH);
    const int maxTop = std::max(0, static_cast<int>(items.size()) - rows);
    int selected = 0;
    int top = 0;
    bool dirty = true;
    uint32_t lastDraw = 0;

    navigation::waitRelease();
    for (;;) {
        const uint32_t now = millis();
        if (dirty || now - lastDraw > 300) {
            drawList(items, selected, top, rows, forUpload);
            dirty = false;
            lastDraw = now;
        }

        const navigation::EventData event = navigation::poll();
        if (event.type == navigation::Event::Back) {
            navigation::waitRelease();
            return -1;
        }
        if (event.type == navigation::Event::Select) {
            navigation::waitRelease();
            return selected;
        }
        if (event.type == navigation::Event::Up) {
            selected = std::max(0, selected - 1);
            dirty = true;
        } else if (event.type == navigation::Event::Down) {
            selected = std::min(static_cast<int>(items.size()) - 1, selected + 1);
            dirty = true;
        } else if (event.type == navigation::Event::Left) {
            selected = std::max(0, selected - rows);
            dirty = true;
        } else if (event.type == navigation::Event::Right) {
            selected = std::min(static_cast<int>(items.size()) - 1, selected + rows);
            dirty = true;
        }

        if (selected < top) {
            top = selected;
        }
        if (selected >= top + rows) {
            top = selected - rows + 1;
        }
        top = std::max(0, std::min(top, maxTop));
        delay(20);
    }
}

void showDetail(const Capture& item) {
    const bool sd = useSD();
    std::vector<std::string> lines;
    lines.push_back("File: " + shorten(item.name, 30));
    lines.push_back(std::string("Tipo: ") + (item.hc ? "hc22000" : "PCAP") +
                    "  " + formatSize(item.size));
    lines.push_back(std::string("Upload: ") +
                    (item.uploaded ? "inviato" : "non inviato"));
    if (item.hc) {
        lines.push_back("Righe hash: " +
                        std::to_string(countHashLines(capturePath(item.name))));
    } else {
        const std::string sibling = pcaptools::hc22000Path(item.name);
        lines.push_back(storage::exists(capturePath(sibling), sd)
                            ? std::string("Match PW: ") + shorten(sibling, 24)
                            : std::string("Match PW: serve .hc22000"));
    }

    CrackIndex index;
    index.build();
    std::string password;
    if (captureCracked(item.name, index, &password)) {
        lines.push_back(">Password trovata:");
        const auto wrapped = wrapForUi(password.empty() ? "(vuota)" : password, 32, 2);
        for (const auto& line : wrapped) {
            lines.push_back("  " + line);
        }
    } else {
        lines.push_back("Password: non trovata");
    }
    ui::message("Dettaglio", lines, kModuleName);
}

void listCapturesFlow() {
    std::vector<Capture> items;
    if (!loadCaptures(items, true)) {
        ui::message("Catture", {"!Nessun filesystem disponibile."}, kModuleName);
        return;
    }
    if (items.empty()) {
        ui::message("Catture",
                    {"Nessun file in /captures.", "Registra con WiFi > Sniffer",
                     "oppure converti un .pcap."},
                    kModuleName);
        return;
    }
    const int choice = pickCapture(items, false);
    if (choice >= 0) {
        showDetail(items[static_cast<size_t>(choice)]);
    }
}

void uploadFlow() {
    std::string key;
    if (!loadKey(key)) {
        ui::message("Upload", {"!Chiave API assente.", "Impostala dal menu WPA-SEC."},
                    kModuleName);
        return;
    }

    std::vector<Capture> items;
    if (!loadCaptures(items, true)) {
        ui::message("Upload", {"!Nessun filesystem disponibile."}, kModuleName);
        return;
    }
    if (items.empty()) {
        ui::message("Upload", {"Nessun file in /captures."}, kModuleName);
        return;
    }

    const int choice = pickCapture(items, true);
    if (choice < 0) {
        return;
    }
    const Capture& item = items[static_cast<size_t>(choice)];

    std::vector<std::string> lines;
    lines.push_back("File: " + shorten(item.name, 30));
    lines.push_back("Dimensione: " + formatSize(item.size));
    lines.push_back("Host: wpa-sec.stanev.org");
    if (item.uploaded) {
        lines.push_back("Gia' inviato in precedenza.");
    }
    lines.push_back("I dati della cattura saranno");
    lines.push_back("inviati al servizio esterno.");
    lines.push_back("Solo reti proprie o autorizzate.");
    lines.push_back("Procedere?");
    if (!ui::confirm("Invia cattura", lines, kModuleName)) {
        return;
    }

    if (!prepareNetwork()) {
        return;
    }

    const UploadResult result = uploadFile(item, key);

    std::vector<std::string> message;
    if (result.cancelled) {
        message.push_back("Operazione annullata.");
        if (!result.note.empty()) {
            message.push_back(result.note);
        }
        if (!result.error.empty()) {
            message.push_back(shorten(result.error, 37));
        }
    } else if (result.ok) {
        message.push_back(">File inviato.");
        message.push_back("File: " + shorten(item.name, 30));
        message.push_back("HTTP " + std::to_string(result.status) +
                          (result.tlsVerified ? "  TLS verificato"
                                              : "  TLS non verificato"));
        const auto wrapped = wrapForUi(result.response, 36, 3);
        for (const auto& line : wrapped) {
            message.push_back(line);
        }
        if (!markUploaded(item.name, item.size)) {
            message.push_back("!Stato upload non salvato.");
        }
    } else {
        message.push_back("!Invio non riuscito.");
        message.push_back("File: " + shorten(item.name, 30));
        if (result.status > 0) {
            message.push_back("HTTP " + std::to_string(result.status));
        }
        if (!result.note.empty()) {
            message.push_back(result.note);
        }
        const auto errorLines = wrapForUi(result.error, 36, 2);
        for (const auto& line : errorLines) {
            message.push_back(line);
        }
        const auto wrapped = wrapForUi(result.response, 36, 2);
        for (const auto& line : wrapped) {
            message.push_back(line);
        }
    }
    ui::message("Upload", message, kModuleName);
}

void downloadFlow() {
    std::string key;
    if (!loadKey(key)) {
        ui::message("Potfile", {"!Chiave API assente.", "Impostala dal menu WPA-SEC."},
                    kModuleName);
        return;
    }
    if (!ui::confirm("Scarica potfile",
                     {"Scaricare il potfile da", "wpa-sec.stanev.org?",
                      "La chiave viaggia nel URL su TLS.", "Procedere?"},
                     kModuleName)) {
        return;
    }
    if (!prepareNetwork()) {
        return;
    }

    const DownloadResult result = downloadPotfile(key);

    std::vector<std::string> message;
    if (result.cancelled) {
        message.push_back("Download annullato.");
        message.push_back("Il potfile precedente resta valido.");
        if (!result.note.empty()) {
            message.push_back(result.note);
        }
        if (!result.error.empty()) {
            message.push_back(shorten(result.error, 37));
        }
    } else if (result.ok) {
        message.push_back(">Potfile salvato.");
        message.push_back("Dimensione: " + formatSize(result.size));
        message.push_back("Righe: " + std::to_string(result.lines));
        message.push_back("Reti con password: " + std::to_string(result.hashes));
        if (result.truncated) {
            message.push_back("Scansione parziale (limite 256K).");
        }
        message.push_back(result.tlsVerified ? "TLS verificato."
                                             : "!TLS non verificato.");
    } else {
        message.push_back("!Download non riuscito.");
        if (!result.note.empty()) {
            message.push_back(result.note);
        }
        const auto errorLines = wrapForUi(result.error, 36, 2);
        for (const auto& line : errorLines) {
            message.push_back(line);
        }
        const auto wrapped = wrapForUi(result.response, 36, 2);
        for (const auto& line : wrapped) {
            message.push_back(line);
        }
    }
    ui::message("Potfile", message, kModuleName);
}

// ---------------------------------------------------------------------------
//  Chiave
// ---------------------------------------------------------------------------

void setKeyFlow() {
    std::vector<textinput::Field> fields = {
        {"Chiave API (32 hex)", "", 32, true, false},
    };
    if (!textinput::edit("Chiave WPA-SEC", kModuleName, fields)) {
        return;
    }
    std::string key = trim(fields[0].value);
    if (key.empty()) {
        ui::message("Chiave", {"Nessuna modifica."}, kModuleName);
        return;
    }
    if (!normalizeKey(key)) {
        ui::message("Chiave",
                    {"!Chiave non valida.", "Servono 32 caratteri hex", "(0-9, a-f).",
                     "Nessun dato salvato."},
                    kModuleName);
        return;
    }
    if (!saveKey(key)) {
        ui::message("Chiave", {"!Salvataggio non riuscito.", "Verifica la microSD."},
                    kModuleName);
        return;
    }
    ui::message("Chiave", {">Chiave API salvata.", "Non verra' mostrata ne'",
                           "scritta nei log."},
                kModuleName);
}

void clearKeyFlow() {
    const bool sd = useSD();
    if (!storage::exists(kKeyPath, sd)) {
        ui::message("Cancella chiave", {"Nessuna chiave salvata."}, kModuleName);
        return;
    }
    if (!ui::confirm("Cancella chiave",
                     {"Rimuovere la chiave API da", "/wpasec/key.txt?"}, kModuleName)) {
        return;
    }
    const bool removed = storage::removeFile(kKeyPath, sd);
    ui::message("Cancella chiave",
                {removed ? ">Chiave cancellata." : "!Cancellazione non riuscita."},
                kModuleName);
}

// ---------------------------------------------------------------------------
//  Stato e info
// ---------------------------------------------------------------------------

void statusFlow() {
    const bool sd = useSD();
    std::string key;
    const bool hasKey = loadKey(key);

    uint32_t hc = 0;
    uint32_t pcap = 0;
    const std::vector<storage::Entry> entries =
        storage::listEntries(pcaptools::kCaptureDir, sd);
    for (const auto& entry : entries) {
        if (entry.isDir) {
            continue;
        }
        if (pcaptools::isHc22000Name(entry.name)) {
            ++hc;
        } else if (pcaptools::isPcapName(entry.name)) {
            ++pcap;
        }
    }

    uint32_t uploaded = 0;
    std::string registry;
    if (storage::readText(kUploadedPath, registry, kMaxUploadedBytes, sd)) {
        size_t pos = 0;
        while (pos < registry.size()) {
            const size_t end = registry.find('\n', pos);
            const std::string line =
                trim(registry.substr(pos, end == std::string::npos ? std::string::npos
                                                                   : end - pos));
            std::string name;
            size_t size = 0;
            if (parseUploadedLine(line, name, size)) {
                ++uploaded;
            }
            if (end == std::string::npos) {
                break;
            }
            pos = end + 1;
        }
    }

    CrackIndex index;
    const bool hasPotfile = index.build();

    std::vector<std::string> lines;
    lines.push_back(std::string("Chiave: ") + (hasKey ? "presente" : "assente"));
    lines.push_back("Catture: " + std::to_string(hc + pcap) + " (hc " +
                    std::to_string(hc) + ", pcap " + std::to_string(pcap) + ")");
    lines.push_back("Inviati: " + std::to_string(uploaded));
    if (hasPotfile) {
        lines.push_back("Potfile: " + formatSize(storage::fileSize(kPotfilePath, sd)) +
                        ", " + std::to_string(index.lines) + " righe");
        lines.push_back("Reti con password: " + std::to_string(index.bssids.size()) +
                        (index.truncated ? " (parziale)" : ""));
    } else {
        lines.push_back("Potfile: assente");
        lines.push_back("Reti con password: 0");
    }
    lines.push_back(std::string("TLS: ") +
                    (g_clockFailed ? "NTP assente, CA da verificare"
                                   : (g_caFailed ? "ultima CA fallita"
                                                 : "CA GTS Root R4")));
    lines.push_back("Heap: " + formatHeap(ESP.getFreeHeap()) + " liberi, " +
                    formatHeap(ESP.getMaxAllocHeap()) + " blocco");
    lines.push_back(std::string("SD: ") + (storage::hasSD() ? "montata" : "LittleFS"));
    ui::message("Stato", lines, kModuleName);
}

void infoFlow() {
    ui::message("Info WPA-SEC",
                {"Endpoint: wpa-sec.stanev.org",
                 "Upload: POST /?submit&key=...",
                 "Potfile: GET /?api&dl=1&key=...",
                 "Multipart, campo 'file'.",
                 "Potfile: bssid:sta:ssid:pass",
                 "o righe WPA* (compatibilita').",
                 "TLS: CA GTS R4, fallback avvisato.",
                 "Solo reti proprie/autorizzate."},
                kModuleName);
}

// Controllo minimo delle funzioni pure: chiave, BSSID e formati potfile.
bool selfTest() {
    std::string key = "0123456789ABCDEF0123456789abcdef";
    if (!normalizeKey(key) || key != "0123456789abcdef0123456789abcdef") {
        return false;
    }
    std::string shortKey = "0123456789abcdef0123456789abcde";
    if (normalizeKey(shortKey)) {
        return false;
    }

    uint64_t colonBssid = 0;
    uint64_t wpaBssid = 0;
    if (!bssidFromPotLine("020000000001:020000000002:ORBIT:TEST:test-pass",
                          colonBssid) ||
        !bssidFromPotLine(
            "WPA*01*00112233445566778899aabbccddeeff*020000000001*020000000002*"
            "4f524249542d54455354***:pass",
            wpaBssid) ||
        colonBssid != wpaBssid) {
        return false;
    }

    std::string password;
    // SSID con ':' e password ultimo campo.
    if (!potfileLinePassword("020000000001:020000000002:ORBIT:TEST:test-pass",
                             password) ||
        password != "test-pass") {
        return false;
    }
    if (!potfileLinePassword(
            "WPA*01*00112233445566778899aabbccddeeff*020000000001*020000000002*"
            "4f524249542d54455354***:pass",
            password) ||
        password != "pass") {
        return false;
    }

    uint64_t colonMac = 0;
    if (!bssidFromPotLine(
            "02:00:00:00:00:01:02:00:00:00:00:02:ORBIT-TEST:test-pass",
            colonMac) ||
        colonMac != colonBssid ||
        !potfileLinePassword(
            "02:00:00:00:00:01:02:00:00:00:00:02:ORBIT-TEST:test-pass",
            password) ||
        password != "test-pass") {
        return false;
    }
    if (potfileLooksValid("<html><body>bad</body></html>")) {
        return false;
    }
    return hasErrorWords("not a valid capture") &&
           hasSuccessWords("capture accepted") &&
           potfileLooksValid("020000000001:020000000002:ORBIT-TEST:pass");
}

}  // namespace

void runMenu() {
    g_caFailed = false;
    g_clockFailed = false;
    g_clockAttempted = false;
    g_insecureAck = false;
    g_networkUsed = false;

    if (!storage::hasSD() && !storage::hasLittleFS()) {
        ui::message(kModuleName, {"!Nessun filesystem disponibile."}, "Menu principale");
        return;
    }
    if (!selfTest()) {
        logger::error("WPA-SEC: self-test fallito");
        ui::message(kModuleName, {"!Errore interno del modulo."}, "Menu principale");
        return;
    }

    {
        MenuManager menu(kModuleName, "Menu principale");
        menu.addItem("Imposta chiave", "key", [] { setKeyFlow(); });
        menu.addItem("Cancella chiave", "lock", [] { clearKeyFlow(); });
        menu.addItem("Elenco catture", "capture", [] { listCapturesFlow(); });
        menu.addItem("Carica catture", "scan", [] { uploadFlow(); });
        menu.addItem("Scarica potfile", "file", [] { downloadFlow(); });
        menu.addItem("Stato", "info", [] { statusFlow(); });
        menu.addItem("Info", "info", [] { infoFlow(); });
        menu.setHint("ESC=indietro  ;.=nav  ENTER=apri");
        menu.run(true);
    }

    // Se il modulo ha toccato la radio, la si spegne sempre all'uscita,
    // anche dopo timeout, errori o annullamenti.
    if (g_networkUsed) {
        logger::info("WPA-SEC: uscita, radio spenta");
        wifisetup::disconnect();
        WiFi.mode(WIFI_OFF);
    }
    g_networkUsed = false;
    g_caFailed = false;
    g_clockFailed = false;
    g_clockAttempted = false;
    g_insecureAck = false;
}

}  // namespace wpasec

#endif  // ENABLE_MODULE_WPASEC
