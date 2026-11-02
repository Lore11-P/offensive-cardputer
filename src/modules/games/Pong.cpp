#include "Pong.h"

#include <Arduino.h>

#include <algorithm>
#include <cmath>

#include "Display.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "Scores.h"

#if ENABLE_MODULE_GAMES

namespace games {
namespace {

constexpr int kFieldX = 5;
constexpr int kFieldY = 16;
constexpr int kFieldW = 230;
constexpr int kFieldH = 114;
constexpr int kPaddleW = 5;
constexpr int kPaddleH = 30;
constexpr int kBall = 4;
constexpr int kWinScore = 7;

void drawOverlay(const char* title, uint16_t color, const char* hint) {
    const int w = 176;
    const int h = 46;
    const int x = (display::width() - w) / 2;
    const int y = (display::height() - h) / 2;
    display::g().fillRect(x, y, w, h, display::kBlack);
    display::g().drawRect(x, y, w, h, color);
    display::textCentered(y + 8, title, color, 2);
    display::textCentered(y + 28, hint, display::kTextDim, 0);
}

}  // namespace

void runPong() {
    navigation::waitRelease();
    uint32_t best = scores::best("pong");

    for (;;) {
        float playerY = (kFieldH - kPaddleH) / 2.0f + kFieldY;
        float aiY = playerY;
        float ballX = kFieldX + kFieldW / 2.0f;
        float ballY = kFieldY + kFieldH / 2.0f;
        float vx = 0.0f;
        float vy = 0.0f;
        int playerScore = 0;
        int aiScore = 0;
        bool finished = false;
        bool won = false;
        uint32_t serveAt = millis() + 600;
        uint32_t lastFrame = millis();

        while (true) {
            navigation::update();
            const navigation::EventData ev = navigation::poll();
            const uint32_t now = millis();
            if (ev.type == navigation::Event::Back) {
                return;
            }

            if (finished) {
                if (ev.type == navigation::Event::Select) {
                    break;
                }
                delay(20);
                continue;
            }

            float dt = (now - lastFrame) / 1000.0f;
            lastFrame = now;
            dt = std::min(dt, 0.034f);

            if (navigation::pressed(';')) {
                playerY -= 190.0f * dt;
            }
            if (navigation::pressed('.')) {
                playerY += 190.0f * dt;
            }
            playerY = std::max(static_cast<float>(kFieldY),
                               std::min(playerY, static_cast<float>(kFieldY + kFieldH - kPaddleH)));

            const float aiTarget = ballY - kPaddleH / 2.0f;
            const float aiSpeed = 118.0f;
            if (aiTarget < aiY) {
                aiY = std::max(aiY - aiSpeed * dt, aiTarget);
            } else {
                aiY = std::min(aiY + aiSpeed * dt, aiTarget);
            }
            aiY = std::max(static_cast<float>(kFieldY),
                           std::min(aiY, static_cast<float>(kFieldY + kFieldH - kPaddleH)));

            if (vx == 0.0f && vy == 0.0f && now >= serveAt) {
                const float angle = (static_cast<float>(esp_random() % 61) - 30.0f) *
                                    3.14159f / 180.0f;
                vx = (esp_random() & 1 ? 1.0f : -1.0f) * 105.0f * cosf(angle);
                vy = 105.0f * sinf(angle);
            }

            ballX += vx * dt;
            ballY += vy * dt;

            if (ballY <= kFieldY) {
                ballY = kFieldY;
                vy = fabsf(vy);
            } else if (ballY + kBall >= kFieldY + kFieldH) {
                ballY = kFieldY + kFieldH - kBall;
                vy = -fabsf(vy);
            }

            const float playerPaddleX = kFieldX + 5;
            const float aiPaddleX = kFieldX + kFieldW - 10;
            if (vx < 0.0f && ballX <= playerPaddleX + kPaddleW && ballX + kBall >= playerPaddleX &&
                ballY + kBall >= playerY && ballY <= playerY + kPaddleH) {
                ballX = playerPaddleX + kPaddleW;
                const float offset = (ballY + kBall / 2.0f - (playerY + kPaddleH / 2.0f)) /
                                     (kPaddleH / 2.0f);
                const float speed = std::min(175.0f, sqrtf(vx * vx + vy * vy) + 6.0f);
                vx = fabsf(speed * 0.92f);
                vy = speed * offset * 0.75f;
            }
            if (vx > 0.0f && ballX + kBall >= aiPaddleX && ballX <= aiPaddleX + kPaddleW &&
                ballY + kBall >= aiY && ballY <= aiY + kPaddleH) {
                ballX = aiPaddleX - kBall;
                const float offset = (ballY + kBall / 2.0f - (aiY + kPaddleH / 2.0f)) /
                                     (kPaddleH / 2.0f);
                const float speed = std::min(175.0f, sqrtf(vx * vx + vy * vy) + 5.0f);
                vx = -fabsf(speed * 0.92f);
                vy = speed * offset * 0.75f;
            }

            if (ballX + kBall < kFieldX) {
                ++aiScore;
                ballX = kFieldX + kFieldW / 2.0f;
                ballY = kFieldY + kFieldH / 2.0f;
                vx = 0.0f;
                vy = 0.0f;
                serveAt = now + 600;
            } else if (ballX > kFieldX + kFieldW) {
                ++playerScore;
                ballX = kFieldX + kFieldW / 2.0f;
                ballY = kFieldY + kFieldH / 2.0f;
                vx = 0.0f;
                vy = 0.0f;
                serveAt = now + 600;
            }

            if (playerScore >= kWinScore || aiScore >= kWinScore) {
                finished = true;
                won = playerScore >= kWinScore;
                if (won) {
                    scores::submit("pong", static_cast<uint32_t>(playerScore));
                    best = std::max(best, static_cast<uint32_t>(playerScore));
                }
            }

            display::clear(display::kBackground);
            display::g().drawRect(kFieldX, kFieldY, kFieldW, kFieldH, display::kAccent);
            display::g().drawFastVLine(kFieldX + kFieldW / 2, kFieldY + 1, kFieldH - 2,
                                       display::kPanel);
            display::text(8, 3, "TU " + std::to_string(playerScore), display::kText, 0);
            display::text(174, 3, std::to_string(aiScore) + " CPU", display::kText, 0);
            display::text(96, 3, "RECORD " + std::to_string(best), display::kInfo, 0);
            display::g().fillRect(static_cast<int>(playerPaddleX), static_cast<int>(playerY),
                                  kPaddleW, kPaddleH, display::kAccent);
            display::g().fillRect(static_cast<int>(aiPaddleX), static_cast<int>(aiY), kPaddleW,
                                  kPaddleH, display::kDanger);
            display::g().fillRect(static_cast<int>(ballX), static_cast<int>(ballY), kBall, kBall,
                                  display::kText);
            if (finished) {
                drawOverlay(won ? "HAI VINTO!" : "CPU VINCE",
                            won ? display::kAccent : display::kDanger,
                            "INVIO = riprova   ` = esci");
            }
            display::push();
            delay(8);
        }
        navigation::waitRelease();
    }
}

}  // namespace games

#endif  // ENABLE_MODULE_GAMES
