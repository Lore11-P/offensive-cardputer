#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace orbit_tools {

// RFC 4648 Base32, case-insensitive. Spazi e '-' sono ignorati come
// separatori di comodo; '=' e' ammesso solo come padding finale.
inline bool base32Decode(const std::string& text, uint8_t* out, size_t capacity,
                         size_t& outLen) {
    uint32_t buffer = 0;
    int bits = 0;
    size_t length = 0;
    bool padding = false;
    for (unsigned char c : text) {
        if (c == ' ' || c == '\t' || c == '-') {
            continue;
        }
        if (c == '=') {
            padding = true;
            continue;
        }
        if (padding) {
            return false;
        }
        int value = 0;
        if (c >= 'A' && c <= 'Z') {
            value = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            value = c - 'a';
        } else if (c >= '2' && c <= '7') {
            value = 26 + (c - '2');
        } else {
            return false;
        }
        buffer = (buffer << 5) | static_cast<uint32_t>(value);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            if (length >= capacity) {
                return false;
            }
            out[length++] = static_cast<uint8_t>((buffer >> bits) & 0xFF);
        }
    }
    outLen = length;
    return length > 0;
}

inline bool base32Valid(const std::string& text) {
    uint8_t key[64] = {};
    size_t length = 0;
    const bool ok = base32Decode(text, key, sizeof(key), length);
    std::memset(key, 0, sizeof(key));
    return ok;
}

}  // namespace orbit_tools
