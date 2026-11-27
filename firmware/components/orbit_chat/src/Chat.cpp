#include "orbit_chat/Chat.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Display.h"
#include "Logger.h"
#include "MenuManager.h"
#include "Navigation.h"
#include "Ui.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/util/util.h"
#include "mbedtls/aes.h"
#include "mbedtls/sha256.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "orbit_compat.h"
#include "orbit_settings/Settings.h"
#include "services/gap/ble_svc_gap.h"

// Port della chat BLE 2.x: stesso protocollo su advertising manufacturer
// (company ID 0xFFFF, magic 0xC8/0xC9), stessa cifratura AES-128-CBC con
// chiave/IV derivati da SHA-256 dei nickname. Nessun GATT, nessun pairing.
//
// Nota: il nickname 2.x veniva da config::chatNickname(); qui e' fisso
// ("ORBIT") perche' la configurazione non e' ancora portata. Il nome deve
// restare lungo al massimo kNickLen: il layout del pacchetto dipende da
// quella lunghezza.

namespace orbit_chat {
namespace {

constexpr uint8_t kMagicMessage = 0xC8;
constexpr uint8_t kMagicControl = 0xC9;
constexpr uint16_t kCompanyId = 0xFFFF;
constexpr size_t kNickLen = 6;  // = config::kChatNicknameMax del firmware 2.x
constexpr size_t kTagLen = 2;
constexpr size_t kCipherLen = 16;
constexpr size_t kTextLen = 15;
constexpr size_t kMessageAppLen = 1 + kNickLen + kTagLen + 2 + kCipherLen;
constexpr size_t kPacketLen = 2 + kMessageAppLen;  // 29 byte
// AD legacy: [len][0xFF][company_id][payload] = 1 + 1 + 29 = 31 byte.
constexpr size_t kAdvLen = kPacketLen + 2;
constexpr size_t kHistoryMax = 8;
constexpr uint32_t kPeerTimeoutMs = 15000;
constexpr uint32_t kPresenceIntervalMs = 5000;
constexpr uint32_t kNotifyDurationMs = 4000;
constexpr uint32_t kControlHoldMs = 600;
constexpr uint16_t kAdvInterval = 160;  // 100 ms (unita' da 0,625 ms)
constexpr uint16_t kScanUnit = 80;      // 50 ms (unita' da 0,625 ms)
constexpr uint8_t kControlQuery = 1;
constexpr uint8_t kControlResponse = 2;
constexpr uint8_t kControlPresence = 3;

// Il layout deve restare quello del 2.x: 29 byte di manufacturer value in un
// unico AD structure da 31 byte (massimo legacy).
static_assert(kNickLen == 6, "nickname chat: layout 2.x da 6 byte");
static_assert(kPacketLen == 29, "pacchetto chat: layout 2.x da 29 byte");
static_assert(kAdvLen == 31, "advertising legacy: massimo 31 byte");

constexpr char kDeviceName[] = "ORBIT";
constexpr char kDefaultNickname[] = "ORBIT";
constexpr char kDefaultRecipient[] = "ORBIT";

struct ChatMessage {
    char sender[kNickLen + 1] = {};
    char text[kTextLen + 1] = {};
    bool mine = false;
};

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

// Array fissi: i callback NimBLE girano nel task host e non devono allocare.
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
volatile bool g_chatActive = false;
volatile bool g_controlResponsePending = false;
volatile uint32_t g_controlResponseId = 0;
char g_controlResponseName[kNickLen + 1] = {};
uint32_t g_notifyUntil = 0;
char g_notifyName[kNickLen + 1] = {};
uint32_t g_nextPresenceAt = 0;

// Stato dello stack NimBLE. g_hostInited copre init e deinit: nessun percorso
// chiama nimble_port_init due volte senza un deinit riuscito.
bool g_hostInited = false;
volatile bool g_synced = false;
uint8_t g_ownAddrType = BLE_OWN_ADDR_PUBLIC;

int gapEvent(struct ble_gap_event* event, void* arg);

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

void sha256Of(const uint8_t* data, size_t len, uint8_t out[32]) {
    const uint8_t empty = 0;
    const uint8_t* input = len == 0 ? &empty : data;
    if (mbedtls_sha256(input, len, out, 0) != 0) {
        std::memset(out, 0, 32);
    }
}

void sha256Of(const char* value, uint8_t out[32]) {
    sha256Of(reinterpret_cast<const uint8_t*>(value), std::strlen(value), out);
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
    sha256Of(material, key);
}

void deriveIv(const char* sender, const char* recipient, uint16_t counter,
              uint8_t iv[16]) {
    char material[kMaterialMax];
    keyMaterial(sender, recipient, material);
    char withCounter[kMaterialMax + 12];
    std::snprintf(withCounter, sizeof(withCounter), "%s%c%u", material, 0x1F,
                  static_cast<unsigned>(counter));
    uint8_t hash[32];
    sha256Of(withCounter, hash);
    std::memcpy(iv, hash, 16);
}

void nameTag(const char* name, uint8_t tag[kTagLen]) {
    char canonical[kNickLen + 1];
    canonicalInto(name, canonical);
    uint8_t hash[32];
    sha256Of(canonical, hash);
    std::memcpy(tag, hash, kTagLen);
}

bool encryptText(const char* sender, const char* recipient, uint16_t counter,
                 const char* text, uint8_t cipher[kCipherLen]) {
    uint8_t key[32];
    uint8_t iv[16];
    deriveKey(sender, recipient, key);
    deriveIv(sender, recipient, counter, iv);

    uint8_t plain[kCipherLen] = {};
    const size_t len = std::min(std::strlen(text), kTextLen);
    std::memcpy(plain, text, len);
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

// Estrae il manufacturer data (AD type 0xFF) dal payload grezzo.
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

bool buildMessagePacket(const char* text, const char* recipient,
                        uint16_t counter, uint8_t out[kPacketLen]) {
    std::memset(out, 0, kPacketLen);
    writeU16(out, kCompanyId);
    out[2] = kMagicMessage;

    const size_t senderLen = std::min(std::strlen(g_selfNick), kNickLen);
    std::memcpy(out + 3, g_selfNick, senderLen);
    std::memset(out + 3 + senderLen, ' ', kNickLen - senderLen);

    uint8_t tag[kTagLen];
    nameTag(recipient, tag);
    std::memcpy(out + 3 + kNickLen, tag, kTagLen);
    writeU16(out + 3 + kNickLen + kTagLen, counter);

    uint8_t cipher[kCipherLen];
    if (!encryptText(g_selfNick, recipient, counter, text, cipher)) {
        return false;
    }
    std::memcpy(out + 3 + kNickLen + kTagLen + 2, cipher, kCipherLen);
    return true;
}

// Il firmware 2.x allocava kPacketLen byte anche per i control packet: i
// campi oltre queryId restano a zero e il pacchetto in aria e' lungo 29 byte.
void buildControlPacket(uint8_t type, const char* name, uint32_t queryId,
                        uint8_t out[kPacketLen]) {
    std::memset(out, 0, kPacketLen);
    writeU16(out, kCompanyId);
    out[2] = kMagicControl;
    out[3] = type;

    const size_t targetLen = std::min(std::strlen(name), kNickLen);
    std::memcpy(out + 4, name, targetLen);
    std::memset(out + 4 + targetLen, ' ', kNickLen - targetLen);
    writeU32(out + 4 + kNickLen, queryId);
}

bool publishPacket(const uint8_t* packet) {
    if (!g_hostInited || !g_synced) {
        return false;
    }

    uint8_t adv[kAdvLen];
    adv[0] = static_cast<uint8_t>(kPacketLen + 1);
    adv[1] = 0xFF;
    std::memcpy(adv + 2, packet, kPacketLen);

    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
        delay(5);
    }

    int rc = ble_gap_adv_set_data(adv, sizeof(adv));
    if (rc != 0) {
        logger::warn("Chat BLE: adv data rc=%d", rc);
        return false;
    }

    struct ble_gap_adv_params params = {};
    params.conn_mode = BLE_GAP_CONN_MODE_NON;
    params.disc_mode = BLE_GAP_DISC_MODE_NON;
    params.itvl_min = kAdvInterval;
    params.itvl_max = kAdvInterval;
    rc = ble_gap_adv_start(g_ownAddrType, nullptr, BLE_HS_FOREVER, &params,
                           gapEvent, nullptr);
    if (rc != 0) {
        logger::warn("Chat BLE: adv start rc=%d", rc);
        return false;
    }
    return true;
}

bool publishControl(uint8_t type, const char* name, uint32_t queryId) {
    uint8_t packet[kPacketLen];
    buildControlPacket(type, name, queryId, packet);
    return publishPacket(packet);
}

bool sameAddress(const uint8_t a[6], const uint8_t b[6]) {
    return std::memcmp(a, b, 6) == 0;
}

// true se l'indirizzo non era stato visto negli ultimi kPeerTimeoutMs.
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
    copyFixed(g_notifyName, sizeof(g_notifyName), name);
    g_notifyUntil = now + kNotifyDurationMs;
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
        // Risposta solo con la chat aperta: fuori dalla chat il device non
        // annuncia il proprio nickname.
        if (g_chatActive && canonicalEquals(name, g_selfNick)) {
            portENTER_CRITICAL(&g_lock);
            copyFixed(g_controlResponseName, sizeof(g_controlResponseName),
                      name);
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

    // kControlResponse: il 2.x lo usava per la verifica di disponibilita' del
    // nickname. Qui il nickname e' fisso, quindi non c'e' una query locale da
    // soddisfare e la risposta viene ignorata.
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
    publishControl(kControlPresence, g_selfNick, 0);
    g_nextPresenceAt = millis() + kPresenceIntervalMs;
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

bool publishMessage(const char* text, const char* recipient) {
    uint8_t packet[kPacketLen];
    if (!buildMessagePacket(text, recipient, g_counter++, packet)) {
        return false;
    }
    if (!publishPacket(packet)) {
        logger::warn("Chat BLE: invio fallito");
        return false;
    }
    g_nextPresenceAt = millis() + kPresenceIntervalMs;
    return true;
}

void addLocalMessage(const char* text) {
    portENTER_CRITICAL(&g_lock);
    pushMessageLocked(g_selfNick, text, true);
    portEXIT_CRITICAL(&g_lock);
}

void handleAdvertisement(const struct ble_gap_disc_desc* desc) {
    if (!g_chatActive || desc == nullptr || desc->data == nullptr) {
        return;
    }

    const uint8_t* native = desc->addr.val;
    if (sameAddress(native, g_selfAddr)) {
        return;
    }

    uint8_t data[64];
    size_t dataLen = 0;
    if (!readManufacturerData(desc->data, desc->length_data, data,
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

bool startScan() {
    if (!g_hostInited || !g_synced) {
        return false;
    }
    struct ble_gap_disc_params params = {};
    params.itvl = kScanUnit;
    params.window = kScanUnit;
    params.passive = 1;
    params.filter_duplicates = 0;  // come wantDuplicates=true del 2.x
    const int rc = ble_gap_disc(g_ownAddrType, BLE_HS_FOREVER, &params,
                                gapEvent, nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        logger::warn("Chat BLE: scan rc=%d", rc);
        return false;
    }
    return true;
}

int gapEvent(struct ble_gap_event* event, void* arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_DISC:
            handleAdvertisement(&event->disc);
            return 0;
        case BLE_GAP_EVENT_DISC_COMPLETE:
            // Con BLE_HS_FOREVER non dovrebbe accadere; se il controller
            // interrompe la scansione la riavviamo finche' la chat e' aperta.
            if (g_chatActive) {
                startScan();
            }
            return 0;
        default:
            return 0;
    }
}

void onReset(int reason) {
    g_synced = false;
    logger::warn("Chat BLE: host reset, reason=%d", reason);
}

void onSync() {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        logger::error("Chat BLE: indirizzo BLE non disponibile, rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &g_ownAddrType);
    if (rc != 0) {
        logger::error("Chat BLE: tipo indirizzo BLE, rc=%d", rc);
        return;
    }
    // Stesso indirizzo pubblico usato dal 2.x per scartare i propri pacchetti.
    std::memset(g_selfAddr, 0, sizeof(g_selfAddr));
    if (ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, g_selfAddr, nullptr) != 0) {
        logger::warn("Chat BLE: indirizzo pubblico non leggibile");
    }
    g_synced = true;
}

void hostTask(void* param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

bool waitSynced(uint32_t timeoutMs) {
    const uint32_t start = millis();
    while (!g_synced && millis() - start < timeoutMs) {
        delay(10);
    }
    return g_synced;
}

bool ensureBle() {
    if (g_hostInited) {
        return waitSynced(1000);
    }

    // nimble_port_init() inizializza e abilita il controller una sola volta
    // (equivale a esp_nimble_hci_and_controller_init); non chiamare qui
    // esp_bt_controller_enable.
    const esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        logger::error("Chat BLE: nimble_port_init rc=%d", err);
        return false;
    }
    g_hostInited = true;

    ble_hs_cfg.reset_cb = onReset;
    ble_hs_cfg.sync_cb = onSync;

    const int rc = ble_svc_gap_device_name_set(kDeviceName);
    if (rc != 0) {
        logger::warn("Chat BLE: nome GAP rc=%d", rc);
    }

    nimble_port_freertos_init(hostTask);
    if (!waitSynced(2000)) {
        logger::error("Chat BLE: host non sincronizzato");
        return false;
    }
    return true;
}

void stopBle() {
    if (!g_hostInited) {
        return;
    }

    g_chatActive = false;
    // Se il sync non e' mai avvenuto le API GAP non sono utilizzabili.
    if (g_synced) {
        // Ritornano EALREADY se la procedura non e' attiva: ignorato.
        ble_gap_disc_cancel();
        ble_gap_adv_stop();
    }
    delay(50);

    const int rc = nimble_port_stop();
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        const esp_err_t err = nimble_port_deinit();
        if (err != ESP_OK) {
            logger::error("Chat BLE: nimble_port_deinit rc=%d", err);
        } else {
            g_hostInited = false;
        }
    } else {
        // Fallimento raro (host in stato inconsistente): non deinizializzare
        // a forza mentre il task host potrebbe essere ancora vivo.
        logger::error("Chat BLE: nimble_port_stop rc=%d", rc);
    }

    g_synced = false;
    g_ownAddrType = BLE_OWN_ADDR_PUBLIC;
    std::memset(g_selfAddr, 0, sizeof(g_selfAddr));
}

void textFormat(int x, int y, uint16_t color, uint8_t font, const char* fmt,
                ...) {
    char buffer[96];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    display::text(x, y, buffer, color, font);
}

void drawChat() {
    g_dirty = false;

    // Snapshot sotto lock: il disegno non tocca lo stato condiviso col task
    // NimBLE.
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
        textFormat(4, 24, display::kAccent, 0, "! %s e' nel raggio",
                   notifyName);
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
        const int maxLines =
            (display::height() - 12 - kFirstLineY) / kLineHeight;
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

void drawEditor(const char* title, const char* parent, const char* buffer) {
    display::clear();
    display::header(title, parent);
    display::text(4, 34, ">", display::kAccent, 0);
    display::text(14, 34, buffer, display::kText, 0);
    const int cursorX =
        14 + display::textWidth(buffer, 0);
    display::text(cursorX, 34, "_", display::kAccent, 0);
    display::footer("ENTER=conferma  ESC=annulla");
    display::push();
}

// Editor testuale minimale: sostituisce textinput del 2.x (non portato).
// Restituisce true solo se l'utente conferma con ENTER.
bool editText(const char* title, const char* parent, const char* initial,
              size_t maxLen, char* out, size_t outCap) {
    char buffer[kTextLen + 1] = {};
    maxLen = std::min(maxLen, kTextLen);
    const size_t initialLen = std::min(std::strlen(initial), maxLen);
    std::memcpy(buffer, initial, initialLen);
    buffer[initialLen] = '\0';
    size_t len = initialLen;

    navigation::waitRelease();
    for (;;) {
        navigation::update();
        const navigation::EventData event = navigation::poll();
        if (event.type == navigation::Event::Select) {
            copyFixed(out, outCap, buffer);
            return true;
        }
        if (event.type == navigation::Event::Back) {
            if (navigation::backspacePressed() && len > 0) {
                buffer[--len] = '\0';
            } else if (!navigation::backspacePressed()) {
                return false;  // ESC/backtick
            }
        } else if (event.type == navigation::Event::Text && len < maxLen) {
            buffer[len++] = event.ch;
            buffer[len] = '\0';
        } else if (len < maxLen) {
            // Le frecce sono caratteri fisici sulla Cardputer.
            char c = 0;
            if (event.type == navigation::Event::Up) {
                c = ';';
            } else if (event.type == navigation::Event::Down) {
                c = '.';
            } else if (event.type == navigation::Event::Left) {
                c = ',';
            } else if (event.type == navigation::Event::Right) {
                c = '/';
            }
            if (c != 0) {
                buffer[len++] = c;
                buffer[len] = '\0';
            }
        }
        drawEditor(title, parent, buffer);
        delay(25);
    }
}

void composeMessage() {
    char text[kTextLen + 1] = {};
    if (!editText("Nuovo messaggio", "Chat BLE", "", kTextLen, text,
                  sizeof(text))) {
        return;
    }
    if (text[0] == '\0') {
        return;
    }

    // Il messaggio compare nella chat solo se e' stato realmente trasmesso.
    if (!publishMessage(text, g_selfRecipient)) {
        ui::message("Chat BLE",
                    {"!Invio non riuscito.", "Messaggio non aggiunto."},
                    "Chat");
        return;
    }
    addLocalMessage(text);
}

void editRecipient() {
    char value[kNickLen + 1] = {};
    if (!editText("Destinatario chat", "Chat", g_selfRecipient, kNickLen, value,
                  sizeof(value))) {
        return;
    }
    if (value[0] == '\0') {
        ui::message("Destinatario chat", {"!Destinatario non valido."}, "Chat");
        return;
    }
    // Solo RAM: la configurazione persistente non e' ancora portata.
    copyFixed(g_selfRecipient, sizeof(g_selfRecipient), value);
    char line[32];
    std::snprintf(line, sizeof(line), ">A: %s", g_selfRecipient);
    ui::message("Destinatario chat", {line}, "Chat");
}

void editNickname() {
    char value[kNickLen + 1] = {};
    std::snprintf(value, sizeof(value), "%s", orbit_settings::nickname().c_str());
    if (!editText("Nickname chat", "Chat", value, kNickLen, value, sizeof(value))) {
        return;
    }
    if (value[0] == '\0') {
        ui::message("Nickname chat", {"!Nickname non valido."}, "Chat");
        return;
    }
    orbit_settings::setNickname(value);
    copyFixed(g_selfNick, sizeof(g_selfNick), orbit_settings::nickname().c_str());
    char line[32];
    std::snprintf(line, sizeof(line), "Tu: %s", g_selfNick);
    ui::message("Nickname chat", {line, "Max 6 caratteri."}, "Chat");
}

void runChat() {
    if (!ensureBle()) {
        ui::message("Chat BLE",
                    {"!Stack BLE non disponibile.", "Riprova tra qualche secondo."},
                    "Chat");
        stopBle();
        return;
    }

    copyFixed(g_selfNick, sizeof(g_selfNick), orbit_settings::nickname().c_str());
    if (g_selfRecipient[0] == '\0') {
        copyFixed(g_selfRecipient, sizeof(g_selfRecipient), kDefaultRecipient);
    }

    g_counter = static_cast<uint16_t>(esp_random());
    portENTER_CRITICAL(&g_lock);
    g_messageStart = 0;
    g_messageCount = 0;
    std::memset(g_peers, 0, sizeof(g_peers));
    std::memset(g_counters, 0, sizeof(g_counters));
    g_controlResponsePending = false;
    g_notifyUntil = 0;
    g_notifyName[0] = '\0';
    portEXIT_CRITICAL(&g_lock);
    g_dirty = true;

    // La scansione non parte finche' l'host non e' sincronizzato: ensureBle()
    // lo ha gia' garantito.
    if (!startScan()) {
        ui::message("Chat BLE",
                    {"!Scansione BLE non avviata.",
                     "Riprova tra qualche secondo."},
                    "Chat");
        stopBle();
        return;
    }

    logger::info("Chat BLE: in ascolto come %s", g_selfNick);
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
        if (static_cast<int32_t>(now - g_nextPresenceAt) >= 0) {
            publishPresence();
        }

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

    // Deinit completo: il task host NimBLE e il controller BT liberano la
    // memoria prima di tornare al menu (e ai giochi Wren).
    stopBle();
    navigation::waitRelease();
    logger::info("Chat BLE: chiusa");
}

}  // namespace

void shutdown() { stopBle(); }

void runMenu() {
    MenuManager menu("Chat", "Menu principale");
    menu.addItem("Apri chat", "chat", runChat);
    menu.addItem("Destinatario", "settings", editRecipient);
    menu.addItem("Nickname", "settings", editNickname);
    menu.addItem("Info", "info", [] {
        ui::message("Chat BLE",
                    {">Messaggi cifrati AES via BLE.",
                     "Ogni messaggio e' indirizzato a un",
                     "nickname; gli altri non lo mostrano.",
                     "Max 15 caratteri per messaggio.",
                     "Nickname: Chat/Impostazioni (max 6).",
                     "Nessun pairing: non usare per dati",
                     "davvero sensibili."},
                    "Chat");
    });
    menu.setHint("ESC=indietro  ;.=nav  ENTER=apri");
    menu.run(true);

    // Rete di sicurezza: qualunque uscita dal menu lascia il BT spento.
    shutdown();
}

}  // namespace orbit_chat
