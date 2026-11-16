#include "Display.h"

#include <Arduino.h>
#include <FS.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Logger.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "Orbit.h"
#include "OrbitPalette.h"
#include "PixelFont.h"
#include "Storage.h"

namespace display {

// ---------------------------------------------------------------------------
//  Palette Orbit: sette atmosfere, un solo set di token.
// ---------------------------------------------------------------------------
uint16_t pal[orbit_palette::kTokenCount];

uint16_t kBlack = 0x0000;
uint16_t kWhite = 0xFFFF;
uint16_t kBackground = 0x0843;
uint16_t kPanel = 0x18E8;
uint16_t kHeader = 0x1085;
uint16_t kAccent = 0xFB4F;
uint16_t kAccentDark = 0x998A;
uint16_t kText = 0xDF1F;
uint16_t kTextDim = 0x73D6;
uint16_t kWarning = 0xFE4A;
uint16_t kDanger = 0xFA4B;
uint16_t kInfo = 0x8D1F;
uint16_t kPaper = 0x1085;
uint16_t kPaperLight = 0x18E8;
uint16_t kInk = 0xDF1F;
uint16_t kMuted = 0x73D6;
uint16_t kNeoYellow = 0xE5A9;
uint16_t kNeoCyan = 0x8D1F;
uint16_t kNeoPink = 0xFDB7;
uint16_t kNeoGreen = 0xFE4A;
uint16_t kNeoOrange = 0xFF12;
uint16_t kNeoRed = 0xFA4B;

namespace {

uint8_t s_theme = 0;

void applyPalette(uint8_t id) {
    if (id >= kThemeCount) {
        id = 0;
    }
    const uint16_t* row = orbit_palette::kThemes[id];
    std::memcpy(pal, row, sizeof(pal));
    kBlack = pal[T_black];
    kWhite = pal[T_hi];
    kBackground = pal[T_bg];
    kPanel = pal[T_bg3];
    kHeader = pal[T_bg2];
    kAccent = pal[T_acc];
    kAccentDark = pal[T_accD];
    kText = pal[T_text];
    kTextDim = pal[T_dim];
    kWarning = pal[T_warn];
    kDanger = pal[T_err];
    kInfo = pal[T_sec];
    // Superfici: "carta" = pannello, inchiostro = testo. Con PAPER il tema si
    // inverte da solo e tutte le schermate restano leggibili.
    kPaper = pal[T_bg2];
    kPaperLight = pal[T_bg3];
    kInk = pal[T_text];
    kMuted = pal[T_dim];
    kNeoYellow = pal[T_gold];
    kNeoCyan = pal[T_sec];
    kNeoPink = pal[T_accL];
    kNeoGreen = pal[T_ok];
    kNeoOrange = pal[T_goldL];
    kNeoRed = pal[T_err];
}

struct PaletteBoot {
    PaletteBoot() { applyPalette(0); }
};
PaletteBoot s_paletteBoot;

}  // namespace

void setTheme(uint8_t id) {
    if (id >= kThemeCount) {
        id = 0;
    }
    s_theme = id;
    applyPalette(id);
}

uint8_t themeId() {
    return s_theme;
}

const char* themeName(uint8_t id) {
    return orbit_palette::kNames[id >= kThemeCount ? 0 : id];
}

const char* themeNote(uint8_t id) {
    return orbit_palette::kNotes[id >= kThemeCount ? 0 : id];
}

uint16_t themeColor(uint8_t id, Tok token) {
    if (id >= kThemeCount) {
        id = 0;
    }
    if (token >= T_COUNT) {
        return 0;
    }
    return orbit_palette::kThemes[id][token];
}

namespace {

M5Canvas s_sprite(&M5Cardputer.Display);
bool s_spriteOk = false;
uint8_t s_rotation = 1;

// Buffer di riserva (non DMA) usato solo se l'allocazione DMA dello sprite
// fallisce: senza framebuffer la UI ripiegherebbe sul disegno diretto e
// flickerebbe. Con questa riserva il double buffer resta disponibile.
uint16_t* s_fallbackBuffer = nullptr;
bool s_fallbackMode = false;
uint32_t s_spriteBytes = 0;

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

void releaseFallbackBuffer() {
    if (s_fallbackBuffer != nullptr) {
        heap_caps_free(s_fallbackBuffer);
        s_fallbackBuffer = nullptr;
    }
    s_fallbackMode = false;
}

// Crea (o riusa) un framebuffer full-screen 16 bpp per la dimensione data.
// Prima prova la memoria DMA (pushSprite piu' veloce); se non basta ripiega
// su heap normale, che non puo' fare DMA ma resta un vero double buffer.
bool ensureSprite(int w, int h) {
    if (s_spriteOk && s_sprite.width() == w && s_sprite.height() == h) {
        return true;
    }

    s_spriteOk = false;
    s_spriteBytes = 0;
    s_sprite.deleteSprite();
    releaseFallbackBuffer();
    s_sprite.setColorDepth(16);

    if (s_sprite.createSprite(w, h) != nullptr) {
        s_spriteOk = true;
        s_spriteBytes = static_cast<uint32_t>(w) * h * 2;
        return true;
    }

    const uint32_t heapFree = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const uint32_t dmaFree = heap_caps_get_free_size(MALLOC_CAP_DMA);

    const size_t bytes = static_cast<size_t>(w) * h * 2;
    void* buffer = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    if (buffer != nullptr) {
        s_fallbackBuffer = static_cast<uint16_t*>(buffer);
        s_sprite.setBuffer(buffer, w, h, 16);
        s_spriteOk = (s_sprite.getBuffer() != nullptr);
        s_fallbackMode = s_spriteOk;
        if (s_spriteOk) {
            s_spriteBytes = static_cast<uint32_t>(bytes);
            logger::warn("Framebuffer DMA non disponibile: uso heap normale (heap %u KB)",
                         static_cast<unsigned>(heapFree / 1024));
        }
    }

    if (!s_spriteOk) {
        logger::warn("Framebuffer non allocato: disegno diretto (heap %u KB, DMA %u KB)",
                     static_cast<unsigned>(heapFree / 1024),
                     static_cast<unsigned>(dmaFree / 1024));
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
    } else if (id == "chat") {
        const uint16_t c = kNeoCyan;
        d.fillRoundRect(x + 2, y + 4, s - 4, s - 11, 4, c);
        d.drawRoundRect(x + 2, y + 4, s - 4, s - 11, 4, kInk);
        d.fillTriangle(x + 7, y + s - 7, x + 15, y + s - 7, x + 7, y + s - 1, c);
        d.drawLine(x + 7, y + s - 7, x + 7, y + s - 1, kInk);
        d.fillRect(x + 9, y + 10, s - 18, 2, kInk);
        d.fillRect(x + 9, y + 15, s - 24, 2, kInk);
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
        text(x + 4, y + s / 2 - 4, "2048", kInk, 0);
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
    } else if (id == "pacman") {
        d.fillCircle(cx, cy + 2, r - 3, kNeoYellow);
        d.fillTriangle(cx, cy + 2, x + s - 2, cy - 4, x + s - 2, cy + 9, kInk);
        d.fillCircle(cx - 2, cy - 3, 1, kInk);
    } else if (id == "chess") {
        d.fillRect(x + 5, y + 6, 22, 22, kPaperLight);
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                if ((row + col) % 2 == 1) {
                    d.fillRect(x + 5 + col * 5, y + 6 + row * 5, 5, 5, kInk);
                }
            }
        }
        d.drawRect(x + 5, y + 6, 22, 22, kInk);
        d.fillCircle(cx, y + 15, 4, kNeoYellow);
        d.drawCircle(cx, y + 15, 4, kInk);
        d.fillRect(cx - 4, y + 19, 9, 3, kNeoYellow);
        d.drawRect(cx - 4, y + 19, 9, 3, kInk);
    } else if (id == "minecraft") {
        d.fillRect(x + 5, y + 8, 22, 19, 0x8A61);
        d.fillRect(x + 5, y + 8, 22, 6, kNeoGreen);
        d.drawRect(x + 5, y + 8, 22, 19, kInk);
        d.drawPixel(x + 9, y + 17, 0x6200);
        d.drawPixel(x + 22, y + 21, 0x6200);
        d.drawPixel(x + 15, y + 12, 0x03A0);
        d.drawPixel(x + 11, y + 14, 0x03A0);
        d.drawPixel(x + 23, y + 11, 0x03A0);
    } else if (id == "usb") {
        // Tridente USB stilizzato.
        const uint16_t c = kInfo;
        d.fillCircle(cx, y + s - 6, 2, c);
        d.drawLine(cx, y + s - 8, cx, y + 5, c);
        d.fillTriangle(cx - 2, y + 5, cx + 2, y + 5, cx, y + 1, c);
        d.drawLine(cx, y + 14, cx - 6, y + 10, c);
        d.fillRect(cx - 8, y + 7, 4, 4, c);
        d.drawLine(cx, y + 18, cx + 6, y + 14, c);
        d.fillCircle(cx + 7, y + 13, 2, c);
    } else if (id == "capture") {
        // Elenco di frame con un'antenna.
        const uint16_t c = kAccent;
        d.drawRect(x + 3, y + 5, 14, s - 10, c);
        d.drawLine(x + 6, y + 10, x + 14, y + 10, c);
        d.drawLine(x + 6, y + 15, x + 14, y + 15, c);
        d.drawLine(x + 6, y + 20, x + 11, y + 20, c);
        d.drawArc(x + s - 7, y + s / 2, 3, 5, 250, 290, kInfo);
        d.drawArc(x + s - 7, y + s / 2, 6, 9, 250, 290, kInfo);
        d.fillCircle(x + s - 7, y + s / 2, 2, kInfo);
    } else if (id == "download") {
        // Freccia verso il basso sopra un vassoio.
        const uint16_t c = kInfo;
        d.drawLine(cx, y + 5, cx, y + s - 13, c);
        d.drawLine(cx, y + s - 13, cx - 5, y + s - 18, c);
        d.drawLine(cx, y + s - 13, cx + 5, y + s - 18, c);
        d.drawLine(x + 4, y + s - 8, x + s - 4, y + s - 8, c);
        d.drawLine(x + 4, y + s - 8, x + 4, y + s - 5, c);
        d.drawLine(x + s - 4, y + s - 8, x + s - 4, y + s - 5, c);
    } else if (id == "music") {
        // Doppia nota: due teste piene, due gambi e la traversa.
        const uint16_t c = kAccent;
        d.fillCircle(x + 10, y + s - 10, 4, c);
        d.fillCircle(x + s - 10, y + s - 13, 4, c);
        d.fillRect(x + 13, y + 7, 2, s - 17, c);
        d.fillRect(x + s - 7, y + 4, 2, s - 17, c);
        d.fillRect(x + 13, y + 7, s - 20, 3, c);
    } else {
        d.drawRoundRect(x + 2, y + 2, s - 4, s - 4, 4, kTextDim);
        const char initial = id.empty() ? '?' : static_cast<char>(toupper(id[0]));
        char buf[2] = {initial, 0};
        text(x + s / 2 - 3, y + s / 2 - 5, buf, kTextDim, 2);
    }
}

}  // namespace

bool begin() {
    // Parte direttamente in landscape (rotazione 1, il default): creare prima
    // lo sprite in portrait e poi ricrearlo raddoppiava le allocazioni grosse
    // all'avvio, con rischio di fallire per frammentazione dell'heap.
    s_rotation = 1;
    M5Cardputer.Display.setRotation(s_rotation);
    M5Cardputer.Display.setBrightness(128);
    M5Cardputer.Display.fillScreen(kBlack);
    const bool ok = ensureSprite(M5Cardputer.Display.width(), M5Cardputer.Display.height());
    logger::info("Framebuffer %ux%u %s (%u KB, heap libero %u KB)",
                 static_cast<unsigned>(M5Cardputer.Display.width()),
                 static_cast<unsigned>(M5Cardputer.Display.height()),
                 !ok ? "diretto" : (s_fallbackMode ? "sprite RAM" : "sprite DMA"),
                 static_cast<unsigned>(s_spriteBytes / 1024),
                 static_cast<unsigned>(ESP.getFreeHeap() / 1024));
    if (ok) {
        g().fillScreen(kBackground);
        push();
    }
    return ok;
}

void applyRotation(uint8_t index) {
    const uint8_t previous = s_rotation;
    s_rotation = index & 0x03;
    M5Cardputer.Display.setRotation(s_rotation);

    if (!ensureSprite(M5Cardputer.Display.width(), M5Cardputer.Display.height())) {
        // Il nuovo framebuffer non e' allocabile: torna alla rotazione
        // precedente invece di restare senza double buffer (flicker).
        s_rotation = previous;
        M5Cardputer.Display.setRotation(s_rotation);
        ensureSprite(M5Cardputer.Display.width(), M5Cardputer.Display.height());
        logger::warn("Rotazione annullata: framebuffer non allocabile");
    }

    clear();
    push();
}

bool framebufferActive() {
    return s_spriteOk;
}

uint32_t framebufferBytes() {
    return s_spriteBytes;
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

namespace {

// Font pixel 5x7 disegnato dal codice: nessun font della libreria grafica.
//  - font 0: micro, advance 6, altezza 8 (label, hint)
//  - font 2: body, advance 7, altezza 9 (testo corrente)
//  - font 4: headline, scala 2x, advance 12, altezza 16
int advanceFor(uint8_t font) {
    if (font == 4) {
        return 12;
    }
    return font == 0 ? 6 : 7;
}

int glyphHeight(uint8_t font) {
    if (font == 4) {
        return 16;
    }
    return font == 0 ? 8 : 9;
}

void drawGlyph(int x, int y, char ch, uint16_t color, uint8_t scale) {
    uint8_t c = static_cast<uint8_t>(ch);
    if (c < pixelfont::kFirst || c > pixelfont::kLast) {
        c = '?';
    }
    const uint8_t* glyph = pixelfont::kGlyphs[c - pixelfont::kFirst];
    auto& d = g();
    for (int col = 0; col < 5; ++col) {
        const uint8_t bits = glyph[col];
        if (bits == 0) {
            continue;
        }
        // Colonna in run verticali: meno chiamate di drawPixel.
        int row = 0;
        while (row < 8) {
            if (!(bits & (1u << row))) {
                ++row;
                continue;
            }
            int end = row;
            while (end < 8 && (bits & (1u << end))) {
                ++end;
            }
            if (scale == 1) {
                d.drawFastVLine(x + col, y + row, end - row, color);
            } else {
                d.fillRect(x + col * scale, y + row * scale, scale, (end - row) * scale, color);
            }
            row = end;
        }
    }
}

}  // namespace

int textWidth(const std::string& s, uint8_t font) {
    if (s.empty()) {
        return 0;
    }
    return static_cast<int>(s.size()) * advanceFor(font) - (advanceFor(font) - 5 * (font == 4 ? 2 : 1));
}

int textHeight(uint8_t font) {
    return glyphHeight(font);
}

void text(int x, int y, const std::string& s, uint16_t color, uint8_t font) {
    const uint8_t scale = (font == 4) ? 2 : 1;
    const int adv = advanceFor(font);
    int cx = x;
    for (char ch : s) {
        if (ch == ' ') {
            cx += adv;
            continue;
        }
        drawGlyph(cx, y, ch, color, scale);
        cx += adv;
    }
}

void textCentered(int y, const std::string& s, uint16_t color, uint8_t font) {
    text((width() - textWidth(s, font)) / 2, y, s, color, font);
}

void wrapText(int x, int y, int maxWidth, const std::vector<std::string>& lines, uint16_t color,
              uint8_t font, int lineHeight) {
    (void)maxWidth;  // Le righe sono gia' mandate a capo dal chiamante.
    for (const auto& line : lines) {
        text(x, y, line, color, font);
        y += lineHeight;
    }
}

namespace {

// Stato del rilevamento euristico della ricarica per i modelli senza PMIC.
int16_t s_battBaseline = -1;
uint32_t s_battSampleAt = 0;
uint8_t s_battRise = 0;
bool s_battCharging = false;

}  // namespace

void resetChargingDetector() {
    s_battBaseline = -1;
    s_battSampleAt = 0;
    s_battRise = 0;
    s_battCharging = false;
}

bool chargingLikely() {
    // Solo lo stato esplicito "in carica" e' affidabile: su Cardputer/ADV
    // isCharging() ritorna charge_unknown, che come bool sarebbe sempre vero.
    if (M5Cardputer.Power.isCharging() == m5::Power_Class::is_charging) {
        return true;
    }
    const int16_t mv = M5Cardputer.Power.getBatteryVoltage();
    if (mv <= 0) {
        return false;
    }
    const uint32_t now = millis();
    if (s_battBaseline < 0) {
        s_battBaseline = mv;
        s_battSampleAt = now;
        return false;
    }
    if (now - s_battSampleAt < 2000) {
        return s_battCharging;
    }
    s_battSampleAt = now;

    const int16_t delta = static_cast<int16_t>(mv - s_battBaseline);
    if (s_battCharging) {
        // Il caricatore e' stato staccato: la tensione crolla sotto la linea
        // di base registrata prima della ricarica.
        if (delta < -15) {
            s_battCharging = false;
            s_battRise = 0;
            s_battBaseline = mv;
        }
        return s_battCharging;
    }

    // Gradino persistente sopra la tensione a riposo: probabile caricatore.
    if (delta > 15) {
        if (s_battRise < 3) {
            ++s_battRise;
        }
    } else {
        s_battRise = 0;
    }
    // La linea di base insegue lentamente la tensione a riposo.
    s_battBaseline = static_cast<int16_t>((s_battBaseline * 7 + mv) / 8);
    if (s_battRise >= 3) {
        s_battCharging = true;
    }
    return s_battCharging;
}

void drawBatteryWidget(int right, int y) {
    auto& d = g();
    const int level = static_cast<int>(M5Cardputer.Power.getBatteryLevel());
    const bool charging = chargingLikely();
    const int percent = level < 0 ? -1 : std::min(level, 100);

    uint16_t color = C(T_dim);
    char label[8];
    if (percent < 0) {
        std::snprintf(label, sizeof(label), "--%%");
    } else {
        std::snprintf(label, sizeof(label), "%d%%", percent);
        color = percent <= 15 ? C(T_err) : (percent <= 35 ? C(T_warn) : C(T_ok));
    }

    constexpr int kBodyW = 18;
    constexpr int kBodyH = 8;
    const int bodyX = right - (kBodyW + 2);
    const int bodyY = y;
    text(bodyX - 4 - textWidth(label, 0), y - 1, label, color, 0);

    // Fuel gauge telemetrico: cornice + segmenti.
    d.drawRect(bodyX, bodyY, kBodyW, kBodyH, C(T_line));
    d.fillRect(bodyX + kBodyW, bodyY + 2, 2, kBodyH - 4, C(T_line));
    if (percent > 0) {
        const int inner = kBodyW - 4;
        const int fillW = inner * percent / 100;
        for (int i = 0; i < fillW; i += 3) {
            const int seg = std::min(2, fillW - i);
            d.fillRect(bodyX + 2 + i, bodyY + 2, seg, kBodyH - 4, color);
        }
    }
    if (charging) {
        const int cx = bodyX + kBodyW / 2;
        d.drawLine(cx + 1, bodyY + 1, cx - 2, bodyY + 3, C(T_acc));
        d.drawLine(cx - 2, bodyY + 3, cx + 2, bodyY + 3, C(T_acc));
        d.drawLine(cx + 2, bodyY + 3, cx - 1, bodyY + 6, C(T_acc));
    }
}

void header(const std::string& title, const std::string& parent) {
    auto& d = g();
    constexpr int kBar = 22;
    d.fillRect(0, 0, width(), kBar, C(T_bg2));
    d.drawFastHLine(0, 0, width(), C(T_line));
    d.drawFastHLine(0, kBar - 1, width(), C(T_line));
    // Tacche telemetriche sul bordo superiore.
    for (int x = 26; x < width() - 70; x += 14) {
        d.drawPixel(x, 0, C(T_line2));
    }

    // Emblema orbitale: anello + satellite.
    d.drawCircle(8, 11, 4, C(T_acc));
    d.drawPixel(8, 11, C(T_text));
    d.drawPixel(12, 8, C(T_sec));

    std::string full = parent.empty() ? title : (parent + " > " + title);

    constexpr int kTitleX = 17;
    constexpr int kReserve = 44;  // solo la batteria: il segnale sta nel footer
    const int maxTitleW = width() - kTitleX - kReserve;
    uint8_t font = 2;
    if (textWidth(full, 2) > maxTitleW) {
        font = 0;
    }
    const bool split = !parent.empty() && textWidth(full, font) <= maxTitleW;
    if (!split && textWidth(full, font) > maxTitleW) {
        while (!full.empty() && textWidth(full + "..", font) > maxTitleW) {
            full.pop_back();
        }
        full += "..";
    }
    const int textY = font == 0 ? 7 : 6;
    if (split) {
        // Breadcrumb attenuato, titolo corrente in evidenza.
        const std::string crumb = parent + " > ";
        text(kTitleX, textY, crumb, C(T_dim), font);
        text(kTitleX + textWidth(crumb, font), textY, title, C(T_text), font);
    } else {
        text(kTitleX, textY, full, C(T_text), font);
    }

    // Batteria (fuel gauge) a destra: unico elemento dello stato di bordo.
    drawBatteryWidget(width() - 3, 7);
}

void footer(const std::string& hint, const std::string& trailing) {
    auto& d = g();
    const int h = height();
    d.fillRect(0, h - 12, width(), 12, C(T_bg2));
    d.drawFastHLine(0, h - 12, width(), C(T_line));
    // Segmento d'accento: la "riga di stato" del computer di bordo.
    d.fillRect(0, h - 12, 14, 1, C(T_acc));
    d.drawPixel(4, h - 7, C(T_acc));
    text(9, h - 10, hint, C(T_dim), 0);
    if (!trailing.empty()) {
        text(width() - 6 - textWidth(trailing, 0), h - 10, trailing, C(T_acc), 0);
        return;
    }
    // LED di stato pulsante.
    const bool blink = (millis() / 700) % 2 == 0;
    d.fillCircle(width() - 6, h - 6, 2, blink ? C(T_ok) : C(T_line2));
}

void drawIcon(const std::string& icon, int x, int y, int size) {
    size = std::max(8, std::min(32, size));
    // Prima le icone procedurali Orbit: nessun asset, stessa estetica in ogni
    // tema. I BMP restano come riserva per id non ancora mappati.
    if (drawOrbitIcon(icon, x, y, size)) {
        return;
    }
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

namespace {

// Attende ms millisecondi ridisegnando; true se l'utente preme un tasto.
template <typename Fn>
bool bootFrames(uint32_t ms, Fn&& draw) {
    const uint32_t start = millis();
    uint32_t frame = 0;
    for (;;) {
        const uint32_t now = millis();
        const uint32_t elapsed = now - start;
        if (elapsed >= ms) {
            break;
        }
        draw(elapsed, frame);
        push();
        if (M5Cardputer.Keyboard.isPressed()) {
            return true;
        }
        delay(30);
        ++frame;
    }
    return false;
}

inline float easeOut(float t) {
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return 1.0f - (1.0f - t) * (1.0f - t);
}

}  // namespace

void splash() {
    clear(C(T_sky0));

    // ── Fase 1: carta del titolo. Il pianeta sorge, il razzo entra in orbita.
    const uint32_t titleMs = 1250;
    bool skipped = bootFrames(titleMs, [&](uint32_t t, uint32_t frame) {
        const float p = static_cast<float>(t) / static_cast<float>(titleMs);
        orbit::sky(0, 0, width(), height(), 0x51ED);
        const int cy = 150 - static_cast<int>(easeOut(p) * 46.0f);
        orbit::orbitPath(width() / 2, cy, 96, 24, C(T_line2), C(T_line), static_cast<int>(frame / 3));
        orbit::planet(width() / 2, cy, 38, -0.6f, -0.5f, true);

        const float a = -1.15f + easeOut(p) * 2.6f;
        const int rx = static_cast<int>(sinf(a) * 96);
        const int ry = static_cast<int>(cosf(a) * 24);
        orbit::rocket(width() / 2 + rx, cy + ry, 13, a - 0.35f, true,
                      static_cast<uint8_t>(frame));

        if (p > 0.18f) {
            const uint16_t titleColor = C(T_hi);
            textCentered(14, "ORBIT", titleColor, 4);
            textCentered(34, "CARDPUTER FLIGHT DECK", C(T_acc), 0);
            textCentered(45, "firmware multi-funzione", C(T_dim), 0);
        }
        // Telemetria di fondo: striscia HUD che chiude la scena.
        auto& d = g();
        d.fillRect(0, height() - 16, width(), 16, C(T_bg2));
        d.drawFastHLine(0, height() - 16, width(), C(T_line));
        d.fillRect(0, height() - 16, 14, 1, C(T_acc));
        text(6, height() - 14, "SYS " + std::string(fw::kBuildDate), C(T_dim), 0);
        orbit::signalBars(width() - 22, height() - 14, 4, C(T_ok), C(T_line2));
    });

    if (!skipped) {
        // ── Fase 2: rampa di lancio, countdown, decollo.
        const uint32_t bootMs = 1900;
        const int padY = 104;
        skipped = bootFrames(bootMs, [&](uint32_t t, uint32_t frame) {
            const float p = static_cast<float>(t) / static_cast<float>(bootMs);
            orbit::sky(0, 0, width(), height(), 0xB007);
            auto& d = g();

            // Terreno, piattaforma e torre di lancio (complesso a destra).
            const int padX = 194;
            d.fillRect(0, padY, width(), height() - padY, C(T_sky2));
            d.drawFastHLine(0, padY, width(), C(T_line));
            for (int x = 6; x < width(); x += 11) {
                d.drawPixel(x, padY + 4, C(T_line2));
            }
            d.fillRect(padX - 18, padY - 2, 38, 3, C(T_line));
            d.fillRect(212, padY - 40, 4, 38, C(T_line));
            d.fillRect(206, padY - 40, 10, 2, C(T_line));
            d.drawLine(206, padY - 32, 202, padY - 32, C(T_line2));
            d.drawLine(206, padY - 24, 199, padY - 24, C(T_line2));
            d.drawFastVLine(214, padY - 50, 10, C(T_line2));
            d.fillRect(213, padY - 53, 3, 3, C(T_err));
            d.drawPixel(214, padY - 52, C(T_hi));

            // Countdown.
            const int step = static_cast<int>(t / 380);
            const char* label = step == 0 ? "T-3" : (step == 1 ? "T-2" : (step == 2 ? "T-1" : "T+0"));
            const bool ignition = (t > 1450);
            text(10, 12, "RAMP 01", C(T_dim), 0);
            text(10, 22, label, ignition ? C(T_acc) : C(T_hi), 4);
            text(10, 44, ignition ? "MOTORI ACCESI" : "SEQUENZA DI LANCIO",
                 ignition ? C(T_flame0) : C(T_sec), 0);
            text(10, 55, ignition ? "spinta nominale" : "telemetria nominale", C(T_dim), 0);
            text(10, 66, ignition ? "GATE OPEN" : "GATE HOLD",
                 ignition ? C(T_ok) : C(T_line2), 0);

            // Razzo: fermo sulla rampa, poi decollo con accelerazione.
            int rocketY = padY - 16;
            bool flame = false;
            if (ignition) {
                flame = true;
                const float lift = easeOut((t - 1450) / 450.0f);
                rocketY -= static_cast<int>(lift * 125.0f);
            }
            if (!ignition && step >= 3) {
                flame = true;  // sfiammata di accensione
            }
            // Fumo alla base.
            const int puffs = ignition ? std::min(6, static_cast<int>((t - 1450) / 60) + 1) : 1;
            for (int i = 0; i < puffs; ++i) {
                const int px = padX - 22 + ((i * 37) % 46);
                const int py = padY - 2 - ((i * 13) % 10);
                const int pr = 5 + (i * 3) % 7;
                orbit::ditherRect(px - pr, py - pr / 2, pr * 2, pr, C(T_line2), C(T_sky2), 8);
            }
            if (rocketY > -36) {
                orbit::rocket(padX, rocketY, 24, 0.0f, flame,
                              static_cast<uint8_t>(frame + 2));
            }

            // Barra di stato del lancio.
            const int barW = width() - 20;
            d.drawRect(10, height() - 18, barW, 7, C(T_line));
            d.fillRect(12, height() - 16,
                       static_cast<int>((barW - 4) * std::min(1.0f, p * 1.05f)), 3,
                       ignition ? C(T_flame1) : C(T_sec));
            text(12, height() - 28, ignition ? "DECOLLO" : "PRE-FLIGHT CHECK", C(T_dim), 0);
        });
    }

    // Chiusura: lampo d'orbita e passaggio al menu.
    clear(C(T_bg));
    orbit::sky(0, 0, width(), height(), 0x0B17);
    textCentered(height() / 2 - 12, "ORBIT READY", C(T_hi), 4);
    textCentered(height() / 2 + 10, "flight deck online", C(T_acc), 0);
    progressBar(40, height() - 26, width() - 80, 6, 1.0f, C(T_ok));
    push();
    delay(skipped ? 120 : 260);
    navigation::waitRelease();
}

void led(bool on, int x, int y, uint16_t color) {
    auto& d = g();
    d.fillCircle(x, y, 3, on ? color : C(T_line2));
    if (on) {
        d.drawCircle(x, y, 5, orbit::blend(color, C(T_bg), 150));
        d.drawPixel(x, y, C(T_hi));
    }
}

void progressBar(int x, int y, int w, int h, float fraction, uint16_t color) {
    auto& d = g();
    fraction = std::max(0.0f, std::min(1.0f, fraction));
    d.drawRect(x, y, w, h, C(T_line));
    d.fillRect(x + 1, y + 1, w - 2, h - 2, C(T_bg));
    const int inner = w - 4;
    const int fill = static_cast<int>(inner * fraction);
    if (fill > 0) {
        d.fillRect(x + 2, y + 2, fill, h - 4, color);
    }
    // Tacche ogni 8 px: scala di telemetria.
    for (int i = 0; i < inner; i += 8) {
        if (i >= fill) {
            d.drawFastVLine(x + 2 + i, y + 2, h - 4, C(T_line2));
        }
    }
    if (fraction > 0.0f && fraction < 1.0f) {
        d.fillRect(x + 2 + fill - 1, y + 1, 1, h - 2, C(T_hi));
    }
}

void progressRing(int cx, int cy, int radius, float fraction, uint16_t color) {
    auto& d = g();
    fraction = std::max(0.0f, std::min(1.0f, fraction));
    const int total = 96;
    const int lit = static_cast<int>(total * fraction);
    for (int i = 0; i < total; ++i) {
        const float a = (static_cast<float>(i) / total) * 6.2831853f - 1.5707963f;
        const int x = cx + static_cast<int>(cosf(a) * radius);
        const int y = cy + static_cast<int>(sinf(a) * radius);
        const bool on = i < lit;
        const bool tick = (i % 8) == 0;
        const uint16_t c = on ? color : C(T_line2);
        d.drawPixel(x, y, c);
        if (tick) {
            const int x2 = cx + static_cast<int>(cosf(a) * (radius + 2));
            const int y2 = cy + static_cast<int>(sinf(a) * (radius + 2));
            d.drawPixel(x2, y2, c);
        }
        if (on) {
            const int x2 = cx + static_cast<int>(cosf(a) * (radius - 1));
            const int y2 = cy + static_cast<int>(sinf(a) * (radius - 1));
            d.drawPixel(x2, y2, orbit::blend(color, C(T_bg), 120));
        }
    }
    d.drawPixel(cx, cy, C(T_dim));
}

void trajectory(int x, int y, int w, int h, float fraction, const std::string& label) {
    auto& d = g();
    fraction = std::max(0.0f, std::min(1.0f, fraction));
    // Curva di ascesa: quarto di ellisse dal basso a sinistra all'alto a destra.
    const int steps = 48;
    const int lit = static_cast<int>(steps * fraction);
    int lastX = x;
    int lastY = y + h;
    for (int i = 0; i <= steps; ++i) {
        const float a = 3.1415927f * 0.5f * (1.0f - static_cast<float>(i) / steps);
        const int px = x + static_cast<int>((1.0f - cosf(a)) * w);
        const int py = y + h - static_cast<int>(sinf(a) * h);
        if (i % 3 == 0) {
            d.drawPixel(px, py, i <= lit ? C(T_sec) : C(T_line2));
        }
        if (i == lit) {
            lastX = px;
            lastY = py;
        }
    }
    d.drawFastHLine(x, y + h, w, C(T_line2));
    orbit::rocket(lastX, lastY, 11, 0.6f, fraction > 0.02f && fraction < 0.995f, 0);
    if (!label.empty()) {
        text(x, y + h + 3, label, C(T_dim), 0);
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
