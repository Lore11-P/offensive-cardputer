#include "Pacman.h"

#include <Arduino.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Display.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "Scores.h"

#if ENABLE_MODULE_GAMES

namespace games {
namespace {

constexpr int kCols = 19;
constexpr int kRows = 11;
constexpr int kTile = 10;
constexpr int kFieldX = (240 - kCols * kTile) / 2;  // 25
constexpr int kFieldY = 20;

const uint16_t kWall = 0x001F;
const uint16_t kWallEdge = 0x04BF;
const uint16_t kGhostBlue = 0x2D7F;

const char* const kMaze[kRows] = {
    "###################",
    "#........#........#",
    "#o##.###.#.###.##o#",
    "#.................#",
    "#.##.#.#####.#.##.#",
    "#....#...#...#....#",
    "#.##.###.#.###.##.#",
    "#.................#",
    "#o##.###.#.###.##o#",
    "#........#........#",
    "###################",
};

enum Dir { kNone = -1, kLeft = 0, kUp = 1, kRight = 2, kDown = 3 };

const int kDX[4] = {-1, 0, 1, 0};
const int kDY[4] = {0, -1, 0, 1};

struct Actor {
    int col = 0;
    int row = 0;
    int tc = 0;
    int tr = 0;
    int dir = kNone;
    int face = kRight;
    int want = kNone;
    float t = 0.0f;
};

bool g_walls[kRows][kCols];
bool g_pellets[kRows][kCols];
bool g_power[kRows][kCols];
int g_pelletsLeft = 0;
int g_level = 1;

Actor g_pac;
Actor g_ghosts[4];
const int kGhostHome[4][2] = {{9, 3}, {10, 3}, {6, 5}, {12, 5}};
const uint16_t kGhostColor[4] = {0xF800, 0xF81F, 0x07FF, 0xFD20};
bool g_ghostEaten[4] = {false, false, false, false};
uint32_t g_ghostEatenUntil[4] = {0, 0, 0, 0};
uint32_t g_frightenedUntil = 0;
int g_ghostCombo = 0;
int g_lives = 3;
int g_score = 0;
uint32_t g_best = 0;
bool g_gameOver = false;
uint32_t g_pauseUntil = 0;

bool walkable(int col, int row) {
    if (col < 0 || col >= kCols || row < 0 || row >= kRows) {
        return false;
    }
    return !g_walls[row][col];
}

void beep(float freq, uint32_t ms) {
    M5Cardputer.Speaker.tone(freq, ms);
}

void actorPos(const Actor& a, float& fx, float& fy) {
    fx = a.col + 0.5f;
    fy = a.row + 0.5f;
    if (a.dir >= 0) {
        fx += kDX[a.dir] * a.t;
        fy += kDY[a.dir] * a.t;
    }
}

void chooseGhostDir(int i) {
    Actor& g = g_ghosts[i];
    const bool frightened = (millis() < g_frightenedUntil) && !g_ghostEaten[i];
    const int back = (g.dir >= 0) ? ((g.dir + 2) & 3) : -1;
    int bestDir = -1;
    float bestScore = 0.0f;
    for (int d = 0; d < 4; ++d) {
        if (d == back) {
            continue;
        }
        const int nc = g.col + kDX[d];
        const int nr = g.row + kDY[d];
        if (!walkable(nc, nr)) {
            continue;
        }
        const float dc = static_cast<float>(nc - g_pac.col);
        const float dr = static_cast<float>(nr - g_pac.row);
        float score = dc * dc + dr * dr;
        if (frightened) {
            score = -score;
        }
        if (d == g.dir) {
            score -= 0.01f;
        }
        if (bestDir < 0 || score < bestScore) {
            bestScore = score;
            bestDir = d;
        }
    }
    if (bestDir < 0 && back >= 0 &&
        walkable(g.col + kDX[back], g.row + kDY[back])) {
        bestDir = back;
    }
    if (bestDir < 0) {
        g.dir = kNone;
        g.tc = g.col;
        g.tr = g.row;
        g.t = 0.0f;
        return;
    }
    g.dir = bestDir;
    g.tc = g.col + kDX[bestDir];
    g.tr = g.row + kDY[bestDir];
}

void resetActors() {
    g_pac = Actor();
    g_pac.col = 9;
    g_pac.row = 7;
    g_pac.tc = 8;
    g_pac.tr = 7;
    g_pac.dir = kLeft;
    g_pac.face = kLeft;
    g_pac.want = kNone;

    g_frightenedUntil = 0;
    g_ghostCombo = 0;
    for (int i = 0; i < 4; ++i) {
        Actor& g = g_ghosts[i];
        g = Actor();
        g.col = kGhostHome[i][0];
        g.row = kGhostHome[i][1];
        g.tc = g.col;
        g.tr = g.row;
        g.dir = kNone;
        g_ghostEaten[i] = false;
        g_ghostEatenUntil[i] = 0;
        chooseGhostDir(i);
    }
}

void loadLevel() {
    g_pelletsLeft = 0;
    for (int r = 0; r < kRows; ++r) {
        const size_t len = std::strlen(kMaze[r]);
        for (int c = 0; c < kCols; ++c) {
            const char ch = (c < static_cast<int>(len)) ? kMaze[r][c] : '#';
            g_walls[r][c] = (ch == '#');
            g_pellets[r][c] = (ch == '.' || ch == 'o');
            g_power[r][c] = (ch == 'o');
            if (g_pellets[r][c]) {
                ++g_pelletsLeft;
            }
        }
    }
    // Le caselle di partenza non devono nascondere pellet sotto gli attori.
    const int clearCols[5] = {9, 9, 10, 6, 12};
    const int clearRows[5] = {7, 3, 3, 5, 5};
    for (int i = 0; i < 5; ++i) {
        const int r = clearRows[i];
        const int c = clearCols[i];
        if (g_pellets[r][c]) {
            g_pellets[r][c] = false;
            --g_pelletsLeft;
        }
    }
    resetActors();
}

void eatPellet(int col, int row) {
    if (!g_pellets[row][col]) {
        return;
    }
    g_pellets[row][col] = false;
    --g_pelletsLeft;
    if (g_power[row][col]) {
        g_score += 50;
        g_frightenedUntil = millis() + 7000;
        g_ghostCombo = 0;
        beep(880.0f, 60);
    } else {
        g_score += 10;
    }
}

void choosePacDir() {
    Actor& a = g_pac;
    if (a.want >= 0 && walkable(a.col + kDX[a.want], a.row + kDY[a.want])) {
        a.dir = a.want;
    }
    if (a.dir >= 0 && walkable(a.col + kDX[a.dir], a.row + kDY[a.dir])) {
        a.tc = a.col + kDX[a.dir];
        a.tr = a.row + kDY[a.dir];
        a.face = a.dir;
    } else {
        a.dir = kNone;
        a.tc = a.col;
        a.tr = a.row;
    }
}

void advancePac(float dist) {
    Actor& a = g_pac;
    if (a.dir < 0) {
        return;
    }
    a.t += dist;
    while (a.t >= 1.0f) {
        a.t -= 1.0f;
        a.col = a.tc;
        a.row = a.tr;
        eatPellet(a.col, a.row);
        choosePacDir();
        if (a.dir < 0) {
            a.t = 0.0f;
            break;
        }
    }
}

void advanceGhost(int i, float dist) {
    if (g_ghostEaten[i]) {
        return;
    }
    Actor& a = g_ghosts[i];
    if (a.dir < 0) {
        chooseGhostDir(i);
        if (a.dir < 0) {
            return;
        }
    }
    a.t += dist;
    while (a.t >= 1.0f) {
        a.t -= 1.0f;
        a.col = a.tc;
        a.row = a.tr;
        chooseGhostDir(i);
        if (a.dir < 0) {
            a.t = 0.0f;
            break;
        }
    }
}

float ghostSpeed(int i, uint32_t now) {
    if (now < g_frightenedUntil && !g_ghostEaten[i]) {
        return 2.8f;
    }
    return std::min(6.2f, 4.0f + 0.35f * static_cast<float>(g_level - 1));
}

void respawnEatenGhosts(uint32_t now) {
    for (int i = 0; i < 4; ++i) {
        if (!g_ghostEaten[i] || now < g_ghostEatenUntil[i]) {
            continue;
        }
        Actor& g = g_ghosts[i];
        g = Actor();
        g.col = kGhostHome[i][0];
        g.row = kGhostHome[i][1];
        g.tc = g.col;
        g.tr = g.row;
        g_ghostEaten[i] = false;
        chooseGhostDir(i);
    }
}

void collide(uint32_t now) {
    if (g_gameOver || now < g_pauseUntil) {
        return;
    }
    float px = 0.0f;
    float py = 0.0f;
    actorPos(g_pac, px, py);
    for (int i = 0; i < 4; ++i) {
        if (g_ghostEaten[i]) {
            continue;
        }
        float gx = 0.0f;
        float gy = 0.0f;
        actorPos(g_ghosts[i], gx, gy);
        const float dx = gx - px;
        const float dy = gy - py;
        if (dx * dx + dy * dy > 0.42f) {
            continue;
        }
        if (now < g_frightenedUntil) {
            g_ghostEaten[i] = true;
            g_ghostEatenUntil[i] = now + 1200;
            g_score += 200 << (g_ghostCombo > 3 ? 3 : g_ghostCombo);
            ++g_ghostCombo;
            beep(1400.0f, 40);
            continue;
        }
        --g_lives;
        beep(180.0f, 320);
        if (g_lives <= 0) {
            g_gameOver = true;
            scores::submit("pacman", static_cast<uint32_t>(g_score));
            if (static_cast<uint32_t>(g_score) > g_best) {
                g_best = static_cast<uint32_t>(g_score);
            }
        } else {
            resetActors();
            g_pauseUntil = now + 900;
        }
        return;
    }
}

void drawMaze(uint32_t now) {
    auto& d = display::g();
    for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
            const int x = kFieldX + c * kTile;
            const int y = kFieldY + r * kTile;
            if (g_walls[r][c]) {
                d.fillRoundRect(x, y, kTile, kTile, 2, kWall);
                d.drawRoundRect(x, y, kTile, kTile, 2, kWallEdge);
            } else if (g_pellets[r][c]) {
                if (g_power[r][c]) {
                    const int pulse = static_cast<int>((now / 180) % 2);
                    d.fillCircle(x + kTile / 2, y + kTile / 2, 3 + pulse,
                                 display::kPaper);
                } else {
                    d.fillRect(x + kTile / 2 - 1, y + kTile / 2 - 1, 2, 2,
                               display::kPaper);
                }
            }
        }
    }
}

void drawPac(uint32_t now) {
    if (g_gameOver) {
        return;
    }
    float fx = 0.0f;
    float fy = 0.0f;
    actorPos(g_pac, fx, fy);
    const int cx = kFieldX + static_cast<int>(fx * kTile);
    const int cy = kFieldY + static_cast<int>(fy * kTile);
    const int r = 4;
    static const float kAngle[4] = {3.14159f, -1.57080f, 0.0f, 1.57080f};
    const float base = kAngle[g_pac.face < 0 ? kRight : g_pac.face];
    const float phase = std::fabs(std::sin(now * 0.012f));
    const float half = 0.10f + phase * 0.55f;
    display::g().fillCircle(cx, cy, r, display::kNeoYellow);
    const int x1 = cx + static_cast<int>(std::cos(base - half) * (r + 1));
    const int y1 = cy + static_cast<int>(std::sin(base - half) * (r + 1));
    const int x2 = cx + static_cast<int>(std::cos(base + half) * (r + 1));
    const int y2 = cy + static_cast<int>(std::sin(base + half) * (r + 1));
    display::g().fillTriangle(cx, cy, x1, y1, x2, y2, display::kBlack);
}

void drawGhosts(uint32_t now) {
    auto& d = display::g();
    const bool frightened = now < g_frightenedUntil;
    const bool flashing = frightened && (g_frightenedUntil - now) < 2000 &&
                          ((now / 180) % 2 == 0);
    for (int i = 0; i < 4; ++i) {
        float fx = 0.0f;
        float fy = 0.0f;
        actorPos(g_ghosts[i], fx, fy);
        const int cx = kFieldX + static_cast<int>(fx * kTile);
        const int cy = kFieldY + static_cast<int>(fy * kTile);
        if (g_ghostEaten[i]) {
            d.fillCircle(cx - 2, cy - 1, 2, display::kWhite);
            d.fillCircle(cx + 2, cy - 1, 2, display::kWhite);
            d.fillRect(cx - 2, cy - 1, 1, 1, display::kInk);
            d.fillRect(cx + 2, cy - 1, 1, 1, display::kInk);
            continue;
        }
        const uint16_t body = frightened
                                  ? (flashing ? display::kWhite : kGhostBlue)
                                  : kGhostColor[i];
        d.fillCircle(cx, cy - 1, 4, body);
        d.fillRect(cx - 4, cy - 1, 9, 5, body);
        d.fillTriangle(cx - 2, cy + 3, cx, cy + 5, cx + 2, cy + 3,
                       display::kBlack);
        d.fillCircle(cx - 2, cy - 2, 2, display::kWhite);
        d.fillCircle(cx + 2, cy - 2, 2, display::kWhite);
        const int look = (g_ghosts[i].dir >= 0) ? g_ghosts[i].dir : kRight;
        const int pupilX = (kDX[look] != 0) ? (kDX[look] > 0 ? 1 : -1) : 0;
        const int pupilY = (kDY[look] != 0) ? (kDY[look] > 0 ? 1 : -1) : 0;
        d.fillCircle(cx - 2 + pupilX, cy - 2 + pupilY, 1, display::kInk);
        d.fillCircle(cx + 2 + pupilX, cy - 2 + pupilY, 1, display::kInk);
    }
}

void drawHud() {
    auto& d = display::g();
    d.fillRect(0, 0, 240, 19, display::kBlack);
    display::text(4, 5, "PAC-MAN", display::kNeoYellow, 0);
    display::text(56, 5, "PUNTI " + std::to_string(g_score), display::kText, 0);
    display::text(124, 5,
                  "REC " + std::to_string(std::max(g_best, static_cast<uint32_t>(g_score))),
                  display::kInfo, 0);
    display::text(184, 5, "VITE " + std::to_string(g_lives), display::kNeoRed, 0);
}

void drawOverlay() {
    const int w = 180;
    const int h = 54;
    const int x = (240 - w) / 2;
    const int y = 36;
    auto& d = display::g();
    d.fillRect(x, y, w, h, display::kBlack);
    d.drawRect(x, y, w, h, display::kDanger);
    display::textCentered(y + 7, "GAME OVER", display::kDanger, 2);
    display::textCentered(
        y + 25, "PUNTI " + std::to_string(g_score) + "  REC " + std::to_string(g_best),
        display::kText, 0);
    display::textCentered(y + 38, "INVIO = riprova   ` = esci", display::kTextDim, 0);
}

void drawScene(uint32_t now) {
    display::clear(display::kBlack);
    drawMaze(now);
    drawGhosts(now);
    drawPac(now);
    drawHud();
    if (g_gameOver) {
        drawOverlay();
    }
}

void handleInput(const navigation::EventData& ev) {
    int want = kNone;
    if (ev.type == navigation::Event::Up) {
        want = kUp;
    } else if (ev.type == navigation::Event::Down) {
        want = kDown;
    } else if (ev.type == navigation::Event::Left) {
        want = kLeft;
    } else if (ev.type == navigation::Event::Right) {
        want = kRight;
    } else if (ev.type == navigation::Event::Text) {
        switch (ev.ch) {
            case 'w':
            case 'W':
                want = kUp;
                break;
            case 's':
            case 'S':
                want = kDown;
                break;
            case 'a':
            case 'A':
                want = kLeft;
                break;
            case 'd':
            case 'D':
                want = kRight;
                break;
            default:
                break;
        }
    }
    if (want < 0) {
        return;
    }
    g_pac.want = want;
    if (g_pac.dir < 0 && walkable(g_pac.col + kDX[want], g_pac.row + kDY[want])) {
        g_pac.dir = want;
        g_pac.tc = g_pac.col + kDX[want];
        g_pac.tr = g_pac.row + kDY[want];
        g_pac.face = want;
        g_pac.t = 0.0f;
    }
}

}  // namespace

void runPacman() {
    g_best = scores::best("pacman");
    navigation::waitRelease();

    for (;;) {
        g_level = 1;
        g_score = 0;
        g_lives = 3;
        g_gameOver = false;
        g_pauseUntil = millis() + 600;
        loadLevel();

        uint32_t last = millis();
        bool exitGame = false;
        while (!exitGame) {
            navigation::update();
            const navigation::EventData ev = navigation::poll();
            if (ev.type == navigation::Event::Back) {
                if (g_score > 0) {
                    scores::submit("pacman", static_cast<uint32_t>(g_score));
                }
                return;
            }

            const uint32_t now = millis();
            if (g_gameOver) {
                if (ev.type == navigation::Event::Select) {
                    exitGame = true;
                    continue;
                }
                drawScene(now);
                display::push();
                delay(20);
                continue;
            }

            handleInput(ev);

            float dt = static_cast<float>(now - last) / 1000.0f;
            last = now;
            if (dt > 0.1f) {
                dt = 0.1f;
            }

            if (now >= g_pauseUntil) {
                if (g_pelletsLeft == 0) {
                    scores::submit("pacman", static_cast<uint32_t>(g_score));
                    if (static_cast<uint32_t>(g_score) > g_best) {
                        g_best = static_cast<uint32_t>(g_score);
                    }
                    ++g_level;
                    loadLevel();
                    g_pauseUntil = millis() + 800;
                } else {
                    advancePac(6.0f * dt);
                    for (int i = 0; i < 4; ++i) {
                        advanceGhost(i, ghostSpeed(i, now) * dt);
                    }
                    respawnEatenGhosts(now);
                    collide(now);
                }
            }

            drawScene(now);
            display::push();
            delay(8);
        }
        navigation::waitRelease();
    }
}

}  // namespace games

#endif  // ENABLE_MODULE_GAMES
