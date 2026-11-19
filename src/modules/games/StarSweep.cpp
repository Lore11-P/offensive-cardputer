#include "StarSweep.h"

#include <Arduino.h>

#include <algorithm>

#include "Display.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "Scores.h"

#if ENABLE_MODULE_GAMES

namespace games {
namespace {

// Campo di gioco in landscape 240x135: HUD in alto, formazione al centro.
constexpr int kFieldX = 4;
constexpr int kFieldY = 16;
constexpr int kFieldW = 232;
constexpr int kFieldH = 116;

constexpr int kCols = 6;
constexpr int kRows = 4;
constexpr int kCellW = 30;
constexpr int kCellH = 15;
constexpr int kEnemyW = 16;
constexpr int kEnemyH = 9;
constexpr int kFormX0 = (240 - ((kCols - 1) * kCellW + kEnemyW)) / 2;
constexpr int kFormY0 = 26;
constexpr int kStepX = 6;
constexpr int kDropY = 8;

constexpr int kPlayerW = 16;
constexpr int kPlayerH = 8;
constexpr int kPlayerY = 119;
constexpr float kPlayerSpeed = 150.0f;

constexpr int kMaxPlayerShots = 6;
constexpr int kMaxEnemyShots = 12;
constexpr int kMaxStars = 20;
constexpr uint32_t kShotCooldown = 220;
constexpr float kPlayerShotSpeed = 280.0f;
constexpr float kEnemyShotSpeed = 60.0f;

struct Bullet {
    float x = 0.0f;
    float y = 0.0f;
    bool alive = false;
};

struct Star {
    int x = 0;
    float y = 0.0f;
    uint8_t speed = 10;
};

uint16_t enemyColor(int row) {
    switch (row) {
        case 0:
            return display::kDanger;
        case 1:
            return display::kWarning;
        case 2:
            return display::kInfo;
        default:
            return display::kAccent;
    }
}

// Un solo tono ogni 80 ms: evita di accodare suoni quando la formazione
// incassa piu' colpi nello stesso frame.
void beep(uint32_t freq, uint32_t ms, uint32_t& last, uint32_t now) {
    if (now - last < 80) {
        return;
    }
    last = now;
    M5Cardputer.Speaker.tone(static_cast<float>(freq), ms);
}

void drawOverlay(int score, uint32_t best) {
    const int w = 196;
    const int h = 64;
    const int x = (display::width() - w) / 2;
    const int y = (display::height() - h) / 2;
    display::g().fillRect(x, y, w, h, display::kBlack);
    display::g().drawRect(x, y, w, h, display::kDanger);
    display::textCentered(y + 7, "GAME OVER", display::kDanger, 2);
    display::textCentered(y + 25, "PUNTI " + std::to_string(score),
                          display::kText, 0);
    display::textCentered(y + 35, "RECORD " + std::to_string(best),
                          display::kTextDim, 0);
    // Costruita una sola volta: l'overlay viene ridisegnato piu' volte.
    static const std::string hint = "INVIO = riprova   ESC = esci";
    display::textCentered(y + 50, hint, display::kTextDim, 0);
}

}  // namespace

void runStarSweep() {
    uint32_t best = scores::best("starsweep");
    navigation::waitRelease();

    for (;;) {
        // Stato di una partita. Solo array a dimensione fissa: il loop non
        // alloca memoria.
        bool alive[kRows][kCols];
        for (auto& row : alive) {
            for (bool& enemy : row) {
                enemy = true;
            }
        }
        int formX = kFormX0;
        int formY = kFormY0;
        int dir = 1;
        float playerX = (display::width() - kPlayerW) / 2.0f;

        Bullet playerShots[kMaxPlayerShots];
        Bullet enemyShots[kMaxEnemyShots];
        Star stars[kMaxStars];
        for (Star& star : stars) {
            star.x = kFieldX + 4 +
                     static_cast<int>(esp_random() % (kFieldW - 8));
            star.y = static_cast<float>(kFieldY +
                                        static_cast<int>(esp_random() % 24));
            star.speed = static_cast<uint8_t>(8 + esp_random() % 18);
        }

        int score = 0;
        int lives = 3;
        int wave = 1;
        bool gameOver = false;

        uint32_t now = millis();
        uint32_t lastFrame = now;
        uint32_t renderedAt = 0;
        uint32_t lastShot = now - kShotCooldown;
        uint32_t invulnUntil = 0;
        uint32_t nextStep = now + 500;
        uint32_t nextEnemyShot = now + 1300;
        uint32_t lastBeep = now - 200;

        while (true) {
            navigation::update();
            const navigation::EventData ev = navigation::poll();
            now = millis();

            // Unico modo per uscire dal gioco.
            if (ev.type == navigation::Event::Back) {
                if (!gameOver && score > 0) {
                    scores::submit("starsweep",
                                   static_cast<uint32_t>(score));
                }
                return;
            }

            if (gameOver && ev.type == navigation::Event::Select) {
                break;
            }

            if (!gameOver) {
                float dt = (now - lastFrame) / 1000.0f;
                dt = std::min(dt, 0.034f);
                lastFrame = now;

                if (navigation::pressed(',')) {
                    playerX -= kPlayerSpeed * dt;
                }
                if (navigation::pressed('/')) {
                    playerX += kPlayerSpeed * dt;
                }
                playerX = std::max(
                    static_cast<float>(kFieldX),
                    std::min(playerX, static_cast<float>(kFieldX + kFieldW -
                                                        kPlayerW)));

                for (Star& star : stars) {
                    star.y += star.speed * dt;
                    if (star.y > kFieldY + kFieldH) {
                        star.y = static_cast<float>(kFieldY + 2);
                        star.x = kFieldX + 4 +
                                 static_cast<int>(esp_random() % (kFieldW - 8));
                    }
                }

                // Sparo del giocatore, con cooldown e slot libero.
                if ((ev.type == navigation::Event::Select ||
                     navigation::enterPressed()) &&
                    now - lastShot >= kShotCooldown) {
                    for (Bullet& shot : playerShots) {
                        if (!shot.alive) {
                            shot.x = playerX + kPlayerW / 2.0f;
                            shot.y = static_cast<float>(kPlayerY - 5);
                            shot.alive = true;
                            lastShot = now;
                            beep(880, 18, lastBeep, now);
                            break;
                        }
                    }
                }

                // Avanzamento a scatti: al bordo inversione e discesa.
                if (now >= nextStep) {
                    nextStep = now + static_cast<uint32_t>(
                                          std::max(85, 210 - (wave - 1) * 20));
                    bool any = false;
                    int minCol = kCols;
                    int maxCol = -1;
                    int maxRow = 0;
                    for (int row = 0; row < kRows; ++row) {
                        for (int col = 0; col < kCols; ++col) {
                            if (!alive[row][col]) {
                                continue;
                            }
                            any = true;
                            minCol = std::min(minCol, col);
                            maxCol = std::max(maxCol, col);
                            maxRow = std::max(maxRow, row);
                        }
                    }

                    if (!any) {
                        // Ondata completata: nuova formazione, piu' veloce.
                        ++wave;
                        formX = kFormX0;
                        formY = kFormY0;
                        dir = 1;
                        for (auto& row : alive) {
                            for (bool& enemy : row) {
                                enemy = true;
                            }
                        }
                        for (Bullet& shot : enemyShots) {
                            shot.alive = false;
                        }
                        nextStep = now + 700;
                        nextEnemyShot = now + 1300;
                        beep(1200, 40, lastBeep, now);
                    } else {
                        const int rightEdge = formX + maxCol * kCellW + kEnemyW;
                        const int leftEdge = formX + minCol * kCellW;
                        if (dir > 0 && rightEdge + kStepX > kFieldX + kFieldW) {
                            dir = -1;
                            formY += kDropY;
                        } else if (dir < 0 && leftEdge - kStepX < kFieldX) {
                            dir = 1;
                            formY += kDropY;
                        } else {
                            formX += dir * kStepX;
                        }
                        // Se la formazione raggiunge la zona del giocatore,
                        // la partita finisce subito.
                        if (formY + maxRow * kCellH + kEnemyH >=
                            kPlayerY - 6) {
                            gameOver = true;
                            scores::submit("starsweep",
                                           static_cast<uint32_t>(score));
                            best = std::max(best,
                                            static_cast<uint32_t>(score));
                        }
                    }
                }

                if (!gameOver) {
                    // Proiettili del giocatore contro i nemici (1 HP).
                    for (Bullet& shot : playerShots) {
                        if (!shot.alive) {
                            continue;
                        }
                        shot.y -= kPlayerShotSpeed * dt;
                        if (shot.y + 5 < kFieldY) {
                            shot.alive = false;
                            continue;
                        }
                        for (int row = 0; row < kRows && shot.alive; ++row) {
                            for (int col = 0; col < kCols; ++col) {
                                if (!alive[row][col]) {
                                    continue;
                                }
                                const int ex = formX + col * kCellW;
                                const int ey = formY + row * kCellH;
                                if (shot.x - 1 < ex + kEnemyW &&
                                    shot.x + 1 > ex && shot.y < ey + kEnemyH &&
                                    shot.y + 5 > ey) {
                                    alive[row][col] = false;
                                    shot.alive = false;
                                    score += 10;
                                    beep(240, 30, lastBeep, now);
                                    break;
                                }
                            }
                        }
                    }

                    // Fuoco nemico: parte solo dalla riga piu' bassa di una
                    // colonna viva, con frequenza crescente per ondata.
                    if (now >= nextEnemyShot) {
                        const int base =
                            std::max(550, 1700 - (wave - 1) * 170);
                        nextEnemyShot = now + static_cast<uint32_t>(
                                                  base + static_cast<int>(
                                                             esp_random() % 500));
                        int fireCol[kCols];
                        int fireRow[kCols];
                        int count = 0;
                        for (int col = 0; col < kCols; ++col) {
                            for (int row = kRows - 1; row >= 0; --row) {
                                if (alive[row][col]) {
                                    fireCol[count] = col;
                                    fireRow[count] = row;
                                    ++count;
                                    break;
                                }
                            }
                        }
                        if (count > 0) {
                            for (Bullet& shot : enemyShots) {
                                if (shot.alive) {
                                    continue;
                                }
                                const int pick =
                                    static_cast<int>(esp_random() % count);
                                shot.x = static_cast<float>(
                                    formX + fireCol[pick] * kCellW +
                                    kEnemyW / 2);
                                shot.y = static_cast<float>(
                                    formY + fireRow[pick] * kCellH +
                                    kEnemyH);
                                shot.alive = true;
                                break;
                            }
                        }
                    }

                    // Proiettili nemici: lenti, verso il basso.
                    for (Bullet& shot : enemyShots) {
                        if (!shot.alive) {
                            continue;
                        }
                        shot.y += kEnemyShotSpeed * dt;
                        if (shot.y > display::height()) {
                            shot.alive = false;
                            continue;
                        }
                        if (now < invulnUntil) {
                            continue;
                        }
                        if (shot.x - 1 < playerX + kPlayerW &&
                            shot.x + 1 > playerX &&
                            shot.y < kPlayerY + kPlayerH &&
                            shot.y + 5 > kPlayerY) {
                            shot.alive = false;
                            // Colpo subito: si azzera il fuoco nemico e si
                            // resta invulnerabili per 1,5 s.
                            for (Bullet& other : enemyShots) {
                                other.alive = false;
                            }
                            --lives;
                            beep(160, 120, lastBeep, now);
                            if (lives <= 0) {
                                gameOver = true;
                                scores::submit(
                                    "starsweep",
                                    static_cast<uint32_t>(score));
                                best = std::max(
                                    best, static_cast<uint32_t>(score));
                            } else {
                                invulnUntil = now + 1500;
                            }
                            break;
                        }
                    }
                }
            }

            // Disegno a ~60 fps, indipendente dalla logica.
            if (now - renderedAt >= 16) {
                display::clear(display::kBackground);

                const std::string points = "PUNTI " + std::to_string(score);
                const std::string livesText =
                    "VITE " + std::to_string(std::max(lives, 0));
                const std::string waveText =
                    "ONDATA " + std::to_string(wave);
                display::text(4, 3, points, display::kText, 0);
                display::text(4 + display::textWidth(points, 0) + 8, 3,
                              livesText, display::kInfo, 0);
                display::text(display::width() - 4 -
                                  display::textWidth(waveText, 0),
                              3, waveText, display::kInfo, 0);

                display::g().drawRect(kFieldX - 1, kFieldY - 1, kFieldW + 2,
                                      kFieldH + 2, display::kAccentDark);

                for (const Star& star : stars) {
                    display::g().fillRect(star.x, static_cast<int>(star.y),
                                          1, 1, display::kTextDim);
                }

                for (int row = 0; row < kRows; ++row) {
                    for (int col = 0; col < kCols; ++col) {
                        if (!alive[row][col]) {
                            continue;
                        }
                        const int ex = formX + col * kCellW;
                        const int ey = formY + row * kCellH;
                        const uint16_t color = enemyColor(row);
                        display::g().fillRect(ex + 5, ey, 6, 3, color);
                        display::g().fillRect(ex + 2, ey + 3, 12, 3, color);
                        display::g().fillRect(ex, ey + 5, kEnemyW, 4, color);
                    }
                }

                // Lampeggio durante l'invulnerabilita'.
                if (now >= invulnUntil || (now / 120) % 2 == 0) {
                    const int px = static_cast<int>(playerX);
                    display::g().fillRect(px + 6, kPlayerY, 4, 2,
                                          display::kInfo);
                    display::g().fillRect(px + 3, kPlayerY + 2, 10, 3,
                                          display::kInfo);
                    display::g().fillRect(px, kPlayerY + 5, kPlayerW, 3,
                                          display::kAccent);
                }

                for (const Bullet& shot : playerShots) {
                    if (shot.alive) {
                        display::g().fillRect(
                            static_cast<int>(shot.x) - 1,
                            static_cast<int>(shot.y), 2, 5,
                            display::kAccent);
                    }
                }
                for (const Bullet& shot : enemyShots) {
                    if (shot.alive) {
                        display::g().fillRect(
                            static_cast<int>(shot.x) - 1,
                            static_cast<int>(shot.y), 2, 5,
                            display::kWarning);
                    }
                }

                if (gameOver) {
                    drawOverlay(score,
                                std::max(best,
                                         static_cast<uint32_t>(score)));
                }

                display::push();
                renderedAt = now;
            }

            delay(8);
        }

        navigation::waitRelease();
    }
}

}  // namespace games

#endif  // ENABLE_MODULE_GAMES
