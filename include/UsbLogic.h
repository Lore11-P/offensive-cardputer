#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Protocollo USB file transfer e helper puri (testabili su host).
namespace usb {

constexpr size_t kMaxPathBytes = 160;
constexpr uint32_t kMaxFileBytes = 64u * 1024u * 1024u;
constexpr size_t kChunkBytes = 8192;

// CRC-32 (polinomio riflesso 0xEDB88320), compatibile con zlib.crc32.
inline uint32_t crc32(const uint8_t* data, size_t len, uint32_t seed = 0) {
    uint32_t crc = ~seed;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

// Normalizza un path relativo alla microSD: '/' come separatore, niente
// leading slash, niente segmenti vuoti, "." o "..", niente ':' o control
// character. Esempio: "/ir/tv/sony.ir" -> "ir/tv/sony.ir".
inline bool sanitizePath(const std::string& in, std::string& out) {
    out.clear();
    if (in.empty() || in.size() > kMaxPathBytes) {
        return false;
    }
    std::string path;
    path.reserve(in.size());
    for (char c : in) {
        if (c == '\\') {
            path.push_back('/');
        } else if (static_cast<unsigned char>(c) < 0x20 || c == ':') {
            return false;
        } else {
            path.push_back(c);
        }
    }
    size_t pos = (!path.empty() && path[0] == '/') ? 1 : 0;
    if (pos >= path.size()) {
        return false;
    }
    while (pos <= path.size()) {
        const size_t slash = path.find('/', pos);
        const size_t end = slash == std::string::npos ? path.size() : slash;
        const std::string segment = path.substr(pos, end - pos);
        if (segment.empty() || segment == "." || segment == "..") {
            out.clear();
            return false;
        }
        if (!out.empty()) {
            out.push_back('/');
        }
        out += segment;
        if (slash == std::string::npos) {
            break;
        }
        pos = slash + 1;
    }
    return !out.empty() && out.size() <= kMaxPathBytes;
}

}  // namespace usb
