#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Tipi e regole pure dello store: testabili su host senza WiFi/display.
namespace store {

constexpr size_t kMaxCatalogBytes = 32768;
constexpr size_t kMaxGameBytes = 65536;
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
bool validId(const std::string& id);
bool isSha256Hex(const std::string& text);

// "/games/<id>.wren" oppure "" se l'id non e' valido.
std::string gamePath(const std::string& id);

// RGB 0..255 -> RGB565 usato da display.
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b);

}  // namespace store
