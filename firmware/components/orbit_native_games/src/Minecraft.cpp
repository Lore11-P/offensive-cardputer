// ============================================================================
//  Minecraft 3D - sandbox a blocchi in prima persona
//
//  Il mondo e' una heightmap 64x64: ogni colonna ha un'altezza e un tipo di
//  blocco. La scena si disegna con un raycast per colonna (voxel space) sopra
//  un framebuffer 16 bpp, quindi niente geometria 3D vera: solo span verticali
//  con shading per pendenza, distanza e pattern a scacchiera dei blocchi.
//
//  Comandi volutamente minimi: movimento, sguardo e piazzamento dei blocchi.
// ============================================================================

#include "Minecraft.h"

#include "orbit_compat.h"
#include "esp_random.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <new>

#include "Display.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "Ui.h"

#if ENABLE_MODULE_GAMES

namespace games {
namespace {

constexpr int kCols = 120;
constexpr int kColW = 2;
constexpr int kViewTop = 12;
constexpr int kViewBottom = 116;
constexpr int kViewH = kViewBottom - kViewTop;
constexpr int kHorizon = kViewTop + kViewH / 2;

constexpr float kFocal = 104.0f;
constexpr float kHalfFovTan = 0.66f;
constexpr float kMaxDist = 72.0f;
constexpr float kEyeHeight = 1.62f;
constexpr int kWorldW = 64;
constexpr int kWorldD = 64;
constexpr int kMaxHeight = 24;
constexpr float kReach = 6.0f;

enum : uint8_t {
    kGrass = 0,
    kDirt,
    kStone,
    kSand,
    kWood,
    kTypeCount,
};

const uint8_t kPalette[4] = {kGrass, kStone, kWood, kSand};

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Palette "natura": colori pieni, niente dithering.
const uint16_t kGrassColor = rgb(108, 172, 74);
const uint16_t kDirtColor = rgb(134, 96, 67);
const uint16_t kStoneColor = rgb(132, 134, 140);
const uint16_t kSandColor = rgb(219, 207, 163);
const uint16_t kWoodColor = rgb(167, 132, 80);

const uint16_t kSkyTop = rgb(42, 88, 148);
const uint16_t kSkyHorizon = rgb(178, 206, 228);
const uint16_t kFogColor = kSkyHorizon;

// Mondo su heap: 8 KB in .bss solo mentre la partita e' aperta.
uint8_t (*g_height)[kWorldW] = nullptr;
uint8_t (*g_block)[kWorldW] = nullptr;

struct Player {
    float x = 32.0f;
    float z = 32.0f;
    float yaw = -1.5708f;
    float pitch = -0.28f;
    float eyeZ = 0.0f;
};

Player g_player;
int g_selected = 0;
int g_targetX = -1;
int g_targetZ = -1;
bool g_targetValid = false;
uint32_t g_rng = 1;

uint32_t rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

uint16_t blockColor(uint8_t type) {
    switch (type) {
        case kDirt:
            return kDirtColor;
        case kStone:
            return kStoneColor;
        case kSand:
            return kSandColor;
        case kWood:
            return kWoodColor;
        case kGrass:
        default:
            return kGrassColor;
    }
}

const char* blockName(uint8_t type) {
    switch (type) {
        case kDirt:
            return "TERRA";
        case kStone:
            return "PIETRA";
        case kSand:
            return "SABBIA";
        case kWood:
            return "LEGNO";
        case kGrass:
        default:
            return "ERBA";
    }
}

// --- colore: scala, mix e conversione ---------------------------------------

uint16_t shade(uint16_t color, float factor) {
    int r = (color >> 11) & 0x1F;
    int g = (color >> 5) & 0x3F;
    int b = color & 0x1F;
    r = std::max(0, std::min(31, static_cast<int>(r * factor)));
    g = std::max(0, std::min(63, static_cast<int>(g * factor)));
    b = std::max(0, std::min(31, static_cast<int>(b * factor)));
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

uint16_t mix(uint16_t a, uint16_t b, float t) {
    t = std::max(0.0f, std::min(1.0f, t));
    const int ar = (a >> 11) & 0x1F;
    const int ag = (a >> 5) & 0x3F;
    const int ab = a & 0x1F;
    const int br = (b >> 11) & 0x1F;
    const int bg = (b >> 5) & 0x3F;
    const int bb = b & 0x1F;
    const int r = static_cast<int>(ar + (br - ar) * t);
    const int g = static_cast<int>(ag + (bg - ag) * t);
    const int bl = static_cast<int>(ab + (bb - ab) * t);
    return static_cast<uint16_t>((r << 11) | (g << 5) | bl);
}

// --- rumore per la generazione del terreno ----------------------------------

uint32_t hash2(int x, int z, uint32_t seed) {
    uint32_t h = seed + static_cast<uint32_t>(x) * 374761393u +
                 static_cast<uint32_t>(z) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

float smoothStep(float t) {
    return t * t * (3.0f - 2.0f * t);
}

float valueNoise(float x, float z, uint32_t seed) {
    const int x0 = static_cast<int>(floorf(x));
    const int z0 = static_cast<int>(floorf(z));
    const float fx = smoothStep(x - static_cast<float>(x0));
    const float fz = smoothStep(z - static_cast<float>(z0));
    const float v00 = static_cast<float>(hash2(x0, z0, seed) & 0xFFFF) / 65535.0f;
    const float v10 = static_cast<float>(hash2(x0 + 1, z0, seed) & 0xFFFF) / 65535.0f;
    const float v01 = static_cast<float>(hash2(x0, z0 + 1, seed) & 0xFFFF) / 65535.0f;
    const float v11 = static_cast<float>(hash2(x0 + 1, z0 + 1, seed) & 0xFFFF) / 65535.0f;
    const float a = v00 + (v10 - v00) * fx;
    const float b = v01 + (v11 - v01) * fx;
    return a + (b - a) * fz;
}

uint8_t typeFor(int x, int z, int h) {
    if (h <= 4) {
        return kSand;
    }
    if (h >= 14) {
        return kStone;
    }
    const uint32_t patch = hash2(x / 3, z / 3, 0xA53Cu) % 100u;
    return patch < 10u ? kDirt : kGrass;
}

void generateWorld(uint32_t seed) {
    g_rng = seed == 0 ? 0x9E3779B9u : seed;
    if (g_rng == 0) {
        g_rng = 1;
    }

    const float cx = kWorldW / 2.0f - 0.5f;
    const float cz = kWorldD / 2.0f - 0.5f;
    for (int z = 0; z < kWorldD; ++z) {
        for (int x = 0; x < kWorldW; ++x) {
            const float n1 = valueNoise(x * 0.055f, z * 0.055f, seed);
            const float n2 = valueNoise(x * 0.150f, z * 0.150f, seed ^ 0x9E37u);
            const float ridge =
                1.0f - fabsf(valueNoise(x * 0.028f, z * 0.028f, seed ^ 0x51EDu) * 2.0f - 1.0f);

            // Isola: collina centrale che digrada verso una spiaggia piatta.
            const float dx = (x - cx) / cx;
            const float dz = (z - cz) / cz;
            const float d = sqrtf(dx * dx + dz * dz);
            const float hill = std::max(0.0f, 1.0f - d);
            float height = 2.4f + hill * hill * 7.5f + n1 * 4.5f + n2 * 2.0f +
                           ridge * 2.0f;
            if (d > 0.74f) {
                height -= (d - 0.74f) * 26.0f;
            }

            int h = static_cast<int>(height);
            h = std::max(1, std::min(kMaxHeight, h));
            g_height[z][x] = static_cast<uint8_t>(h);
            g_block[z][x] = typeFor(x, z, h);
        }
    }

    // Massi sparsi: una colonna di pietra che sporge dall'erba.
    for (int i = 0; i < 48; ++i) {
        const int x = 3 + static_cast<int>(rnd() % (kWorldW - 6));
        const int z = 3 + static_cast<int>(rnd() % (kWorldD - 6));
        if (g_block[z][x] != kGrass || g_height[z][x] < 5) {
            continue;
        }
        g_height[z][x] = static_cast<uint8_t>(
            std::min(kMaxHeight, static_cast<int>(g_height[z][x]) + 1));
        g_block[z][x] = kStone;
    }
}

int heightCell(int x, int z) {
    if (x < 0 || x >= kWorldW || z < 0 || z >= kWorldD) {
        return 0;
    }
    return g_height[z][x];
}

// Altezza interpolata: serve solo al movimento fluido del giocatore.
float heightAt(float x, float z) {
    const int x0 = static_cast<int>(floorf(x));
    const int z0 = static_cast<int>(floorf(z));
    const float fx = x - static_cast<float>(x0);
    const float fz = z - static_cast<float>(z0);
    const float h00 = static_cast<float>(heightCell(x0, z0));
    const float h10 = static_cast<float>(heightCell(x0 + 1, z0));
    const float h01 = static_cast<float>(heightCell(x0, z0 + 1));
    const float h11 = static_cast<float>(heightCell(x0 + 1, z0 + 1));
    const float a = h00 + (h10 - h00) * fx;
    const float b = h01 + (h11 - h01) * fx;
    return a + (b - a) * fz;
}

// --- giocatore --------------------------------------------------------------

void startRun(uint32_t seed) {
    generateWorld(seed);
    g_player.x = kWorldW / 2.0f;
    g_player.z = kWorldD / 2.0f;
    g_player.yaw = -1.5708f;
    g_player.pitch = -0.28f;
    g_player.eyeZ = heightAt(g_player.x, g_player.z) + kEyeHeight;
    g_selected = 0;
    g_targetX = -1;
    g_targetZ = -1;
    g_targetValid = false;
}

void beep(float freq, uint32_t ms) {
    M5Cardputer.Speaker.tone(freq, ms);
}

void updatePlayer(float dt) {
    constexpr float kMoveSpeed = 4.6f;
    constexpr float kTurnSpeed = 2.3f;
    constexpr float kPitchSpeed = 1.5f;

    if (navigation::pressed(',')) {
        g_player.yaw -= kTurnSpeed * dt;
    }
    if (navigation::pressed('/')) {
        g_player.yaw += kTurnSpeed * dt;
    }
    if (navigation::pressed(';')) {
        g_player.pitch += kPitchSpeed * dt;
    }
    if (navigation::pressed('.')) {
        g_player.pitch -= kPitchSpeed * dt;
    }
    g_player.pitch = std::max(-0.58f, std::min(0.58f, g_player.pitch));

    const float dirX = cosf(g_player.yaw);
    const float dirZ = sinf(g_player.yaw);

    float forward = 0.0f;
    float strafe = 0.0f;
    if (navigation::pressed('w')) {
        forward += 1.0f;
    }
    if (navigation::pressed('s')) {
        forward -= 1.0f;
    }
    if (navigation::pressed('d')) {
        strafe += 1.0f;
    }
    if (navigation::pressed('a')) {
        strafe -= 1.0f;
    }

    if (forward != 0.0f || strafe != 0.0f) {
        const float len = sqrtf(forward * forward + strafe * strafe);
        forward /= len;
        strafe /= len;
        const float nx = g_player.x + (dirX * forward - dirZ * strafe) * kMoveSpeed * dt;
        const float nz = g_player.z + (dirZ * forward + dirX * strafe) * kMoveSpeed * dt;
        const float limit = 1.6f;
        if (nx > limit && nx < kWorldW - limit && nz > limit && nz < kWorldD - limit) {
            // Si sale al massimo di un blocco per volta: niente scalate.
            const float ground = heightAt(g_player.x, g_player.z);
            const float next = heightAt(nx, nz);
            if (next - ground <= 1.05f) {
                g_player.x = nx;
                g_player.z = nz;
            }
        }
    }

    const float targetEye = heightAt(g_player.x, g_player.z) + kEyeHeight;
    g_player.eyeZ += (targetEye - g_player.eyeZ) * std::min(1.0f, dt * 12.0f);
}

// --- puntamento e piazzamento ----------------------------------------------

void updateTarget() {
    g_targetValid = false;
    const float cp = cosf(g_player.pitch);
    const float dx = cosf(g_player.yaw) * cp;
    const float dz = sinf(g_player.yaw) * cp;
    const float dy = sinf(g_player.pitch);

    for (float t = 0.25f; t < kReach; t += 0.055f) {
        const float x = g_player.x + dx * t;
        const float z = g_player.z + dz * t;
        const float y = g_player.eyeZ + dy * t;
        if (y > heightAt(x, z)) {
            continue;
        }
        const int ix = static_cast<int>(floorf(x));
        const int iz = static_cast<int>(floorf(z));
        if (ix < 0 || ix >= kWorldW || iz < 0 || iz >= kWorldD) {
            return;
        }
        if (ix == static_cast<int>(floorf(g_player.x)) &&
            iz == static_cast<int>(floorf(g_player.z))) {
            return;  // niente blocchi sotto i piedi
        }
        g_targetX = ix;
        g_targetZ = iz;
        g_targetValid = true;
        return;
    }
}

void placeBlock() {
    if (!g_targetValid) {
        return;
    }
    const int h = g_height[g_targetZ][g_targetX];
    if (h >= kMaxHeight) {
        beep(180.0f, 30);
        return;
    }
    g_height[g_targetZ][g_targetX] = static_cast<uint8_t>(h + 1);
    g_block[g_targetZ][g_targetX] = kPalette[g_selected];
    beep(520.0f, 14);
}

// --- rendering --------------------------------------------------------------

void drawSky(int horizon) {
    auto& d = display::g();
    const int bottom = std::min(horizon, kViewBottom);
    const int top = kViewTop;
    if (bottom <= top) {
        return;
    }
    constexpr int kBands = 10;
    for (int i = 0; i < kBands; ++i) {
        const int y0 = top + i * (bottom - top) / kBands;
        const int y1 = top + (i + 1) * (bottom - top) / kBands;
        const float t = static_cast<float>(i) / (kBands - 1);
        const uint16_t c = mix(kSkyTop, kSkyHorizon, t * t);
        d.fillRect(0, y0, display::width(), std::max(1, y1 - y0), c);
    }
}

void drawGroundCover(int horizon) {
    auto& d = display::g();
    const int top = std::max(horizon, kViewTop);
    if (top >= kViewBottom) {
        return;
    }
    d.fillRect(0, top, display::width(), kViewBottom - top, kFogColor);
}

void drawSun(int horizon) {
    auto& d = display::g();
    constexpr float kSunAzimuth = 2.35f;
    float rel = g_player.yaw - kSunAzimuth;
    while (rel > 3.14159f) {
        rel -= 6.28318f;
    }
    while (rel < -3.14159f) {
        rel += 6.28318f;
    }
    const float halfFov = atanf(kHalfFovTan);
    if (fabsf(rel) > halfFov * 1.15f) {
        return;
    }
    const int sx = static_cast<int>(display::width() * 0.5f +
                                    (tanf(rel) / kHalfFovTan) * display::width() * 0.5f);
    const int sy = horizon - static_cast<int>(tanf(0.62f) * kFocal);
    if (sy < kViewTop - 10 || sy > horizon) {
        return;
    }
    d.fillCircle(sx, sy, 9, mix(kSkyHorizon, rgb(255, 236, 196), 0.45f));
    d.fillCircle(sx, sy, 6, rgb(255, 244, 208));
}

void drawTerrain(int horizon) {
    auto& d = display::g();
    const float dirX = cosf(g_player.yaw);
    const float dirZ = sinf(g_player.yaw);
    const float planeX = -dirZ * kHalfFovTan;
    const float planeZ = dirX * kHalfFovTan;

    // Luce del sole fissa, normalizzata a mano.
    constexpr float kSunX = -0.42f;
    constexpr float kSunY = 0.83f;
    constexpr float kSunZ = -0.36f;

    for (int col = 0; col < kCols; ++col) {
        const float cameraX = 2.0f * col / (kCols - 1) - 1.0f;
        const float rayX = dirX + planeX * cameraX;
        const float rayZ = dirZ + planeZ * cameraX;

        int ybuf = kViewBottom;
        float t = 0.30f;
        for (int step = 0; step < 120 && t < kMaxDist; ++step) {
            const int cx = static_cast<int>(floorf(g_player.x + rayX * t));
            const int cz = static_cast<int>(floorf(g_player.z + rayZ * t));
            if (cx < 0 || cx >= kWorldW || cz < 0 || cz >= kWorldD) {
                break;
            }

            const int h = g_height[cz][cx];
            const float yf = static_cast<float>(horizon) +
                             (g_player.eyeZ - static_cast<float>(h)) * kFocal / t;
            const int y = static_cast<int>(yf);

            if (y < ybuf) {
                const int y0 = std::max(y, kViewTop);
                const int y1 = std::min(ybuf, kViewBottom);
                if (y1 > y0) {
                    // Normale dal gradiente della heightmap.
                    const float nx = static_cast<float>(heightCell(cx - 1, cz) -
                                                        heightCell(cx + 1, cz));
                    const float nz = static_cast<float>(heightCell(cx, cz - 1) -
                                                        heightCell(cx, cz + 1));
                    const float invLen = 1.0f / sqrtf(nx * nx + 4.0f + nz * nz);
                    const float dot = (nx * kSunX + 2.0f * kSunY + nz * kSunZ) * invLen;
                    float light = 0.60f + 0.40f * std::max(0.0f, dot);
                    if (((cx + cz) & 1) != 0) {
                        light *= 0.93f;  // scacchiera dei blocchi
                    }
                    // Grana per cella: rompe le superfici troppo piatte.
                    light *= 0.95f +
                             static_cast<float>(hash2(cx, cz, 0x77u) % 9u) / 9.0f * 0.08f;
                    const float fog = 1.0f / (1.0f + t * 0.055f);
                    uint16_t color = shade(blockColor(g_block[cz][cx]), light);
                    color = mix(kFogColor, color, fog);

                    const int x = col * kColW;
                    d.fillRect(x, y0, kColW, y1 - y0, color);
                    if (y0 == y) {
                        d.fillRect(x, y0, kColW, 1,
                                   mix(color, rgb(255, 255, 255), 0.18f));
                    }
                }
                ybuf = y0;
                if (ybuf <= kViewTop) {
                    break;
                }
            }

            t += std::max(0.10f, t * 0.035f);
        }
    }
}

void drawCubeIcon(int x, int y, int size, uint8_t type) {
    auto& d = display::g();
    const int half = size / 2;
    const uint16_t top = blockColor(type);
    const uint16_t left = shade(top, 0.62f);
    const uint16_t right = shade(top, 0.82f);
    const int tx = x + half;
    d.fillTriangle(tx, y, x + size, y + size / 4, tx, y + half, top);
    d.fillTriangle(tx, y, x, y + size / 4, tx, y + half, top);
    d.fillTriangle(x, y + size / 4, tx, y + half, tx, y + size, left);
    d.fillTriangle(x, y + size / 4, tx, y + size, x, y + size * 3 / 4, left);
    d.fillTriangle(x + size, y + size / 4, tx, y + half, tx, y + size, right);
    d.fillTriangle(x + size, y + size / 4, tx, y + size, x + size, y + size * 3 / 4, right);
}

void drawCrosshair() {
    auto& d = display::g();
    const int cx = display::width() / 2;
    const int cy = kHorizon;
    const uint16_t color = g_targetValid ? display::kAccent : 0xEF5C;
    d.fillRect(cx - 5, cy, 4, 1, color);
    d.fillRect(cx + 2, cy, 4, 1, color);
    d.fillRect(cx, cy - 5, 1, 4, color);
    d.fillRect(cx, cy + 2, 1, 4, color);
    d.drawPixel(cx, cy, color);
}

void drawTargetMarker(int horizon) {
    if (!g_targetValid) {
        return;
    }
    auto& d = display::g();
    const float wx = static_cast<float>(g_targetX) + 0.5f;
    const float wz = static_cast<float>(g_targetZ) + 0.5f;
    const float wy = static_cast<float>(g_height[g_targetZ][g_targetX] + 1);
    const float dx = wx - g_player.x;
    const float dz = wz - g_player.z;
    const float dirX = cosf(g_player.yaw);
    const float dirZ = sinf(g_player.yaw);
    const float forward = dx * dirX + dz * dirZ;
    if (forward < 0.25f) {
        return;
    }
    const float side = dx * (-dirZ) + dz * dirX;
    const float cameraX = (side / forward) / kHalfFovTan;
    if (cameraX < -1.1f || cameraX > 1.1f) {
        return;
    }
    const int sx = static_cast<int>((cameraX * 0.5f + 0.5f) * display::width());
    const int sy = static_cast<int>(static_cast<float>(horizon) -
                                    (wy - g_player.eyeZ) * kFocal / forward);
    if (sx < 2 || sx > display::width() - 2 || sy < kViewTop || sy > kViewBottom - 2) {
        return;
    }
    // Parentesi sul blocco puntato.
    const uint16_t color = display::kAccent;
    d.fillRect(sx - 4, sy - 4, 3, 1, color);
    d.fillRect(sx - 4, sy - 4, 1, 3, color);
    d.fillRect(sx + 2, sy - 4, 3, 1, color);
    d.fillRect(sx + 4, sy - 4, 1, 3, color);
    d.fillRect(sx - 4, sy + 2, 3, 1, color);
    d.fillRect(sx - 4, sy + 4, 1, 3, color);
    d.fillRect(sx + 2, sy + 2, 3, 1, color);
    d.fillRect(sx + 4, sy + 4, 1, 3, color);
}

void drawHud() {
    auto& d = display::g();
    char buf[32];

    d.fillRect(0, 0, display::width(), kViewTop, display::kBlack);
    d.drawFastHLine(0, kViewTop - 1, display::width(), 0x2945);
    d.fillRect(3, 4, 3, 3, kGrassColor);
    display::text(9, 3, "MINECRAFT 3D", kGrassColor, 0);
    std::snprintf(buf, sizeof(buf), "X%02d Z%02d Y%02d",
                  static_cast<int>(g_player.x), static_cast<int>(g_player.z),
                  static_cast<int>(g_player.eyeZ));
    display::text(display::width() - 78, 3, buf, display::kTextDim, 0);

    d.fillRect(0, kViewBottom, display::width(), display::height() - kViewBottom,
               display::kBlack);
    display::text(9, kViewBottom + 2, "WASD muovi   ;.,/ guarda   ENTER posa",
                  display::kTextDim, 0);
    std::snprintf(buf, sizeof(buf), "1-4 blocco: %s", blockName(kPalette[g_selected]));
    display::text(4, kViewBottom + 11, buf, display::kText, 0);
    display::text(display::width() - 40, kViewBottom + 11, "` esci", display::kTextDim, 0);
}

void drawScene() {
    const int horizon = kHorizon + static_cast<int>(tanf(g_player.pitch) * kFocal);
    drawSky(horizon);
    drawGroundCover(horizon);
    drawSun(horizon);
    drawTerrain(horizon);
    drawTargetMarker(horizon);
    drawCrosshair();
    drawHud();
}

// --- schermate --------------------------------------------------------------

void drawIntro() {
    auto& d = display::g();
    display::clear(display::kBlack);

    constexpr int kBands = 12;
    for (int i = 0; i < kBands; ++i) {
        const int y0 = i * display::height() / kBands;
        const int y1 = (i + 1) * display::height() / kBands;
        const float t = static_cast<float>(i) / (kBands - 1);
        d.fillRect(0, y0, display::width(), std::max(1, y1 - y0),
                   mix(rgb(10, 16, 24), rgb(28, 54, 52), t));
    }

    display::textCentered(16, "MINECRAFT 3D", kGrassColor, 2);
    display::textCentered(34, "sandbox a blocchi", display::kTextDim, 0);

    drawCubeIcon(30, 50, 26, kGrass);
    drawCubeIcon(70, 50, 26, kStone);
    drawCubeIcon(110, 50, 26, kWood);
    drawCubeIcon(150, 50, 26, kSand);
    drawCubeIcon(190, 50, 26, kDirt);

    static const char* const kLines[] = {
        "W A S D   muovi",
        "; . , /   guarda",
        "ENTER / B   posa il blocco",
        "1 - 4   scegli il blocco",
        "`   esci",
    };
    int y = 82;
    for (const char* line : kLines) {
        display::textCentered(y, line, display::kText, 0);
        y += 10;
    }
    display::textCentered(130, "ENTER per giocare", display::kAccent, 0);
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

    g_height = new (std::nothrow) uint8_t[kWorldD][kWorldW];
    g_block = new (std::nothrow) uint8_t[kWorldD][kWorldW];
    if (g_height == nullptr || g_block == nullptr) {
        delete[] g_height;
        delete[] g_block;
        g_height = nullptr;
        g_block = nullptr;
        ui::message("Minecraft", {"Memoria insufficiente per il mondo."},
                    "Giochi");
        return;
    }

    startRun(esp_random());
    uint32_t lastFrame = millis();
    uint32_t lastDraw = 0;

    for (;;) {
        navigation::update();
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            break;
        }
        if (ev.type == navigation::Event::Select) {
            placeBlock();
        } else if (ev.type == navigation::Event::Text) {
            if (ev.ch >= '1' && ev.ch <= '4') {
                g_selected = ev.ch - '1';
                beep(700.0f, 10);
            } else if (ev.ch == 'b' || ev.ch == 'B') {
                placeBlock();
            }
        }

        const uint32_t now = millis();
        const float dt = std::min(0.05f, static_cast<float>(now - lastFrame) / 1000.0f);
        lastFrame = now;

        updatePlayer(dt);
        updateTarget();

        if (now - lastDraw >= 40) {
            lastDraw = now;
            drawScene();
            display::push();
        }
        delay(2);
    }

    delete[] g_height;
    delete[] g_block;
    g_height = nullptr;
    g_block = nullptr;
}

}  // namespace games

#endif  // ENABLE_MODULE_GAMES
