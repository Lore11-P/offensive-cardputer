#include "Minecraft.h"

#include <Arduino.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Display.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "Scores.h"

#if ENABLE_MODULE_GAMES

namespace games {
namespace {

constexpr int kTile = 4;
constexpr int kWorldW = 160;
constexpr int kWorldH = 72;
constexpr int kViewW = 60;
constexpr int kViewH = 26;
constexpr int kHudH = 15;
constexpr int kHotbarY = 119;
constexpr int kHotbarH = 16;
constexpr int kReach = 8;

enum : uint8_t {
    kAir = 0,
    kGrass,
    kDirt,
    kStone,
    kWood,
    kLeaves,
    kCoal,
    kIron,
    kDiamond,
    kBedrock,
    kPortal,
    kTypeCount,
};

// Hotbar: solo blocchi piazzabili.
const uint8_t kHotbar[4] = {kDirt, kStone, kWood, kLeaves};

uint8_t g_world[kWorldH][kWorldW];
int8_t g_surface[kWorldW];

uint32_t g_rng = 1;
int g_spawnX = 24;
int g_spawnGround = 20;
int g_portalY = 17;
int g_diamonds = 0;
int g_blocksMined = 0;
int g_inv[kTypeCount];
int g_selected = 0;
int g_curX = 20;
int g_curY = 10;
bool g_won = false;
uint32_t g_finishMs = 0;
uint32_t g_startedAt = 0;
uint32_t g_hintUntil = 0;

struct Player {
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    bool onGround = false;
    int8_t face = 1;
};

constexpr float kPw = 6.0f;
constexpr float kPh = 11.0f;
Player g_player;

uint32_t rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

uint8_t at(int x, int y) {
    if (x < 0 || x >= kWorldW || y < 0 || y >= kWorldH) {
        return kBedrock;  // fuori dal mondo: solido
    }
    return g_world[y][x];
}

void setAt(int x, int y, uint8_t value) {
    if (x >= 0 && x < kWorldW && y >= 0 && y < kWorldH) {
        g_world[y][x] = value;
    }
}

bool solidAt(int x, int y) {
    const uint8_t b = at(x, y);
    return b != kAir && b != kPortal;
}

bool boxSolid(float px, float py) {
    const int x0 = static_cast<int>(floorf(px / kTile));
    const int x1 = static_cast<int>(floorf((px + kPw - 0.05f) / kTile));
    const int y0 = static_cast<int>(floorf(py / kTile));
    const int y1 = static_cast<int>(floorf((py + kPh - 0.05f) / kTile));
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (solidAt(x, y)) {
                return true;
            }
        }
    }
    return false;
}

void beep(float freq, uint32_t ms) {
    M5Cardputer.Speaker.tone(freq, ms);
}

void generate(uint32_t seed) {
    g_rng = seed == 0 ? 0x9E3779B9u : seed;
    if (g_rng == 0) {
        g_rng = 1;
    }
    for (int y = 0; y < kWorldH; ++y) {
        for (int x = 0; x < kWorldW; ++x) {
            g_world[y][x] = kAir;
        }
    }

    // Profilo collinare con deriva limitata.
    int height = 18 + static_cast<int>(rnd() % 5);
    int slope = 0;
    for (int x = 0; x < kWorldW; ++x) {
        slope += static_cast<int>(rnd() % 3) - 1;
        slope = std::max(-1, std::min(1, slope));
        height += slope;
        height = std::max(16, std::min(24, height));
        g_surface[x] = static_cast<int8_t>(height);
    }

    for (int x = 0; x < kWorldW; ++x) {
        const int h = g_surface[x];
        for (int y = h; y < kWorldH; ++y) {
            uint8_t block = kStone;
            if (y == h) {
                block = kGrass;
            } else if (y <= h + 3) {
                block = kDirt;
            } else if (y == kWorldH - 1) {
                block = kBedrock;
            } else {
                const int depth = y - h;
                const uint32_t roll = rnd() % 1000;
                if (depth > 30 && roll < 5) {
                    block = kDiamond;
                } else if (depth > 14 && roll < 16) {
                    block = kIron;
                } else if (depth > 5 && roll < 34) {
                    block = kCoal;
                }
            }
            g_world[y][x] = block;
        }
    }

    // Bordi indistruttibili.
    for (int y = 0; y < kWorldH; ++y) {
        g_world[y][0] = kBedrock;
        g_world[y][kWorldW - 1] = kBedrock;
    }
    for (int x = 0; x < kWorldW; ++x) {
        g_world[kWorldH - 1][x] = kBedrock;
    }

    // Caverne: passeggiate casuali sotto la superficie.
    static const int8_t kDx[4] = {1, -1, 0, 0};
    static const int8_t kDy[4] = {0, 0, 1, -1};
    for (int i = 0; i < 7; ++i) {
        int cx = 4 + static_cast<int>(rnd() % (kWorldW - 8));
        int cy = 28 + static_cast<int>(rnd() % (kWorldH - 32));
        int dir = static_cast<int>(rnd() % 4);
        for (int step = 0; step < 70; ++step) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (std::abs(dx) + std::abs(dy) > 1) {
                        continue;
                    }
                    const int x = cx + dx;
                    const int y = cy + dy;
                    if (x > 1 && x < kWorldW - 2 && y > 27 && y < kWorldH - 2 &&
                        g_world[y][x] != kBedrock) {
                        g_world[y][x] = kAir;
                    }
                }
            }
            if (rnd() % 100 < 28) {
                dir = static_cast<int>(rnd() % 4);
            }
            cx = std::max(2, std::min(kWorldW - 3, cx + kDx[dir]));
            cy = std::max(28, std::min(kWorldH - 3, cy + kDy[dir]));
        }
    }

    // Alberi.
    for (int x = 3; x < kWorldW - 3; ++x) {
        if (g_world[g_surface[x]][x] != kGrass || rnd() % 100 >= 10) {
            continue;
        }
        const int trunk = 3 + static_cast<int>(rnd() % 3);
        const int top = g_surface[x] - trunk;
        if (top < 3) {
            continue;
        }
        for (int y = g_surface[x] - 1; y >= top; --y) {
            g_world[y][x] = kWood;
        }
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                if (std::abs(dx) == 2 && dy != 0) {
                    continue;
                }
                const int lx = x + dx;
                const int ly = top + dy;
                if (lx > 0 && lx < kWorldW - 1 && ly > 0 && g_world[ly][lx] == kAir) {
                    g_world[ly][lx] = kLeaves;
                }
            }
        }
    }

    // Spawn e portale: si libera un'area pianeggiante alla quota piu' bassa
    // della zona, cosi' il giocatore e il portale non finiscono dentro una
    // collina.
    g_spawnX = 24;
    const int left = g_spawnX - 6;
    const int right = g_spawnX + 6;
    int gy = 0;
    for (int x = left; x <= right; ++x) {
        if (x <= 1 || x >= kWorldW - 2) {
            continue;
        }
        gy = std::max(gy, static_cast<int>(g_surface[x]));
    }
    for (int x = left; x <= right; ++x) {
        if (x <= 1 || x >= kWorldW - 2) {
            continue;
        }
        for (int y = 0; y < gy; ++y) {
            g_world[y][x] = kAir;
        }
        g_world[gy][x] = kGrass;
    }
    const int px = g_spawnX;
    g_spawnGround = gy;
    g_portalY = gy - 3;
    // Portale 2x3 con architrave: i lati restano aperti per poterci entrare.
    for (int x = px - 1; x <= px + 2; ++x) {
        setAt(x, gy - 4, kBedrock);
    }
    for (int y = gy - 3; y <= gy - 1; ++y) {
        setAt(px, y, kPortal);
        setAt(px + 1, y, kPortal);
    }

    for (int i = 0; i < kTypeCount; ++i) {
        g_inv[i] = 0;
    }
    g_inv[kDirt] = 12;
    g_inv[kStone] = 6;
}

void startRun(uint32_t seed) {
    generate(seed);
    g_player.x = (g_spawnX - 4) * kTile + 1.0f;
    g_player.y = g_spawnGround * kTile - kPh;
    g_player.vx = 0.0f;
    g_player.vy = 0.0f;
    g_player.onGround = false;
    g_player.face = 1;
    g_curX = static_cast<int>((g_player.x + kPw / 2.0f) / kTile);
    g_curY = static_cast<int>((g_player.y + kPh / 2.0f) / kTile);
    g_diamonds = 0;
    g_blocksMined = 0;
    g_selected = 0;
    g_won = false;
    g_finishMs = 0;
    g_hintUntil = 0;
    g_startedAt = millis();
}

int playerTileX() {
    return static_cast<int>((g_player.x + kPw / 2.0f) / kTile);
}

int playerTileY() {
    return static_cast<int>((g_player.y + kPh / 2.0f) / kTile);
}

bool inReach(int tx, int ty) {
    return std::abs(tx - playerTileX()) <= kReach && std::abs(ty - playerTileY()) <= kReach;
}

void clampCursor() {
    const int pcx = playerTileX();
    const int pcy = playerTileY();
    g_curX = std::max(pcx - kReach, std::min(pcx + kReach, g_curX));
    g_curY = std::max(pcy - kReach, std::min(pcy + kReach, g_curY));
    g_curX = std::max(1, std::min(kWorldW - 2, g_curX));
    g_curY = std::max(0, std::min(kWorldH - 2, g_curY));
}

void mineAt() {
    if (!inReach(g_curX, g_curY)) {
        return;
    }
    const uint8_t block = at(g_curX, g_curY);
    if (block == kAir || block == kBedrock || block == kPortal) {
        return;
    }
    const uint8_t drop = block == kGrass ? kDirt : block;
    setAt(g_curX, g_curY, kAir);
    if (drop < kTypeCount) {
        ++g_inv[drop];
    }
    ++g_blocksMined;
    if (block == kDiamond) {
        ++g_diamonds;
        beep(1400.0f, 40);
    } else {
        beep(650.0f, 10);
    }
}

void placeAt() {
    if (!inReach(g_curX, g_curY) || at(g_curX, g_curY) != kAir) {
        return;
    }
    const uint8_t block = kHotbar[g_selected];
    if (block >= kTypeCount || g_inv[block] <= 0) {
        return;
    }
    const float tx = static_cast<float>(g_curX * kTile);
    const float ty = static_cast<float>(g_curY * kTile);
    if (tx < g_player.x + kPw && tx + kTile > g_player.x && ty < g_player.y + kPh &&
        ty + kTile > g_player.y) {
        return;
    }
    setAt(g_curX, g_curY, block);
    --g_inv[block];
    beep(980.0f, 10);
}

void updatePlayer(float dt) {
    float target = 0.0f;
    if (navigation::pressed('a')) {
        target -= 38.0f;
        g_player.face = -1;
    }
    if (navigation::pressed('d')) {
        target += 38.0f;
        g_player.face = 1;
    }
    g_player.vx = target;

    g_player.vy += 300.0f * dt;
    g_player.vy = std::min(g_player.vy, 180.0f);

    const float nx = g_player.x + g_player.vx * dt;
    if (!boxSolid(nx, g_player.y)) {
        g_player.x = nx;
    }
    const float ny = g_player.y + g_player.vy * dt;
    if (!boxSolid(g_player.x, ny)) {
        g_player.y = ny;
    } else {
        g_player.vy = 0.0f;
    }
    g_player.onGround = boxSolid(g_player.x, g_player.y + 3.0f);
    if (g_player.onGround && navigation::pressed('w')) {
        g_player.vy = -100.0f;
        g_player.onGround = false;
    }
}

bool touchingPortal() {
    for (int y = g_portalY; y < g_portalY + 3; ++y) {
        for (int x = g_spawnX; x <= g_spawnX + 1; ++x) {
            const float rx = static_cast<float>(x * kTile);
            const float ry = static_cast<float>(y * kTile);
            if (g_player.x < rx + kTile && g_player.x + kPw > rx && g_player.y < ry + kTile &&
                g_player.y + kPh > ry) {
                return true;
            }
        }
    }
    return false;
}

uint16_t sampleColor(uint8_t block) {
    switch (block) {
        case kGrass:
            return 0x05E0;
        case kDirt:
            return 0x8A61;
        case kStone:
            return 0x7BEF;
        case kWood:
            return 0x9A60;
        case kLeaves:
            return 0x2E00;
        default:
            return 0x4A49;
    }
}

void drawBlock(uint8_t block, int x, int y, uint32_t now) {
    auto& d = display::g();
    switch (block) {
        case kGrass:
            d.fillRect(x, y, kTile, kTile, 0x8A61);
            d.fillRect(x, y, kTile, 1, 0x05E0);
            d.drawPixel(x + 1, y + 2, 0x03A0);
            break;
        case kDirt:
            d.fillRect(x, y, kTile, kTile, 0x8A61);
            d.drawPixel(x + 2, y + 2, 0x6200);
            break;
        case kStone:
            d.fillRect(x, y, kTile, kTile, 0x7BEF);
            d.drawPixel(x + 1, y + 1, 0x9CF3);
            d.drawPixel(x + 2, y + 2, 0x630C);
            break;
        case kWood:
            d.fillRect(x, y, kTile, kTile, 0x9A60);
            d.drawPixel(x + 1, y, 0x6A00);
            d.drawPixel(x + 1, y + 2, 0x6A00);
            break;
        case kLeaves:
            d.fillRect(x, y, kTile, kTile, 0x2E00);
            d.drawPixel(x + 1, y + 1, 0x1C80);
            d.drawPixel(x + 2, y + 2, 0x3F00);
            break;
        case kCoal:
            d.fillRect(x, y, kTile, kTile, 0x7BEF);
            d.drawPixel(x + 1, y + 1, 0x2104);
            d.drawPixel(x + 2, y + 2, 0x2104);
            break;
        case kIron:
            d.fillRect(x, y, kTile, kTile, 0x7BEF);
            d.drawPixel(x + 1, y + 1, 0xD69A);
            d.drawPixel(x + 2, y + 2, 0xD69A);
            break;
        case kDiamond:
            d.fillRect(x, y, kTile, kTile, 0x7BEF);
            d.drawPixel(x + 1, y + 1, 0x07FF);
            d.drawPixel(x + 2, y + 2, 0x07FF);
            break;
        case kBedrock:
            d.fillRect(x, y, kTile, kTile, 0x4208);
            d.drawPixel(x + 1, y + 1, 0x2104);
            break;
        case kPortal: {
            const bool active = g_diamonds >= 3;
            const bool blink = ((now / 250) & 1) == 0;
            const uint16_t base = active ? (blink ? 0x780F : 0xB81F) : 0x300A;
            d.fillRect(x, y, kTile, kTile, base);
            if (blink) {
                d.drawPixel(x + 1, y + 1, 0xF81F);
            }
            break;
        }
        default:
            break;
    }
}

void drawWorld(uint32_t now, uint32_t elapsedSec) {
    auto& d = display::g();
    int camX = playerTileX() - kViewW / 2;
    int camY = playerTileY() - kViewH / 2;
    camX = std::max(0, std::min(kWorldW - kViewW, camX));
    camY = std::max(0, std::min(kWorldH - kViewH, camY));

    // Sfondo: cielo in alto, buio sotterraneo.
    for (int vy = 0; vy < kViewH; ++vy) {
        const int wy = camY + vy;
        const uint16_t bg = wy < 12 ? 0x3D7F : (wy < 26 ? 0x2A5F : 0x0861);
        d.fillRect(0, kHudH + vy * kTile, display::width(), kTile, bg);
    }

    for (int vy = 0; vy < kViewH; ++vy) {
        const int wy = camY + vy;
        for (int vx = 0; vx < kViewW; ++vx) {
            const int wx = camX + vx;
            const uint8_t block = at(wx, wy);
            if (block == kAir) {
                continue;
            }
            drawBlock(block, vx * kTile, kHudH + vy * kTile, now);
        }
    }

    // Mirino.
    const int cx = (g_curX - camX) * kTile;
    const int cy = kHudH + (g_curY - camY) * kTile;
    if (cx >= 0 && cx < display::width() - kTile && cy >= kHudH &&
        cy < kHudH + kViewH * kTile) {
        const uint16_t col = inReach(g_curX, g_curY) ? display::kNeoYellow : 0x4A49;
        d.drawRect(cx, cy, kTile, kTile, col);
        d.drawPixel(cx - 1, cy - 1, col);
        d.drawPixel(cx + kTile, cy + kTile, col);
    }

    // Giocatore.
    const int px = static_cast<int>(g_player.x) - camX * kTile;
    const int py = kHudH + static_cast<int>(g_player.y) - camY * kTile;
    d.fillRect(px + 1, py, 4, 1, 0x4200);
    d.fillRect(px + 1, py + 1, 4, 2, 0xFD6B);
    d.drawPixel(px + (g_player.face > 0 ? 4 : 1), py + 1, 0x2104);
    d.fillRect(px, py + 3, 6, 4, 0x04BF);
    d.fillRect(px, py + 7, 2, 4, 0x3186);
    d.fillRect(px + 4, py + 7, 2, 4, 0x3186);

    // HUD superiore.
    d.fillRect(0, 0, display::width(), kHudH, display::kBlack);
    d.drawFastHLine(0, kHudH - 1, display::width(), 0x2945);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "DIAMANTI %d/3", g_diamonds);
    display::text(3, 2, buf, g_diamonds >= 3 ? display::kNeoCyan : display::kText, 0);
    std::snprintf(buf, sizeof(buf), "Y%d", playerTileY());
    display::text(104, 2, buf, display::kTextDim, 0);
    std::snprintf(buf, sizeof(buf), "%02u:%02u", static_cast<unsigned>(elapsedSec / 60),
                  static_cast<unsigned>(elapsedSec % 60));
    display::text(200, 2, buf, display::kTextDim, 0);

    // Hotbar.
    d.fillRect(0, kHotbarY, display::width(), kHotbarH, display::kBlack);
    constexpr int kSlotW = 32;
    const int startX = (display::width() - 4 * kSlotW) / 2;
    for (int i = 0; i < 4; ++i) {
        const int x = startX + i * kSlotW;
        const uint8_t block = kHotbar[i];
        const bool selected = i == g_selected;
        d.drawRect(x, kHotbarY + 2, kSlotW - 2, kHotbarH - 4,
                   selected ? display::kNeoYellow : 0x4A49);
        d.fillRect(x + 3, kHotbarY + 6, 8, 6, sampleColor(block));
        std::snprintf(buf, sizeof(buf), "%d", g_inv[block]);
        display::text(x + 13, kHotbarY + 4, buf,
                      g_inv[block] > 0 ? display::kText : display::kMuted, 0);
        std::snprintf(buf, sizeof(buf), "%d", i + 1);
        display::text(x + 13, kHotbarY + 10, buf, 0x4A49, 0);
    }

    if (millis() < g_hintUntil) {
        display::textCentered(kHotbarY - 12, "Servono 3 diamanti", display::kWarning, 0);
    }
}

void drawIntro() {
    display::clear(display::kBlack);
    display::textCentered(10, "MINECRAFT MINI", display::kNeoGreen, 2);
    display::textCentered(32, "Raccogli 3 diamanti", display::kText, 0);
    display::textCentered(42, "e torna al portale.", display::kText, 0);
    static const char* const kLines[] = {
        "A/D muovi  W salta",
        "frecce  sposta il mirino",
        "INVIO  scava il blocco",
        "B  posa il blocco scelto",
        "1-4  cambia blocco",
        "`  esci",
    };
    int y = 58;
    for (const char* line : kLines) {
        display::textCentered(y, line, display::kTextDim, 0);
        y += 10;
    }
    display::textCentered(122, "ENTER per iniziare", display::kNeoYellow, 0);
    display::push();
}

void drawWin(uint32_t best) {
    auto& d = display::g();
    const int x = 26;
    const int y = 34;
    const int w = display::width() - 2 * x;
    const int h = 74;
    d.fillRect(x, y, w, h, display::kBlack);
    d.drawRect(x, y, w, h, display::kNeoGreen);
    display::textCentered(y + 7, "PORTALE ATTIVO!", display::kNeoGreen, 2);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "Tempo %u:%02u", static_cast<unsigned>(g_finishMs / 60000),
                  static_cast<unsigned>((g_finishMs / 1000) % 60));
    display::textCentered(y + 28, buf, display::kText, 0);
    std::snprintf(buf, sizeof(buf), "Blocchi %d   REC %u", g_blocksMined,
                  static_cast<unsigned>(best));
    display::textCentered(y + 40, buf, display::kTextDim, 0);
    display::textCentered(y + 54, "ENTER nuova partita", display::kNeoYellow, 0);
    display::textCentered(y + 64, "` esci", display::kTextDim, 0);
    display::push();
}

}  // namespace

void runMinecraft() {
    navigation::waitRelease();
    drawIntro();

    for (;;) {
        navigation::update();
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            return;
        }
        if (ev.type == navigation::Event::Select) {
            break;
        }
        delay(20);
    }
    navigation::waitRelease();

    uint32_t seed = esp_random();
    startRun(seed);
    uint32_t best = scores::best("minecraft");
    uint32_t lastFrame = millis();
    uint32_t renderedAt = 0;
    uint32_t elapsedAtWin = 0;

    while (true) {
        navigation::update();
        const navigation::EventData ev = navigation::poll();
        const uint32_t now = millis();

        if (ev.type == navigation::Event::Back) {
            scores::submit("minecraft", static_cast<uint32_t>(g_blocksMined));
            break;
        }

        if (g_won) {
            if (ev.type == navigation::Event::Select) {
                startRun(esp_random());
                lastFrame = now;
                renderedAt = 0;
                continue;
            }
            if (now - renderedAt > 60) {
                renderedAt = now;
                drawWin(best);
            }
            delay(10);
            continue;
        }

        if (ev.type == navigation::Event::Up) {
            --g_curY;
        } else if (ev.type == navigation::Event::Down) {
            ++g_curY;
        } else if (ev.type == navigation::Event::Left) {
            --g_curX;
        } else if (ev.type == navigation::Event::Right) {
            ++g_curX;
        } else if (ev.type == navigation::Event::Select) {
            mineAt();
        } else if (ev.type == navigation::Event::Text) {
            if (ev.ch == 'b' || ev.ch == 'B') {
                placeAt();
            } else if (ev.ch >= '1' && ev.ch <= '4') {
                g_selected = ev.ch - '1';
            }
        }

        clampCursor();
        const float dt = std::min(0.05f, static_cast<float>(now - lastFrame) / 1000.0f);
        lastFrame = now;
        updatePlayer(dt);

        if (g_diamonds >= 3 && touchingPortal()) {
            g_won = true;
            g_finishMs = now - g_startedAt;
            elapsedAtWin = g_finishMs;
            scores::submit("minecraft", static_cast<uint32_t>(g_blocksMined));
            best = std::max(best, static_cast<uint32_t>(g_blocksMined));
            beep(1200.0f, 80);
        } else if (g_diamonds < 3 && touchingPortal()) {
            g_hintUntil = now + 1500;
        }

        if (now - renderedAt >= 33) {
            renderedAt = now;
            const uint32_t elapsed = g_won ? elapsedAtWin / 1000 : (now - g_startedAt) / 1000;
            drawWorld(now, elapsed);
            display::push();
        }
        delay(4);
    }
}

}  // namespace games

#endif  // ENABLE_MODULE_GAMES
