#include "Doom.h"

#include <Arduino.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Display.h"
#include "GamesMenu.h"
#include "ModuleFlags.h"
#include "Navigation.h"

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
bool g_dead = false;
bool g_victory = false;
bool g_transition = false;
uint32_t g_transitionUntil = 0;
uint32_t g_flashUntil = 0;
uint32_t g_hitUntil = 0;

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

int aliveEnemies() {
    int count = 0;
    for (const auto& enemy : g_enemies) {
        count += enemy.alive ? 1 : 0;
    }
    return count;
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
    static const uint16_t kSky[6] = {0x0008, 0x0841, 0x1082,
                                     0x18C3, 0x2104, 0x28E4};
    static const uint16_t kFloor[6] = {0x2945, 0x2104, 0x18C3,
                                       0x1082, 0x0841, 0x0000};
    const int skyH = kHorizon - kViewTop;
    const int floorH = kViewBottom - kHorizon;
    for (int i = 0; i < 6; ++i) {
        const int y0 = kViewTop + i * skyH / 6;
        const int y1 = kViewTop + (i + 1) * skyH / 6;
        display::g().fillRect(0, y0, 240, y1 - y0 + 1, kSky[i]);
        const int f0 = kHorizon + i * floorH / 6;
        const int f1 = kHorizon + (i + 1) * floorH / 6;
        display::g().fillRect(0, f0, 240, f1 - f0 + 1, kFloor[i]);
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

        float factor = 1.0f / (1.0f + safePerp * 0.12f);
        if (side == 1) {
            factor *= 0.76f;
        }
        uint16_t color = (tile == 'X') ? display::kAccent : 0x6B4D;
        color = shadeColor(color, factor);
        if (tile == 'X') {
            color = shadeColor(display::kAccent, 0.55f + 0.45f * factor);
        }

        display::g().fillRect(col * kColW, start, kColW, end - start, color);
        if (end - start > 14) {
            display::g().fillRect(col * kColW, start + (end - start) / 3, kColW,
                                  1, shadeColor(color, 0.75f));
        }
    }
}

void drawEnemies() {
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
        const Enemy& enemy = g_enemies[order[k]];
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
        int startCol = std::max(0, screenX - halfCols);
        int endCol = std::min(kCols - 1, screenX + halfCols);

        const int top = kHorizon - height / 2 + height / 8;
        const int bottom = top + height;
        const int headEnd = top + height * 35 / 100;
        const int bodyEnd = top + height * 78 / 100;

        for (int col = startCol; col <= endCol; ++col) {
            if (transformY >= g_zBuffer[col]) {
                continue;
            }
            const int x = col * kColW;
            const int headStart = std::max(top, kViewTop);
            const int legsEnd = std::min(bottom, kViewBottom);
            if (headStart < std::min(headEnd, kViewBottom)) {
                display::g().fillRect(x, headStart, kColW,
                                      std::min(headEnd, kViewBottom) - headStart,
                                      0xD9A0);
            }
            const int bodyStart = std::max(headEnd, kViewTop);
            if (bodyStart < std::min(bodyEnd, kViewBottom)) {
                display::g().fillRect(x, bodyStart, kColW,
                                      std::min(bodyEnd, kViewBottom) - bodyStart,
                                      0x9000);
            }
            const int legsStart = std::max(bodyEnd, kViewTop);
            if (legsStart < legsEnd) {
                display::g().fillRect(x, legsStart, kColW, legsEnd - legsStart,
                                      0x5800);
            }
            const bool eye = (col == screenX - 1 || col == screenX + 1);
            if (eye && headEnd > top + 3) {
                display::g().fillRect(x, top + height / 10, kColW, 2,
                                      display::kWhite);
            }
        }
    }
}

void drawWeapon(uint32_t now) {
    const int cx = 120;
    if (now < g_flashUntil) {
        display::g().fillCircle(cx, kViewBottom - 22, 5, display::kWarning);
        display::g().fillCircle(cx, kViewBottom - 22, 2, display::kWhite);
    }
    display::g().fillRect(cx - 4, kViewBottom - 16, 8, 16, 0x4208);
    display::g().fillRect(cx - 13, kViewBottom - 5, 26, 5, 0x2104);
    display::g().fillRect(cx - 2, kViewBottom - 22, 4, 9, 0x8410);

    // Mirino.
    display::g().fillRect(cx - 5, kHorizon, 4, 1, display::kTextDim);
    display::g().fillRect(cx + 2, kHorizon, 4, 1, display::kTextDim);
    display::g().fillRect(cx - 1, kHorizon - 5, 1, 4, display::kTextDim);
    display::g().fillRect(cx - 1, kHorizon + 2, 1, 4, display::kTextDim);
}

void drawHud() {
    display::g().fillRect(0, 0, 240, kViewTop, display::kHeader);
    display::text(4, 3, "SETTORE " + std::to_string(g_level + 1) + "/3",
                  display::kAccent, 0);
    const uint16_t hpColor = (g_health > 60) ? display::kText
                              : (g_health > 30) ? display::kWarning
                                                 : display::kDanger;
    display::text(100, 3, "HP " + std::to_string(std::max(g_health, 0)),
                  hpColor, 0);
    display::text(170, 3, "NEMICI " + std::to_string(aliveEnemies()),
                  display::kText, 0);

    display::text(4, 118, "; . muovi    , / gira", display::kTextDim, 0);
    display::text(4, 127, "a d strafe   SPC spara   ` esci", display::kTextDim,
                  0);
}

void drawOverlay(const char* title, const char* hint, uint16_t color) {
    const int x = 24;
    const int y = 40;
    const int w = 192;
    const int h = 54;
    display::g().fillRect(x, y, w, h, display::kBlack);
    display::g().drawRect(x, y, w, h, color);
    display::textCentered(y + 10, title, color, 2);
    display::textCentered(y + 32, hint, display::kTextDim, 0);
}

void drawScene(uint32_t now) {
    drawBackground();
    renderWalls();
    drawEnemies();
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
            if (g_health <= 0) {
                g_health = 0;
                g_dead = true;
                return;
            }
        }
    }
}

void shoot(uint32_t now) {
    g_flashUntil = now + 70;
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
        if (--g_enemies[best].hp <= 0) {
            g_enemies[best].alive = false;
        }
    }
}

}  // namespace

void runDoomLite() {
    g_health = 100;
    g_dead = false;
    g_victory = false;
    g_transition = false;
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
            break;
        }

        if (g_dead || g_victory) {
            if (ev.type == navigation::Event::Select) {
                g_health = 100;
                g_dead = false;
                g_victory = false;
                g_transition = false;
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
