#include "Chat.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <NimBLEDevice.h>
#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Config.h"
#include "Display.h"
#include "Logger.h"
#include "MenuManager.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "PowerTimer.h"
#include "TextInput.h"
#include "Ui.h"

#if ENABLE_MODULE_CHAT && ENABLE_MODULE_BLE

namespace chat {
namespace {

constexpr uint8_t kMagicMessage = 0xC8;
constexpr uint8_t kMagicControl = 0xC9;
constexpr uint16_t kCompanyId = 0xFFFF;
constexpr size_t kNickLen = config::kChatNicknameMax;
constexpr size_t kTagLen = 2;
constexpr size_t kCipherLen = 16;
constexpr size_t kTextLen = 15;
constexpr size_t kMessageAppLen =
    1 + kNickLen + kTagLen + 2 + kCipherLen;
constexpr size_t kPacketLen = 2 + kMessageAppLen;
constexpr size_t kHistoryMax = 8;
constexpr uint32_t kPeerTimeoutMs = 15000;
constexpr uint32_t kNameQueryTimeoutMs = 1500;
constexpr uint32_t kPresenceIntervalMs = 5000;
constexpr uint32_t kNotifyDurationMs = 4000;
constexpr uint32_t kControlHoldMs = 600;
constexpr uint16_t kAdvInterval = 160;  // 100 ms (unita' da 0,625 ms)
constexpr uint8_t kControlQuery = 1;
constexpr uint8_t kControlResponse = 2;
constexpr uint8_t kControlPresence = 3;

struct ChatMessage {
    char sender[kNickLen + 1] = {};
    char text[kTextLen + 1] = {};
    bool mine = false;
};

// Copia troncata su buffer fisso: nessuna allocazione, usabile dentro le
// sezioni critiche (interrupt disabilitati).
void copyFixed(char* out, size_t cap, const char* text) {
    if (out == nullptr || cap == 0) {
        return;
    }
    size_t written = 0;
    if (text != nullptr) {
        while (written + 1 < cap && text[written] != '\0') {
            out[written] = text[written];
            ++written;
        }
    }
    out[written] = '\0';
}

constexpr size_t kMaxPeers = 16;
constexpr size_t kMaxCounters = 16;

struct PeerEntry {
    uint8_t addr[6] = {};
    uint32_t lastMs = 0;
    bool used = false;
};

struct CounterEntry {
    uint8_t addr[6] = {};
    uint16_t counter = 0;
    bool used = false;
};

enum class NameCheck { Unique, Taken, Error };

// Array fissi al posto di vector/map: la chat riceve nel task NimBLE e ogni
// nodo di std::map era una malloc. I limiti sono dichiarati, non impliciti.
ChatMessage g_messages[kHistoryMax];
size_t g_messageStart = 0;
size_t g_messageCount = 0;
PeerEntry g_peers[kMaxPeers];
CounterEntry g_counters[kMaxCounters];
portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
volatile bool g_dirty = true;
uint16_t g_counter = 0;
uint8_t g_selfAddr[6] = {};
char g_selfNick[kNickLen + 1] = {};
char g_selfRecipient[kNickLen + 1] = {};
volatile bool g_nameQueryActive = false;
volatile bool g_nameQueryTaken = false;
volatile uint32_t g_nameQueryId = 0;
char g_nameQueryName[kNickLen + 1] = {};
volatile bool g_chatActive = false;
volatile bool g_controlResponsePending = false;
volatile uint32_t g_controlResponseId = 0;
char g_controlResponseName[kNickLen + 1] = {};
volatile bool g_notifyBeepPending = false;
uint32_t g_notifyUntil = 0;
char g_notifyName[kNickLen + 1] = {};
uint32_t g_nextPresenceAt = 0;

void ensureBle() {
    if (!NimBLEDevice::getInitialized()) {
        NimBLEDevice::init("CardputerFW");
    }
}

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

// Identita' in forma fissa: nessuna config::chatNickname() (std::string per
// valore) dentro i callback.
void refreshIdentity() {
    const std::string nick = config::chatNickname();
    std::snprintf(g_selfNick, sizeof(g_selfNick), "%s", nick.c_str());
    const std::string recipient = config::chatRecipient();
    std::snprintf(g_selfRecipient, sizeof(g_selfRecipient), "%s",
                  recipient.c_str());
    std::memcpy(g_selfAddr, NimBLEDevice::getAddress().getNative(), 6);
}

std::string trimRight(std::string value) {
    while (!value.empty() && (value.back() == ' ' || value.back() == '\0')) {
        value.pop_back();
    }
    return value;
}

std::string printable(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c >= 0x20 && c < 0x7F) {
            out.push_back(c);
        }
    }
    return out;
}

std::string canonicalName(const std::string& value) {
    std::string out = printable(trimRight(value));
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    const uint8_t empty = 0;
    const uint8_t* input = len == 0 ? &empty : data;
    if (mbedtls_sha256_ret(input, len, out, 0) != 0) {
        std::memset(out, 0, 32);
    }
}

void sha256(const std::string& value, uint8_t out[32]) {
    sha256(reinterpret_cast<const uint8_t*>(value.data()), value.size(), out);
}

void sha256(const char* value, uint8_t out[32]) {
    sha256(reinterpret_cast<const uint8_t*>(value), std::strlen(value), out);
}

// Canonicalizzazione su buffer fisso: printable + trimRight + minuscolo.
void canonicalInto(const char* value, char out[kNickLen + 1]) {
    size_t len = std::strlen(value);
    while (len > 0 && (value[len - 1] == ' ' || value[len - 1] == '\0')) {
        --len;
    }
    size_t written = 0;
    for (size_t i = 0; i < len && written < kNickLen; ++i) {
        char c = value[i];
        if (c < 0x20 || c >= 0x7F) {
            continue;
        }
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        out[written++] = c;
    }
    out[written] = '\0';
}

constexpr size_t kMaterialMax = 2 * (kNickLen + 1) + 1;

void keyMaterial(const char* sender, const char* recipient,
                 char out[kMaterialMax]) {
    char a[kNickLen + 1];
    char b[kNickLen + 1];
    canonicalInto(sender, a);
    canonicalInto(recipient, b);
    std::snprintf(out, kMaterialMax, "%s%c%s", a, 0x1F, b);
}

// shortcut: la chiave AES e' solo SHA-256 dei due nickname, senza segreto
// condiviso ne' ECDH/pairing: chi conosce i nickname puo' decifrare. Per una
// riservatezza reale serve uno scambio X25519 con fingerprint verificato.
void deriveKey(const char* sender, const char* recipient, uint8_t key[32]) {
    char material[kMaterialMax];
    keyMaterial(sender, recipient, material);
    sha256(material, key);
}

void deriveIv(const char* sender, const char* recipient, uint16_t counter,
              uint8_t iv[16]) {
    char material[kMaterialMax];
    keyMaterial(sender, recipient, material);
    char withCounter[kMaterialMax + 12];
    std::snprintf(withCounter, sizeof(withCounter), "%s%c%u", material, 0x1F,
                  static_cast<unsigned>(counter));
    uint8_t hash[32];
    sha256(withCounter, hash);
    std::memcpy(iv, hash, 16);
}

void nameTag(const char* name, uint8_t tag[kTagLen]) {
    char canonical[kNickLen + 1];
    canonicalInto(name, canonical);
    uint8_t hash[32];
    sha256(canonical, hash);
    std::memcpy(tag, hash, kTagLen);
}

bool encryptText(const std::string& sender, const std::string& recipient,
                 uint16_t counter, const std::string& text,
                 uint8_t cipher[kCipherLen]) {
    uint8_t key[32];
    uint8_t iv[16];
    deriveKey(sender.c_str(), recipient.c_str(), key);
    deriveIv(sender.c_str(), recipient.c_str(), counter, iv);

    uint8_t plain[kCipherLen] = {};
    const size_t len = std::min(text.size(), kTextLen);
    std::memcpy(plain, text.data(), len);
    const uint8_t pad = static_cast<uint8_t>(kCipherLen - len);
    std::memset(plain + len, pad, pad);

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    int rc = mbedtls_aes_setkey_enc(&ctx, key, 128);
    if (rc == 0) {
        rc = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, kCipherLen, iv,
                                   plain, cipher);
    }
    mbedtls_aes_free(&ctx);
    return rc == 0;
}

bool decryptText(const char* sender, const char* recipient, uint16_t counter,
                 const uint8_t cipher[kCipherLen], char text[kTextLen + 1]) {
    uint8_t key[32];
    uint8_t iv[16];
    deriveKey(sender, recipient, key);
    deriveIv(sender, recipient, counter, iv);

    uint8_t plain[kCipherLen] = {};
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    int rc = mbedtls_aes_setkey_dec(&ctx, key, 128);
    if (rc == 0) {
        rc = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, kCipherLen, iv,
                                   cipher, plain);
    }
    mbedtls_aes_free(&ctx);
    if (rc != 0) {
        return false;
    }

    const uint8_t pad = plain[kCipherLen - 1];
    if (pad == 0 || pad > kCipherLen) {
        return false;
    }
    for (size_t i = kCipherLen - pad; i < kCipherLen; ++i) {
        if (plain[i] != pad) {
            return false;
        }
    }

    size_t len = kCipherLen - pad;
    while (len > 0 && (plain[len - 1] == ' ' || plain[len - 1] == '\0')) {
        --len;
    }
    size_t written = 0;
    for (size_t i = 0; i < len; ++i) {
        const char c = static_cast<char>(plain[i]);
        if (c >= 0x20 && c < 0x7F) {
            text[written++] = c;
        }
    }
    text[written] = '\0';
    return written > 0;
}

uint16_t readU16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8);
}

void writeU16(uint8_t* data, uint16_t value) {
    data[0] = static_cast<uint8_t>(value & 0xFF);
    data[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

uint32_t readU32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

void writeU32(uint8_t* data, uint32_t value) {
    data[0] = static_cast<uint8_t>(value & 0xFF);
    data[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    data[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

void fieldName(const uint8_t* raw, size_t len, char out[kNickLen + 1]) {
    size_t end = std::min(len, kNickLen);
    while (end > 0 && (raw[end - 1] == ' ' || raw[end - 1] == '\0')) {
        --end;
    }
    size_t written = 0;
    for (size_t i = 0; i < end; ++i) {
        const char c = static_cast<char>(raw[i]);
        if (c >= 0x20 && c < 0x7F) {
            out[written++] = c;
        }
    }
    out[written] = '\0';
}

bool parseMessagePacket(const uint8_t* raw, size_t len, uint16_t& counter,
                        char sender[kNickLen + 1], uint8_t tag[kTagLen],
                        uint8_t cipher[kCipherLen]) {
    if (raw == nullptr || len < kPacketLen) {
        return false;
    }
    if (readU16(raw) != kCompanyId || raw[2] != kMagicMessage) {
        return false;
    }

    fieldName(raw + 3, kNickLen, sender);
    if (sender[0] == '\0') {
        std::snprintf(sender, kNickLen + 1, "%s", "???");
    }
    std::memcpy(tag, raw + 3 + kNickLen, kTagLen);
    counter = readU16(raw + 3 + kNickLen + kTagLen);
    std::memcpy(cipher, raw + 3 + kNickLen + kTagLen + 2, kCipherLen);
    return true;
}

bool parseControlPacket(const uint8_t* raw, size_t len, uint8_t& type,
                        char name[kNickLen + 1], uint32_t& queryId) {
    if (raw == nullptr || len < kPacketLen) {
        return false;
    }
    if (readU16(raw) != kCompanyId || raw[2] != kMagicControl) {
        return false;
    }
    type = raw[3];
    fieldName(raw + 4, kNickLen, name);
    queryId = readU32(raw + 4 + kNickLen);
    return name[0] != '\0' &&
           (type == kControlQuery || type == kControlResponse ||
            type == kControlPresence);
}

// Estrae il manufacturer data (AD type 0xFF) dal payload grezzo, senza la
// std::string restituita da getManufacturerData().
bool readManufacturerData(const uint8_t* payload, size_t len, uint8_t* out,
                          size_t cap, size_t& outLen) {
    size_t pos = 0;
    while (pos + 1 < len) {
        const uint8_t fieldLen = payload[pos];
        if (fieldLen == 0 || pos + 1 + fieldLen > len) {
            break;
        }
        if (payload[pos + 1] == 0xFF && fieldLen > 1) {
            const size_t valueLen = fieldLen - 1;
            if (valueLen > cap) {
                return false;
            }
            std::memcpy(out, payload + pos + 2, valueLen);
            outLen = valueLen;
            return true;
        }
        pos += 1 + fieldLen;
    }
    return false;
}

std::vector<uint8_t> buildMessagePacket(const std::string& text,
                                        const std::string& recipient,
                                        uint16_t counter) {
    std::vector<uint8_t> data(kPacketLen, 0);
    writeU16(data.data(), kCompanyId);
    data[2] = kMagicMessage;

    std::string sender = config::chatNickname();
    if (sender.size() > kNickLen) {
        sender.resize(kNickLen);
    }
    std::memcpy(data.data() + 3, sender.data(), sender.size());
    std::memset(data.data() + 3 + sender.size(), ' ', kNickLen - sender.size());

    uint8_t tag[kTagLen];
    nameTag(recipient.c_str(), tag);
    std::memcpy(data.data() + 3 + kNickLen, tag, kTagLen);
    writeU16(data.data() + 3 + kNickLen + kTagLen, counter);

    uint8_t cipher[kCipherLen];
    if (!encryptText(config::chatNickname(), recipient, counter, text, cipher)) {
        return {};
    }
    std::memcpy(data.data() + 3 + kNickLen + kTagLen + 2, cipher, kCipherLen);
    return data;
}

std::vector<uint8_t> buildControlPacket(uint8_t type, const char* name,
                                       uint32_t queryId) {
    std::vector<uint8_t> data(kPacketLen, 0);
    writeU16(data.data(), kCompanyId);
    data[2] = kMagicControl;
    data[3] = type;

    const size_t targetLen = std::min(std::strlen(name), kNickLen);
    std::memcpy(data.data() + 4, name, targetLen);
    std::memset(data.data() + 4 + targetLen, ' ', kNickLen - targetLen);
    writeU32(data.data() + 4 + kNickLen, queryId);
    return data;
}

bool publishPacket(const std::vector<uint8_t>& payload) {
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    if (advertising == nullptr || payload.empty()) {
        return false;
    }

    if (advertising->isAdvertising()) {
        advertising->stop();
        delay(5);
    }

    NimBLEAdvertisementData advertisement;
    advertisement.setManufacturerData(payload);
    advertising->setAdvertisementType(BLE_GAP_CONN_MODE_NON);
    advertising->setScanResponse(false);
    advertising->setMinInterval(kAdvInterval);
    advertising->setMaxInterval(kAdvInterval);
    advertising->setAdvertisementData(advertisement);
    return advertising->start(0, nullptr, nullptr);
}

bool publishControl(uint8_t type, const char* name, uint32_t queryId) {
    return publishPacket(buildControlPacket(type, name, queryId));
}

void beginNameQuery(const char* name, uint32_t queryId) {
    char canonical[kNickLen + 1];
    canonicalInto(name, canonical);
    portENTER_CRITICAL(&g_lock);
    copyFixed(g_nameQueryName, sizeof(g_nameQueryName), canonical);
    g_nameQueryId = queryId;
    g_nameQueryTaken = false;
    g_nameQueryActive = true;
    portEXIT_CRITICAL(&g_lock);
}

bool nameQueryTaken() {
    portENTER_CRITICAL(&g_lock);
    const bool taken = g_nameQueryTaken;
    portEXIT_CRITICAL(&g_lock);
    return taken;
}

void endNameQuery() {
    portENTER_CRITICAL(&g_lock);
    g_nameQueryActive = false;
    portEXIT_CRITICAL(&g_lock);
}

bool sameAddress(const uint8_t a[6], const uint8_t b[6]) {
    return std::memcmp(a, b, 6) == 0;
}

bool touchPeerLocked(const uint8_t address[6], uint32_t now) {
    int freeSlot = -1;
    int oldestSlot = -1;
    for (int i = 0; i < static_cast<int>(kMaxPeers); ++i) {
        PeerEntry& peer = g_peers[i];
        if (!peer.used) {
            if (freeSlot < 0) {
                freeSlot = i;
            }
            continue;
        }
        if (sameAddress(peer.addr, address)) {
            const bool fresh = now - peer.lastMs > kPeerTimeoutMs;
            peer.lastMs = now;
            return fresh;
        }
        if (oldestSlot < 0 || peer.lastMs < g_peers[oldestSlot].lastMs) {
            oldestSlot = i;
        }
    }
    const int slot = freeSlot >= 0 ? freeSlot : oldestSlot;
    if (slot < 0) {
        return true;
    }
    PeerEntry& peer = g_peers[slot];
    std::memcpy(peer.addr, address, 6);
    peer.lastMs = now;
    peer.used = true;
    return true;
}

void notifyPeerLocked(const char* name, uint32_t now) {
    if (!config::chatNotifyEnabled()) {
        return;
    }
    copyFixed(g_notifyName, sizeof(g_notifyName), name);
    g_notifyUntil = now + kNotifyDurationMs;
    g_notifyBeepPending = true;
    g_dirty = true;
}

bool canonicalEquals(const char* a, const char* b) {
    char left[kNickLen + 1];
    char right[kNickLen + 1];
    canonicalInto(a, left);
    canonicalInto(b, right);
    return std::strcmp(left, right) == 0;
}

void handleControlPacket(uint8_t type, const char* name, uint32_t queryId,
                         const uint8_t address[6]) {
    if (type == kControlQuery) {
        if (g_chatActive && canonicalEquals(name, g_selfNick)) {
            portENTER_CRITICAL(&g_lock);
            copyFixed(g_controlResponseName, sizeof(g_controlResponseName), name);
            g_controlResponseId = queryId;
            g_controlResponsePending = true;
            portEXIT_CRITICAL(&g_lock);
        }
        return;
    }

    if (type == kControlPresence) {
        if (!g_chatActive) {
            return;
        }
        const uint32_t now = millis();
        bool fresh = false;
        portENTER_CRITICAL(&g_lock);
        fresh = touchPeerLocked(address, now);
        if (fresh) {
            notifyPeerLocked(name, now);
        }
        portEXIT_CRITICAL(&g_lock);
        if (fresh) {
            g_dirty = true;
        }
        return;
    }

    char canonical[kNickLen + 1];
    canonicalInto(name, canonical);
    bool matches = false;
    portENTER_CRITICAL(&g_lock);
    if (g_nameQueryActive && queryId == g_nameQueryId &&
        std::strcmp(canonical, g_nameQueryName) == 0) {
        g_nameQueryTaken = true;
        matches = true;
    }
    portEXIT_CRITICAL(&g_lock);

    if (matches) {
        logger::info("Chat BLE: nickname '%s' gia' in uso", name);
    }
}

void publishPendingControl() {
    char name[kNickLen + 1];
    uint32_t queryId = 0;
    portENTER_CRITICAL(&g_lock);
    if (!g_controlResponsePending) {
        portEXIT_CRITICAL(&g_lock);
        return;
    }
    std::memcpy(name, g_controlResponseName, sizeof(name));
    queryId = g_controlResponseId;
    g_controlResponsePending = false;
    portEXIT_CRITICAL(&g_lock);

    publishControl(kControlResponse, name, queryId);
    g_nextPresenceAt = millis() + kControlHoldMs;
}

void publishPresence() {
    if (!config::chatNotifyEnabled()) {
        return;
    }
    publishControl(kControlPresence, g_selfNick, 0);
    g_nextPresenceAt = millis() + kPresenceIntervalMs;
}

void serviceNotifications() {
    bool beep = false;
    portENTER_CRITICAL(&g_lock);
    if (g_notifyBeepPending) {
        g_notifyBeepPending = false;
        beep = true;
    }
    portEXIT_CRITICAL(&g_lock);

    if (beep) {
        M5Cardputer.Speaker.tone(1800, 80);
    }
}

void pushMessageLocked(const char* sender, const char* text, bool mine) {
    size_t slot;
    if (g_messageCount < kHistoryMax) {
        slot = (g_messageStart + g_messageCount) % kHistoryMax;
        ++g_messageCount;
    } else {
        slot = g_messageStart;
        g_messageStart = (g_messageStart + 1) % kHistoryMax;
    }
    ChatMessage& message = g_messages[slot];
    copyFixed(message.sender, sizeof(message.sender), sender);
    copyFixed(message.text, sizeof(message.text), text);
    message.mine = mine;
    g_dirty = true;
}

// true se la coppia indirizzo/contatore e' nuova (non un duplicato).
bool noteCounterLocked(const uint8_t address[6], uint16_t counter) {
    int freeSlot = -1;
    for (int i = 0; i < static_cast<int>(kMaxCounters); ++i) {
        CounterEntry& entry = g_counters[i];
        if (!entry.used) {
            if (freeSlot < 0) {
                freeSlot = i;
            }
            continue;
        }
        if (sameAddress(entry.addr, address)) {
            if (entry.counter == counter) {
                return false;
            }
            entry.counter = counter;
            return true;
        }
    }
    if (freeSlot < 0) {
        std::memmove(&g_counters[0], &g_counters[1],
                     sizeof(CounterEntry) * (kMaxCounters - 1));
        freeSlot = static_cast<int>(kMaxCounters) - 1;
    }
    CounterEntry& entry = g_counters[freeSlot];
    std::memcpy(entry.addr, address, 6);
    entry.counter = counter;
    entry.used = true;
    return true;
}

class ChatScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
public:
    void onResult(NimBLEAdvertisedDevice* device) override {
        if (device == nullptr) {
            return;
        }

        const uint8_t* native = device->getAddress().getNative();
        if (sameAddress(native, g_selfAddr)) {
            return;
        }

        uint8_t data[64];
        size_t dataLen = 0;
        if (!readManufacturerData(device->getPayload(),
                                  device->getPayloadLength(), data,
                                  sizeof(data), dataLen)) {
            return;
        }

        uint8_t type = 0;
        char name[kNickLen + 1];
        uint32_t queryId = 0;
        if (parseControlPacket(data, dataLen, type, name, queryId)) {
            handleControlPacket(type, name, queryId, native);
            return;
        }

        uint16_t counter = 0;
        char sender[kNickLen + 1];
        uint8_t tag[kTagLen];
        uint8_t cipher[kCipherLen];
        if (!parseMessagePacket(data, dataLen, counter, sender, tag, cipher)) {
            return;
        }

        uint8_t ownTag[kTagLen];
        nameTag(g_selfNick, ownTag);
        if (std::memcmp(tag, ownTag, kTagLen) != 0) {
            return;
        }

        char text[kTextLen + 1];
        if (!decryptText(sender, g_selfNick, counter, cipher, text)) {
            return;
        }

        const uint32_t now = millis();
        bool added = false;
        portENTER_CRITICAL(&g_lock);
        const bool freshPeer = touchPeerLocked(native, now);
        if (freshPeer) {
            notifyPeerLocked(sender, now);
        }
        if (noteCounterLocked(native, counter)) {
            pushMessageLocked(sender, text, false);
            added = true;
        }
        portEXIT_CRITICAL(&g_lock);

        if (added) {
            // Solo metadati: il contenuto dei messaggi non deve finire sul
            // seriale USB (log persistenti / acquisibili da terzi).
            logger::info("Chat BLE: messaggio ricevuto, %u byte, ctr=%u",
                         static_cast<unsigned>(std::strlen(text)),
                         static_cast<unsigned>(counter));
        }
    }
};

// Una sola istanza viva per tutto il programma: NimBLEScan::stop() non
// attende il task host, quindi onResult() puo' arrivare dopo lo stop. Un
// oggetto di stack verrebbe distrutto mentre la vtable e' ancora in uso.
ChatScanCallbacks g_chatScanCallbacks;

void addLocalMessage(const std::string& text) {
    portENTER_CRITICAL(&g_lock);
    pushMessageLocked(g_selfNick, text.c_str(), true);
    portEXIT_CRITICAL(&g_lock);
}

bool publishMessage(const std::string& text, const std::string& recipient) {
    const std::vector<uint8_t> payload =
        buildMessagePacket(text, recipient, g_counter++);
    if (!publishPacket(payload)) {
        logger::warn("Chat BLE: invio fallito");
        return false;
    }
    g_nextPresenceAt = millis() + kPresenceIntervalMs;
    return true;
}

void drawChat() {
    g_dirty = false;

    // Snapshot su array statico: il disegno avviene ogni secondo o su evento,
    // e le capacita' degli std::string restano allocate tra un frame e l'altro.
    static ChatMessage messages[kHistoryMax];
    size_t messageCount = 0;
    size_t peers = 0;
    char notifyName[kNickLen + 1] = {};

    portENTER_CRITICAL(&g_lock);
    messageCount = g_messageCount;
    for (size_t i = 0; i < messageCount; ++i) {
        messages[i] = g_messages[(g_messageStart + i) % kHistoryMax];
    }
    const uint32_t now = millis();
    for (PeerEntry& peer : g_peers) {
        if (!peer.used) {
            continue;
        }
        if (now - peer.lastMs > kPeerTimeoutMs) {
            peer.used = false;
        } else {
            ++peers;
        }
    }
    if (now < g_notifyUntil) {
        copyFixed(notifyName, sizeof(notifyName), g_notifyName);
    }
    portEXIT_CRITICAL(&g_lock);

    display::clear();
    display::header("Chat BLE", "Menu principale");

    if (notifyName[0] != '\0') {
        textFormat(4, 24, display::kAccent, 0, "! %s e' nel raggio", notifyName);
    } else if (peers > 0) {
        textFormat(4, 24, display::kTextDim, 0, "Tu: %s > %s (%u)", g_selfNick,
                   g_selfRecipient, static_cast<unsigned>(peers));
    } else {
        textFormat(4, 24, display::kTextDim, 0, "Tu: %s > %s", g_selfNick,
                   g_selfRecipient);
    }

    if (messageCount == 0) {
        display::textCentered(52, "Nessun messaggio", display::kText, 2);
        display::textCentered(70, "ENTER = scrivi", display::kTextDim, 0);
    } else {
        constexpr int kLineHeight = 10;
        constexpr int kFirstLineY = 38;
        const int maxLines = (display::height() - 12 - kFirstLineY) / kLineHeight;
        const size_t start = messageCount > static_cast<size_t>(maxLines)
                                 ? messageCount - static_cast<size_t>(maxLines)
                                 : 0;
        int y = kFirstLineY;
        char line[64];
        for (size_t i = start; i < messageCount; ++i) {
            const ChatMessage& message = messages[i];
            std::snprintf(line, sizeof(line), "%s: %s",
                          message.mine ? "Tu" : message.sender, message.text);
            if (std::strlen(line) > 39) {
                line[38] = '~';
                line[39] = '\0';
            }
            textFormat(4, y, message.mine ? display::kAccent : display::kText, 0,
                       "%s", line);
            y += kLineHeight;
        }
    }

    display::footer("ENTER=scrivi  ESC=esci");
    display::push();
}

void composeMessage() {
    std::vector<textinput::Field> fields;
    fields.push_back({"A", config::chatRecipient(), config::kChatNicknameMax,
                      false, false});
    fields.push_back({"Messaggio", "", kTextLen, false, false});
    if (!textinput::edit("Nuovo messaggio", "Chat BLE", fields)) {
        return;
    }
    if (fields[1].value.empty()) {
        return;
    }

    config::setChatRecipient(fields[0].value);
    std::snprintf(g_selfRecipient, sizeof(g_selfRecipient), "%s",
                  config::chatRecipient().c_str());
    const std::string recipient = config::chatRecipient();
    // Il messaggio compare nella chat solo se e' stato realmente trasmesso.
    if (!publishMessage(fields[1].value, recipient)) {
        ui::message("Chat BLE", {"!Invio non riuscito.", "Messaggio non aggiunto."},
                    "Chat");
        return;
    }
    addLocalMessage(fields[1].value);
}

NameCheck checkNicknameAvailable(const std::string& candidate) {
    ensureBle();
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC);

    NimBLEScan* scan = NimBLEDevice::getScan();
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    if (scan == nullptr || advertising == nullptr) {
        return NameCheck::Error;
    }

    if (scan->isScanning()) {
        scan->stop();
    }
    if (advertising->isAdvertising()) {
        advertising->stop();
    }

    refreshIdentity();

    scan->setAdvertisedDeviceCallbacks(&g_chatScanCallbacks, true);
    scan->setActiveScan(false);
    scan->setInterval(50);
    scan->setWindow(50);
    scan->setMaxResults(0);
    scan->clearResults();

    if (!scan->start(0, nullptr, false)) {
        scan->setAdvertisedDeviceCallbacks(nullptr, false);
        return NameCheck::Error;
    }

    const uint32_t queryId = esp_random();
    beginNameQuery(candidate.c_str(), queryId);
    if (!publishControl(kControlQuery, candidate.c_str(), queryId)) {
        endNameQuery();
        scan->stop();
        scan->setAdvertisedDeviceCallbacks(nullptr, false);
        scan->clearResults();
        return NameCheck::Error;
    }

    const uint32_t start = millis();
    while (millis() - start < kNameQueryTimeoutMs) {
        delay(10);
    }

    const NameCheck result =
        nameQueryTaken() ? NameCheck::Taken : NameCheck::Unique;
    endNameQuery();

    if (scan->isScanning()) {
        scan->stop();
    }
    scan->setAdvertisedDeviceCallbacks(nullptr, false);
    scan->clearResults();
    if (advertising->isAdvertising()) {
        advertising->stop();
    }
    return result;
}

void editNickname() {
    std::vector<textinput::Field> fields;
    fields.push_back({"Nickname", config::chatNickname(),
                      config::kChatNicknameMax, false, false});
    if (!textinput::edit("Nickname chat", "Chat", fields)) {
        return;
    }

    const std::string candidate = printable(trimRight(fields[0].value));
    if (candidate.empty()) {
        ui::message("Nickname chat", {"!Nickname non valido."}, "Chat");
        return;
    }
    if (canonicalName(candidate) == canonicalName(config::chatNickname())) {
        return;
    }

    const NameCheck result = checkNicknameAvailable(candidate);
    if (result == NameCheck::Taken) {
        ui::message("Nickname chat",
                    {"!Nickname gia' in uso.",
                     "Scegline un altro: la ricerca e'",
                     "case-insensitive."},
                    "Chat");
        return;
    }
    if (result == NameCheck::Error) {
        ui::message("Nickname chat",
                    {"!Verifica non riuscita.",
                     "Riprova tra qualche secondo."},
                    "Chat");
        return;
    }

    config::setChatNickname(candidate);
    // Il setter salva ma non riporta l'esito: una seconda save() idempotente
    // dice se NVS ha davvero accettato i valori.
    if (!config::save()) {
        ui::message("Nickname chat",
                    {">Nickname: " + config::chatNickname(),
                     "!Salvataggio NVS non riuscito.",
                     "Il valore vale solo fino al riavvio."},
                    "Chat");
        return;
    }
    ui::message("Nickname chat",
                {">Nickname: " + config::chatNickname(),
                 "Disponibile e salvato."},
                "Chat");
}

void editRecipient() {
    std::vector<textinput::Field> fields;
    fields.push_back({"Destinatario", config::chatRecipient(),
                      config::kChatNicknameMax, false, false});
    if (!textinput::edit("Destinatario chat", "Chat", fields)) {
        return;
    }
    config::setChatRecipient(fields[0].value);
    if (!config::save()) {
        ui::message("Destinatario chat",
                    {">A: " + config::chatRecipient(),
                     "!Salvataggio NVS non riuscito.",
                     "Il valore vale solo fino al riavvio."},
                    "Chat");
        return;
    }
    ui::message("Destinatario chat",
                {">A: " + config::chatRecipient()}, "Chat");
}

void runChat() {
    ensureBle();
    // Usa sempre l'indirizzo pubblico: evita che un modulo precedente
    // (es. BLE spam) lasci attivo un MAC random non coerente con getAddress().
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC);

    NimBLEScan* scan = NimBLEDevice::getScan();
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    if (scan == nullptr || advertising == nullptr) {
        ui::message("Chat BLE",
                    {"!Stack BLE non disponibile.", "Riavvia il dispositivo."},
                    "Chat");
        return;
    }

    refreshIdentity();
    g_counter = static_cast<uint16_t>(esp_random());
    portENTER_CRITICAL(&g_lock);
    g_messageStart = 0;
    g_messageCount = 0;
    std::memset(g_peers, 0, sizeof(g_peers));
    std::memset(g_counters, 0, sizeof(g_counters));
    portEXIT_CRITICAL(&g_lock);
    g_dirty = true;

    if (scan->isScanning()) {
        scan->stop();
    }
    if (advertising->isAdvertising()) {
        advertising->stop();
    }

    scan->setAdvertisedDeviceCallbacks(&g_chatScanCallbacks, true);
    scan->setActiveScan(false);
    scan->setInterval(50);
    scan->setWindow(50);
    scan->setMaxResults(0);
    scan->clearResults();

    if (!scan->start(0, nullptr, false)) {
        scan->setAdvertisedDeviceCallbacks(nullptr, false);
        ui::message("Chat BLE",
                    {"!Scansione BLE non avviata.",
                     "Riprova tra qualche secondo."},
                    "Chat");
        return;
    }

    logger::info("Chat BLE: in ascolto come %s", config::chatNickname().c_str());
    g_chatActive = true;
    g_nextPresenceAt = 0;

    navigation::waitRelease();
    // Schermata di monitoraggio: niente spegnimento automatico a meta' chat.
    powertimer::Hold hold;
    bool running = true;
    uint32_t lastDraw = 0;
    while (running) {
        navigation::update();
        const navigation::EventData event = navigation::poll();
        publishPendingControl();

        const uint32_t now = millis();
        if (config::chatNotifyEnabled() &&
            static_cast<int32_t>(now - g_nextPresenceAt) >= 0) {
            publishPresence();
        } else if (!config::chatNotifyEnabled() &&
                   static_cast<int32_t>(now - g_nextPresenceAt) >= 0 &&
                   advertising->isAdvertising()) {
            // In modalita' stealth nessun annuncio rinnova l'advertising: senza
            // questo il testo dell'ultimo messaggio resterebbe in onda a 100 ms
            // fino all'uscita dalla chat.
            advertising->stop();
        }
        serviceNotifications();

        if (event.type == navigation::Event::Select) {
            navigation::waitRelease();
            composeMessage();
            g_dirty = true;
            navigation::waitRelease();
        } else if (event.type == navigation::Event::Back) {
            running = false;
        }

        if (g_dirty || now - lastDraw > 1000) {
            drawChat();
            lastDraw = now;
        }
        delay(25);
    }

    g_chatActive = false;
    portENTER_CRITICAL(&g_lock);
    g_controlResponsePending = false;
    portEXIT_CRITICAL(&g_lock);

    if (scan->isScanning()) {
        scan->stop();
    }
    scan->setAdvertisedDeviceCallbacks(nullptr, false);
    scan->clearResults();
    if (advertising->isAdvertising()) {
        advertising->stop();
    }
    navigation::waitRelease();
    logger::info("Chat BLE: chiusa");
}

}  // namespace

void toggleNotifications() {
    const bool enabled = !config::chatNotifyEnabled();
    config::setChatNotifyEnabled(enabled);
    std::vector<std::string> lines = {
        enabled ? ">Presenza e avvisi attivi."
                : ">Presenza e avvisi disattivati.",
        enabled ? "Gli altri vedono il tuo nickname."
                : "Nessun annuncio: modalita' stealth.",
        "L'avviso appare con la Chat aperta."};
    if (!config::save()) {
        lines.push_back("!Salvataggio NVS non riuscito.");
        lines.push_back("Il valore vale solo fino al riavvio.");
    }
    ui::message("Notifiche chat", lines, "Chat");
}

void runMenu() {
    MenuManager menu("Chat", "Menu principale");
    menu.addItem("Apri chat", "chat", runChat);
    menu.addItem("Nickname", "settings", editNickname);
    menu.addItem("Destinatario", "settings", editRecipient);
    menu.addItem("Notifiche", "settings", toggleNotifications);
    menu.addItem("Info", "info", [] {
        ui::message("Chat BLE",
                    {">Messaggi cifrati AES via BLE.",
                     "Ogni messaggio e' indirizzato a un",
                     "nickname; gli altri non lo mostrano.",
                     "Max 15 caratteri per messaggio.",
                     "Con Chat aperta avvisa quando un",
                     "peer entra nel raggio.",
                     "Nessun pairing: non usare per dati",
                     "davvero sensibili."},
                    "Chat");
    });
    menu.setHint("ESC=indietro  ;.=nav  ENTER=apri");
    menu.run(true);
}

}  // namespace chat

#endif  // ENABLE_MODULE_CHAT && ENABLE_MODULE_BLE
