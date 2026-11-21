#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Protocollo ESP-NOW di ORBIT: frame piccoli (max 216 byte), header fisso da
// 16 byte con magic/versione/tipo/sessione/seq/lunghezza/CRC32 del payload.
// Tutti i campi numerici sono little endian.
namespace espnow {
namespace proto {

constexpr uint8_t kMagic = 0x4F;  // 'O'
constexpr uint8_t kVersion = 1;

constexpr size_t kHeaderSize = 16;
constexpr size_t kMaxPayload = 200;  // ESP_NOW_MAX_DATA_LEN e' 250
constexpr size_t kMaxPacket = kHeaderSize + kMaxPayload;
constexpr size_t kChunkSize = kMaxPayload;

constexpr uint32_t kMaxFileBytes = 512u * 1024u;
constexpr size_t kMaxName = 16;
constexpr size_t kMaxFileName = 32;
constexpr size_t kMaxCommand = 120;
constexpr size_t kMaxClipboard = 160;

// Tipi di frame.
enum Type : uint8_t {
    TypeHello = 1,       // announce: nome del peer
    TypeFileMeta = 2,    // nome file, dimensione, CRC32 totale, nome mittente
    TypeFileData = 3,    // blocco da kChunkSize byte
    TypeAck = 4,         // conferma: flags = AckKind, payload[0] = AckStatus
    TypeCommand = 5,     // comando testuale (solo whitelist in ricezione)
    TypeClipboard = 6,   // testo clipboard condiviso
    TypeClipRequest = 7  // richiesta del clipboard corrente ai peer
};

enum AckKind : uint8_t { AckMeta = 0, AckData = 1, AckFinal = 2 };

enum AckStatus : uint8_t {
    StOk = 0,
    StCrc = 1,
    StIo = 2,
    StSeq = 3,
    StBusy = 4,
    StSize = 5
};

// flags per i frame non-ACK.
constexpr uint8_t kFlagNoAck = 0x01;  // broadcast: il ricevente non risponde
constexpr uint8_t kFlagLast = 0x02;   // ultimo blocco del file

struct Packet {
    uint8_t type = 0;
    uint8_t flags = 0;
    uint32_t session = 0;
    uint16_t seq = 0;
    uint16_t len = 0;
    uint32_t crc = 0;
    uint8_t payload[kMaxPayload] = {};
};

inline uint16_t readU16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8);
}

inline uint32_t readU32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

inline void writeU16(uint8_t* data, uint16_t value) {
    data[0] = static_cast<uint8_t>(value & 0xFF);
    data[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

inline void writeU32(uint8_t* data, uint32_t value) {
    data[0] = static_cast<uint8_t>(value & 0xFF);
    data[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    data[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

// CRC32 standard (polinomio riflesso 0xEDB88320), calcolabile a blocchi.
inline uint32_t crc32Begin() {
    return 0xFFFFFFFFu;
}

inline uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

inline uint32_t crc32End(uint32_t crc) {
    return crc ^ 0xFFFFFFFFu;
}

inline uint32_t crc32(const uint8_t* data, size_t len) {
    return crc32End(crc32Update(crc32Begin(), data, len));
}

inline size_t encodePacket(const Packet& packet, uint8_t* out, size_t capacity) {
    if (out == nullptr || packet.len > kMaxPayload ||
        capacity < kHeaderSize + packet.len) {
        return 0;
    }
    out[0] = kMagic;
    out[1] = kVersion;
    out[2] = packet.type;
    out[3] = packet.flags;
    writeU32(out + 4, packet.session);
    writeU16(out + 8, packet.seq);
    writeU16(out + 10, packet.len);
    writeU32(out + 12, crc32(packet.payload, packet.len));
    std::memcpy(out + kHeaderSize, packet.payload, packet.len);
    return kHeaderSize + packet.len;
}

inline bool decodePacket(const uint8_t* in, size_t size, Packet& out) {
    if (in == nullptr || size < kHeaderSize) {
        return false;
    }
    if (in[0] != kMagic || in[1] != kVersion) {
        return false;
    }
    out.type = in[2];
    out.flags = in[3];
    out.session = readU32(in + 4);
    out.seq = readU16(in + 8);
    out.len = readU16(in + 10);
    out.crc = readU32(in + 12);
    if (out.len > kMaxPayload || size != kHeaderSize + out.len) {
        return false;
    }
    if (crc32(in + kHeaderSize, out.len) != out.crc) {
        return false;
    }
    if (out.len > 0) {
        std::memcpy(out.payload, in + kHeaderSize, out.len);
    }
    return true;
}

// Autotest del codec: vettore CRC noto e rifiuto di frame corrotti/troncati.
inline bool selfTest() {
    const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    if (crc32(check, sizeof(check)) != 0xCBF43926u) {
        return false;
    }

    Packet source{};
    source.type = TypeFileData;
    source.flags = kFlagLast;
    source.session = 0x11223344u;
    source.seq = 7;
    const uint8_t body[] = {'h', 'e', 'l', 'l', 'o'};
    source.len = sizeof(body);
    std::memcpy(source.payload, body, sizeof(body));

    uint8_t buffer[kMaxPacket];
    const size_t size = encodePacket(source, buffer, sizeof(buffer));
    if (size != kHeaderSize + sizeof(body)) {
        return false;
    }

    Packet decoded{};
    if (!decodePacket(buffer, size, decoded)) {
        return false;
    }
    if (decoded.type != source.type || decoded.session != source.session ||
        decoded.seq != source.seq || decoded.len != source.len ||
        std::memcmp(decoded.payload, body, sizeof(body)) != 0) {
        return false;
    }

    if (decodePacket(buffer, size - 1, decoded)) {
        return false;  // troncato
    }
    buffer[0] = 0;
    if (decodePacket(buffer, size, decoded)) {
        return false;  // magic errato
    }
    buffer[0] = kMagic;
    buffer[size - 1] ^= 0xFF;
    if (decodePacket(buffer, size, decoded)) {
        return false;  // CRC payload errato
    }
    buffer[size - 1] ^= 0xFF;

    Packet oversized{};
    oversized.len = static_cast<uint16_t>(kMaxPayload + 1);
    if (encodePacket(oversized, buffer, sizeof(buffer)) != 0) {
        return false;
    }
    return true;
}

}  // namespace proto
}  // namespace espnow
