#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Tipi e regole pure dello store: testabili su host senza WiFi/display.
namespace store {

constexpr size_t kMaxCatalogBytes = 32768;
constexpr size_t kMaxGameBytes = 32768;
constexpr size_t kMaxGames = 24;

struct GameInfo {
    std::string id;
    std::string name;
    std::string version;
    std::string desc;
    std::string url;
    std::string sha256;  // 64 hex minuscole
    uint32_t size = 0;
};

// id usato come nome file: niente path traversal, niente spazi.
inline bool validId(const std::string& id) {
    if (id.empty() || id.size() > 24) {
        return false;
    }
    for (char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

inline bool isSha256Hex(const std::string& text) {
    if (text.size() != 64) {
        return false;
    }
    for (char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

// "/games/<id>.wren" oppure "" se l'id non e' valido.
inline std::string gamePath(const std::string& id) {
    return validId(id) ? std::string("/games/") + id + ".wren" : std::string();
}

// RGB 0..255 -> RGB565 usato da display.
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

}  // namespace store
