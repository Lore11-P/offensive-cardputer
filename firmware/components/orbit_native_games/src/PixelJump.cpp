#include "PixelJump.h"

#include "orbit_compat.h"

#include <algorithm>
#include <cstdio>

#include "Display.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "Scores.h"

#if ENABLE_MODULE_GAMES

namespace games {
namespace {

constexpr int kTile = 12;
constexpr int kRows = 10;
constexpr int kPlayY = 15;
constexpr int kScreenW = 240;
constexpr int kWorldH = kRows * kTile;
constexpr int kMaxCols = 80;
constexpr int kMaxEnemies = 24;
constexpr int kPlayerW = 8;
constexpr int kPlayerH = 12;
constexpr int kEnemyW = 10;
constexpr int kEnemyH = 10;
constexpr int kLevelCount = 2;
constexpr float kGravity = 650.0f;
constexpr float kMaxFall = 280.0f;
constexpr float kRunSpeed = 100.0f;
constexpr float kAccel = 700.0f;
constexpr float kBrake = 900.0f;
constexpr float kJumpSpeed = -260.0f;
constexpr float kEnemySpeed = 18.0f;
constexpr int kCoyoteMs = 90;

// Livelli in .rodata (flash): 10 righe da 12 px, tile da 12 px.
// # terreno, = piattaforma solida, o moneta, E nemico, F bandiera,
// P partenza, . vuoto.
const char* const kLevels[kLevelCount][kRows] = {
    {
        "............................................................",  // 0
        "............................................................",  // 1
        "............................................................",  // 2
        "...............oo...............oo..........................",  // 3
        "..............====.............====.........................",  // 4
        "............................................................",  // 5
        "..........oo...........oo...o...............oo..............",  // 6
        ".........====.............=====............=====............",  // 7
        "..P............E....oo...........E..oo..........E....oo..F..",  // 8
        "######################...#############...###################",  // 9
    },
    {
        "................................................................................",  // 0
        "................................................................................",  // 1
        "...........................oo...................................................",  // 2
        "..........................====..................................................",  // 3
        ".....................oo.........................oo..............oo..............",  // 4
        "....................====.......................====............====.............",  // 5
        ".............oo..........oo....oo.......oo....o........oo...........oo..........",  // 6
        "............====........====...........====...........====.........====.........",  // 7
        "..P.......E..oo..........E..oo...........E.oo...........E.oo............E..ooF..",  // 8
        "###############...############....###########....###########...#################",  // 9
    },
};

struct Player {
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    bool onGround = false;
    int dir = 1;
};

struct Enemy {
    float x = 0.0f;
    float y = 0.0f;
    float vy = 0.0f;
    int dir = 1;
    bool alive = false;
    bool onGround = false;
};

enum class Phase { Playing, GameOver, Won };

struct Game {
    Player player;
    Enemy enemies[kMaxEnemies];
    char map[kRows][kMaxCols];
    int enemyCount = 0;
    int cols = 0;
    int flagCol = -1;
    int flagRow = -1;
    int startX = 24;
    int startY = (kRows - 2) * kTile;
    int camera = 0;
    int score = 0;
    int coins = 0;
    int lives = 3;
    int level = 0;
    float fallSpeed = 0.0f;
    float prevBottom = 0.0f;
    uint32_t invulnUntil = 0;
    uint32_t lastGroundAt = 0;
    uint32_t best = 0;
    bool submitted = false;
    Phase phase = Phase::Playing;
};

Game g;

bool solidAt(int col, int row) {
    if (col < 0 || col >= g.cols) {
        return true;  // bordi del livello: muri invisibili
    }
    if (row < 0) {
        return false;
    }
    if (row >= kRows) {
        return false;  // sotto il livello si cade
    }
    const char t = g.map[row][col];
    return t == '#' || t == '=';
}

void addScore(int amount) {
    g.score = std::min(999999, g.score + amount);
}

void submitScore() {
    if (g.submitted) {
        return;
    }
    scores::submit("pixeljump", static_cast<uint32_t>(g.score));
    g.best = std::max(g.best, static_cast<uint32_t>(g.score));
    g.submitted = true;
}

void placePlayer() {
    Player& p = g.player;
    p.x = static_cast<float>(g.startX);
    p.y = static_cast<float>(g.startY);
    p.vx = 0.0f;
    p.vy = 0.0f;
    p.dir = 1;
    p.onGround = true;
    g.camera = 0;
    g.lastGroundAt = millis();
    g.invulnUntil = millis() + 900;
}

void loadLevel(int index) {
    if (index < 0) {
        index = 0;
    }
    if (index >= kLevelCount) {
        index = kLevelCount - 1;
    }
    g.level = index;

    for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kMaxCols; ++c) {
            g.map[r][c] = '.';
        }
    }

    int lens[kRows] = {};
    int cols = 0;
    const char* const* rows = kLevels[index];
    if (rows != nullptr) {
        for (int r = 0; r < kRows; ++r) {
            const char* row = rows[r];
            int n = 0;
            if (row != nullptr) {
                while (n < kMaxCols && row[n] != '\0') {
                    ++n;
                }
            }
            lens[r] = n;
            cols = std::max(cols, n);
        }
    }

    if (cols < 20) {
        // Livello mancante o troppo corto: pista di emergenza giocabile.
        g.cols = 40;
        for (int c = 0; c < g.cols; ++c) {
            g.map[kRows - 1][c] = '#';
        }
        g.map[kRows - 2][2] = 'P';
        g.map[kRows - 2][g.cols - 3] = 'F';
    } else {
        g.cols = cols;
        for (int r = 0; r < kRows; ++r) {
            for (int c = 0; c < g.cols; ++c) {
                if (c < lens[r] && rows[r] != nullptr) {
                    g.map[r][c] = rows[r][c];
                }
            }
        }
    }

    g.enemyCount = 0;
    g.flagCol = -1;
    g.flagRow = -1;
    g.startX = 2 * kTile + (kTile - kPlayerW) / 2;
    g.startY = (kRows - 2) * kTile;

    for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < g.cols; ++c) {
            const char t = g.map[r][c];
            if (t == 'P') {
                g.startX = c * kTile + (kTile - kPlayerW) / 2;
                g.startY = r * kTile + (kTile - kPlayerH);
                g.map[r][c] = '.';
            } else if (t == 'E') {
                if (g.enemyCount < kMaxEnemies) {
                    Enemy& e = g.enemies[g.enemyCount];
                    ++g.enemyCount;
                    e.x = static_cast<float>(c * kTile + (kTile - kEnemyW) / 2);
                    e.y = static_cast<float>(r * kTile + (kTile - kEnemyH));
                    e.vy = 0.0f;
                    e.dir = ((c + r) & 1) ? 1 : -1;
                    e.alive = true;
                    e.onGround = false;
                }
                g.map[r][c] = '.';
            } else if (t == 'F') {
                g.flagCol = c;
                g.flagRow = r;
            }
        }
    }

    placePlayer();
}

void startGame() {
    g.lives = 3;
    g.score = 0;
    g.coins = 0;
    g.submitted = false;
    g.phase = Phase::Playing;
    loadLevel(0);
}

void respawn(uint32_t now) {
    Player& p = g.player;
    p.x = static_cast<float>(g.startX);
    p.y = static_cast<float>(g.startY);
    p.vx = 0.0f;
    p.vy = 0.0f;
    p.dir = 1;
    p.onGround = true;
    g.camera = 0;
    g.lastGroundAt = now;
    g.invulnUntil = now + 1200;
}

void loseLife(uint32_t now) {
    if (g.phase != Phase::Playing) {
        return;
    }
    --g.lives;
    if (g.lives <= 0) {
        g.lives = 0;
        g.phase = Phase::GameOver;
        submitScore();
    } else {
        respawn(now);
    }
}

void movePlayerX(float dt) {
    Player& p = g.player;
    p.x += p.vx * dt;

    const int top = static_cast<int>(p.y / kTile);
    const int bottom = static_cast<int>((p.y + kPlayerH - 0.01f) / kTile);
    if (p.vx > 0.0f) {
        const int col = static_cast<int>((p.x + kPlayerW - 0.01f) / kTile);
        for (int r = top; r <= bottom; ++r) {
            if (solidAt(col, r)) {
                p.x = static_cast<float>(col * kTile - kPlayerW);
                p.vx = 0.0f;
                break;
            }
        }
    } else if (p.vx < 0.0f) {
        const int col = static_cast<int>(p.x / kTile);
        for (int r = top; r <= bottom; ++r) {
            if (solidAt(col, r)) {
                p.x = static_cast<float>((col + 1) * kTile);
                p.vx = 0.0f;
                break;
            }
        }
    }
}

void movePlayerY(float dt) {
    Player& p = g.player;
    p.onGround = false;
    p.y += p.vy * dt;

    const int left = static_cast<int>(p.x / kTile);
    const int right = static_cast<int>((p.x + kPlayerW - 0.01f) / kTile);
    if (p.vy > 0.0f) {
        const int row = static_cast<int>((p.y + kPlayerH - 0.01f) / kTile);
        for (int c = left; c <= right; ++c) {
            if (solidAt(c, row)) {
                p.y = static_cast<float>(row * kTile - kPlayerH);
                p.vy = 0.0f;
                p.onGround = true;
                break;
            }
        }
    } else if (p.vy < 0.0f) {
        const int row = static_cast<int>(p.y / kTile);
        for (int c = left; c <= right; ++c) {
            if (solidAt(c, row)) {
                p.y = static_cast<float>((row + 1) * kTile);
                p.vy = 0.0f;
                break;
            }
        }
    }

    if (p.y < 0.0f) {
        p.y = 0.0f;
        if (p.vy < 0.0f) {
            p.vy = 0.0f;
        }
    }
}

void updatePlayer(float dt, uint32_t now, bool jumpPressed) {
    Player& p = g.player;
    const bool left = navigation::pressed(',');
    const bool right = navigation::pressed('/');

    if (left == right) {
        const float delta = kBrake * dt;
        if (p.vx > 0.0f) {
            p.vx = std::max(0.0f, p.vx - delta);
        } else {
            p.vx = std::min(0.0f, p.vx + delta);
        }
    } else if (left) {
        p.vx -= kAccel * dt;
        p.dir = -1;
    } else {
        p.vx += kAccel * dt;
        p.dir = 1;
    }
    p.vx = std::max(-kRunSpeed, std::min(kRunSpeed, p.vx));

    if (jumpPressed &&
        (p.onGround || (now - g.lastGroundAt) < static_cast<uint32_t>(kCoyoteMs))) {
        p.vy = kJumpSpeed;
        p.onGround = false;
        g.lastGroundAt = now - 1000;
    }

    p.vy += kGravity * dt;
    p.vy = std::min(p.vy, kMaxFall);
    g.fallSpeed = p.vy;
    g.prevBottom = p.y + kPlayerH;

    movePlayerX(dt);
    movePlayerY(dt);

    if (p.onGround) {
        g.lastGroundAt = now;
    }
}

void updateEnemies(float dt) {
    for (int i = 0; i < g.enemyCount; ++i) {
        Enemy& e = g.enemies[i];
        if (!e.alive) {
            continue;
        }

        const float step = kEnemySpeed * dt * static_cast<float>(e.dir);
        e.x += step;

        const int midRow = static_cast<int>((e.y + kEnemyH * 0.5f) / kTile);
        const int frontCol =
            static_cast<int>((e.dir > 0 ? e.x + kEnemyW : e.x) / kTile);
        const int footRow = static_cast<int>((e.y + kEnemyH + 1.0f) / kTile);
        if (solidAt(frontCol, midRow) ||
            (e.onGround && !solidAt(frontCol, footRow))) {
            e.x -= step;  // muro o bordo: torna indietro e gira
            e.dir = -e.dir;
        }

        e.vy = std::min(e.vy + kGravity * dt, kMaxFall);
        e.y += e.vy * dt;
        e.onGround = false;
        if (e.vy > 0.0f) {
            const int left = static_cast<int>(e.x / kTile);
            const int right = static_cast<int>((e.x + kEnemyW - 0.01f) / kTile);
            const int row = static_cast<int>((e.y + kEnemyH - 0.01f) / kTile);
            for (int c = left; c <= right; ++c) {
                if (solidAt(c, row)) {
                    e.y = static_cast<float>(row * kTile - kEnemyH);
                    e.vy = 0.0f;
                    e.onGround = true;
                    break;
                }
            }
        }

        if (e.x < 0.0f) {
            e.x = 0.0f;
            e.dir = 1;
        }
        const float maxX = static_cast<float>(g.cols * kTile - kEnemyW);
        if (e.x > maxX) {
            e.x = maxX;
            e.dir = -1;
        }
    }
}

void collectCoins() {
    const Player& p = g.player;
    const int c0 = static_cast<int>(p.x / kTile);
    const int c1 = static_cast<int>((p.x + kPlayerW - 0.01f) / kTile);
    const int r0 = static_cast<int>(p.y / kTile);
    const int r1 = static_cast<int>((p.y + kPlayerH - 0.01f) / kTile);

    for (int r = std::max(0, r0); r <= std::min(kRows - 1, r1); ++r) {
        for (int c = std::max(0, c0); c <= std::min(g.cols - 1, c1); ++c) {
            if (g.map[r][c] == 'o') {
                g.map[r][c] = '.';
                ++g.coins;
                addScore(10);
            }
        }
    }
}

void checkFlag() {
    if (g.flagCol < 0 || g.flagRow < 0) {
        return;
    }
    const Player& p = g.player;
    const float fx = static_cast<float>(g.flagCol * kTile);
    const float fy = static_cast<float>(g.flagRow * kTile);
    if (p.x + kPlayerW <= fx || p.x >= fx + kTile ||
        p.y + kPlayerH <= fy || p.y >= fy + kTile) {
        return;
    }

    addScore(100);
    if (g.level + 1 >= kLevelCount) {
        g.phase = Phase::Won;
        submitScore();
    } else {
        loadLevel(g.level + 1);
    }
}

void handleCollisions(uint32_t now) {
    Player& p = g.player;

    if (p.y > kWorldH + kPlayerH) {
        loseLife(now);
        return;
    }

    for (int i = 0; i < g.enemyCount; ++i) {
        Enemy& e = g.enemies[i];
        if (!e.alive) {
            continue;
        }
        if (p.x + kPlayerW <= e.x || p.x >= e.x + kEnemyW ||
            p.y + kPlayerH <= e.y || p.y >= e.y + kEnemyH) {
            continue;
        }

        if (g.fallSpeed > 40.0f && g.prevBottom <= e.y + 5.0f) {
            e.alive = false;
            p.vy = -175.0f;
            p.onGround = false;
            addScore(20);  // schiacciato saltandogli sopra
        } else if (now >= g.invulnUntil) {
            loseLife(now);
            return;
        }
    }

    if (g.phase != Phase::Playing) {
        return;
    }
    collectCoins();

    if (g.phase != Phase::Playing) {
        return;
    }
    checkFlag();
}

void drawOverlay() {
    const bool won = g.phase == Phase::Won;
    const uint16_t color = won ? display::kNeoGreen : display::kDanger;
    const int w = 190;
    const int h = 70;
    const int x = (kScreenW - w) / 2;
    const int y = (display::height() - h) / 2;

    auto& d = display::g();
    d.fillRect(x, y, w, h, display::kBlack);
    d.drawRect(x, y, w, h, color);

    display::textCentered(y + 4, "PIXEL JUMP", display::kTextDim, 0);
    display::textCentered(y + 13, won ? "HAI VINTO!" : "GAME OVER", color, 2);

    char buf[20];
    std::snprintf(buf, sizeof(buf), "%s %d", won ? "TOTALE" : "PUNTI", g.score);
    display::textCentered(y + 32, buf, display::kText, 0);

    const uint32_t shownBest =
        std::min<uint32_t>(std::max(g.best, static_cast<uint32_t>(g.score)), 999999u);
    std::snprintf(buf, sizeof(buf), "RECORD %lu",
                  static_cast<unsigned long>(shownBest));
    display::textCentered(y + 42, buf, display::kInfo, 0);

    display::textCentered(y + 53, "INVIO=riprova", display::kTextDim, 0);
    display::textCentered(y + 61, "ESC=esci", display::kTextDim, 0);
}

void drawHud() {
    auto& d = display::g();
    d.fillRect(0, 0, kScreenW, kPlayY, display::kBlack);
    d.drawFastHLine(0, kPlayY - 1, kScreenW, display::kAccentDark);

    char buf[16];
    std::snprintf(buf, sizeof(buf), "VITE %d", std::max(0, g.lives));
    display::text(4, 4, buf, display::kText, 0);

    std::snprintf(buf, sizeof(buf), "MONETE %d", g.coins);
    display::text(44, 4, buf, display::kNeoYellow, 0);

    std::snprintf(buf, sizeof(buf), "PUNTI %d", g.score);
    display::text(106, 4, buf, display::kText, 0);

    std::snprintf(buf, sizeof(buf), "LIV %d/%d", g.level + 1, kLevelCount);
    display::text(192, 4, buf, display::kInfo, 0);
}

void drawWorld(uint32_t now) {
    const int maxCamera = std::max(0, g.cols * kTile - kScreenW);
    const int target =
        static_cast<int>(g.player.x + kPlayerW / 2.0f) - kScreenW / 2;
    g.camera = std::max(0, std::min(target, maxCamera));

    display::clear(display::kBackground);
    auto& d = display::g();

    const int firstCol = std::max(0, g.camera / kTile);
    const int lastCol =
        std::min(g.cols - 1, (g.camera + kScreenW - 1) / kTile);
    for (int row = 0; row < kRows; ++row) {
        const int y = kPlayY + row * kTile;
        for (int col = firstCol; col <= lastCol; ++col) {
            const int x = col * kTile - g.camera;
            const char t = g.map[row][col];
            if (t == '#') {
                d.fillRect(x, y, kTile, kTile, display::kAccentDark);
                d.fillRect(x, y, kTile, 2, display::kAccent);
            } else if (t == '=') {
                d.fillRect(x, y, kTile, kTile, display::kInfo);
                d.fillRect(x, y, kTile, 2, display::kText);
            } else if (t == 'o') {
                d.fillCircle(x + kTile / 2, y + kTile / 2, 3,
                             display::kNeoYellow);
                d.fillRect(x + kTile / 2 - 1, y + kTile / 2 - 1, 1, 1,
                           display::kText);
            } else if (t == 'F') {
                d.fillRect(x + 5, y - 12, 2, 24, display::kTextDim);
                d.fillRect(x + 7, y - 10, 6, 5, display::kNeoGreen);
                d.fillRect(x + 3, y + 10, 9, 2, display::kAccentDark);
            }
        }
    }

    for (int i = 0; i < g.enemyCount; ++i) {
        const Enemy& e = g.enemies[i];
        if (!e.alive) {
            continue;
        }
        const int x = static_cast<int>(e.x) - g.camera;
        const int y = kPlayY + static_cast<int>(e.y);
        d.fillRect(x + 1, y + 2, 8, 7, display::kDanger);
        d.fillRect(x, y + 5, 10, 4, display::kDanger);
        d.fillRect(x + 2, y + 1, 2, 2, display::kDanger);
        d.fillRect(x + 6, y + 1, 2, 2, display::kDanger);
        d.fillRect(x + 2, y + 4, 2, 2, display::kBlack);
        d.fillRect(x + 6, y + 4, 2, 2, display::kBlack);
        d.fillRect(x + 1, y + 9, 2, 1, display::kBlack);
        d.fillRect(x + 7, y + 9, 2, 1, display::kBlack);
    }

    if (now >= g.invulnUntil || ((now / 70u) & 1u) == 0u) {
        const Player& p = g.player;
        const int x = static_cast<int>(p.x) - g.camera;
        const int y = kPlayY + static_cast<int>(p.y);
        d.fillRect(x + 3, y, 2, 1, display::kNeoYellow);
        d.fillRect(x + 1, y + 1, 6, 4, display::kInfo);
        d.fillRect(x + (p.dir > 0 ? 4 : 2), y + 2, 2, 2, display::kBlack);
        d.fillRect(x + 1, y + 5, 6, 4, display::kAccent);
        d.fillRect(x, y + 6, 1, 2, display::kAccentDark);
        d.fillRect(x + 7, y + 6, 1, 2, display::kAccentDark);
        d.fillRect(x + 1, y + 9, 2, 3, display::kAccentDark);
        d.fillRect(x + 5, y + 9, 2, 3, display::kAccentDark);
        d.fillRect(x + 3, y + 6, 2, 2, display::kNeoYellow);
    }

    drawHud();
    if (g.phase != Phase::Playing) {
        drawOverlay();
    }
    display::push();
}

}  // namespace

void runPixelJump() {
    g.best = scores::best("pixeljump");
    navigation::waitRelease();

    for (;;) {
        startGame();
        uint32_t lastFrame = millis();
        bool retry = false;

        while (!retry) {
            navigation::update();
            const navigation::EventData ev = navigation::poll();
            const uint32_t now = millis();

            if (ev.type == navigation::Event::Back) {
                if (!g.submitted) {
                    submitScore();
                }
                return;
            }

            if (g.phase != Phase::Playing) {
                if (ev.type == navigation::Event::Select) {
                    retry = true;
                }
            } else {
                float dt = (now - lastFrame) / 1000.0f;
                lastFrame = now;
                // dt max 33 ms: caduta max ~9 px/frame, sotto il tile da 12 px.
                dt = std::min(dt, 0.033f);
                dt = std::max(dt, 0.0f);

                updatePlayer(dt, now, ev.type == navigation::Event::Up);
                if (g.phase == Phase::Playing) {
                    updateEnemies(dt);
                    handleCollisions(now);
                }
            }

            drawWorld(now);
            delay(12);
        }

        navigation::waitRelease();
    }
}

}  // namespace games

#endif  // ENABLE_MODULE_GAMES
