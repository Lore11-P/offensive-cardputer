#include "Display.h"

#include <Arduino.h>
#include <FS.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Logger.h"
#include "ModuleFlags.h"
#include "Storage.h"

namespace display {
namespace {

M5Canvas s_sprite(&M5Cardputer.Display);
bool s_spriteOk = false;
uint8_t s_rotation = 1;

constexpr int kIconSize = 32;
constexpr int kIconCacheCount = 8;

struct IconCacheEntry {
    bool valid = false;
    char id[20] = {0};
    uint16_t pixels[kIconSize * kIconSize] = {0};
};

IconCacheEntry s_iconCache[kIconCacheCount];
int s_iconEvict = 0;

inline uint16_t read16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t read32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline uint16_t toRgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

bool createSprite() {
    s_sprite.deleteSprite();
    s_sprite.setColorDepth(16);
    const int w = M5Cardputer.Display.width();
    const int h = M5Cardputer.Display.height();
    s_spriteOk = (s_sprite.createSprite(w, h) != nullptr);
    if (!s_spriteOk) {
        logger::warn("Framebuffer non allocato: disegno diretto");
    }
    return s_spriteOk;
}

IconCacheEntry* findIcon(const char* id) {
    for (auto& entry : s_iconCache) {
        if (entry.valid && std::strncmp(entry.id, id, sizeof(entry.id) - 1) == 0) {
            return &entry;
        }
    }
    return nullptr;
}

// Decodifica BMP 32x32 24/32 bit (formato usato dagli asset in /icons).
bool decodeBmp32(const char* path, uint16_t* out) {
    File f = storage::openRead(path);
    if (!f) {
        return false;
    }
    uint8_t header[54] = {0};
    if (f.read(header, sizeof(header)) != sizeof(header) || header[0] != 'B' || header[1] != 'M') {
        f.close();
        return false;
    }
    const uint32_t dataOffset = read32(header + 10);
    const int32_t width = static_cast<int32_t>(read32(header + 18));
    const int32_t rawHeight = static_cast<int32_t>(read32(header + 22));
    const uint16_t bpp = read16(header + 28);
    const uint32_t compression = read32(header + 30);

    const int32_t absHeight = rawHeight < 0 ? -rawHeight : rawHeight;
    if (width != kIconSize || absHeight != kIconSize || (bpp != 24 && bpp != 32) ||
        compression != 0) {
        f.close();
        return false;
    }

    const bool topDown = rawHeight < 0;
    const int32_t height = absHeight;
    const int bytesPerPixel = bpp / 8;
    const int rowSize = ((width * bytesPerPixel + 3) / 4) * 4;
    std::vector<uint8_t> row(static_cast<size_t>(rowSize));

    for (int y = 0; y < height; ++y) {
        const int srcRow = topDown ? y : (height - 1 - y);
        if (!f.seek(dataOffset + static_cast<uint32_t>(srcRow) * rowSize)) {
            f.close();
            return false;
        }
        const size_t got = f.read(row.data(), static_cast<size_t>(rowSize));
        if (got != static_cast<size_t>(rowSize)) {
            f.close();
            return false;
        }
        for (int x = 0; x < width; ++x) {
            const uint8_t* px = row.data() + x * bytesPerPixel;
            out[static_cast<size_t>(y) * kIconSize + x] = toRgb565(px[2], px[1], px[0]);
        }
    }
    f.close();
    return true;
}

bool loadIcon(const std::string& id, uint16_t* out) {
    if (id.empty()) {
        return false;
    }
    IconCacheEntry* cached = findIcon(id.c_str());
    if (cached) {
        std::memcpy(out, cached->pixels, sizeof(cached->pixels));
        return true;
    }

    std::string path = "/icons/" + id + ".bmp";
    if (!decodeBmp32(path.c_str(), out)) {
        return false;
    }

    IconCacheEntry& entry = s_iconCache[s_iconEvict];
    s_iconEvict = (s_iconEvict + 1) % kIconCacheCount;
    entry.valid = true;
    std::snprintf(entry.id, sizeof(entry.id), "%s", id.c_str());
    std::memcpy(entry.pixels, out, sizeof(entry.pixels));
    return true;
}

void arrow(int x, int y, int s, uint16_t color, int dir) {
    auto& d = g();
    if (dir == 0) {
        d.fillTriangle(x + s / 2, y, x, y + s, x + s, y + s, color);
    } else if (dir == 1) {
        d.fillTriangle(x, y + s / 2, x + s, y, x + s, y + s, color);
    } else if (dir == 2) {
        d.fillTriangle(x + s / 2, y + s, x, y, x + s, y, color);
    } else {
        d.fillTriangle(x + s, y + s / 2, x, y, x, y + s, color);
    }
}

// Icone vettoriali di fallback, usate quando il BMP non e' presente.
void drawFallbackIcon(const std::string& id, int x, int y, int s) {
    auto& d = g();
    const int cx = x + s / 2;
    const int cy = y + s / 2;
    const int r = s / 2;

    if (id == "ble") {
        const uint16_t c = 0x4D7F;  // blu BLE
        d.drawLine(cx, y + 3, cx, y + s - 3, c);
        d.drawLine(cx, y + 3, x + s - 7, y + 9, c);
        d.drawLine(x + s - 7, y + 9, x + 7, cy, c);
        d.drawLine(x + 7, cy, x + s - 7, y + s - 9, c);
        d.drawLine(x + s - 7, y + s - 9, cx, y + s - 3, c);
    } else if (id == "wifi") {
        const uint16_t c = kAccent;
        d.drawArc(cx, y + s - 6, 3, 5, 225, 315, c);
        d.drawArc(cx, y + s - 6, 8, 10, 225, 315, c);
        d.drawArc(cx, y + s - 6, 13, 15, 225, 315, c);
        d.fillCircle(cx, y + s - 6, 2, c);
    } else if (id == "ir") {
        const uint16_t c = kWarning;
        d.fillRoundRect(x + 3, y + 9, 9, 13, 2, c);
        d.drawLine(x + 14, y + 16, x + s - 4, y + 16, c);
        arrow(x + s - 9, y + 8, 9, c, 1);
        d.drawLine(x + 2, y + 8, x + 2, y + 24, kTextDim);
    } else if (id == "games") {
        d.fillRoundRect(x + 2, y + 9, s - 4, s - 18, 6, 0x05FF);
        d.fillCircle(x + 9, y + 15, 3, kBlack);
        d.fillCircle(x + s - 10, y + 20, 2, kDanger);
        d.fillCircle(x + s - 15, y + 15, 2, kAccent);
        d.drawLine(x + 5, y + 5, x + s / 2, y + 10, kTextDim);
    } else if (id == "settings") {
        const uint16_t c = 0xBDF7;
        for (int i = 0; i < 8; ++i) {
            const float a = i * 45.0f * 0.0174533f;
            const int x0 = cx + static_cast<int>(cosf(a) * (r - 6));
            const int y0 = cy + static_cast<int>(sinf(a) * (r - 6));
            const int x1 = cx + static_cast<int>(cosf(a) * (r - 1));
            const int y1 = cy + static_cast<int>(sinf(a) * (r - 1));
            d.drawLine(x0, y0, x1, y1, c);
        }
        d.drawCircle(cx, cy, r - 8, c);
        d.fillCircle(cx, cy, 2, c);
    } else if (id == "scan" || id == "ble_scan" || id == "wifi_scan") {
        d.drawCircle(cx - 2, cy - 2, r - 6, kInfo);
        d.drawLine(cx + r - 9, cy + r - 9, x + s - 2, y + s - 2, kInfo);
    } else if (id == "tv") {
        d.fillRoundRect(x + 2, y + 5, s - 4, s - 13, 2, 0x2965);
        d.drawRect(x + 4, y + 7, s - 8, s - 17, kAccent);
        d.drawLine(cx, y + s - 8, cx, y + s - 3, kTextDim);
        d.drawLine(cx - 6, y + s - 3, cx + 6, y + s - 3, kTextDim);
    } else if (id == "jammer") {
        const uint16_t c = kDanger;
        d.fillCircle(x + 6, y + s - 8, 3, c);
        d.drawArc(x + 6, y + s - 8, 6, 8, 300, 360, c);
        d.drawArc(x + 6, y + s - 8, 11, 13, 300, 360, c);
        d.drawLine(x + 6, y + s - 8, x + s - 4, y + 6, c);
    } else if (id == "portal") {
        d.drawRoundRect(x + 2, y + 4, s - 4, s - 8, 3, kInfo);
        d.drawLine(x + 2, y + 10, x + s - 2, y + 10, kTextDim);
        d.fillCircle(x + 6, y + 7, 1, kDanger);
        d.fillCircle(x + 10, y + 7, 1, kWarning);
        d.fillCircle(x + 14, y + 7, 1, kAccent);
    } else if (id == "lock") {
        d.fillRoundRect(x + 6, y + 14, s - 12, s - 18, 2, kWarning);
        d.drawArc(cx, y + 13, 5, 7, 180, 360, kWarning);
        d.fillCircle(cx, y + 21, 2, kBlack);
    } else if (id == "tetris") {
        const int u = std::max(4, s / 5);
        d.fillRect(x + 2, y + 2 + u, u, u, kNeoCyan);
        d.fillRect(x + 2 + u, y + 2 + u, u, u, kNeoYellow);
        d.fillRect(x + 2 + 2 * u, y + 2 + u, u, u, kNeoPink);
        d.fillRect(x + 2 + u, y + 2, u, u, kNeoGreen);
        d.drawRect(x + 2, y + 2 + u, 3 * u, u, kInk);
        d.drawRect(x + 2 + u, y + 2, u, u, kInk);
    } else if (id == "breakout") {
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 3; ++col) {
                d.fillRect(x + 3 + col * (s - 8) / 3, y + 4 + row * 5,
                           (s - 10) / 3, 4,
                           (row + col) % 2 ? kNeoCyan : kNeoPink);
            }
        }
        d.fillRect(x + s / 3, y + s - 8, s / 3, 3, kInk);
        d.fillCircle(x + s - 6, y + s - 12, 2, kNeoYellow);
    } else if (id == "doom") {
        d.drawCircle(cx, cy, r - 4, kNeoRed);
        d.drawLine(cx - r + 4, cy, cx + r - 4, cy, kNeoRed);
        d.drawLine(cx, cy - r + 4, cx, cy + r - 4, kNeoRed);
        d.fillRect(x + 3, y + s - 8, s - 6, 3, kInk);
        d.fillRect(cx - 2, y + 5, 4, s - 13, kMuted);
    } else if (id == "snake") {
        const uint16_t c = kNeoGreen;
        d.fillRect(x + 3, y + s - 9, 7, 6, c);
        d.fillRect(x + 9, y + s - 9, 7, 6, c);
        d.fillRect(x + 15, y + s - 14, 6, 11, c);
        d.fillRect(x + 18, y + s - 20, 6, 8, c);
        d.fillCircle(x + s - 6, y + 7, 2, kNeoRed);
        d.fillCircle(x + 20, y + s - 18, 1, kInk);
    } else if (id == "pong") {
        d.fillRect(x + 3, y + 6, 4, s - 12, kNeoCyan);
        d.fillRect(x + s - 7, y + 10, 4, s - 12, kNeoPink);
        d.fillCircle(cx + 2, cy, 2, kNeoYellow);
        d.drawRect(x + 2, y + 2, s - 4, s - 4, kInk);
    } else if (id == "2048") {
        d.fillRoundRect(x + 2, y + 2, s - 4, s - 4, 2, kNeoYellow);
        d.drawRoundRect(x + 2, y + 2, s - 4, s - 4, 2, kInk);
        d.setTextDatum(0);
        d.setFont(&fonts::Font0);
        d.setTextColor(kInk);
        d.drawString("2048", x + 4, y + s / 2 - 4);
    } else if (id == "flappy") {
        d.fillRect(x + s - 11, y + 4, 7, s - 8, kNeoGreen);
        d.drawRect(x + s - 11, y + 4, 7, s - 8, kInk);
        d.fillRect(x + s - 13, y + 6, 11, 4, kAccentDark);
        d.fillCircle(x + 10, cy - 2, 5, kNeoYellow);
        d.drawCircle(x + 10, cy - 2, 5, kInk);
        d.fillTriangle(x + 14, cy - 3, x + 19, cy - 1, x + 14, cy + 1,
                       kNeoRed);
        d.fillCircle(x + 12, cy - 4, 1, kInk);
    } else if (id == "simon") {
        const int q = s / 2 - 3;
        d.fillRoundRect(x + 3, y + 3, q, q, 3, kNeoGreen);
        d.fillRoundRect(x + s - q - 3, y + 3, q, q, 3, kNeoRed);
        d.fillRoundRect(x + 3, y + s - q - 3, q, q, 3, kNeoYellow);
        d.fillRoundRect(x + s - q - 3, y + s - q - 3, q, q, 3, kNeoCyan);
        d.drawRoundRect(x + 3, y + 3, q, q, 3, kInk);
        d.drawRoundRect(x + s - q - 3, y + 3, q, q, 3, kInk);
        d.drawRoundRect(x + 3, y + s - q - 3, q, q, 3, kInk);
        d.drawRoundRect(x + s - q - 3, y + s - q - 3, q, q, 3, kInk);
    } else if (id == "mines") {
        d.fillRect(x + 4, y + 4, s - 8, s - 8, kNeoYellow);
        d.drawRect(x + 4, y + 4, s - 8, s - 8, kInk);
        for (int i = 0; i < 3; ++i) {
            d.drawLine(x + 4, y + 9 + i * 7, x + s - 4, y + 9 + i * 7, kMuted);
            d.drawLine(x + 9 + i * 7, y + 4, x + 9 + i * 7, y + s - 4, kMuted);
        }
        d.fillCircle(x + 12, y + 13, 4, kInk);
        d.drawLine(x + 19, y + 8, x + 19, y + 19, kInk);
        d.fillTriangle(x + 19, y + 8, x + 25, y + 11, x + 19, y + 14,
                       kNeoRed);
    } else if (id == "connect4") {
        d.fillRoundRect(x + 2, y + 4, s - 4, s - 8, 2, 0x031F);
        d.drawRoundRect(x + 2, y + 4, s - 4, s - 8, 2, kInk);
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 3; ++col) {
                const uint16_t color = (row * 3 + col) % 2 ? kNeoYellow : kNeoRed;
                d.fillCircle(x + 8 + col * 8, y + 11 + row * 8, 3, color);
                d.drawCircle(x + 8 + col * 8, y + 11 + row * 8, 3, kInk);
            }
        }
    } else if (id == "fav") {
        d.fillTriangle(cx, y + 3, cx - 4, cy + 2, cx + 4, cy + 2, kNeoYellow);
        d.fillTriangle(cx - 9, cy - 1, cx + 9, cy - 1, cx, y + s - 3, kNeoYellow);
        d.fillTriangle(cx - 8, cy + 8, cx + 8, cy + 8, cx, y + 8, kNeoYellow);
        d.drawCircle(cx, cy + 2, 2, kInk);
    } else if (id == "sd") {
        d.fillRoundRect(x + 5, y + 2, s - 10, s - 4, 2, kNeoCyan);
        d.drawRoundRect(x + 5, y + 2, s - 10, s - 4, 2, kInk);
        d.fillRect(x + 9, y + 5, s - 18, 4, kInk);
        d.fillRect(x + 9, y + 11, 3, 3, kInk);
        d.fillRect(x + 14, y + 11, 3, 3, kInk);
        d.drawRect(x + 9, y + 17, s - 18, 6, kInk);
    } else if (id == "folder") {
        d.fillRect(x + 2, y + 7, s - 4, s - 11, kNeoYellow);
        d.fillRect(x + 3, y + 4, s / 2, 5, kNeoYellow);
        d.drawRect(x + 2, y + 7, s - 4, s - 11, kInk);
        d.drawRect(x + 3, y + 4, s / 2, 5, kInk);
    } else if (id == "file") {
        d.fillRect(x + 6, y + 2, s - 12, s - 4, kPaperLight);
        d.drawRect(x + 6, y + 2, s - 12, s - 4, kInk);
        for (int i = 0; i < 4; ++i) {
            d.drawLine(x + 9, y + 8 + i * 4, x + s - 9, y + 8 + i * 4, kMuted);
        }
    } else if (id == "camera") {
        d.fillRoundRect(x + 2, y + 8, s - 4, s - 12, 2, kNeoPink);
        d.fillRect(x + s / 3, y + 5, s / 3, 4, kNeoPink);
        d.drawRoundRect(x + 2, y + 8, s - 4, s - 12, 2, kInk);
        d.drawCircle(cx, cy + 2, r - 8, kInk);
        d.drawCircle(cx, cy + 2, r - 11, kPaperLight);
    } else if (id == "backup") {
        d.fillRect(x + 4, y + 3, s - 8, s - 6, kNeoOrange);
        d.drawRect(x + 4, y + 3, s - 8, s - 6, kInk);
        d.fillRect(x + 8, y + 5, s - 16, 7, kPaperLight);
        d.fillRect(x + 8, y + 15, s - 16, s - 20, kInk);
    } else if (id == "trophy") {
        d.fillRect(x + 8, y + 3, s - 16, 3, kNeoYellow);
        d.fillTriangle(x + 8, y + 5, x + s - 8, y + 5, cx, y + s - 7, kNeoYellow);
        d.drawLine(x + 5, y + 6, x + 9, y + 11, kInk);
        d.drawLine(x + s - 5, y + 6, x + s - 9, y + 11, kInk);
        d.fillRect(cx - 2, y + s - 8, 4, 5, kInk);
        d.fillRect(cx - 6, y + s - 3, 12, 2, kInk);
    } else if (id == "info") {
        d.fillCircle(cx, cy, r - 3, kNeoCyan);
        d.drawCircle(cx, cy, r - 3, kInk);
        d.fillRect(cx - 1, y + 10, 2, 3, kInk);
        d.fillRect(cx - 1, y + 15, 2, 7, kInk);
    } else if (id == "trash") {
        d.fillRect(x + 7, y + 8, s - 14, s - 12, kNeoRed);
        d.drawRect(x + 7, y + 8, s - 14, s - 12, kInk);
        d.fillRect(x + 5, y + 5, s - 10, 3, kInk);
        d.fillRect(x + s / 2 - 2, y + 2, 4, 3, kInk);
        d.drawLine(cx - 3, y + 12, cx - 3, y + s - 6, kInk);
        d.drawLine(cx + 3, y + 12, cx + 3, y + s - 6, kInk);
    } else if (id == "calc") {
        d.fillRoundRect(x + 4, y + 2, s - 8, s - 4, 2, kNeoGreen);
        d.drawRoundRect(x + 4, y + 2, s - 8, s - 4, 2, kInk);
        d.fillRect(x + 7, y + 5, s - 14, 6, kPaperLight);
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                d.fillRect(x + 7 + col * 5, y + 14 + row * 4, 3, 2, kInk);
            }
        }
    } else if (id == "clock") {
        d.drawCircle(cx, cy + 2, r - 4, kInk);
        d.fillRect(cx - 1, cy - 4, 2, 7, kNeoRed);
        d.fillRect(cx, cy + 1, 5, 2, kNeoRed);
        d.fillRect(cx - 4, y + 2, 8, 3, kInk);
    } else if (id == "dice") {
        d.fillRoundRect(x + 4, y + 4, s - 8, s - 8, 3, kPaperLight);
        d.drawRoundRect(x + 4, y + 4, s - 8, s - 8, 3, kInk);
        d.fillCircle(x + 10, y + 10, 2, kNeoRed);
        d.fillCircle(x + s - 10, y + 10, 2, kNeoRed);
        d.fillCircle(cx, cy, 2, kNeoRed);
        d.fillCircle(x + 10, y + s - 10, 2, kNeoRed);
        d.fillCircle(x + s - 10, y + s - 10, 2, kNeoRed);
    } else if (id == "key") {
        d.drawCircle(x + 10, cy, 6, kNeoYellow);
        d.drawCircle(x + 10, cy, 3, kInk);
        d.drawLine(x + 15, cy, x + s - 3, cy, kNeoYellow);
        d.drawLine(x + s - 7, cy, x + s - 7, cy + 5, kNeoYellow);
        d.drawLine(x + s - 3, cy, x + s - 3, cy + 3, kNeoYellow);
    } else if (id == "bulb") {
        d.fillCircle(cx, y + 12, r - 6, kNeoYellow);
        d.drawCircle(cx, y + 12, r - 6, kInk);
        d.fillRect(cx - 4, y + 21, 8, 5, kMuted);
        d.drawLine(cx - 5, y + 10, cx - 1, y + 14, kInk);
        d.drawLine(cx + 5, y + 10, cx + 1, y + 14, kInk);
    } else {
        // Fallback generico: riquadro con iniziale dell'icona.
        d.drawRoundRect(x + 2, y + 2, s - 4, s - 4, 4, kTextDim);
        const char initial = id.empty() ? '?' : static_cast<char>(toupper(id[0]));
        char buf[2] = {initial, 0};
        d.setTextDatum(0);
        d.setFont(&fonts::Font2);
        d.setTextColor(kTextDim);
        d.drawString(buf, x + s / 2 - 4, y + s / 2 - 7);
    }
}

}  // namespace

bool begin() {
    M5Cardputer.Display.setRotation(0);
    M5Cardputer.Display.setBrightness(128);
    M5Cardputer.Display.fillScreen(kBlack);
    const bool ok = createSprite();
    if (ok) {
        g().fillScreen(kBackground);
        push();
    }
    return ok;
}

void applyRotation(uint8_t index) {
    s_rotation = index & 0x03;
    M5Cardputer.Display.setRotation(s_rotation);
    createSprite();
    clear();
    push();
}

uint8_t rotation() {
    return s_rotation;
}

lgfx::LGFXBase& g() {
    if (s_spriteOk) {
        return static_cast<lgfx::LGFXBase&>(s_sprite);
    }
    return static_cast<lgfx::LGFXBase&>(M5Cardputer.Display);
}

int width() {
    return g().width();
}

int height() {
    return g().height();
}

void clear(uint16_t color) {
    g().fillScreen(color);
}

void push() {
    if (s_spriteOk) {
        s_sprite.pushSprite(0, 0);
    }
}

void text(int x, int y, const std::string& s, uint16_t color, uint8_t font) {
    auto& d = g();
    const lgfx::IFont* f = &fonts::Font2;
    if (font == 0) {
        f = &fonts::Font0;
    } else if (font == 4) {
        f = &fonts::Font4;
    }
    d.setFont(f);
    d.setTextSize(1.0f);
    d.setTextDatum(0);  // top-left
    d.setTextWrap(false);
    d.setTextColor(color);
    d.drawString(s.c_str(), x, y);
}

void textCentered(int y, const std::string& s, uint16_t color, uint8_t font) {
    auto& d = g();
    if (font == 0) {
        d.setFont(&fonts::Font0);
    } else if (font == 4) {
        d.setFont(&fonts::Font4);
    } else {
        d.setFont(&fonts::Font2);
    }
    d.setTextSize(1.0f);
    d.setTextDatum(0);
    const int w = d.textWidth(s.c_str());
    text((width() - w) / 2, y, s, color, font);
}

void wrapText(int x, int y, int maxWidth, const std::vector<std::string>& lines, uint16_t color,
              uint8_t font, int lineHeight) {
    (void)maxWidth;  // Le righe sono gia' mandate a capo dal chiamante.
    for (const auto& line : lines) {
        text(x, y, line, color, font);
        y += lineHeight;
    }
}

void header(const std::string& title, const std::string& parent) {
    auto& d = g();
    d.fillRect(0, 0, width(), 21, kInk);
    d.fillRect(0, 20, width(), 2, kNeoYellow);
    d.fillRect(3, 4, 4, 12, kNeoCyan);

    std::string full = parent.empty() ? title : (parent + " > " + title);
    // Breadcrumb lunghi vengono compattati.
    const int maxChars = (width() - 16) / 6;
    if (static_cast<int>(full.size()) > maxChars && maxChars > 3) {
        full = full.substr(full.size() - static_cast<size_t>(maxChars - 3));
        full = "..." + full;
    }
    text(10, 3, full, kPaper, full.size() > 26 ? 0 : 2);
}

void footer(const std::string& hint) {
    auto& d = g();
    const int h = height();
    d.fillRect(0, h - 12, width(), 12, kInk);
    d.fillRect(0, h - 12, width(), 2, kNeoYellow);
    text(3, h - 10, hint, kPaper, 0);
}

void drawIcon(const std::string& icon, int x, int y, int size) {
    size = std::max(8, std::min(32, size));
    // Buffer statici, non locali: questa funzione occupava 4 KB di stack
    // (src + scaled) e, insieme al fallback vettoriale delle icone mancanti,
    // faceva traboccare lo stack del task dei menu (vedi panic "Stack canary
    // watchpoint triggered"). La UI e' a thread singolo: i buffer condivisi
    // sono sicuri e tolgono 4 KB dalla pila di ogni disegno icona.
    static uint16_t src[kIconSize * kIconSize];
    if (loadIcon(icon, src)) {
        static uint16_t scaled[32 * 32];
        for (int ty = 0; ty < size; ++ty) {
            const int sy = ty * kIconSize / size;
            for (int tx = 0; tx < size; ++tx) {
                const int sx = tx * kIconSize / size;
                scaled[ty * size + tx] = src[sy * kIconSize + sx];
            }
        }
        g().pushImage(x, y, size, size, scaled);
    } else {
        drawFallbackIcon(icon, x, y, size);
    }
}

void splash() {
    clear(kPaper);
    auto& d = g();
    d.fillRect(0, 0, width(), 5, kInk);
    d.fillRect(0, 30, width(), 4, kNeoYellow);
    textCentered(44, fw::kTitle, kInk, 4);
    textCentered(68, "M5Stack Cardputer ADV", kInk, 2);
    textCentered(86, "ESP32-S3 | firmware multi-funzione", kMuted, 0);
    progressBar(40, 108, width() - 80, 8, 1.0f, kNeoGreen);
    textCentered(height() - 18, std::string("build ") + fw::kBuildDate, kMuted, 0);
    push();
    delay(900);
}

void led(bool on, int x, int y, uint16_t color) {
    auto& d = g();
    d.fillCircle(x, y, 4, on ? color : kPanel);
    if (on) {
        d.drawCircle(x, y, 6, color);
    }
}

void progressBar(int x, int y, int w, int h, float fraction, uint16_t color) {
    auto& d = g();
    fraction = std::max(0.0f, std::min(1.0f, fraction));
    d.drawRoundRect(x, y, w, h, h / 2, kInk);
    const int fill = static_cast<int>((w - 4) * fraction);
    if (fill > 0) {
        d.fillRoundRect(x + 2, y + 2, fill, h - 4, (h - 4) / 2, color);
    }
}

bool saveScreenshot(const std::string& path) {
    File f = storage::openWrite(path, true);
    if (!f) {
        return false;
    }

    const int w = width();
    const int h = height();
    const int rowSize = ((w * 3 + 3) / 4) * 4;
    const uint32_t pixelBytes = static_cast<uint32_t>(rowSize) * h;
    uint8_t header[54] = {0};
    header[0] = 'B';
    header[1] = 'M';
    auto put16 = [](uint8_t* p, uint16_t v) {
        p[0] = static_cast<uint8_t>(v & 0xFF);
        p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    };
    auto put32 = [](uint8_t* p, uint32_t v) {
        p[0] = static_cast<uint8_t>(v & 0xFF);
        p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    };
    put32(header + 2, 54 + pixelBytes);
    put32(header + 10, 54);
    put32(header + 14, 40);
    put32(header + 18, static_cast<uint32_t>(w));
    put32(header + 22, static_cast<uint32_t>(h));
    put16(header + 26, 1);
    put16(header + 28, 24);
    put32(header + 34, pixelBytes);
    put32(header + 38, 2835);
    put32(header + 42, 2835);

    if (f.write(header, sizeof(header)) != sizeof(header)) {
        f.close();
        return false;
    }

    std::vector<uint8_t> row(static_cast<size_t>(rowSize), 0);
    for (int y = h - 1; y >= 0; --y) {
        for (int x = 0; x < w; ++x) {
            const uint16_t c = g().readPixel(x, y);
            const uint8_t r = static_cast<uint8_t>(((c >> 11) & 0x1F) * 255 / 31);
            const uint8_t gg = static_cast<uint8_t>(((c >> 5) & 0x3F) * 255 / 63);
            const uint8_t b = static_cast<uint8_t>((c & 0x1F) * 255 / 31);
            row[static_cast<size_t>(x) * 3] = b;
            row[static_cast<size_t>(x) * 3 + 1] = gg;
            row[static_cast<size_t>(x) * 3 + 2] = r;
        }
        if (f.write(row.data(), row.size()) != row.size()) {
            f.close();
            return false;
        }
    }
    f.close();
    return true;
}

}  // namespace display
