#include "EspNowMenu.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <Preferences.h>
#include <SD.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_system.h>
#include <esp_wifi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Display.h"
#include "EspNowProtocol.h"
#include "Logger.h"
#include "MenuManager.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "Storage.h"
#include "TextInput.h"
#include "Ui.h"

#if ENABLE_MODULE_ESPNOW

namespace espnow {
namespace {

namespace ep = ::espnow::proto;

constexpr uint8_t kDefaultChannel = 1;
constexpr size_t kMaxPeers = 12;
constexpr uint32_t kHelloIntervalMs = 4000;
constexpr uint32_t kAckTimeoutMs = 450;
constexpr uint32_t kFinalAckTimeoutMs = 10000;
constexpr uint32_t kRxTimeoutMs = 6000;
constexpr uint8_t kTxTries = 3;
constexpr uint8_t kTxTimeout = 0xFD;
constexpr uint8_t kTxAborted = 0xFE;
constexpr size_t kHistoryMaxLines = 60;
constexpr size_t kHistoryReadBytes = 16 * 1024;
constexpr uint32_t kPeerClipIntervalMs = 1000;
constexpr uint32_t kPeerMsgIntervalMs = 3000;
constexpr uint32_t kPeerCmdIntervalMs = 300;
constexpr int kMainItems = 6;

constexpr const char* kNamespace = "orbit_espnow";
constexpr const char* kInboxDir = "/espnow/inbox";
constexpr const char* kTmpDir = "/espnow/tmp";
constexpr const char* kHistoryPath = "/espnow/commands.txt";
constexpr const char* kBroadcastLabel = "Broadcast (tutti)";

const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

const char* const kMainLabels[kMainItems] = {"Invia file", "Inbox", "Comandi",
                                             "Clipboard", "Peer",   "Setup"};
const char* const kMainIcons[kMainItems] = {"file", "folder", "chat",
                                            "copy", "radar",  "settings"};

using Mac = std::array<uint8_t, 6>;

struct Peer {
    uint8_t mac[6] = {};
    char name[ep::kMaxName + 1] = {};
    int8_t rssi = -127;
    uint32_t lastSeen = 0;
    uint32_t lastClipMs = 0;
    uint32_t lastMsgMs = 0;
    uint32_t lastCmdMs = 0;
};

struct AckSlot {
    bool valid = false;
    uint32_t session = 0;
    uint16_t seq = 0;
    uint8_t status = 0;
};

struct RxState {
    bool active = false;
    bool noAck = false;
    uint32_t session = 0;
    uint16_t totalChunks = 0;
    uint16_t expected = 0;
    uint32_t expectedCrc = 0;
    uint32_t fileSize = 0;
    uint32_t received = 0;
    uint32_t lastMs = 0;
    uint8_t mac[6] = {};
    File file;
    std::string tmpPath;
    std::string displayName;
};

struct PendingCmd {
    char from[ep::kMaxName + 1] = {};
    char text[ep::kMaxCommand + 1] = {};
    bool ignored = false;
};

// Lavoro di finalizzazione spostato dal callback WiFi al task UI: il callback
// non tocca SD (ne' rinomina ne' sidecar) e non blocca la radio.
struct FinalizeJob {
    bool pending = false;
    uint32_t session = 0;
    uint16_t lastSeq = 0;
    bool noAck = false;
    uint8_t mac[6] = {};
    std::string tmpPath;
    std::string displayName;
    uint32_t fileSize = 0;
    uint32_t expectedCrc = 0;
};

enum class CmdKind { Invalid, Beep, Led, Msg };

struct CommandSpec {
    CmdKind kind = CmdKind::Invalid;
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    std::string text;
};

SemaphoreHandle_t g_mutex = nullptr;
SemaphoreHandle_t g_sdMutex = nullptr;
std::atomic<int> g_callbacksActive{0};

struct Guard {
    Guard() {
        if (g_mutex != nullptr) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
        }
    }
    ~Guard() {
        if (g_mutex != nullptr) {
            xSemaphoreGive(g_mutex);
        }
    }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

// Un solo lock per tutte le operazioni SD del modulo: callback WiFi, browser,
// inbox, storico e trasferimenti non possono intrecciarsi sul filesystem.
struct SdGuard {
    SdGuard() {
        if (g_sdMutex != nullptr) {
            xSemaphoreTake(g_sdMutex, portMAX_DELAY);
        }
    }
    ~SdGuard() {
        if (g_sdMutex != nullptr) {
            xSemaphoreGive(g_sdMutex);
        }
    }
    SdGuard(const SdGuard&) = delete;
    SdGuard& operator=(const SdGuard&) = delete;
};

struct CallbackScope {
    CallbackScope() { g_callbacksActive.fetch_add(1, std::memory_order_relaxed); }
    ~CallbackScope() { g_callbacksActive.fetch_sub(1, std::memory_order_relaxed); }
    CallbackScope(const CallbackScope&) = delete;
    CallbackScope& operator=(const CallbackScope&) = delete;
};

volatile bool g_ready = false;
bool g_radioInit = false;
bool g_wifiStarted = false;
bool g_prefsOpen = false;
bool g_sniffer = false;
wifi_mode_t g_prevMode = WIFI_OFF;

Preferences g_prefs;
std::string g_name;
uint8_t g_channel = kDefaultChannel;
uint8_t g_selfMac[6] = {};
uint32_t g_lastHello = 0;

Peer g_peers[kMaxPeers];
int g_peerCount = 0;

AckSlot g_ack[3];
uint32_t g_txSession = 0;
uint8_t g_txMac[6] = {};
bool g_txBroadcast = false;

RxState g_rx;
FinalizeJob g_finalize;
uint32_t g_lastRxSession = 0;
uint16_t g_lastRxSeq = 0;
uint32_t g_lastRxAt = 0;
uint8_t g_lastRxMac[6] = {};

std::string g_clipboard;
uint32_t g_clipRevision = 0;
bool g_clipDirty = false;
uint32_t g_clipNextSaveAt = 0;

PendingCmd g_pending[4];
int g_pendingCount = 0;

volatile bool g_beepPending = false;
std::string g_notice;
uint32_t g_noticeUntil = 0;

// --- Testo e nomi -----------------------------------------------------------

std::string shorten(const std::string& value, size_t maxChars) {
    if (value.size() <= maxChars) {
        return value;
    }
    if (maxChars <= 2) {
        return value.substr(0, maxChars);
    }
    return value.substr(0, maxChars - 2) + "..";
}

std::string cleanText(const uint8_t* data, size_t len, size_t maxLen) {
    std::string out;
    out.reserve(std::min(len, maxLen));
    bool lastSpace = false;
    for (size_t i = 0; i < len && out.size() < maxLen; ++i) {
        uint8_t c = data[i];
        if (c == '\r' || c == '\n' || c == '\t') {
            c = ' ';
        }
        if (c < 0x20 || c >= 0x7F) {
            continue;
        }
        if (c == ' ' && (out.empty() || lastSpace)) {
            continue;
        }
        out.push_back(static_cast<char>(c));
        lastSpace = (c == ' ');
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

std::string cleanText(const std::string& value, size_t maxLen) {
    return cleanText(reinterpret_cast<const uint8_t*>(value.data()), value.size(),
                     maxLen);
}

// Comandi: nessuna normalizzazione. Spazi iniziali/finali, maiuscole e
// caratteri di controllo restano e fanno fallire il matching esatto.
std::string strictPrintable(const uint8_t* data, size_t len, size_t maxLen) {
    if (data == nullptr || len > maxLen) {
        return std::string();
    }
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        const uint8_t c = data[i];
        if (c < 0x20 || c >= 0x7F) {
            return std::string();
        }
        out.push_back(static_cast<char>(c));
    }
    return out;
}

// Clipboard: conserva gli spazi, scarta i caratteri non stampabili.
std::string printableText(const uint8_t* data, size_t len, size_t maxLen) {
    std::string out;
    out.reserve(std::min(len, maxLen));
    for (size_t i = 0; i < len && out.size() < maxLen; ++i) {
        const uint8_t c = data[i];
        if (c >= 0x20 && c < 0x7F) {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

std::string cleanPeerName(const uint8_t* data, size_t len) {
    return cleanText(data, len, ep::kMaxName);
}

std::string cleanFileName(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(std::min(len, ep::kMaxFileName));
    for (size_t i = 0; i < len && out.size() < 28; ++i) {
        const unsigned char c = data[i];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || c == '.' || c == '_' || c == '-') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
        }
    }
    while (!out.empty() && (out.front() == '.' || out.front() == '-' ||
                            out.front() == '_')) {
        out.erase(out.begin());
    }
    if (out.empty()) {
        out = "ricevuto.bin";
    }
    std::string lower = out;
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    if (lower == "con" || lower == "prn" || lower == "aux" || lower == "nul") {
        out = "file_" + out;
    }
    return out;
}

std::string shortName(const std::string& value) {
    return shorten(value.empty() ? std::string("?") : value, 8);
}

std::string formatBytes(uint32_t bytes) {
    char buffer[24];
    if (bytes >= 1024) {
        std::snprintf(buffer, sizeof(buffer), "%lu.%lu KB",
                      static_cast<unsigned long>(bytes / 1024),
                      static_cast<unsigned long>((bytes % 1024) * 10 / 1024));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%lu B",
                      static_cast<unsigned long>(bytes));
    }
    return buffer;
}

std::string baseName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool isCrcSidecar(const std::string& name) {
    return name.size() >= 4 &&
           name.compare(name.size() - 4, 4, ".crc") == 0;
}

void macText(const uint8_t* mac, char out[18]) {
    std::snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
                  mac[2], mac[3], mac[4], mac[5]);
}

std::string ownName() {
    Guard guard;
    return g_name;
}

std::string ownMacText() {
    char mac[18];
    macText(g_selfMac, mac);
    return mac;
}

std::vector<std::string> wrapLines(const std::string& text, size_t width,
                                   size_t maxLines) {
    std::vector<std::string> lines;
    if (text.empty() || maxLines == 0) {
        return lines;
    }
    width = std::max<size_t>(8, width);
    for (size_t pos = 0; pos < text.size() && lines.size() < maxLines; pos += width) {
        lines.push_back(text.substr(pos, width));
    }
    if (lines.size() == maxLines) {
        std::string& last = lines.back();
        if (last.size() > 18) {
            last.resize(18);
            last += "..";
        }
    }
    return lines;
}

// --- Stato condiviso --------------------------------------------------------

void setNotice(const std::string& text, uint32_t durationMs) {
    Guard guard;
    g_notice = text;
    g_noticeUntil = millis() + durationMs;
}

std::string currentNotice() {
    Guard guard;
    if (g_noticeUntil == 0 || static_cast<int32_t>(millis() - g_noticeUntil) >= 0) {
        g_notice.clear();
        g_noticeUntil = 0;
        return std::string();
    }
    return g_notice;
}

int findPeerLocked(const uint8_t* mac) {
    for (int i = 0; i < g_peerCount; ++i) {
        if (std::memcmp(g_peers[i].mac, mac, 6) == 0) {
            return i;
        }
    }
    return -1;
}

void touchPeerLocked(const uint8_t* mac, const char* name) {
    int slot = findPeerLocked(mac);
    if (slot < 0) {
        if (g_peerCount < static_cast<int>(kMaxPeers)) {
            slot = g_peerCount++;
        } else {
            slot = 0;
            for (int i = 1; i < g_peerCount; ++i) {
                if (g_peers[i].lastSeen < g_peers[slot].lastSeen) {
                    slot = i;
                }
            }
        }
        std::memset(&g_peers[slot], 0, sizeof(Peer));
        std::memcpy(g_peers[slot].mac, mac, 6);
        g_peers[slot].rssi = -127;
    }
    g_peers[slot].lastSeen = millis();
    if (name != nullptr && name[0] != '\0') {
        std::snprintf(g_peers[slot].name, sizeof(g_peers[slot].name), "%s", name);
    }
}

void touchPeer(const uint8_t* mac, const char* name) {
    Guard guard;
    touchPeerLocked(mac, name);
}

void updatePeerRssi(const uint8_t* mac, int8_t rssi) {
    Guard guard;
    const int slot = findPeerLocked(mac);
    if (slot >= 0) {
        g_peers[slot].rssi = rssi;
        g_peers[slot].lastSeen = millis();
    }
}

int peerCount() {
    Guard guard;
    return g_peerCount;
}

std::vector<Peer> snapshotPeers() {
    Peer copy[kMaxPeers];
    int count = 0;
    {
        Guard guard;
        count = g_peerCount;
        std::memcpy(copy, g_peers, sizeof(Peer) * static_cast<size_t>(count));
    }
    return std::vector<Peer>(copy, copy + count);
}

void beginTx(uint32_t session, const uint8_t* targetMac, bool broadcast) {
    Guard guard;
    g_txSession = session;
    g_txBroadcast = broadcast;
    const uint8_t* target =
        (broadcast || targetMac == nullptr) ? kBroadcastMac : targetMac;
    std::memcpy(g_txMac, target, 6);
    for (AckSlot& slot : g_ack) {
        slot = AckSlot{};
    }
}

uint8_t waitAck(uint8_t kind, uint16_t seq, uint32_t timeoutMs) {
    const uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        {
            Guard guard;
            const AckSlot& slot = g_ack[kind & 0x03];
            if (slot.valid && slot.session == g_txSession && slot.seq == seq) {
                return slot.status;
            }
        }
        if (navigation::poll().type == navigation::Event::Back) {
            return kTxAborted;
        }
        delay(2);
    }
    return kTxTimeout;
}

// --- Radio ESP-NOW ----------------------------------------------------------

void sendHello();
void stopRadio();
bool startRadio();
void serviceMesh();

bool ensurePeer(const uint8_t* mac) {
    if (!g_ready) {
        return false;
    }
    if (esp_now_is_peer_exist(mac)) {
        return true;
    }
    esp_now_peer_info_t info{};
    std::memcpy(info.peer_addr, mac, 6);
    info.channel = 0;
    info.ifidx = WIFI_IF_STA;
    info.encrypt = false;
    const esp_err_t err = esp_now_add_peer(&info);
    return err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST;
}

esp_err_t transmit(const uint8_t* mac, const ep::Packet& packet) {
    if (!g_ready) {
        return ESP_FAIL;
    }
    uint8_t buffer[ep::kMaxPacket];
    const size_t size = ep::encodePacket(packet, buffer, sizeof(buffer));
    if (size == 0) {
        return ESP_FAIL;
    }
    return esp_now_send(mac, buffer, size);
}

bool sendBroadcast(const ep::Packet& packet) {
    if (!g_ready) {
        return false;
    }
    uint8_t buffer[ep::kMaxPacket];
    const size_t size = ep::encodePacket(packet, buffer, sizeof(buffer));
    if (size == 0) {
        return false;
    }
    for (int attempt = 0; attempt < 12; ++attempt) {
        if (esp_now_send(kBroadcastMac, buffer, size) == ESP_OK) {
            return true;
        }
        delay(3);
    }
    return false;
}

void replyAck(const uint8_t* mac, uint32_t session, uint8_t kind, uint16_t seq,
              uint8_t status, bool noAck) {
    if (noAck || !g_ready) {
        return;
    }
    ep::Packet packet{};
    packet.type = ep::TypeAck;
    packet.flags = kind;
    packet.session = session;
    packet.seq = seq;
    packet.len = 1;
    packet.payload[0] = status;
    uint8_t buffer[ep::kMaxPacket];
    const size_t size = ep::encodePacket(packet, buffer, sizeof(buffer));
    if (size == 0 || !ensurePeer(mac)) {
        return;
    }
    esp_now_send(mac, buffer, size);
}

void sendHello() {
    if (!g_ready) {
        return;
    }
    const std::string name = ownName();
    ep::Packet packet{};
    packet.type = ep::TypeHello;
    packet.session = esp_random();
    packet.len = static_cast<uint16_t>(1 + name.size());
    packet.payload[0] = static_cast<uint8_t>(name.size());
    std::memcpy(packet.payload + 1, name.data(), name.size());
    sendBroadcast(packet);
    g_lastHello = millis();
}

// --- Ricezione file ---------------------------------------------------------

void closeRxLocked() {
    SdGuard sd;
    if (g_rx.file) {
        g_rx.file.flush();
        g_rx.file.close();
    }
    if (!g_rx.tmpPath.empty()) {
        storage::removeFile(g_rx.tmpPath, true);
        g_rx.tmpPath.clear();
    }
    g_rx.active = false;
}

bool checkRxTimeoutLocked() {
    if (g_rx.active &&
        static_cast<uint32_t>(millis() - g_rx.lastMs) > kRxTimeoutMs) {
        closeRxLocked();
        return true;
    }
    return false;
}

// Richiede SdGuard: il chiamante serializza con le altre operazioni SD.
std::string uniqueInboxPath(const std::string& name) {
    const std::string base = std::string(kInboxDir) + "/" + name;
    if (!storage::exists(base, true)) {
        return base;
    }
    const size_t dot = name.find_last_of('.');
    const std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
    const std::string ext = dot == std::string::npos ? "" : name.substr(dot);
    for (int i = 1; i < 100; ++i) {
        const std::string candidate =
            std::string(kInboxDir) + "/" + stem + "_" + std::to_string(i) + ext;
        if (!storage::exists(candidate, true)) {
            return candidate;
        }
    }
    char fallback[64];
    std::snprintf(fallback, sizeof(fallback), "%s/dup_%08lX.bin", kInboxDir,
                  static_cast<unsigned long>(esp_random()));
    return fallback;
}

// Richiede SdGuard: il chiamante serializza con le altre operazioni SD.
bool fileCrc(const std::string& path, uint32_t& crcOut, uint32_t& sizeOut) {
    File file = storage::openRead(path, true);
    if (!file) {
        return false;
    }
    uint8_t buffer[256];
    uint32_t crc = ep::crc32Begin();
    uint32_t total = 0;
    for (;;) {
        const int got = file.read(buffer, sizeof(buffer));
        if (got < 0) {
            file.close();
            return false;
        }
        if (got == 0) {
            break;
        }
        total += static_cast<uint32_t>(got);
        if (total > ep::kMaxFileBytes) {
            file.close();
            return false;
        }
        crc = ep::crc32Update(crc, buffer, static_cast<size_t>(got));
    }
    file.close();
    crcOut = ep::crc32End(crc);
    sizeOut = total;
    return true;
}

bool openRxLocked(const uint8_t* mac, uint32_t session, const uint8_t* nameData,
                  size_t nameLen, uint32_t fileSize, uint32_t fileCrc) {
    const std::string name = cleanFileName(nameData, nameLen);
    char tmp[64] = {};
    File file;
    {
        SdGuard sd;
        for (int attempt = 0; attempt < 8; ++attempt) {
            std::snprintf(tmp, sizeof(tmp), "%s/rx_%08lX_%d.part", kTmpDir,
                          static_cast<unsigned long>(session), attempt);
            if (!storage::exists(tmp, true)) {
                break;
            }
            tmp[0] = '\0';
        }
        if (tmp[0] == '\0') {
            return false;
        }
        storage::mkdir(kTmpDir, true);
        file = storage::openWrite(tmp, true);
        if (!file) {
            return false;
        }
    }

    g_rx.active = true;
    g_rx.noAck = false;
    g_rx.session = session;
    g_rx.totalChunks =
        static_cast<uint16_t>((fileSize + ep::kChunkSize - 1) / ep::kChunkSize);
    g_rx.expected = 0;
    g_rx.expectedCrc = fileCrc;
    g_rx.fileSize = fileSize;
    g_rx.received = 0;
    g_rx.lastMs = millis();
    std::memcpy(g_rx.mac, mac, 6);
    g_rx.file = file;
    g_rx.tmpPath = tmp;
    g_rx.displayName = name;
    return true;
}

// Verifica CRC, rinomina e sidecar: eseguita dal task UI, mai dal callback
// WiFi. Tutte le operazioni SD sono sotto SdGuard.
void serviceFinalize() {
    FinalizeJob job;
    {
        Guard guard;
        if (!g_finalize.pending) {
            return;
        }
        job = g_finalize;
    }

    uint8_t status = ep::StOk;
    std::string receivedName;
    uint32_t crc = 0;
    uint32_t size = 0;
    {
        SdGuard sd;
        if (!fileCrc(job.tmpPath, crc, size)) {
            status = ep::StIo;
        } else if (size != job.fileSize || crc != job.expectedCrc) {
            status = ep::StCrc;
        }
        if (status == ep::StOk) {
            const std::string finalPath = uniqueInboxPath(job.displayName);
            if (!SD.rename(job.tmpPath.c_str(), finalPath.c_str())) {
                status = ep::StIo;
            } else {
                char meta[64];
                std::snprintf(meta, sizeof(meta), "%08lX %lu\n",
                              static_cast<unsigned long>(crc),
                              static_cast<unsigned long>(size));
                if (!storage::writeText(finalPath + ".crc", meta, true)) {
                    storage::removeFile(finalPath, true);
                    status = ep::StIo;
                } else {
                    receivedName = baseName(finalPath);
                }
            }
        }
        if (status != ep::StOk) {
            storage::removeFile(job.tmpPath, true);
        }
    }

    {
        Guard guard;
        g_finalize = FinalizeJob{};
    }

    if (status == ep::StOk) {
        setNotice("Ricevuto: " + receivedName, 4000);
        Guard guard;
        g_beepPending = true;
    } else {
        setNotice(status == ep::StCrc ? "!CRC errato: file rifiutato"
                                      : "!Ricezione fallita",
                  4000);
    }

    replyAck(job.mac, job.session, ep::AckFinal, job.lastSeq, status, job.noAck);
    if (!job.noAck) {
        // Siamo nel task UI: il piccolo delay non blocca la radio. Il secondo
        // frame riduce il falso "conferma assente" se il primo si perde.
        delay(40);
        replyAck(job.mac, job.session, ep::AckFinal, job.lastSeq, status,
                 job.noAck);
    }
}

void handleMeta(const uint8_t* mac, const ep::Packet& packet) {
    if (packet.len < 10) {
        return;
    }
    const uint8_t nameLen = packet.payload[0];
    if (nameLen == 0 || nameLen > ep::kMaxFileName) {
        return;
    }
    if (packet.len < static_cast<size_t>(1 + nameLen + 9)) {
        return;
    }
    const uint8_t* nameData = packet.payload + 1;
    const uint8_t* numbers = packet.payload + 1 + nameLen;
    const uint32_t fileSize = ep::readU32(numbers);
    const uint32_t fileCrc = ep::readU32(numbers + 4);
    const uint8_t senderLen = numbers[8];
    if (senderLen > ep::kMaxName ||
        packet.len < static_cast<size_t>(1 + nameLen + 9 + senderLen)) {
        return;
    }
    const std::string sender =
        cleanPeerName(numbers + 9, static_cast<size_t>(senderLen));
    touchPeer(mac, sender.empty() ? nullptr : sender.c_str());

    const bool noAck = (packet.flags & ep::kFlagNoAck) != 0;
    uint8_t status = ep::StOk;
    bool duplicate = false;
    if (fileSize == 0 || fileSize > ep::kMaxFileBytes) {
        status = ep::StSize;
    } else {
        Guard guard;
        if (g_finalize.pending ||
            (g_rx.active &&
             (g_rx.session != packet.session ||
              std::memcmp(mac, g_rx.mac, 6) != 0))) {
            status = ep::StBusy;
        } else if (g_rx.active && g_rx.session == packet.session) {
            duplicate = true;
        } else if (!storage::hasSD()) {
            status = ep::StIo;
        } else if (!openRxLocked(mac, packet.session, nameData, nameLen, fileSize,
                                 fileCrc)) {
            status = ep::StIo;
        } else {
            g_rx.noAck = noAck;
        }
    }
    if (duplicate) {
        status = ep::StOk;
    }
    replyAck(mac, packet.session, ep::AckMeta, 0, status, noAck);
}

void handleData(const uint8_t* mac, const ep::Packet& packet) {
    const bool noAck = (packet.flags & ep::kFlagNoAck) != 0;
    bool sessionMatches = false;
    bool completed = false;
    bool finishedDuplicate = false;
    uint8_t status = ep::StOk;

    {
        Guard guard;
        // ACK del blocco finale perso: il peer puo' ritrasmetterlo anche
        // mentre la finalizzazione e' in coda nel task UI o e' gia' finita.
        if (packet.session == g_lastRxSession &&
            packet.seq == g_lastRxSeq &&
            std::memcmp(mac, g_lastRxMac, 6) == 0 &&
            static_cast<uint32_t>(millis() - g_lastRxAt) < 10000) {
            finishedDuplicate = true;
        } else if (!g_rx.active) {
            // nessun trasferimento in corso: ignora
        } else if (std::memcmp(mac, g_rx.mac, 6) != 0) {
            return;  // un altro peer non puo' iniettare blocchi nella sessione
        } else if (g_rx.session != packet.session) {
            return;
        } else {
            sessionMatches = true;
            if (packet.len == 0 || packet.len > ep::kChunkSize) {
                status = ep::StSize;
            } else if (packet.seq < g_rx.expected) {
                status = ep::StOk;  // ritrasmissione: gia' scritto
            } else if (packet.seq > g_rx.expected) {
                status = ep::StSeq;
            } else {
                const size_t expectLen =
                    packet.seq + 1 == g_rx.totalChunks
                        ? static_cast<size_t>(g_rx.fileSize -
                                              static_cast<uint32_t>(packet.seq) *
                                                  ep::kChunkSize)
                        : ep::kChunkSize;
                if (packet.len != expectLen) {
                    status = ep::StSize;
                } else {
                    bool writeOk = false;
                    {
                        SdGuard sd;
                        writeOk =
                            g_rx.file.write(packet.payload, packet.len) ==
                            packet.len;
                    }
                    if (!writeOk) {
                        status = ep::StIo;
                    } else {
                        g_rx.expected++;
                        g_rx.received += packet.len;
                        g_rx.lastMs = millis();
                        if (g_rx.expected >= g_rx.totalChunks) {
                            {
                                SdGuard sd;
                                g_rx.file.flush();
                                g_rx.file.close();
                            }
                            g_rx.active = false;
                            g_finalize.session = g_rx.session;
                            g_finalize.lastSeq = packet.seq;
                            g_finalize.noAck = g_rx.noAck;
                            std::memcpy(g_finalize.mac, g_rx.mac, 6);
                            g_finalize.tmpPath = g_rx.tmpPath;
                            g_finalize.displayName = g_rx.displayName;
                            g_finalize.fileSize = g_rx.fileSize;
                            g_finalize.expectedCrc = g_rx.expectedCrc;
                            g_finalize.pending = true;
                            g_lastRxSession = g_rx.session;
                            g_lastRxSeq = packet.seq;
                            g_lastRxAt = millis();
                            std::memcpy(g_lastRxMac, g_rx.mac, 6);
                            completed = true;
                        }
                    }
                }
            }
        }
    }

    if (finishedDuplicate) {
        replyAck(mac, packet.session, ep::AckData, packet.seq, ep::StOk, noAck);
        return;
    }
    if (!sessionMatches && !completed) {
        return;
    }
    replyAck(mac, packet.session, ep::AckData, packet.seq, status, noAck);
    // La verifica CRC e la pubblicazione avvengono nel task UI tramite
    // serviceFinalize(): il callback resta leggero e non tocca SD.
    (void)completed;
}

void handleAck(const uint8_t* mac, const ep::Packet& packet) {
    if (packet.len != 1 || packet.flags > ep::AckFinal) {
        return;
    }
    bool accepted = false;
    {
        Guard guard;
        // MAC binding: solo il peer a cui stiamo inviando puo' confermare.
        if (g_txBroadcast || std::memcmp(mac, g_txMac, 6) != 0 ||
            packet.session != g_txSession) {
            return;
        }
        AckSlot& slot = g_ack[packet.flags];
        slot.valid = true;
        slot.session = packet.session;
        slot.seq = packet.seq;
        slot.status = packet.payload[0];
        accepted = true;
    }
    if (accepted) {
        touchPeer(mac, nullptr);
    }
}

bool parseByteToken(const std::string& token, uint8_t& out) {
    if (token.empty() || token.size() > 3) {
        return false;
    }
    int value = 0;
    for (char c : token) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    if (value > 255) {
        return false;
    }
    out = static_cast<uint8_t>(value);
    return true;
}

// Whitelist a matching esatto: niente trim di spazi iniziali, niente
// case-insensitive, niente newline. "BEEP", " beep" e "beep\n" sono invalidi.
CommandSpec parseCommand(const std::string& raw) {
    CommandSpec spec;
    for (char c : raw) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u >= 0x7F) {
            return spec;
        }
    }
    if (raw == "beep") {
        spec.kind = CmdKind::Beep;
        return spec;
    }
    if (raw == "led off") {
        spec.kind = CmdKind::Led;
        return spec;
    }
    if (raw.rfind("led ", 0) == 0) {
        const std::string args = raw.substr(4);
        if (args.empty() || args.front() == ' ' || args.back() == ' ') {
            return spec;
        }
        const size_t first = args.find(' ');
        if (first == std::string::npos) {
            return spec;
        }
        const size_t second = args.find(' ', first + 1);
        if (second == std::string::npos ||
            args.find(' ', second + 1) != std::string::npos) {
            return spec;
        }
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        if (!parseByteToken(args.substr(0, first), r) ||
            !parseByteToken(args.substr(first + 1, second - first - 1), g) ||
            !parseByteToken(args.substr(second + 1), b)) {
            return spec;
        }
        spec.kind = CmdKind::Led;
        spec.r = r;
        spec.g = g;
        spec.b = b;
        return spec;
    }
    if (raw.rfind("msg ", 0) == 0) {
        if (raw.size() <= 4 || raw[4] == ' ') {
            return spec;
        }
        spec.kind = CmdKind::Msg;
        spec.text = raw.substr(4, 80);
    }
    return spec;
}

bool commandSelfTest() {
    return parseCommand("beep").kind == CmdKind::Beep &&
           parseCommand("BEEP").kind == CmdKind::Invalid &&
           parseCommand(" beep").kind == CmdKind::Invalid &&
           parseCommand("beep ").kind == CmdKind::Invalid &&
           parseCommand("beep\n").kind == CmdKind::Invalid &&
           parseCommand("led off").kind == CmdKind::Led &&
           parseCommand("led 0 220 80").kind == CmdKind::Led &&
           parseCommand("led 0 220 256").kind == CmdKind::Invalid &&
           parseCommand("led 0 220").kind == CmdKind::Invalid &&
           parseCommand("msg ciao").kind == CmdKind::Msg &&
           parseCommand("msg").kind == CmdKind::Invalid &&
           parseCommand("MSG ciao").kind == CmdKind::Invalid;
}

void handleCommand(const uint8_t* mac, const ep::Packet& packet) {
    if (packet.len < 1) {
        return;
    }
    const uint8_t nameLen = packet.payload[0];
    if (nameLen > ep::kMaxName ||
        packet.len < static_cast<size_t>(1 + nameLen)) {
        return;
    }
    const std::string sender =
        cleanPeerName(packet.payload + 1, static_cast<size_t>(nameLen));
    const std::string text =
        strictPrintable(packet.payload + 1 + nameLen,
                        packet.len - 1 - static_cast<size_t>(nameLen),
                        ep::kMaxCommand);
    if (text.empty()) {
        return;
    }
    touchPeer(mac, sender.empty() ? nullptr : sender.c_str());
    const CommandSpec spec = parseCommand(text);

    Guard guard;
    const int peerSlot = findPeerLocked(mac);
    const uint32_t now = millis();
    if (peerSlot >= 0) {
        if (g_peers[peerSlot].lastCmdMs != 0 &&
            now - g_peers[peerSlot].lastCmdMs < kPeerCmdIntervalMs) {
            return;  // flood generico di comandi
        }
        g_peers[peerSlot].lastCmdMs = now;
        if (spec.kind == CmdKind::Msg &&
            g_peers[peerSlot].lastMsgMs != 0 &&
            now - g_peers[peerSlot].lastMsgMs < kPeerMsgIntervalMs) {
            return;  // rate limit piu' severo per i messaggi
        }
        if (spec.kind == CmdKind::Msg) {
            g_peers[peerSlot].lastMsgMs = now;
        }
    }
    if (g_pendingCount >= static_cast<int>(sizeof(g_pending) / sizeof(g_pending[0]))) {
        return;
    }
    PendingCmd& pending = g_pending[g_pendingCount++];
    std::snprintf(pending.from, sizeof(pending.from), "%s",
                  sender.empty() ? "peer" : sender.c_str());
    std::snprintf(pending.text, sizeof(pending.text), "%s", text.c_str());
    pending.ignored = (spec.kind == CmdKind::Invalid);
}

void handleClipboard(const uint8_t* mac, const ep::Packet& packet) {
    if (packet.len < 1) {
        return;
    }
    const uint8_t nameLen = packet.payload[0];
    if (nameLen > ep::kMaxName ||
        packet.len < static_cast<size_t>(1 + nameLen)) {
        return;
    }
    const std::string sender =
        cleanPeerName(packet.payload + 1, static_cast<size_t>(nameLen));
    const std::string text =
        printableText(packet.payload + 1 + nameLen,
                      packet.len - 1 - static_cast<size_t>(nameLen),
                      ep::kMaxClipboard);
    touchPeer(mac, sender.empty() ? nullptr : sender.c_str());
    if (text.empty()) {
        return;
    }
    Guard guard;
    // Confronto PRIMA di marcare dirty: un peer che ripete lo stesso testo
    // non deve generare scritture NVS ne' notifiche.
    if (text == g_clipboard) {
        return;
    }
    const int slot = findPeerLocked(mac);
    const uint32_t now = millis();
    if (slot >= 0 && g_peers[slot].lastClipMs != 0 &&
        now - g_peers[slot].lastClipMs < kPeerClipIntervalMs) {
        return;  // rate limit per peer
    }
    if (slot >= 0) {
        g_peers[slot].lastClipMs = now;
    }
    g_clipboard = text;
    g_clipRevision++;
    g_clipDirty = true;
    g_notice = "Clipboard da " + (sender.empty() ? std::string("peer") : sender);
    g_noticeUntil = now + 3000;
    g_beepPending = true;
}

void handleClipboardRequest(const uint8_t* mac, const ep::Packet& packet) {
    if (packet.len < 1 || packet.payload[0] > ep::kMaxName ||
        packet.len < static_cast<size_t>(1 + packet.payload[0])) {
        return;
    }
    const std::string sender =
        cleanPeerName(packet.payload + 1, static_cast<size_t>(packet.payload[0]));
    touchPeer(mac, sender.empty() ? nullptr : sender.c_str());

    std::string name;
    std::string text;
    {
        Guard guard;
        name = g_name;
        text = g_clipboard;
    }
    if (text.empty() || name.size() > ep::kMaxName) {
        return;
    }
    ep::Packet reply{};
    reply.type = ep::TypeClipboard;
    reply.session = esp_random();
    reply.len = static_cast<uint16_t>(1 + name.size() + text.size());
    reply.payload[0] = static_cast<uint8_t>(name.size());
    std::memcpy(reply.payload + 1, name.data(), name.size());
    std::memcpy(reply.payload + 1 + name.size(), text.data(), text.size());
    uint8_t buffer[ep::kMaxPacket];
    const size_t size = ep::encodePacket(reply, buffer, sizeof(buffer));
    if (size == 0 || !ensurePeer(mac)) {
        return;
    }
    esp_now_send(mac, buffer, size);
}

void onReceive(const uint8_t* mac, const uint8_t* data, int len) {
    CallbackScope scope;
    if (!g_ready || mac == nullptr || data == nullptr || len <= 0) {
        return;
    }
    ep::Packet packet{};
    if (!ep::decodePacket(data, static_cast<size_t>(len), packet)) {
        return;
    }
    {
        Guard guard;
        checkRxTimeoutLocked();
    }
    switch (packet.type) {
        case ep::TypeHello: {
            if (packet.len >= 1 && packet.payload[0] <= ep::kMaxName &&
                packet.len >= static_cast<size_t>(1 + packet.payload[0])) {
                const std::string name = cleanPeerName(
                    packet.payload + 1, static_cast<size_t>(packet.payload[0]));
                touchPeer(mac, name.empty() ? nullptr : name.c_str());
            }
            break;
        }
        case ep::TypeFileMeta:
            handleMeta(mac, packet);
            break;
        case ep::TypeFileData:
            handleData(mac, packet);
            break;
        case ep::TypeAck:
            handleAck(mac, packet);
            break;
        case ep::TypeCommand:
            handleCommand(mac, packet);
            break;
        case ep::TypeClipboard:
            handleClipboard(mac, packet);
            break;
        case ep::TypeClipRequest:
            handleClipboardRequest(mac, packet);
            break;
        default:
            break;
    }
}

void snifferCallback(void* buffer, wifi_promiscuous_pkt_type_t type) {
    CallbackScope scope;
    if (buffer == nullptr || type != WIFI_PKT_MGMT) {
        return;
    }
    const wifi_promiscuous_pkt_t* packet =
        static_cast<const wifi_promiscuous_pkt_t*>(buffer);
    const uint8_t* frame = packet->payload;
    const uint16_t sigLen = packet->rx_ctrl.sig_len;
    // Action frame vendor-specific Espressif: cosi' aggiorniamo solo RSSI di
    // frame ESP-NOW e non di qualsiasi beacon vicino.
    if (sigLen < 28 || (frame[0] & 0xFC) != 0xD0 || frame[24] != 0x7F ||
        frame[25] != 0x18 || frame[26] != 0xFE || frame[27] != 0x34) {
        return;
    }
    updatePeerRssi(frame + 10, packet->rx_ctrl.rssi);
}

bool startSniffer() {
    if (g_sniffer || !g_wifiStarted) {
        return g_sniffer;
    }
    wifi_promiscuous_filter_t filter{};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    if (esp_wifi_set_promiscuous_filter(&filter) != ESP_OK) {
        return false;
    }
    if (esp_wifi_set_promiscuous_rx_cb(&snifferCallback) != ESP_OK) {
        return false;
    }
    if (esp_wifi_set_promiscuous(true) != ESP_OK) {
        esp_wifi_set_promiscuous_rx_cb(nullptr);
        return false;
    }
    g_sniffer = true;
    return true;
}

void stopSniffer() {
    if (!g_sniffer) {
        return;
    }
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    g_sniffer = false;
}

bool addBroadcastPeer() {
    esp_now_peer_info_t info{};
    std::memcpy(info.peer_addr, kBroadcastMac, 6);
    info.channel = 0;
    info.ifidx = WIFI_IF_STA;
    info.encrypt = false;
    const esp_err_t err = esp_now_add_peer(&info);
    return err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST;
}

void abortReceiveForExit() {
    // Il lavoro pesante e' gia' nel task UI: qui si chiude solo un eventuale
    // parziale e si scarta un finalize rimasto in coda (es. SD assente).
    // Aspetta i callback gia' entrati: g_ready e' false, quindi non ne
    // arrivano di nuovi e il wait ha una fine pratica.
    for (int i = 0; i < 200 && g_callbacksActive.load() > 0; ++i) {
        delay(5);
    }
    Guard guard;
    if (g_rx.active) {
        closeRxLocked();
    }
    if (g_finalize.pending) {
        SdGuard sd;
        if (!g_finalize.tmpPath.empty()) {
            storage::removeFile(g_finalize.tmpPath, true);
        }
        g_finalize = FinalizeJob{};
    }
}

bool initLocks() {
    if (g_mutex == nullptr) {
        g_mutex = xSemaphoreCreateMutex();
    }
    if (g_sdMutex == nullptr) {
        g_sdMutex = xSemaphoreCreateMutex();
    }
    return g_mutex != nullptr && g_sdMutex != nullptr;
}

void destroyLocksIfSafe() {
    bool pending = false;
    if (g_mutex != nullptr) {
        Guard guard;
        pending = g_finalize.pending;
    }
    if (!g_radioInit && !g_wifiStarted && g_callbacksActive.load() == 0 &&
        !pending) {
        if (g_sdMutex != nullptr) {
            vSemaphoreDelete(g_sdMutex);
            g_sdMutex = nullptr;
        }
        if (g_mutex != nullptr) {
            vSemaphoreDelete(g_mutex);
            g_mutex = nullptr;
        }
    } else {
        logger::warn("ESP-NOW: lock non distrutti (callback/finalize attivi)");
    }
}

bool startRadio() {
    if (!initLocks()) {
        return false;
    }

    g_ready = false;
    g_radioInit = false;
    stopSniffer();
    {
        Guard guard;
        if (g_rx.active) {
            closeRxLocked();
        }
        g_finalize = FinalizeJob{};
        g_peerCount = 0;
    }

    if (!WiFi.mode(WIFI_STA)) {
        return false;
    }
    g_wifiStarted = true;
    WiFi.disconnect();
    WiFi.setSleep(false);  // niente modem sleep: ACK e timeout piu' stabili
    if (esp_wifi_set_channel(g_channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        return false;
    }
    if (esp_now_init() != ESP_OK) {
        return false;
    }
    g_radioInit = true;
    if (esp_now_register_recv_cb(&onReceive) != ESP_OK) {
        return false;
    }
    if (!addBroadcastPeer()) {
        return false;
    }
    if (esp_wifi_get_mac(WIFI_IF_STA, g_selfMac) != ESP_OK) {
        std::memset(g_selfMac, 0, sizeof(g_selfMac));
    }
    g_ready = true;
    g_lastHello = 0;
    logger::info("ESP-NOW: canale %u, heap %u KB",
                 static_cast<unsigned>(g_channel),
                 static_cast<unsigned>(ESP.getFreeHeap() / 1024));
    sendHello();
    return true;
}

void stopRadio() {
    stopSniffer();
    // Prova a chiudere un file gia' completo prima di spegnere la radio.
    serviceFinalize();
    g_ready = false;
    abortReceiveForExit();
    if (g_radioInit) {
        esp_now_unregister_recv_cb();
        esp_now_deinit();
        g_radioInit = false;
    }
    // Deinit non deve lasciare callback che usano i lock mentre li
    // distruggiamo. Se dopo l'attesa ce n'e' ancora uno, i mutex restano
    // vivi (li rilascia il processo) invece di causare use-after-free.
    for (int i = 0; i < 200 && g_callbacksActive.load() > 0; ++i) {
        delay(10);
    }
    if (g_callbacksActive.load() > 0) {
        logger::warn("ESP-NOW: callback ancora attivo, mutex non distrutti");
    }
    if (g_wifiStarted) {
        WiFi.disconnect();
        WiFi.mode(g_prevMode == WIFI_OFF ? WIFI_OFF : g_prevMode);
        g_wifiStarted = false;
    }
    Guard guard;
    g_peerCount = 0;
    logger::info("ESP-NOW: radio chiusa, heap %u KB",
                 static_cast<unsigned>(ESP.getFreeHeap() / 1024));
}

// --- Configurazione e storico ----------------------------------------------

bool saveClipboardIfDirty() {
    std::string text;
    {
        Guard guard;
        if (!g_clipDirty) {
            return true;
        }
        if (g_clipNextSaveAt != 0 &&
            static_cast<int32_t>(millis() - g_clipNextSaveAt) < 0) {
            return false;  // backoff dopo un errore NVS
        }
        text = g_clipboard;
    }
    if (!g_prefsOpen) {
        g_clipNextSaveAt = millis() + 5000;
        return false;
    }

    bool ok = true;
    if (text.empty()) {
        g_prefs.remove("clip");
    } else {
        ok = g_prefs.putString("clip", text.c_str()) == text.size();
    }
    if (ok) {
        Guard guard;
        // dirty = testo non ancora persistito. Si azzera SOLO dopo putString
        // riuscita; se nel frattempo e' arrivato un testo nuovo resta dirty.
        if (g_clipboard == text) {
            g_clipDirty = false;
        }
        g_clipNextSaveAt = 0;
    } else {
        g_clipNextSaveAt = millis() + 5000;
    }
    return ok;
}

bool loadConfig() {
    uint64_t efuse = ESP.getEfuseMac();
    char fallback[17];
    std::snprintf(fallback, sizeof(fallback), "CPT-%04X",
                  static_cast<unsigned>(efuse & 0xFFFF));

    g_prefsOpen = g_prefs.begin(kNamespace, false);
    std::string name = fallback;
    std::string clipboard;
    uint8_t channel = kDefaultChannel;
    if (g_prefsOpen) {
        const String savedName = g_prefs.getString("name", "");
        const String savedClip = g_prefs.getString("clip", "");
        channel = g_prefs.getUChar("ch", kDefaultChannel);
        const std::string cleanName =
            cleanPeerName(reinterpret_cast<const uint8_t*>(savedName.c_str()),
                          savedName.length());
        if (!cleanName.empty()) {
            name = cleanName;
        }
        clipboard = cleanText(
            reinterpret_cast<const uint8_t*>(savedClip.c_str()),
            savedClip.length(), ep::kMaxClipboard);
    }
    if (channel < 1 || channel > 13) {
        channel = kDefaultChannel;
    }

    Guard guard;
    g_name = name;
    g_channel = channel;
    g_clipboard = clipboard;
    g_clipDirty = false;
    g_clipNextSaveAt = 0;
    return true;
}

void saveConfigValue(const char* key, const std::string& value) {
    if (g_prefsOpen) {
        g_prefs.putString(key, value.c_str());
    }
}

void ensureDirs() {
    if (!storage::hasSD()) {
        return;
    }
    SdGuard sd;
    storage::mkdir("/espnow", true);
    storage::mkdir(kInboxDir, true);
    storage::mkdir(kTmpDir, true);
    // Residui di un crash precedente: un .part non deve mai raggiungere
    // l'inbox ne' occupare spazio.
    const std::vector<storage::Entry> stale = storage::listEntries(kTmpDir, true);
    for (const storage::Entry& entry : stale) {
        if (!entry.isDir) {
            storage::removeFile(std::string(kTmpDir) + "/" + entry.name, true);
        }
    }
}

enum class TailResult { Missing, Ok, Error };

// Legge la coda del file (max maxBytes). Richiede SdGuard.
TailResult readTextTailLocked(const std::string& path, size_t maxBytes,
                              std::string& out) {
    out.clear();
    File file = storage::openRead(path, true);
    if (!file) {
        return storage::exists(path, true) ? TailResult::Error
                                            : TailResult::Missing;
    }
    const size_t size = static_cast<size_t>(file.size());
    const size_t start = size > maxBytes ? size - maxBytes : 0;
    if (start > 0 && !file.seek(start)) {
        file.close();
        return TailResult::Error;
    }
    out.reserve(std::min(size - start, maxBytes));
    uint8_t buffer[256];
    size_t total = 0;
    while (total < maxBytes) {
        const size_t want = std::min(sizeof(buffer), maxBytes - total);
        const int got = file.read(buffer, want);
        if (got < 0) {
            file.close();
            return TailResult::Error;
        }
        if (got == 0) {
            break;
        }
        out.append(reinterpret_cast<const char*>(buffer),
                   static_cast<size_t>(got));
        total += static_cast<size_t>(got);
    }
    file.close();
    if (start > 0) {
        // La coda puo' iniziare a meta' riga: scarta il primo frammento.
        const size_t newline = out.find('\n');
        if (newline == std::string::npos) {
            out.clear();
        } else {
            out.erase(0, newline + 1);
        }
    }
    return TailResult::Ok;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string line;
    for (char c : text) {
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            if (!line.empty()) {
                lines.push_back(line);
                line.clear();
            }
        } else {
            line.push_back(c);
        }
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    return lines;
}

void logCommand(bool tx, const std::string& who, const std::string& text) {
    if (!storage::hasSD()) {
        return;
    }
    std::string out;
    bool writeOk = false;
    {
        SdGuard sd;
        storage::mkdir("/espnow", true);
        std::string history;
        const TailResult result =
            readTextTailLocked(kHistoryPath, kHistoryReadBytes, history);
        if (result == TailResult::Error) {
            logger::warn("ESP-NOW: storico non leggibile, non lo sovrascrivo");
            return;
        }
        std::vector<std::string> lines = splitLines(history);
        while (lines.size() >= kHistoryMaxLines) {
            lines.erase(lines.begin());
        }
        // Testo esatto (spazi inclusi) per distinguere " beep" da "beep".
        std::string clean = text.size() > 64 ? text.substr(0, 64) : text;
        char stamp[16];
        std::snprintf(stamp, sizeof(stamp), "[%lus]",
                      static_cast<unsigned long>(millis() / 1000));
        lines.push_back(std::string(stamp) + (tx ? " TX " : " RX ") +
                        (who.empty() ? "peer" : who) + ": " +
                        (clean.empty() ? "-" : clean));
        for (const std::string& line : lines) {
            out += line;
            out.push_back('\n');
        }
        writeOk = storage::writeText(kHistoryPath, out, true);
    }
    if (!writeOk) {
        logger::warn("ESP-NOW: scrittura storico fallita");
    }
}

void showHistory() {
    if (!storage::hasSD()) {
        ui::message("Storico comandi", {"Nessun comando registrato."},
                    "ESP-NOW > Comandi");
        return;
    }
    std::string history;
    TailResult result = TailResult::Error;
    {
        SdGuard sd;
        result =
            readTextTailLocked(kHistoryPath, kHistoryReadBytes, history);
    }
    if (result == TailResult::Error) {
        // File grande/illeggibile: non dire "nessun comando", sarebbe falso.
        ui::message("Storico comandi",
                    {"!Storico non leggibile.", "Controlla la microSD."},
                    "ESP-NOW > Comandi");
        return;
    }
    std::vector<std::string> lines = splitLines(history);
    if (result == TailResult::Missing || lines.empty()) {
        ui::message("Storico comandi", {"Nessun comando registrato."},
                    "ESP-NOW > Comandi");
        return;
    }
    const size_t keep = 6;
    if (lines.size() > keep) {
        lines.erase(lines.begin(), lines.end() - static_cast<long>(keep));
    }
    ui::message("Storico comandi", lines, "ESP-NOW > Comandi");
}

// --- Comandi e clipboard ----------------------------------------------------

void applyLed(uint8_t r, uint8_t g, uint8_t b) {
    // Cardputer ADV: WS2812 sullo Stamp-S3, esposto da M5Unified.
    M5.Led.setAllColor(r, g, b);
    M5.Led.display();
}

void processPending(const PendingCmd& pending) {
    const std::string from = pending.from[0] ? pending.from : "peer";
    const std::string text = pending.text;
    logCommand(false, from, text);
    if (pending.ignored) {
        setNotice("Comando ignorato: " + shorten(text, 10), 2500);
        return;
    }
    const CommandSpec spec = parseCommand(text);
    switch (spec.kind) {
        case CmdKind::Beep:
            // Un solo tono breve: nessun delay bloccante nel task UI.
            M5Cardputer.Speaker.tone(1900, 120);
            setNotice("Beep remoto", 1200);
            break;
        case CmdKind::Led:
            applyLed(spec.r, spec.g, spec.b);
            setNotice("LED remoto", 1200);
            break;
        case CmdKind::Msg:
            // Nessuna modale bloccante: un flood di msg non puo' congelare
            // la UI. Il testo resta visibile come banner per qualche secondo.
            M5Cardputer.Speaker.tone(2400, 60);
            setNotice(from + ": " + shorten(spec.text, 28), 6000);
            break;
        case CmdKind::Invalid:
            break;
    }
}

void serviceBackground() {
    saveClipboardIfDirty();
    serviceFinalize();
    bool timedOut = false;
    {
        Guard guard;
        timedOut = checkRxTimeoutLocked();
    }
    if (timedOut) {
        setNotice("Ricezione scaduta", 2500);
    }
}

void serviceMesh() {
    for (;;) {
        PendingCmd pending;
        bool have = false;
        {
            Guard guard;
            if (g_pendingCount > 0) {
                pending = g_pending[0];
                for (int i = 1; i < g_pendingCount; ++i) {
                    g_pending[i - 1] = g_pending[i];
                }
                g_pendingCount--;
                have = true;
            }
        }
        if (!have) {
            break;
        }
        processPending(pending);
    }

    serviceBackground();

    bool beep = false;
    {
        Guard guard;
        if (g_beepPending) {
            g_beepPending = false;
            beep = true;
        }
    }
    if (beep) {
        M5Cardputer.Speaker.tone(2000, 70);
    }
    if (g_ready && millis() - g_lastHello >= kHelloIntervalMs) {
        sendHello();
    }
}

bool sendCommand(const std::string& raw) {
    if (!g_ready) {
        return false;
    }
    const std::string text = cleanText(raw, ep::kMaxCommand);
    const CommandSpec spec = parseCommand(text);
    if (spec.kind == CmdKind::Invalid) {
        return false;
    }
    const std::string name = ownName();
    ep::Packet packet{};
    packet.type = ep::TypeCommand;
    packet.session = esp_random();
    packet.len = static_cast<uint16_t>(1 + name.size() + text.size());
    packet.payload[0] = static_cast<uint8_t>(name.size());
    std::memcpy(packet.payload + 1, name.data(), name.size());
    std::memcpy(packet.payload + 1 + name.size(), text.data(), text.size());
    if (!sendBroadcast(packet)) {
        return false;
    }
    logCommand(true, name, text);
    return true;
}

bool sendClipboardText(const std::string& raw) {
    const std::string text = cleanText(raw, ep::kMaxClipboard);
    if (!g_ready || text.empty()) {
        return false;
    }
    bool changed = false;
    {
        Guard guard;
        if (text != g_clipboard) {
            g_clipboard = text;
            g_clipRevision++;
            g_clipDirty = true;
            changed = true;
        }
    }
    const std::string name = ownName();
    ep::Packet packet{};
    packet.type = ep::TypeClipboard;
    packet.session = esp_random();
    packet.len = static_cast<uint16_t>(1 + name.size() + text.size());
    packet.payload[0] = static_cast<uint8_t>(name.size());
    std::memcpy(packet.payload + 1, name.data(), name.size());
    std::memcpy(packet.payload + 1 + name.size(), text.data(), text.size());
    const bool sent = sendBroadcast(packet);
    if (changed) {
        saveClipboardIfDirty();
    }
    return sent;
}

bool requestClipboard() {
    if (!g_ready) {
        return false;
    }
    uint32_t before = 0;
    {
        Guard guard;
        before = g_clipRevision;
    }
    const std::string name = ownName();
    ep::Packet packet{};
    packet.type = ep::TypeClipRequest;
    packet.session = esp_random();
    packet.len = static_cast<uint16_t>(1 + name.size());
    packet.payload[0] = static_cast<uint8_t>(name.size());
    std::memcpy(packet.payload + 1, name.data(), name.size());
    if (!sendBroadcast(packet)) {
        return false;
    }

    const uint32_t start = millis();
    bool changed = false;
    uint32_t lastDraw = 0;
    while (millis() - start < 2500) {
        serviceBackground();
        {
            Guard guard;
            if (g_clipRevision != before) {
                changed = true;
                break;
            }
        }
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }
        const uint32_t now = millis();
        if (now - lastDraw >= 250) {
            display::clear();
            display::header("Clipboard", "ESP-NOW");
            display::text(4, 40, "Sync dai peer...", display::kText, 2);
            display::text(4, 60, "Attesa risposta.", display::kTextDim, 0);
            display::footer("ESC=annulla");
            display::push();
            lastDraw = now;
        }
        delay(5);
    }
    saveClipboardIfDirty();
    navigation::waitRelease();
    return changed;
}

// --- Invio file -------------------------------------------------------------

void drawTransfer(const std::string& target, const std::string& file,
                  uint32_t sent, uint32_t total, uint16_t chunk,
                  uint16_t chunks) {
    display::clear();
    display::header("Invio file", "ESP-NOW");
    display::text(4, 24, shorten("A: " + target, 39), display::kText, 0);
    display::text(4, 34, shorten(file, 34), display::kText, 0);
    const float fraction =
        total > 0 ? static_cast<float>(sent) / static_cast<float>(total) : 0.0f;
    display::progressBar(4, 50, display::width() - 8, 8, fraction,
                         display::kAccent);
    char line[48];
    std::snprintf(line, sizeof(line), "%lu/%lu KB  %u%%",
                  static_cast<unsigned long>(sent / 1024),
                  static_cast<unsigned long>(total / 1024),
                  static_cast<unsigned>(fraction * 100.0f));
    display::text(4, 64, line, display::kText, 2);
    std::snprintf(line, sizeof(line), "Blocco %u/%u",
                  static_cast<unsigned>(chunk + 1),
                  static_cast<unsigned>(chunks));
    display::text(4, 80, line, display::kTextDim, 0);
    display::footer("ESC=annulla");
    display::push();
}

uint8_t sendReliable(const uint8_t* mac, const ep::Packet& packet,
                     uint8_t ackKind, uint16_t seq) {
    // Si ritrasmette solo in caso di timeout o errore di invio: un ACK
    // applicativo (anche negativo) e' definitivo per quel blocco. Cosi' un
    // ACK in ritardo non viene scambiato per la risposta al nuovo tentativo.
    for (uint8_t attempt = 0; attempt < kTxTries; ++attempt) {
        if (transmit(mac, packet) == ESP_OK) {
            const uint8_t status = waitAck(ackKind, seq, kAckTimeoutMs);
            if (status != kTxTimeout) {
                return status;
            }
        }
        delay(20);
    }
    return kTxTimeout;
}

const char* ackFailureText(uint8_t status) {
    switch (status) {
        case ep::StCrc:
            return "CRC rifiutato dal peer.";
        case ep::StIo:
            return "Errore di scrittura sul peer.";
        case ep::StSeq:
            return "Sequenza fuori ordine.";
        case ep::StBusy:
            return "Peer occupato in un altro trasferimento.";
        case ep::StSize:
            return "Dimensione o blocco non valido.";
        case kTxAborted:
            return "Invio annullato.";
        default:
            return "Timeout: peer non raggiungibile.";
    }
}

void transferFile(const std::string& path, uint32_t declaredSize,
                  const uint8_t* target, const std::string& targetLabel,
                  bool broadcast) {
    if (!g_ready) {
        ui::message("Invia file", {"!Radio non pronta.", "Rientra nel modulo."},
                    "ESP-NOW");
        return;
    }
    bool exists = false;
    uint32_t size = 0;
    {
        SdGuard sd;
        exists = storage::exists(path, true);
        size = static_cast<uint32_t>(storage::fileSize(path, true));
    }
    if (!storage::hasSD() || !exists) {
        ui::message("Invia file", {"!File non trovato.", path}, "ESP-NOW");
        return;
    }
    if (size == 0 || size > ep::kMaxFileBytes || size != declaredSize) {
        ui::message("Invia file", {"!File non valido o troppo grande.",
                                   "Limite: 512 KB."},
                    "ESP-NOW");
        return;
    }

    display::clear();
    display::header("Invio file", "ESP-NOW");
    display::text(4, 40, "Calcolo CRC32...", display::kText, 2);
    display::footer("attendi");
    display::push();

    uint32_t fileCrcValue = 0;
    uint32_t crcSize = 0;
    bool crcOk = false;
    {
        SdGuard sd;
        crcOk = fileCrc(path, fileCrcValue, crcSize);
    }
    if (!crcOk || crcSize != size) {
        ui::message("Invia file", {"!Lettura del file fallita."}, "ESP-NOW");
        return;
    }

    if (!broadcast && target != nullptr && !ensurePeer(target)) {
        ui::message("Invia file", {"!Peer non aggiungibile.", "Riprova il scan."},
                    "ESP-NOW");
        return;
    }

    const std::string fileName = baseName(path);
    if (fileName.size() > ep::kMaxFileName) {
        ui::message("Invia file", {"!Nome file troppo lungo."}, "ESP-NOW");
        return;
    }
    const std::string name = ownName();
    const uint16_t totalChunks =
        static_cast<uint16_t>((size + ep::kChunkSize - 1) / ep::kChunkSize);
    uint32_t session = esp_random();
    if (session == 0) {
        session = 1;
    }
    beginTx(session, target, broadcast);

    ep::Packet meta{};
    meta.type = ep::TypeFileMeta;
    meta.session = session;
    meta.flags = broadcast ? ep::kFlagNoAck : 0;
    meta.payload[0] = static_cast<uint8_t>(fileName.size());
    std::memcpy(meta.payload + 1, fileName.data(), fileName.size());
    ep::writeU32(meta.payload + 1 + fileName.size(), size);
    ep::writeU32(meta.payload + 1 + fileName.size() + 4, fileCrcValue);
    meta.payload[1 + fileName.size() + 8] = static_cast<uint8_t>(name.size());
    std::memcpy(meta.payload + 1 + fileName.size() + 9, name.data(), name.size());
    meta.len = static_cast<uint16_t>(1 + fileName.size() + 8 + 1 + name.size());

    if (broadcast) {
        if (!sendBroadcast(meta)) {
            ui::message("Invia file", {"!Invio broadcast fallito."}, "ESP-NOW");
            return;
        }
        delay(40);
        sendBroadcast(meta);  // meta duplicata: un frame broadcast puo' perdersi
    } else {
        const uint8_t metaStatus =
            sendReliable(target, meta, ep::AckMeta, 0);
        if (metaStatus != ep::StOk) {
            ui::message("Invia file",
                        {"!Il peer non ha accettato il file.",
                         ackFailureText(metaStatus)},
                        "ESP-NOW");
            return;
        }
    }

    File file;
    {
        SdGuard sd;
        file = storage::openRead(path, true);
    }
    if (!file) {
        ui::message("Invia file", {"!Impossibile riaprire il file."}, "ESP-NOW");
        return;
    }

    bool failed = false;
    bool aborted = false;
    uint32_t sent = 0;
    for (uint16_t seq = 0; seq < totalChunks; ++seq) {
        uint8_t buffer[ep::kChunkSize];
        const size_t want =
            seq + 1 == totalChunks
                ? static_cast<size_t>(size - static_cast<uint32_t>(seq) * ep::kChunkSize)
                : ep::kChunkSize;
        int got = 0;
        {
            SdGuard sd;
            got = file.read(buffer, want);
        }
        if (got != static_cast<int>(want)) {
            failed = true;
            break;
        }

        ep::Packet packet{};
        packet.type = ep::TypeFileData;
        packet.session = session;
        packet.seq = seq;
        packet.flags = (broadcast ? ep::kFlagNoAck : 0) |
                       (seq + 1 == totalChunks ? ep::kFlagLast : 0);
        packet.len = static_cast<uint16_t>(want);
        std::memcpy(packet.payload, buffer, want);

        if (broadcast) {
            if (!sendBroadcast(packet)) {
                failed = true;
                break;
            }
            delay(2);
            if (navigation::poll().type == navigation::Event::Back) {
                aborted = true;
                break;
            }
        } else {
            const uint8_t status =
                sendReliable(target, packet, ep::AckData, seq);
            if (status != ep::StOk) {
                failed = true;
                aborted = (status == kTxAborted);
                break;
            }
        }
        sent += static_cast<uint32_t>(want);
        if (seq % 4 == 0 || seq + 1 == totalChunks) {
            drawTransfer(targetLabel, fileName, sent, size, seq, totalChunks);
        }
    }
    {
        SdGuard sd;
        file.close();
    }
    drawTransfer(targetLabel, fileName, sent, size,
                 sent >= size ? static_cast<uint16_t>(totalChunks - 1) : 0,
                 totalChunks);

    if (aborted) {
        ui::message("Invia file",
                    {"Invio annullato.", "Il peer scarta il parziale."},
                    "ESP-NOW");
        navigation::waitRelease();
        return;
    }
    if (failed) {
        ui::message("Invia file",
                    {"!Invio interrotto.", "Il peer scarta il parziale."},
                    "ESP-NOW");
        navigation::waitRelease();
        return;
    }

    if (broadcast) {
        ui::message("Invia file",
                    {">Broadcast completato.", "Senza ACK: il peer verifica",
                     "il CRC prima di pubblicare."},
                    "ESP-NOW");
    } else {
        const uint8_t finalStatus =
            waitAck(ep::AckFinal, static_cast<uint16_t>(totalChunks - 1),
                    kFinalAckTimeoutMs);
        if (finalStatus == ep::StOk) {
            ui::message("Invia file", {">File inviato e verificato.",
                                       formatBytes(size) + "  CRC OK"},
                        "ESP-NOW");
        } else if (finalStatus == ep::StCrc) {
            ui::message("Invia file",
                        {"!Il peer ha rifiutato il file.", "CRC non valido."},
                        "ESP-NOW");
        } else if (finalStatus == kTxAborted) {
            ui::message("Invia file", {"Invio annullato."}, "ESP-NOW");
        } else if (finalStatus == kTxTimeout) {
            ui::message("Invia file",
                        {">File trasmesso.", "!Conferma finale assente.",
                         "Verifica sull'altro dispositivo."},
                        "ESP-NOW");
        } else {
            ui::message("Invia file",
                        {"!Il peer ha rifiutato il file.",
                         ackFailureText(finalStatus)},
                        "ESP-NOW");
        }
    }
    navigation::waitRelease();
}

// --- Browsing SD e inbox ----------------------------------------------------

std::string joinPath(const std::string& base, const std::string& name) {
    if (base.empty() || base == "/") {
        return "/" + name;
    }
    return base.back() == '/' ? base + name : base + "/" + name;
}

void chooseRecipient(const std::string& path, uint32_t size);

void chooseFile(const std::string& path) {
    uint32_t size = 0;
    bool exists = false;
    {
        SdGuard sd;
        exists = storage::exists(path, true);
        size = static_cast<uint32_t>(storage::fileSize(path, true));
    }
    if (!exists || size == 0) {
        ui::message("Scegli file", {"!File non leggibile.", path}, "ESP-NOW");
        return;
    }
    if (size > ep::kMaxFileBytes) {
        ui::message("Scegli file",
                    {"!File troppo grande.", "Limite ESP-NOW: 512 KB.",
                     formatBytes(size)},
                    "ESP-NOW");
        return;
    }
    chooseRecipient(path, size);
}

void browseFiles(const std::string& path, int depth) {
    if (depth > 6) {
        ui::message("Scegli file", {"!Cartelle troppo annidate."}, "ESP-NOW");
        return;
    }
    if (!storage::hasSD()) {
        ui::message("Scegli file",
                    {"!microSD non montata.", "Inserisci una card e riavvia."},
                    "ESP-NOW");
        return;
    }
    std::vector<storage::Entry> entries;
    {
        SdGuard sd;
        entries = storage::listEntries(path, true);
    }
    if (entries.size() > 150) {
        entries.resize(150);
    }
    if (entries.empty()) {
        ui::message("Scegli file", {"Cartella vuota.", path}, "ESP-NOW");
        return;
    }

    MenuManager menu("Scegli file", path == "/" ? "ESP-NOW > Invia" : path);
    for (const storage::Entry& entry : entries) {
        const std::string child = joinPath(path, entry.name);
        if (entry.isDir) {
            menu.addItem(entry.name, "folder",
                         [child, depth] { browseFiles(child, depth + 1); });
        } else {
            menu.addItem(entry.name, "file", [child] { chooseFile(child); });
        }
    }
    menu.setHint("ESC=indietro  ENTER=scegli");
    menu.run(true);
}

void chooseRecipient(const std::string& path, uint32_t size) {
    const std::vector<Peer> peers = snapshotPeers();
    MenuManager menu("Destinatario", "ESP-NOW > Invia");
    menu.addItem(kBroadcastLabel, "radar", [path, size] {
        transferFile(path, size, nullptr, kBroadcastLabel, true);
    });
    for (const Peer& peer : peers) {
        Mac mac{};
        std::memcpy(mac.data(), peer.mac, 6);
        char macString[18];
        macText(peer.mac, macString);
        const std::string label =
            std::string(peer.name[0] ? peer.name : "Peer") + " " +
            std::string(macString + 12);
        menu.addItem(label, "chat", [path, size, mac, label] {
            transferFile(path, size, mac.data(), label, false);
        });
    }
    menu.setHint(peers.empty() ? "Nessun peer: solo broadcast"
                               : "ESC=annulla  ENTER=invia");
    menu.run(true);
}

void verifyInboxFile(const std::string& path) {
    uint32_t crc = 0;
    uint32_t size = 0;
    bool crcOk = false;
    std::string meta;
    bool metaRead = false;
    {
        SdGuard sd;
        crcOk = fileCrc(path, crc, size);
        if (crcOk) {
            metaRead = storage::readText(path + ".crc", meta, 64, true);
        }
    }
    if (!crcOk) {
        ui::message("Verifica file", {"!Lettura del file fallita.", path},
                    "ESP-NOW > Inbox");
        return;
    }
    unsigned int expectedCrc = 0;
    unsigned long expectedSize = 0;
    const bool haveMeta =
        metaRead &&
        std::sscanf(meta.c_str(), "%x %lu", &expectedCrc, &expectedSize) == 2;

    std::vector<std::string> lines;
    lines.push_back(baseName(path));
    lines.push_back("Dimensione: " + formatBytes(size));
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "CRC32: %08lX",
                  static_cast<unsigned long>(crc));
    lines.push_back(buffer);
    if (!haveMeta) {
        lines.push_back("!Metadati CRC assenti");
        lines.push_back("Integrita' non verificabile.");
    } else if (expectedSize == size && expectedCrc == crc) {
        lines.push_back(">CRC valido: file integro");
    } else {
        std::snprintf(buffer, sizeof(buffer), "!Atteso: %08lX %lu",
                      static_cast<unsigned long>(expectedCrc), expectedSize);
        lines.push_back(buffer);
        lines.push_back("!File corrotto o modificato");
    }
    ui::message("Verifica file", lines, "ESP-NOW > Inbox");
}

void deleteInboxFile() {
    std::vector<storage::Entry> entries;
    {
        SdGuard sd;
        entries = storage::listEntries(kInboxDir, true);
    }
    std::vector<storage::Entry> files;
    for (const storage::Entry& entry : entries) {
        if (!entry.isDir && !isCrcSidecar(entry.name)) {
            files.push_back(entry);
        }
    }
    if (files.empty()) {
        ui::message("Cancella file", {"Inbox vuota."}, "ESP-NOW > Inbox");
        return;
    }
    MenuManager menu("Cancella", "ESP-NOW > Inbox");
    for (const storage::Entry& entry : files) {
        const std::string path = joinPath(kInboxDir, entry.name);
        menu.addItem(entry.name, "file", [path, entry] {
            if (!ui::confirm("Cancella file",
                             {entry.name, "L'operazione e' irreversibile."},
                             "ESP-NOW > Inbox")) {
                return;
            }
            {
                SdGuard sd;
                storage::removeFile(path, true);
                storage::removeFile(path + ".crc", true);
            }
            ui::message("Cancella file", {">File rimosso.", entry.name},
                        "ESP-NOW > Inbox");
        });
    }
    menu.run(true);
}

void inboxMenu() {
    if (!storage::hasSD()) {
        ui::message("Inbox", {"!microSD non montata."}, "ESP-NOW");
        return;
    }
    std::vector<storage::Entry> entries;
    {
        SdGuard sd;
        entries = storage::listEntries(kInboxDir, true);
    }
    std::vector<storage::Entry> files;
    for (const storage::Entry& entry : entries) {
        if (entry.isDir || isCrcSidecar(entry.name)) {
            continue;
        }
        files.push_back(entry);
    }
    if (files.empty()) {
        ui::message("Inbox",
                    {"Nessun file ricevuto.", "I file arrivano in",
                     "/espnow/inbox/."},
                    "ESP-NOW");
        return;
    }
    MenuManager menu("Inbox", "ESP-NOW");
    for (const storage::Entry& entry : files) {
        const std::string path = joinPath(kInboxDir, entry.name);
        menu.addItem(entry.name, "file",
                     [path] { verifyInboxFile(path); });
    }
    menu.addSeparator("Gestione");
    menu.addItem("Cancella file...", "settings", deleteInboxFile);
    menu.setHint("ESC=indietro  ENTER=verifica");
    menu.run(true);
}

// --- Menu comandi, clipboard, peer, setup ----------------------------------

void sendPresetCommand(const std::string& command, const std::string& label) {
    if (!sendCommand(command)) {
        ui::message("Comandi", {"!Invio fallito.", "Radio non pronta."},
                    "ESP-NOW > Comandi");
        return;
    }
    ui::message("Comandi", {">Comando inviato: " + label,
                            "Broadcast ai peer."},
                "ESP-NOW > Comandi");
}

void promptMessageCommand() {
    std::vector<textinput::Field> fields(1);
    fields[0].label = "Messaggio";
    fields[0].maxLen = 80;
    if (!textinput::edit("Messaggio", "ESP-NOW > Comandi", fields)) {
        return;
    }
    const std::string text = cleanText(fields[0].value, 80);
    if (text.empty()) {
        return;
    }
    sendPresetCommand("msg " + text, "msg " + shorten(text, 18));
}

void commandMenu() {
    MenuManager menu("Comandi", "ESP-NOW");
    menu.addItem("Beep", "music",
                 [] { sendPresetCommand("beep", "beep"); });
    menu.addItem("LED verde", "sensors", [] {
        sendPresetCommand("led 0 220 80", "led verde");
    });
    menu.addItem("LED rosso", "sensors", [] {
        sendPresetCommand("led 255 40 0", "led rosso");
    });
    menu.addItem("LED off", "sensors",
                 [] { sendPresetCommand("led off", "led off"); });
    menu.addItem("Messaggio...", "chat", promptMessageCommand);
    menu.addSeparator("Storico");
    menu.addItem("Mostra storico", "file", showHistory);
    menu.addItem("Cancella storico", "settings", [] {
        if (!ui::confirm("Storico comandi",
                         {"Cancellare /espnow/commands.txt?"},
                         "ESP-NOW > Comandi")) {
            return;
        }
        {
            SdGuard sd;
            storage::removeFile(kHistoryPath, true);
        }
        ui::message("Storico comandi", {">Storico cancellato."},
                    "ESP-NOW > Comandi");
    });
    menu.setHint("ESC=indietro  ENTER=invia");
    menu.run(true);
}

void showClipboard() {
    std::string text;
    {
        Guard guard;
        text = g_clipboard;
    }
    if (text.empty()) {
        ui::message("Clipboard", {"Nessun testo condiviso."}, "ESP-NOW");
        return;
    }
    ui::message("Clipboard", wrapLines(text, 38, 6), "ESP-NOW");
}

void promptClipboard() {
    std::string current;
    {
        Guard guard;
        current = g_clipboard;
    }
    std::vector<textinput::Field> fields(1);
    fields[0].label = "Testo";
    fields[0].maxLen = ep::kMaxClipboard;
    fields[0].value = current;
    if (!textinput::edit("Clipboard", "ESP-NOW", fields)) {
        return;
    }
    const std::string text = cleanText(fields[0].value, ep::kMaxClipboard);
    if (text.empty()) {
        ui::message("Clipboard", {"!Testo vuoto."}, "ESP-NOW");
        return;
    }
    if (!sendClipboardText(text)) {
        ui::message("Clipboard", {"!Invio fallito.", "Radio non pronta."},
                    "ESP-NOW");
        return;
    }
    ui::message("Clipboard", {">Testo condiviso.", formatBytes(text.size())},
                "ESP-NOW");
}

void clipboardMenu() {
    MenuManager menu("Clipboard", "ESP-NOW");
    menu.addItem("Invia testo...", "chat", promptClipboard);
    menu.addItem("Mostra ultimo", "file", showClipboard);
    menu.addItem("Sync dai peer", "radar", [] {
        if (!g_ready) {
            ui::message("Clipboard", {"!Radio non pronta."}, "ESP-NOW");
            return;
        }
        if (!requestClipboard()) {
            ui::message("Clipboard", {"Nessun peer ha risposto.",
                                      "Serve almeno un peer attivo."},
                        "ESP-NOW");
            return;
        }
        showClipboard();
    });
    menu.setHint("ESC=indietro  ENTER=apri");
    menu.run(true);
}

void drawPeerScan(const Peer* peers, int count, int top) {
    display::clear();
    display::header("Peer", "ESP-NOW");
    char line[64];
    std::snprintf(line, sizeof(line), "Io: %s  %s", shortName(ownName()).c_str(),
                  ownMacText().c_str());
    display::text(4, 23, shorten(line, 39), display::kTextDim, 0);
    const int rows = 7;
    int shown = 0;
    for (int i = top; i < count && shown < rows; ++i, ++shown) {
        char mac[18];
        macText(peers[i].mac, mac);
        char rssi[6];
        if (peers[i].rssi <= -120) {
            std::snprintf(rssi, sizeof(rssi), "--");
        } else {
            std::snprintf(rssi, sizeof(rssi), "%d",
                          static_cast<int>(peers[i].rssi));
        }
        const uint32_t age = (millis() - peers[i].lastSeen) / 1000;
        char name[9];
        std::snprintf(name, sizeof(name), "%.8s",
                      peers[i].name[0] ? peers[i].name : "?");
        std::snprintf(line, sizeof(line), "%-8s %s %4s %3lus", name, mac, rssi,
                      static_cast<unsigned long>(age));
        display::text(4, 34 + shown * 10, line, display::kText, 0);
    }
    if (count == 0) {
        display::text(4, 40, "Nessun peer. Attendi il scan.", display::kTextDim, 0);
    } else if (count > rows) {
        display::text(4, 34 + rows * 10, "+" + std::to_string(count - rows) + " altri",
                      display::kTextDim, 0);
    }
    char trailing[16];
    std::snprintf(trailing, sizeof(trailing), "%d peer", count);
    display::footer("ESC=torna  ;/.=scorri", trailing);
    display::push();
}

void peerScan() {
    if (!g_ready) {
        ui::message("Peer", {"!Radio non pronta."}, "ESP-NOW");
        return;
    }
    startSniffer();
    sendHello();
    int top = 0;
    int count = 0;
    bool dirty = true;
    uint32_t lastDraw = 0;
    uint32_t lastHello = millis();
    for (;;) {
        const uint32_t now = millis();
        if (dirty || now - lastDraw >= 400) {
            const std::vector<Peer> peers = snapshotPeers();
            count = static_cast<int>(peers.size());
            top = std::max(0, std::min(top, std::max(0, count - 7)));
            drawPeerScan(peers.data(), count, top);
            lastDraw = now;
            dirty = false;
        }
        if (now - lastHello >= 2500) {
            sendHello();
            lastHello = now;
        }
        serviceMesh();
        const navigation::EventData event = navigation::poll();
        if (event.type == navigation::Event::Back) {
            break;
        }
        if (event.type == navigation::Event::Up && top > 0) {
            --top;
            dirty = true;
        } else if (event.type == navigation::Event::Down &&
                   top < std::max(0, count - 7)) {
            ++top;
            dirty = true;
        }
        delay(5);
    }
    stopSniffer();
    navigation::waitRelease();
}

void editMeshName() {
    std::vector<textinput::Field> fields(1);
    fields[0].label = "Nome mesh";
    fields[0].maxLen = ep::kMaxName;
    fields[0].value = ownName();
    if (!textinput::edit("Nome mesh", "ESP-NOW", fields)) {
        return;
    }
    const std::string name = cleanPeerName(
        reinterpret_cast<const uint8_t*>(fields[0].value.c_str()),
        fields[0].value.size());
    if (name.empty()) {
        ui::message("Nome mesh", {"!Nome non valido."}, "ESP-NOW");
        return;
    }
    {
        Guard guard;
        g_name = name;
    }
    saveConfigValue("name", name);
    sendHello();
    ui::message("Nome mesh", {">Nome aggiornato.", name}, "ESP-NOW");
}

void editChannel() {
    std::vector<textinput::Field> fields(1);
    fields[0].label = "Canale (1-13)";
    fields[0].maxLen = 2;
    fields[0].numeric = true;
    fields[0].value = std::to_string(g_channel);
    if (!textinput::edit("Canale", "ESP-NOW", fields)) {
        return;
    }
    long parsed = 0;
    if (!textinput::parseInt(fields[0].value, parsed) || parsed < 1 ||
        parsed > 13) {
        ui::message("Canale", {"!Canale non valido.", "Scegli 1..13."},
                    "ESP-NOW");
        return;
    }
    const uint8_t previous = g_channel;
    g_channel = static_cast<uint8_t>(parsed);
    stopRadio();
    if (!startRadio()) {
        g_channel = previous;
        stopRadio();
        const bool restored = startRadio();
        ui::message("Canale",
                    {"!Canale non disponibile.",
                     restored ? "Ripristinato il precedente."
                              : "!Radio non ripartita."},
                    "ESP-NOW");
        return;
    }
    if (g_prefsOpen) {
        g_prefs.putUChar("ch", g_channel);
    }
    ui::message("Canale",
                {">Canale aggiornato.", std::to_string(g_channel)},
                "ESP-NOW");
}

void meshInfo() {
    const std::vector<Peer> peers = snapshotPeers();
    std::vector<std::string> lines;
    lines.push_back("Nome: " + ownName());
    lines.push_back("MAC: " + ownMacText());
    lines.push_back("Canale: " + std::to_string(g_channel));
    lines.push_back("Peer visti: " + std::to_string(peers.size()));
    lines.push_back("Heap libero: " + std::to_string(ESP.getFreeHeap() / 1024) +
                    " KB");
    ui::message("Info mesh", lines, "ESP-NOW");
}

void setupMenu() {
    MenuManager menu("Setup", "ESP-NOW");
    menu.addItem("Nome mesh...", "chat", editMeshName);
    menu.addItem("Canale...", "radar", editChannel);
    menu.addItem("Info mesh", "settings", meshInfo);
    menu.addItem("Invia announce", "radar", [] {
        if (!g_ready) {
            ui::message("Announce", {"!Radio non pronta."}, "ESP-NOW");
            return;
        }
        sendHello();
        ui::message("Announce", {">Announce inviato.", "I peer ti vedranno."},
                    "ESP-NOW");
    });
    menu.setHint("ESC=indietro  ENTER=apri");
    menu.run(true);
}

// --- Menu principale --------------------------------------------------------

void drawMainMenu(int selected) {
    display::clear();
    display::header("ESP-NOW", "Menu principale");
    char status[64];
    std::snprintf(status, sizeof(status), "Nome: %s  CH:%u  Peer:%d",
                  shorten(ownName(), 12).c_str(),
                  static_cast<unsigned>(g_channel), peerCount());
    display::text(4, 23, shorten(status, 39), display::kTextDim, 0);
    const std::string notice = currentNotice();
    int y = 34;
    if (!notice.empty()) {
        display::text(4, 33, shorten(notice, 39),
                      notice[0] == '!' ? display::kDanger : display::kAccent, 0);
        y = 43;
    }
    for (int i = 0; i < kMainItems; ++i) {
        const int rowY = y + i * 13;
        const bool active = i == selected;
        if (active) {
            display::g().fillRect(2, rowY - 1, display::width() - 6, 12,
                                  display::C(display::T_bg3));
            display::g().fillRect(2, rowY - 1, 2, 12, display::kAccent);
        }
        display::drawOrbitIcon(kMainIcons[i], 5, rowY, 12,
                               active ? 0 : display::kTextDim);
        display::text(21, rowY + 2, kMainLabels[i],
                      active ? display::kText : display::kTextDim, 2);
    }
    char trailing[16];
    std::snprintf(trailing, sizeof(trailing), "%d/%d", selected + 1, kMainItems);
    display::footer("ESC=esci ;/.=nav ENTER=apri", trailing);
    display::push();
}

void mainMenu() {
    int selected = 0;
    bool dirty = true;
    uint32_t lastDraw = 0;
    for (;;) {
        serviceMesh();
        const uint32_t now = millis();
        if (dirty || now - lastDraw >= 500) {
            drawMainMenu(selected);
            lastDraw = now;
            dirty = false;
        }
        const navigation::EventData event = navigation::poll();
        if (event.type == navigation::Event::Back) {
            break;
        }
        if (event.type == navigation::Event::Up) {
            selected = (selected + kMainItems - 1) % kMainItems;
            dirty = true;
        } else if (event.type == navigation::Event::Down) {
            selected = (selected + 1) % kMainItems;
            dirty = true;
        } else if (event.type == navigation::Event::Select) {
            navigation::waitRelease();
            switch (selected) {
                case 0:
                    browseFiles("/", 0);
                    break;
                case 1:
                    inboxMenu();
                    break;
                case 2:
                    commandMenu();
                    break;
                case 3:
                    clipboardMenu();
                    break;
                case 4:
                    peerScan();
                    break;
                case 5:
                    setupMenu();
                    break;
                default:
                    break;
            }
            navigation::waitRelease();
            dirty = true;
        }
        delay(5);
    }
}

}  // namespace

void runMenu() {
    if (!espnow::proto::selfTest()) {
        ui::message("ESP-NOW", {"!Autotest protocollo fallito."},
                    "Menu principale");
        return;
    }
    if (!commandSelfTest()) {
        ui::message("ESP-NOW", {"!Autotest whitelist comandi fallito."},
                    "Menu principale");
        return;
    }
    if (!ui::legalWarning(
            "ESP-NOW mesh",
            {"Frame ESP-NOW in chiaro sul canale scelto.",
             "File, comandi e clipboard tra peer.",
             "Nessun router: tutti i peer sullo stesso", "canale (default 1)."},
            "Menu principale")) {
        return;
    }

    if (!initLocks()) {
        ui::message("ESP-NOW", {"!Allocazione lock fallita."},
                    "Menu principale");
        return;
    }
    loadConfig();
    ensureDirs();
    g_prevMode = WiFi.getMode();
    if (!startRadio()) {
        stopRadio();
        if (g_prefsOpen) {
            g_prefs.end();
            g_prefsOpen = false;
        }
        ui::message("ESP-NOW",
                    {"!Radio non avviata.", "Chiudi gli altri moduli radio",
                     "e riprova."},
                    "Menu principale");
        destroyLocksIfSafe();
        return;
    }

    struct Cleanup {
        ~Cleanup() {
            stopRadio();
            saveClipboardIfDirty();
            if (g_prefsOpen) {
                g_prefs.end();
                g_prefsOpen = false;
            }
            destroyLocksIfSafe();
        }
    } cleanup;

    mainMenu();
    saveClipboardIfDirty();
}

}  // namespace espnow

#endif  // ENABLE_MODULE_ESPNOW
