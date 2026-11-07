#include "Doom.h"

#include <Arduino.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Display.h"
#include "GamesMenu.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "Scores.h"

#if ENABLE_MODULE_GAMES

namespace games {
namespace {

constexpr int kCols = 120;
constexpr int kColW = 2;
constexpr int kViewTop = 14;
constexpr int kViewBottom = 116;
constexpr int kViewH = kViewBottom - kViewTop;
constexpr int kHorizon = kViewTop + kViewH / 2;

const char* const kMaps[3][16] = {
    {
        "################",
        "#..............#",
        "#..##......##..#",
        "#..##......##..#",
        "#..............#",
        "#....##..##....#",
        "#....#....#....#",
        "#....#....#....#",
        "#....##..##....#",
        "#..............#",
        "#..##......##..#",
        "#..##......##..#",
        "#..............#",
        "#.....####.....#",
        "#.............X#",
        "################",
    },
    {
        "################",
        "#..............#",
        "#...##....##...#",
        "#...##....##...#",
        "#..............#",
        "#......##......#",
        "#......##......#",
        "#..............#",
        "#..##..##..##..#",
        "#..##..##..##..#",
        "#..............#",
        "#.....#..#.....#",
        "#.....#..#.....#",
        "#..............#",
        "#X.............#",
        "################",
    },
    {
        "################",
        "#.......X......#",
        "#.####....####.#",
        "#.####....####.#",
        "#..............#",
        "#...#######....#",
        "#...#.....#....#",
        "#...#.....#....#",
        "#...#.....#....#",
        "#..............#",
        "#.####....####.#",
        "#.####....####.#",
        "#..............#",
        "#....##..##....#",
        "#..............#",
        "################",
    },
};

const float kStartX[3] = {1.5f, 14.5f, 8.5f};
const float kStartY[3] = {1.5f, 1.5f, 14.5f};
const float kStartAngle[3] = {0.0f, 3.14159f, -1.5708f};
const int kExitX[3] = {14, 1, 8};
const int kExitY[3] = {14, 14, 1};
const float kEnemyPos[3][3][2] = {
    {{7.5f, 6.5f}, {8.5f, 3.5f}, {13.5f, 8.5f}},
    {{6.5f, 5.5f}, {10.5f, 8.5f}, {4.5f, 11.5f}},
    {{7.5f, 6.5f}, {7.5f, 2.5f}, {8.5f, 11.5f}},
};

struct Enemy {
    float x = 0.0f;
    float y = 0.0f;
    int hp = 2;
    uint32_t nextAttack = 0;
    uint32_t hitUntil = 0;
    bool alive = true;
};

char g_map[16][17];
float g_px = 1.5f;
float g_py = 1.5f;
float g_angle = 0.0f;
float g_dirX = 1.0f;
float g_dirY = 0.0f;
float g_planeX = 0.0f;
float g_planeY = 0.66f;
float g_zBuffer[kCols] = {0};
Enemy g_enemies[3];
int g_level = 0;

int g_health = 100;
int g_score = 0;
uint32_t g_best = 0;
bool g_dead = false;
bool g_victory = false;
bool g_transition = false;
bool g_submitted = false;
uint32_t g_transitionUntil = 0;
uint32_t g_flashUntil = 0;
uint32_t g_hitUntil = 0;
uint32_t g_hitMarkerUntil = 0;
float g_walkPhase = 0.0f;

bool isWallChar(char c) {
    return c == '#' || c == 'X' || (c >= '1' && c <= '4');
}

char tileAt(int x, int y) {
    if (x < 0 || x >= 16 || y < 0 || y >= 16) {
        return '#';
    }
    return g_map[y][x];
}

bool canStand(float x, float y) {
    constexpr float r = 0.22f;
    return !isWallChar(tileAt(static_cast<int>(x - r), static_cast<int>(y - r))) &&
           !isWallChar(tileAt(static_cast<int>(x + r), static_cast<int>(y - r))) &&
           !isWallChar(tileAt(static_cast<int>(x - r), static_cast<int>(y + r))) &&
           !isWallChar(tileAt(static_cast<int>(x + r), static_cast<int>(y + r)));
}

void loadLevel(int index) {
    g_level = index;
    for (int y = 0; y < 16; ++y) {
        std::strncpy(g_map[y], kMaps[index][y], 16);
        g_map[y][16] = '\0';
    }
    g_px = kStartX[index];
    g_py = kStartY[index];
    g_angle = kStartAngle[index];
    g_dirX = cosf(g_angle);
    g_dirY = sinf(g_angle);
    g_planeX = -g_dirY * 0.66f;
    g_planeY = g_dirX * 0.66f;
    for (int i = 0; i < 3; ++i) {
        g_enemies[i].x = kEnemyPos[index][i][0];
        g_enemies[i].y = kEnemyPos[index][i][1];
        g_enemies[i].hp = 2;
        g_enemies[i].nextAttack = 0;
        g_enemies[i].hitUntil = 0;
        g_enemies[i].alive = true;
    }
    memset(g_zBuffer, 0, sizeof(g_zBuffer));
}

float wrapAngle(float angle) {
    while (angle > 3.14159f) {
        angle -= 6.28318f;
    }
    while (angle < -3.14159f) {
        angle += 6.28318f;
    }
    return angle;
}

uint16_t shadeColor(uint16_t color, float factor) {
    factor = std::max(0.10f, std::min(1.0f, factor));
    uint16_t r = (color >> 11) & 0x1F;
    uint16_t g = (color >> 5) & 0x3F;
    uint16_t b = color & 0x1F;
    r = static_cast<uint16_t>(r * factor);
    g = static_cast<uint16_t>(g * factor);
    b = static_cast<uint16_t>(b * factor);
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

void drawBackground() {
    static const uint16_t kSky[8] = {0x0006, 0x0027, 0x0841, 0x1082,
                                     0x18C3, 0x2104, 0x2904, 0x2904};
    const int skyH = kHorizon - kViewTop;
    for (int i = 0; i < 8; ++i) {
        const int y0 = kViewTop + i * skyH / 8;
        const int y1 = kViewTop + (i + 1) * skyH / 8;
        display::g().fillRect(0, y0, 240, std::max(1, y1 - y0), kSky[i]);
    }
    // Pavimento in prospettiva: bande sempre piu' rade verso l'orizzonte.
    for (int y = kHorizon; y < kViewBottom; ++y) {
        const float depth = static_cast<float>(kViewBottom - y);
        const int band = static_cast<int>(depth * 0.22f) & 1;
        const float fade =
            0.25f + 0.75f * static_cast<float>(y - kHorizon) /
                        std::max(1, kViewBottom - kHorizon);
        const uint16_t base = band ? 0x2104 : 0x18E3;
        uint16_t c = shadeColor(base, fade);
        if (band) {
            c = shadeColor(c, 0.82f);
        }
        display::g().fillRect(0, y, 240, 1, c);
    }
}

void renderWalls() {
    for (int col = 0; col < kCols; ++col) {
        const float cameraX = 2.0f * col / kCols - 1.0f;
        const float rayX = g_dirX + g_planeX * cameraX;
        const float rayY = g_dirY + g_planeY * cameraX;

        int mapX = static_cast<int>(g_px);
        int mapY = static_cast<int>(g_py);

        const float deltaX = (rayX == 0.0f) ? 1e30f : fabsf(1.0f / rayX);
        const float deltaY = (rayY == 0.0f) ? 1e30f : fabsf(1.0f / rayY);

        const int stepX = (rayX < 0.0f) ? -1 : 1;
        const int stepY = (rayY < 0.0f) ? -1 : 1;

        float sideDistX = (rayX < 0.0f) ? (g_px - mapX) * deltaX
                                        : (mapX + 1.0f - g_px) * deltaX;
        float sideDistY = (rayY < 0.0f) ? (g_py - mapY) * deltaY
                                        : (mapY + 1.0f - g_py) * deltaY;

        int side = 0;
        char tile = '#';
        bool hit = false;
        for (int i = 0; i < 48; ++i) {
            if (sideDistX < sideDistY) {
                sideDistX += deltaX;
                mapX += stepX;
                side = 0;
            } else {
                sideDistY += deltaY;
                mapY += stepY;
                side = 1;
            }
            tile = tileAt(mapX, mapY);
            if (isWallChar(tile)) {
                hit = true;
                break;
            }
        }
        if (!hit) {
            g_zBuffer[col] = 1e30f;
            continue;
        }

        const float perp = (side == 0)
                               ? (mapX - g_px + (1 - stepX) / 2.0f) / rayX
                               : (mapY - g_py + (1 - stepY) / 2.0f) / rayY;
        const float safePerp = std::max(perp, 0.05f);
        g_zBuffer[col] = safePerp;

        const int lineHeight = static_cast<int>(kViewH / safePerp);
        int start = kHorizon - lineHeight / 2;
        int end = kHorizon + lineHeight / 2;
        start = std::max(start, kViewTop);
        end = std::min(end, kViewBottom);
        if (end <= start) {
            continue;
        }

        float factor = 1.0f / (1.0f + safePerp * 0.14f);
        if (side == 1) {
            factor *= 0.72f;
        }
        // Variazione cromatica per tessera: rompe la monotonia dei muri.
        const float tint = 0.90f + 0.10f * static_cast<float>((mapX * 7 + mapY * 13) & 3) / 3.0f;
        factor *= tint;

        float wallX = (side == 0) ? (g_py + safePerp * rayY)
                                  : (g_px + safePerp * rayX);
        wallX -= floorf(wallX);

        uint16_t base = 0x6B4D;  // mattone
        if (tile == 'X') {
            base = display::kAccent;
        } else if (tile == '1') {
            base = 0x2945;
        } else if (tile == '2') {
            base = 0x4A49;
        } else if (tile == '3') {
            base = 0x3186;
        } else if (tile == '4') {
            base = 0x7C00;
        }
        const uint16_t color = shadeColor(base, factor);
        const uint16_t mortar = shadeColor(base, factor * 0.45f);
        const uint16_t highlight = shadeColor(base, factor * 1.25f);
        const int x = col * kColW;
        const int heightPx = end - start;

        display::g().fillRect(x, start, kColW, heightPx, color);

        // Giunti orizzontali (corsi di mattoni).
        const int bands = std::max(2, std::min(6, heightPx / 12));
        const int bandH = std::max(1, heightPx / bands);
        for (int b = 1; b < bands; ++b) {
            const int y = start + b * bandH;
            if (y < end) {
                display::g().fillRect(x, y, kColW, 1, mortar);
            }
        }
        // Giunti verticali sfalsati (opera muraria a corsi alterni).
        for (int b = 0; b < bands; ++b) {
            const int y0 = start + b * bandH;
            const int y1 = (b + 1 >= bands) ? end : start + (b + 1) * bandH;
            const float shift = ((b + mapX + mapY) & 1) ? 0.5f : 0.0f;
            const float uu = wallX * 2.0f + shift;
            const float f = uu - floorf(uu);
            if (f < 0.09f || f > 0.91f) {
                display::g().fillRect(x, y0, kColW, std::max(1, y1 - y0), mortar);
            }
        }
        if (heightPx > 3) {
            display::g().fillRect(x, start, kColW, 1, highlight);
        }
        if (tile == 'X') {
            const bool glow = ((col + static_cast<int>(millis() / 120)) & 3) == 0;
            if (glow) {
                display::g().fillRect(x, start, kColW, heightPx,
                                      shadeColor(display::kAccent, 0.85f));
            }
        }
    }
}

void drawEnemies(uint32_t now) {
    int order[3] = {0, 1, 2};
    float distances[3];
    for (int i = 0; i < 3; ++i) {
        const float dx = g_enemies[i].x - g_px;
        const float dy = g_enemies[i].y - g_py;
        distances[i] = g_enemies[i].alive ? sqrtf(dx * dx + dy * dy) : 1e30f;
    }
    std::sort(order, order + 3,
              [&](int a, int b) { return distances[a] > distances[b]; });

    const float invDet =
        1.0f / (g_planeX * g_dirY - g_dirX * g_planeY);

    for (int k = 0; k < 3; ++k) {
        const int idx = order[k];
        const Enemy& enemy = g_enemies[idx];
        if (!enemy.alive) {
            continue;
        }
        const float dx = enemy.x - g_px;
        const float dy = enemy.y - g_py;
        const float transformX = invDet * (g_dirY * dx - g_dirX * dy);
        const float transformY = invDet * (-g_planeY * dx + g_planeX * dy);
        if (transformY <= 0.20f) {
            continue;
        }

        const int screenX =
            static_cast<int>((kCols / 2.0f) * (1.0f + transformX / transformY));
        int height = static_cast<int>(kViewH / transformY);
        height = std::min(height, 260);
        if (height < 3) {
            continue;
        }
        const int halfCols = std::max(1, height / 4);
        const int startCol = std::max(0, screenX - halfCols);
        const int endCol = std::min(kCols - 1, screenX + halfCols);

        // Passo di camminata e lampo bianco quando il nemico viene colpito.
        const int bob = static_cast<int>(std::sin(now * 0.008f + idx * 2.1f) *
                                         static_cast<float>(height) * 0.02f);
        const int top = kHorizon - height / 2 + height / 8 + bob;
        const int bottom = top + height;
        const int headEnd = top + height * 32 / 100;
        const int bodyEnd = top + height * 74 / 100;
        const bool hurt = now < enemy.hitUntil;

        for (int col = startCol; col <= endCol; ++col) {
            if (transformY >= g_zBuffer[col]) {
                continue;
            }
            const int x = col * kColW;
            const float rx =
                static_cast<float>(col - screenX) / static_cast<float>(halfCols);
            const float arx = std::fabs(rx);
            const bool eye = (arx > 0.12f && arx < 0.45f);

            const uint16_t skin = hurt ? 0xFFFF : 0xD9A0;
            const uint16_t torso = hurt ? 0xF800 : 0x9000;
            const uint16_t legs = hurt ? 0xA000 : 0x5800;
            const uint16_t arm = hurt ? 0xE000 : 0x6800;

            const int headStart = std::max(top, kViewTop);
            const int headStop = std::min(headEnd, kViewBottom);
            if (headStart < headStop) {
                display::g().fillRect(x, headStart, kColW, headStop - headStart,
                                      skin);
                if (eye) {
                    const int eyeY = std::max(top + height / 10, kViewTop);
                    if (eyeY < headStop) {
                        display::g().fillRect(x, eyeY, kColW, 2, display::kInk);
                    }
                }
                if (arx < 0.20f) {
                    const int mouthY = std::max(top + height / 4, kViewTop);
                    if (mouthY < headStop) {
                        display::g().fillRect(x, mouthY, kColW, 1,
                                              display::kDanger);
                    }
                }
            }

            const int bodyStart = std::max(headEnd, kViewTop);
            const int bodyStop = std::min(bodyEnd, kViewBottom);
            if (bodyStart < bodyStop) {
                const uint16_t c = (arx > 0.72f) ? arm : torso;
                display::g().fillRect(x, bodyStart, kColW,
                                      bodyStop - bodyStart, c);
            }

            const int legsStart = std::max(bodyEnd, kViewTop);
            const int legsEnd = std::min(bottom, kViewBottom);
            if (legsStart < legsEnd) {
                const bool gap = (arx < 0.14f) || (arx > 0.86f);
                if (!gap) {
                    display::g().fillRect(x, legsStart, kColW,
                                          legsEnd - legsStart, legs);
                }
            }
        }
    }
}

void drawWeapon(uint32_t now) {
    const int bobX = static_cast<int>(std::sin(g_walkPhase) * 2.0f);
    const int bobY =
        static_cast<int>(std::fabs(std::cos(g_walkPhase)) * 2.0f);
    const int cx = 120 + bobX;
    const int muzzleY = kViewBottom - 22 - bobY;
    if (now < g_flashUntil) {
        display::g().fillCircle(cx, muzzleY, 6, display::kWarning);
        display::g().fillCircle(cx, muzzleY, 3, display::kWhite);
    }
    display::g().fillRect(cx - 4, kViewBottom - 16 + bobY / 2, 8, 16 - bobY / 2,
                          0x4208);
    display::g().fillRect(cx - 13, kViewBottom - 5 + bobY / 2, 26, 5, 0x2104);
    display::g().fillRect(cx - 2, muzzleY, 4, 9, 0x8410);

    // Mirino fisso al centro dello schermo.
    display::g().fillRect(120 - 5, kHorizon, 4, 1, display::kTextDim);
    display::g().fillRect(120 + 2, kHorizon, 4, 1, display::kTextDim);
    display::g().fillRect(120 - 1, kHorizon - 5, 1, 4, display::kTextDim);
    display::g().fillRect(120 - 1, kHorizon + 2, 1, 4, display::kTextDim);

    if (now < g_hitMarkerUntil) {
        display::g().drawLine(115, kHorizon - 4, 123, kHorizon + 4,
                              display::kDanger);
        display::g().drawLine(123, kHorizon - 4, 115, kHorizon + 4,
                              display::kDanger);
    }
}

void drawHud() {
    display::g().fillRect(0, 0, 240, kViewTop, display::kHeader);
    display::text(4, 3, "S" + std::to_string(g_level + 1) + "/3",
                  display::kAccent, 0);
    const uint16_t hpColor = (g_health > 60) ? display::kText
                              : (g_health > 30) ? display::kWarning
                                                 : display::kDanger;
    display::text(38, 3, "HP " + std::to_string(std::max(g_health, 0)),
                  hpColor, 0);
    display::text(82, 3, "PUNTI " + std::to_string(g_score), display::kText, 0);
    display::text(150, 3,
                  "REC " + std::to_string(std::max(g_best, static_cast<uint32_t>(g_score))),
                  display::kInfo, 0);

    display::text(4, 118, "; . muovi    , / gira", display::kTextDim, 0);
    display::text(4, 127, "a d strafe   SPC spara   ` esci", display::kTextDim,
                  0);
}

void drawOverlay(const char* title, const char* hint, uint16_t color) {
    const int x = 24;
    const int y = 36;
    const int w = 192;
    const int h = 62;
    display::g().fillRect(x, y, w, h, display::kBlack);
    display::g().drawRect(x, y, w, h, color);
    display::textCentered(y + 8, title, color, 2);
    display::textCentered(y + 26,
                          "PUNTI " + std::to_string(g_score) + "  REC " +
                              std::to_string(std::max(g_best,
                                                      static_cast<uint32_t>(g_score))),
                          display::kText, 0);
    display::textCentered(y + 42, hint, display::kTextDim, 0);
}

void drawScene(uint32_t now) {
    drawBackground();
    renderWalls();
    drawEnemies(now);
    drawWeapon(now);
    drawHud();
    if (now < g_hitUntil) {
        display::g().drawRect(0, kViewTop, 240, kViewH, display::kDanger);
        display::g().drawRect(1, kViewTop + 1, 238, kViewH - 2,
                              display::kDanger);
    }
    display::push();
}

void updateEnemies(float dt, uint32_t now) {
    for (auto& enemy : g_enemies) {
        if (!enemy.alive) {
            continue;
        }
        const float dx = g_px - enemy.x;
        const float dy = g_py - enemy.y;
        const float dist = sqrtf(dx * dx + dy * dy);
        if (dist > 0.75f && dist < 8.0f && dist > 0.001f) {
            const float step = 0.9f * dt / dist;
            const float nx = enemy.x + dx * step;
            if (canStand(nx, enemy.y)) {
                enemy.x = nx;
            }
            const float ny = enemy.y + dy * step;
            if (canStand(enemy.x, ny)) {
                enemy.y = ny;
            }
        }
        if (dist < 0.95f && now >= enemy.nextAttack) {
            g_health -= 10;
            g_hitUntil = now + 130;
            enemy.nextAttack = now + 900;
            M5Cardputer.Speaker.tone(95.0f, 110);
            if (g_health <= 0) {
                g_health = 0;
                g_dead = true;
                M5Cardputer.Speaker.tone(70.0f, 400);
                return;
            }
        }
    }
}

void shoot(uint32_t now) {
    g_flashUntil = now + 70;
    M5Cardputer.Speaker.tone(150.0f, 28);
    int best = -1;
    float bestDist = 9.0f;
    for (int i = 0; i < 3; ++i) {
        if (!g_enemies[i].alive) {
            continue;
        }
        const float dx = g_enemies[i].x - g_px;
        const float dy = g_enemies[i].y - g_py;
        const float dist = sqrtf(dx * dx + dy * dy);
        if (dist > bestDist) {
            continue;
        }
        const float diff = wrapAngle(atan2f(dy, dx) - g_angle);
        if (fabsf(diff) < 0.22f) {
            best = i;
            bestDist = dist;
        }
    }
    if (best >= 0) {
        g_hitMarkerUntil = now + 90;
        g_enemies[best].hitUntil = now + 110;
        if (--g_enemies[best].hp <= 0) {
            g_enemies[best].alive = false;
            g_score += 100;
            M5Cardputer.Speaker.tone(900.0f, 70);
        } else {
            M5Cardputer.Speaker.tone(520.0f, 40);
        }
    }
}

}  // namespace

void runDoomLite() {
    g_health = 100;
    g_score = 0;
    g_best = scores::best("doom");
    g_dead = false;
    g_victory = false;
    g_transition = false;
    g_submitted = false;
    g_walkPhase = 0.0f;
    loadLevel(0);

    navigation::waitRelease();

    uint32_t lastFrame = millis();
    uint32_t renderedAt = 0;
    bool spaceHeld = false;

    while (true) {
        navigation::update();
        const navigation::EventData ev = navigation::poll();
        const uint32_t now = millis();

        if (ev.type == navigation::Event::Back) {
            if (g_score > 0 && !g_submitted) {
                scores::submit("doom", static_cast<uint32_t>(g_score));
            }
            break;
        }

        if (g_dead || g_victory) {
            if (!g_submitted) {
                scores::submit("doom", static_cast<uint32_t>(g_score));
                g_best = std::max(g_best, static_cast<uint32_t>(g_score));
                g_submitted = true;
            }
            if (ev.type == navigation::Event::Select) {
                g_health = 100;
                g_score = 0;
                g_dead = false;
                g_victory = false;
                g_transition = false;
                g_submitted = false;
                g_walkPhase = 0.0f;
                loadLevel(0);
                lastFrame = now;
            }
            if (now - renderedAt > 60) {
                drawScene(now);
                drawOverlay(g_victory ? "VITTORIA!" : "SEI MORTO",
                            "INVIO = ricomincia   ` = esci",
                            g_victory ? display::kAccent : display::kDanger);
                display::push();
                renderedAt = now;
            }
            delay(10);
            continue;
        }

        if (g_transition) {
            if (now >= g_transitionUntil) {
                g_transition = false;
                if (g_level < 2) {
                    loadLevel(g_level + 1);
                    lastFrame = now;
                } else {
                    g_victory = true;
                }
            }
            if (now - renderedAt > 60) {
                drawScene(now);
                drawOverlay("SETTORE COMPLETATO", "caricamento...",
                            display::kAccent);
                display::push();
                renderedAt = now;
            }
            delay(10);
            continue;
        }

        float dt = (now - lastFrame) / 1000.0f;
        lastFrame = now;
        dt = std::min(dt, 0.034f);

        float forward = 0.0f;
        float strafe = 0.0f;
        float turn = 0.0f;
        if (navigation::pressed(';') || navigation::pressed('w')) {
            forward += 1.0f;
        }
        if (navigation::pressed('.') || navigation::pressed('s')) {
            forward -= 1.0f;
        }
        if (navigation::pressed('a')) {
            strafe -= 1.0f;
        }
        if (navigation::pressed('d')) {
            strafe += 1.0f;
        }
        if (navigation::pressed(',') || navigation::pressed('q')) {
            turn -= 1.0f;
        }
        if (navigation::pressed('/') || navigation::pressed('e')) {
            turn += 1.0f;
        }

        if (turn != 0.0f) {
            g_angle = wrapAngle(g_angle + turn * 2.7f * dt);
            g_dirX = cosf(g_angle);
            g_dirY = sinf(g_angle);
            g_planeX = -g_dirY * 0.66f;
            g_planeY = g_dirX * 0.66f;
        }

        const float speed = 2.1f;
        const float strafeSpeed = 1.7f;
        const float stepX =
            (g_dirX * forward + (-g_dirY) * strafe * (strafeSpeed / speed)) *
            speed * dt;
        const float stepY =
            (g_dirY * forward + g_dirX * strafe * (strafeSpeed / speed)) *
            speed * dt;
        if (canStand(g_px + stepX, g_py)) {
            g_px += stepX;
        }
        if (canStand(g_px, g_py + stepY)) {
            g_py += stepY;
        }
        g_walkPhase += (std::fabs(forward) + std::fabs(strafe) * 0.7f) * dt * 9.0f;

        updateEnemies(dt, now);

        const bool spaceNow = navigation::pressed(' ');
        if (spaceNow && !spaceHeld) {
            shoot(now);
        }
        spaceHeld = spaceNow;

        if (fabsf(g_px - (kExitX[g_level] + 0.5f)) < 1.05f &&
            fabsf(g_py - (kExitY[g_level] + 0.5f)) < 1.05f) {
            g_transition = true;
            g_transitionUntil = now + 1100;
            g_flashUntil = 0;
            g_score += 500;
            M5Cardputer.Speaker.tone(1200.0f, 90);
        }

        if (now - renderedAt > 33) {
            drawScene(now);
            renderedAt = now;
        }

        delay(6);
    }
}

}  // namespace games

#endif  // ENABLE_MODULE_GAMES
