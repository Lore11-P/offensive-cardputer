#include "Breakout.h"

#include <Arduino.h>

#include <algorithm>
#include <cmath>

#include "Display.h"
#include "GamesMenu.h"
#include "ModuleFlags.h"
#include "Navigation.h"

#if ENABLE_MODULE_GAMES

namespace games {
namespace {

constexpr int kFieldX = 5;
constexpr int kFieldY = 16;
constexpr int kFieldW = 230;
constexpr int kFieldH = 116;

constexpr int kPaddleW = 40;
constexpr int kPaddleH = 5;
constexpr int kPaddleY = 123;

constexpr int kBallSize = 4;

constexpr int kCols = 8;
constexpr int kRows = 4;
constexpr int kBrickW = 27;
constexpr int kBrickH = 7;
constexpr int kBrickGapX = 2;
constexpr int kBrickGapY = 3;
constexpr int kBrickX = kFieldX + 1;
constexpr int kBrickY = kFieldY + 6;

constexpr uint16_t kBrickColors[kRows] = {
    display::kDanger, display::kWarning, display::kInfo, display::kAccent};

struct Brick {
    bool alive = true;
};

Brick g_bricks[kRows][kCols];

int brickX(int col) { return kBrickX + col * (kBrickW + kBrickGapX); }
int brickY(int row) { return kBrickY + row * (kBrickH + kBrickGapY); }

void resetBricks() {
    for (auto& row : g_bricks) {
        for (auto& brick : row) {
            brick.alive = true;
        }
    }
}

int aliveBricks() {
    int count = 0;
    for (auto& row : g_bricks) {
        for (auto& brick : row) {
            count += brick.alive ? 1 : 0;
        }
    }
    return count;
}

void drawOverlay(const char* title, uint16_t color, const char* hint) {
    const int w = 170;
    const int h = 46;
    const int x = (display::width() - w) / 2;
    const int y = (display::height() - h) / 2;
    display::g().fillRect(x, y, w, h, display::kBlack);
    display::g().drawRect(x, y, w, h, color);
    display::textCentered(y + 9, title, color, 2);
    display::textCentered(y + 28, hint, display::kTextDim, 0);
}

}  // namespace

void runBreakout() {
    resetBricks();

    float paddleX = (kFieldW - kPaddleW) / 2.0f + kFieldX;
    float ballX = paddleX + kPaddleW / 2.0f;
    float ballY = kPaddleY - kBallSize;
    float vx = 0.0f;
    float vy = 0.0f;
    float speed = 92.0f;
    bool attached = true;
    bool gameOver = false;
    bool won = false;
    int score = 0;
    int lives = 3;
    uint32_t lastFrame = millis();
    uint32_t renderedAt = 0;

    navigation::waitRelease();

    auto resetBall = [&] {
        paddleX = (kFieldW - kPaddleW) / 2.0f + kFieldX;
        ballX = paddleX + kPaddleW / 2.0f;
        ballY = kPaddleY - kBallSize;
        vx = 0.0f;
        vy = 0.0f;
        attached = true;
        speed = 92.0f;
    };

    auto launch = [&] {
        if (!attached) {
            return;
        }
        attached = false;
        const float angle = (static_cast<float>(esp_random() % 61) - 30.0f) *
                            3.14159f / 180.0f;
        vx = speed * sinf(angle);
        vy = -speed * cosf(angle);
        if (fabsf(vy) < 45.0f) {
            vy = -45.0f;
        }
    };

    while (true) {
        navigation::update();
        const navigation::EventData ev = navigation::poll();
        const uint32_t now = millis();

        if (ev.type == navigation::Event::Back) {
            break;
        }

        if (gameOver || won) {
            if (ev.type == navigation::Event::Select) {
                resetBricks();
                resetBall();
                score = 0;
                lives = 3;
                gameOver = false;
                won = false;
                lastFrame = now;
            }
            if (now - renderedAt > 60) {
                drawOverlay(won ? "HAI VINTO!" : "GAME OVER",
                            won ? display::kAccent : display::kDanger,
                            "INVIO = riprova  ` = esci");
                display::push();
                renderedAt = now;
            }
            delay(10);
            continue;
        }

        float dt = (now - lastFrame) / 1000.0f;
        lastFrame = now;
        dt = std::min(dt, 0.034f);

        const bool left = navigation::pressed(',');
        const bool right = navigation::pressed('/');
        if (left) {
            paddleX -= 170.0f * dt;
        }
        if (right) {
            paddleX += 170.0f * dt;
        }
        paddleX = std::max(static_cast<float>(kFieldX),
                           std::min(paddleX, static_cast<float>(kFieldX + kFieldW -
                                                                kPaddleW)));

        if (attached) {
            ballX = paddleX + kPaddleW / 2.0f;
            ballY = kPaddleY - kBallSize;
        }

        if (ev.type == navigation::Event::Select || navigation::enterPressed()) {
            launch();
        }

        if (!attached) {
            bool hitVertical = false;
            bool hitHorizontal = false;

            ballX += vx * dt;
            ballY += vy * dt;

            if (ballX - kBallSize / 2.0f < kFieldX) {
                ballX = kFieldX + kBallSize / 2.0f;
                vx = fabsf(vx);
                hitVertical = true;
            } else if (ballX + kBallSize / 2.0f > kFieldX + kFieldW) {
                ballX = kFieldX + kFieldW - kBallSize / 2.0f;
                vx = -fabsf(vx);
                hitVertical = true;
            }
            if (ballY - kBallSize / 2.0f < kFieldY) {
                ballY = kFieldY + kBallSize / 2.0f;
                vy = fabsf(vy);
                hitHorizontal = true;
            }

            // Rimbalzo sul paddle.
            if (vy > 0.0f && ballY + kBallSize / 2.0f >= kPaddleY &&
                ballY - kBallSize / 2.0f <= kPaddleY + kPaddleH &&
                ballX + kBallSize / 2.0f >= paddleX &&
                ballX - kBallSize / 2.0f <= paddleX + kPaddleW) {
                ballY = kPaddleY - kBallSize / 2.0f;
                const float offset =
                    (ballX - (paddleX + kPaddleW / 2.0f)) / (kPaddleW / 2.0f);
                const float angle = offset * 60.0f * 3.14159f / 180.0f;
                vx = speed * sinf(angle);
                vy = -speed * cosf(angle);
                if (fabsf(vy) < 42.0f) {
                    vy = -42.0f;
                }
            }

            // Collisione mattoni: un solo blocco per frame.
            for (int row = 0; row < kRows && !hitHorizontal && !hitVertical;
                 ++row) {
                for (int col = 0; col < kCols; ++col) {
                    if (!g_bricks[row][col].alive) {
                        continue;
                    }
                    const float bx = static_cast<float>(brickX(col));
                    const float by = static_cast<float>(brickY(row));
                    if (ballX + kBallSize / 2.0f < bx ||
                        ballX - kBallSize / 2.0f > bx + kBrickW ||
                        ballY + kBallSize / 2.0f < by ||
                        ballY - kBallSize / 2.0f > by + kBrickH) {
                        continue;
                    }
                    const float overlapX =
                        std::min(ballX + kBallSize / 2.0f, bx + kBrickW) -
                        std::max(ballX - kBallSize / 2.0f, bx);
                    const float overlapY =
                        std::min(ballY + kBallSize / 2.0f, by + kBrickH) -
                        std::max(ballY - kBallSize / 2.0f, by);
                    if (overlapX < overlapY) {
                        vx = -vx;
                        hitVertical = true;
                    } else {
                        vy = -vy;
                        hitHorizontal = true;
                    }
                    g_bricks[row][col].alive = false;
                    score += 10;
                    speed = std::min(150.0f, speed + 2.5f);
                    if (aliveBricks() == 0) {
                        won = true;
                    }
                    break;
                }
            }

            if (ballY - kBallSize / 2.0f > display::height()) {
                --lives;
                if (lives <= 0) {
                    gameOver = true;
                } else {
                    resetBall();
                }
            }
        }

        if (now - renderedAt > 16) {
            display::clear(display::kBlack);
            display::g().fillRect(kFieldX, kFieldY, kFieldW, kFieldH,
                                  display::kBackground);
            display::g().drawRect(kFieldX - 1, kFieldY - 1, kFieldW + 2,
                                  kFieldH + 2, display::kAccentDark);

            display::text(5, 3, "PUNTI " + std::to_string(score),
                          display::kText, 0);
            display::text(96, 3, "VITE " + std::to_string(std::max(lives, 0)),
                          display::kText, 0);
            display::text(160, 3, "PALLA " + std::to_string(static_cast<int>(speed)),
                          display::kTextDim, 0);

            for (int row = 0; row < kRows; ++row) {
                for (int col = 0; col < kCols; ++col) {
                    if (!g_bricks[row][col].alive) {
                        continue;
                    }
                    display::g().fillRect(brickX(col), brickY(row), kBrickW,
                                          kBrickH, kBrickColors[row]);
                    display::g().drawRect(brickX(col), brickY(row), kBrickW,
                                          kBrickH, display::kBlack);
                }
            }

            display::g().fillRect(static_cast<int>(paddleX), kPaddleY, kPaddleW,
                                  kPaddleH, display::kAccent);
            display::g().fillRect(static_cast<int>(ballX) - kBallSize / 2,
                                  static_cast<int>(ballY) - kBallSize / 2,
                                  kBallSize, kBallSize, display::kWhite);

            display::text(5, 124, "`, / muovi   SPC lancia   ` esci",
                          display::kTextDim, 0);
            display::push();
            renderedAt = now;
        }

        delay(6);
    }
}

}  // namespace games

#endif  // ENABLE_MODULE_GAMES
