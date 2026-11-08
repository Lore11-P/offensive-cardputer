#include "Chat.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <NimBLEDevice.h>
#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "Config.h"
#include "Display.h"
#include "Logger.h"
#include "MenuManager.h"
#include "ModuleFlags.h"
#include "Navigation.h"
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
    std::string sender;
    std::string text;
    bool mine = false;
};

enum class NameCheck { Unique, Taken, Error };

std::vector<ChatMessage> g_messages;
std::map<std::string, uint16_t> g_lastCounter;
std::map<std::string, uint32_t> g_peers;
portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
volatile bool g_dirty = true;
uint16_t g_counter = 0;
std::string g_selfAddress;
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

std::string keyMaterial(const std::string& sender,
                        const std::string& recipient) {
    return canonicalName(sender) + '\x1F' + canonicalName(recipient);
}

void deriveKey(const std::string& sender, const std::string& recipient,
               uint8_t key[32]) {
    sha256(keyMaterial(sender, recipient), key);
}

void deriveIv(const std::string& sender, const std::string& recipient,
              uint16_t counter, uint8_t iv[16]) {
    uint8_t hash[32];
    sha256(keyMaterial(sender, recipient) + '\x1F' + std::to_string(counter),
           hash);
    std::memcpy(iv, hash, 16);
}

void nameTag(const std::string& name, uint8_t tag[kTagLen]) {
    uint8_t hash[32];
    sha256(canonicalName(name), hash);
    std::memcpy(tag, hash, kTagLen);
}

bool encryptText(const std::string& sender, const std::string& recipient,
                 uint16_t counter, const std::string& text,
                 uint8_t cipher[kCipherLen]) {
    uint8_t key[32];
    uint8_t iv[16];
    deriveKey(sender, recipient, key);
    deriveIv(sender, recipient, counter, iv);

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

bool decryptText(const std::string& sender, const std::string& recipient,
                 uint16_t counter, const uint8_t cipher[kCipherLen],
                 std::string& text) {
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

    text.assign(reinterpret_cast<const char*>(plain), kCipherLen - pad);
    text = printable(trimRight(text));
    return !text.empty();
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

bool parseMessagePacket(const std::string& data, uint16_t& counter,
                        std::string& sender, uint8_t tag[kTagLen],
                        uint8_t cipher[kCipherLen]) {
    if (data.size() < kPacketLen) {
        return false;
    }
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(data.data());
    if (readU16(raw) != kCompanyId || raw[2] != kMagicMessage) {
        return false;
    }

    sender.assign(reinterpret_cast<const char*>(raw + 3), kNickLen);
    sender = printable(trimRight(sender));
    if (sender.empty()) {
        sender = "???";
    }
    std::memcpy(tag, raw + 3 + kNickLen, kTagLen);
    counter = readU16(raw + 3 + kNickLen + kTagLen);
    std::memcpy(cipher, raw + 3 + kNickLen + kTagLen + 2, kCipherLen);
    return true;
}

bool parseControlPacket(const std::string& data, uint8_t& type,
                        std::string& name, uint32_t& queryId) {
    if (data.size() < kPacketLen) {
        return false;
    }
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(data.data());
    if (readU16(raw) != kCompanyId || raw[2] != kMagicControl) {
        return false;
    }
    type = raw[3];
    name.assign(reinterpret_cast<const char*>(raw + 4), kNickLen);
    name = printable(trimRight(name));
    queryId = readU32(raw + 4 + kNickLen);
    return !name.empty() &&
           (type == kControlQuery || type == kControlResponse ||
            type == kControlPresence);
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
    nameTag(recipient, tag);
    std::memcpy(data.data() + 3 + kNickLen, tag, kTagLen);
    writeU16(data.data() + 3 + kNickLen + kTagLen, counter);

    uint8_t cipher[kCipherLen];
    if (!encryptText(config::chatNickname(), recipient, counter, text, cipher)) {
        return {};
    }
    std::memcpy(data.data() + 3 + kNickLen + kTagLen + 2, cipher, kCipherLen);
    return data;
}

std::vector<uint8_t> buildControlPacket(uint8_t type, const std::string& name,
                                        uint32_t queryId) {
    std::vector<uint8_t> data(kPacketLen, 0);
    writeU16(data.data(), kCompanyId);
    data[2] = kMagicControl;
    data[3] = type;

    std::string target = name;
    if (target.size() > kNickLen) {
        target.resize(kNickLen);
    }
    std::memcpy(data.data() + 4, target.data(), target.size());
    std::memset(data.data() + 4 + target.size(), ' ', kNickLen - target.size());
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

bool publishControl(uint8_t type, const std::string& name, uint32_t queryId) {
    return publishPacket(buildControlPacket(type, name, queryId));
}

void beginNameQuery(const std::string& name, uint32_t queryId) {
    const std::string canonical = canonicalName(name);
    portENTER_CRITICAL(&g_lock);
    std::snprintf(g_nameQueryName, sizeof(g_nameQueryName), "%s",
                  canonical.c_str());
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

bool touchPeerLocked(const std::string& address, uint32_t now) {
    auto it = g_peers.find(address);
    if (it != g_peers.end() && now - it->second <= kPeerTimeoutMs) {
        it->second = now;
        return false;
    }
    g_peers[address] = now;
    return true;
}

void notifyPeerLocked(const std::string& name, uint32_t now) {
    if (!config::chatNotifyEnabled()) {
        return;
    }
    std::snprintf(g_notifyName, sizeof(g_notifyName), "%s", name.c_str());
    g_notifyUntil = now + kNotifyDurationMs;
    g_notifyBeepPending = true;
    g_dirty = true;
}

void handleControlPacket(uint8_t type, const std::string& name,
                         uint32_t queryId, const std::string& address) {
    if (type == kControlQuery) {
        if (g_chatActive &&
            canonicalName(name) == canonicalName(config::chatNickname())) {
            portENTER_CRITICAL(&g_lock);
            std::snprintf(g_controlResponseName, sizeof(g_controlResponseName),
                          "%s", name.c_str());
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

    const std::string canonical = canonicalName(name);
    bool matches = false;
    portENTER_CRITICAL(&g_lock);
    if (g_nameQueryActive && queryId == g_nameQueryId &&
        canonical == g_nameQueryName) {
        g_nameQueryTaken = true;
        matches = true;
    }
    portEXIT_CRITICAL(&g_lock);

    if (matches) {
        logger::info("Chat BLE: nickname '%s' gia' in uso", name.c_str());
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
    publishControl(kControlPresence, config::chatNickname(), 0);
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

class ChatScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
public:
    void onResult(NimBLEAdvertisedDevice* device) override {
        if (device == nullptr || !device->haveManufacturerData()) {
            return;
        }

        const std::string address = device->getAddress().toString();
        if (address == g_selfAddress) {
            return;
        }

        const std::string data = device->getManufacturerData(0);
        uint8_t type = 0;
        std::string name;
        uint32_t queryId = 0;
        if (parseControlPacket(data, type, name, queryId)) {
            handleControlPacket(type, name, queryId, address);
            return;
        }

        uint16_t counter = 0;
        std::string sender;
        uint8_t tag[kTagLen];
        uint8_t cipher[kCipherLen];
        if (!parseMessagePacket(data, counter, sender, tag, cipher)) {
            return;
        }

        uint8_t ownTag[kTagLen];
        nameTag(config::chatNickname(), ownTag);
        if (std::memcmp(tag, ownTag, kTagLen) != 0) {
            return;
        }

        std::string text;
        if (!decryptText(sender, config::chatNickname(), counter, cipher, text)) {
            return;
        }

        const uint32_t now = millis();
        bool duplicate = false;
        portENTER_CRITICAL(&g_lock);
        const bool freshPeer = touchPeerLocked(address, now);
        if (freshPeer) {
            notifyPeerLocked(sender, now);
        }
        auto it = g_lastCounter.find(address);
        if (it != g_lastCounter.end() && it->second == counter) {
            duplicate = true;
        } else {
            g_lastCounter[address] = counter;
            ChatMessage message;
            message.sender = sender;
            message.text = text;
            g_messages.push_back(std::move(message));
            if (g_messages.size() > kHistoryMax) {
                g_messages.erase(g_messages.begin());
            }
            g_dirty = true;
        }
        portEXIT_CRITICAL(&g_lock);

        if (!duplicate) {
            logger::info("Chat BLE: %s: %s", sender.c_str(), text.c_str());
        }
    }
};

void addLocalMessage(const std::string& text) {
    ChatMessage message;
    message.sender = config::chatNickname();
    message.text = text;
    message.mine = true;

    portENTER_CRITICAL(&g_lock);
    g_messages.push_back(std::move(message));
    if (g_messages.size() > kHistoryMax) {
        g_messages.erase(g_messages.begin());
    }
    g_dirty = true;
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

    std::vector<ChatMessage> messages;
    size_t peers = 0;
    std::string notifyName;

    portENTER_CRITICAL(&g_lock);
    messages = g_messages;
    const uint32_t now = millis();
    for (auto it = g_peers.begin(); it != g_peers.end();) {
        if (now - it->second > kPeerTimeoutMs) {
            it = g_peers.erase(it);
        } else {
            ++peers;
            ++it;
        }
    }
    if (now < g_notifyUntil) {
        notifyName = g_notifyName;
    }
    portEXIT_CRITICAL(&g_lock);

    display::clear();
    display::header("Chat BLE", "Menu principale");

    if (!notifyName.empty()) {
        display::text(4, 24, "! " + notifyName + " e' nel raggio",
                      display::kAccent, 0);
    } else {
        std::string status = "Tu: " + config::chatNickname() + " > " +
                             config::chatRecipient();
        if (peers > 0) {
            status += " (" + std::to_string(peers) + ")";
        }
        display::text(4, 24, status, display::kTextDim, 0);
    }

    if (messages.empty()) {
        display::textCentered(52, "Nessun messaggio", display::kText, 2);
        display::textCentered(70, "ENTER = scrivi", display::kTextDim, 0);
    } else {
        constexpr int kLineHeight = 10;
        constexpr int kFirstLineY = 38;
        const int maxLines = (display::height() - 12 - kFirstLineY) / kLineHeight;
        const size_t start = messages.size() > static_cast<size_t>(maxLines)
                                 ? messages.size() - static_cast<size_t>(maxLines)
                                 : 0;
        int y = kFirstLineY;
        for (size_t i = start; i < messages.size(); ++i) {
            const ChatMessage& message = messages[i];
            std::string line =
                (message.mine ? std::string("Tu") : message.sender) + ": " +
                message.text;
            if (line.size() > 39) {
                line.resize(38);
                line += "~";
            }
            display::text(4, y, line,
                          message.mine ? display::kAccent : display::kText, 0);
            y += kLineHeight;
        }
    }

    display::footer("ENTER=scrivi  `=esci");
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
    const std::string recipient = config::chatRecipient();
    addLocalMessage(fields[1].value);
    publishMessage(fields[1].value, recipient);
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

    g_selfAddress = NimBLEDevice::getAddress().toString();

    ChatScanCallbacks callbacks;
    scan->setAdvertisedDeviceCallbacks(&callbacks, true);
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
    beginNameQuery(candidate, queryId);
    if (!publishControl(kControlQuery, candidate, queryId)) {
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

    g_selfAddress = NimBLEDevice::getAddress().toString();
    g_counter = static_cast<uint16_t>(esp_random());
    g_peers.clear();
    g_dirty = true;

    if (scan->isScanning()) {
        scan->stop();
    }
    if (advertising->isAdvertising()) {
        advertising->stop();
    }

    ChatScanCallbacks callbacks;
    scan->setAdvertisedDeviceCallbacks(&callbacks, true);
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
    ui::message("Notifiche chat",
                {enabled ? ">Presenza e avvisi attivi."
                         : ">Presenza e avvisi disattivati.",
                 enabled ? "Gli altri vedono il tuo nickname."
                         : "Nessun annuncio: modalita' stealth.",
                 "L'avviso appare con la Chat aperta."},
                "Chat");
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
    menu.setHint("`=indietro  ;.=nav  ENTER=apri");
    menu.run(true);
}

}  // namespace chat

#endif  // ENABLE_MODULE_CHAT && ENABLE_MODULE_BLE
