#include "Flappy.h"

#include <Arduino.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "Display.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "Scores.h"

#if ENABLE_MODULE_GAMES

namespace games {
namespace {

constexpr int kGroundY = 116;
constexpr float kBirdX = 52.0f;
constexpr float kBirdR = 6.0f;
constexpr float kGravity = 470.0f;
constexpr float kFlap = -168.0f;
constexpr float kPipeW = 24.0f;
constexpr float kPipeGap = 50.0f;
constexpr float kSpeed = 72.0f;

struct Pipe {
    float x = 0;
    int gapY = 30;
    bool scored = false;
};

void drawOverlay(const std::string& title, const std::string& line, uint16_t color) {
    auto& d = display::g();
    const int w = 166;
    const int h = 48;
    const int x = (display::width() - w) / 2;
    const int y = (display::height() - h) / 2;
    d.fillRect(x + 3, y + 3, w, h, display::kInk);
    d.fillRect(x, y, w, h, display::kPaper);
    d.drawRect(x, y, w, h, color);
    display::textCentered(y + 7, title, color, 2);
    display::textCentered(y + 24, line, display::kInk, 0);
    display::textCentered(y + 35, "INVIO=riprova  `=esci", display::kMuted, 0);
}

void drawBird(float y, float vy) {
    auto& d = display::g();
    const int x = static_cast<int>(kBirdX);
    const int iy = static_cast<int>(y);
    d.fillCircle(x, iy, static_cast<int>(kBirdR), display::kNeoYellow);
    d.drawCircle(x, iy, static_cast<int>(kBirdR), display::kInk);
    const int wingY = iy + (vy < 0 ? 2 : 1);
    d.fillTriangle(x - 4, wingY, x + 1, wingY - 3, x + 2, wingY + 3,
                   display::kNeoOrange);
    d.fillCircle(x + 3, iy - 2, 1, display::kInk);
    d.fillTriangle(x + 5, iy, x + 10, iy + 1, x + 5, iy + 3, display::kNeoRed);
}

}  // namespace

void runFlappy() {
    uint32_t best = scores::best("flappy");
    navigation::waitRelease();

    for (;;) {
        float birdY = 68.0f;
        float vy = 0.0f;
        bool started = false;
        bool dead = false;
        bool spaceHeld = false;
        int score = 0;
        std::vector<Pipe> pipes;
        pipes.push_back({250.0f, 24 + static_cast<int>(esp_random() % 34), false});
        pipes.push_back({340.0f, 24 + static_cast<int>(esp_random() % 34), false});
        uint32_t lastFrame = millis();

        for (;;) {
            navigation::update();
            const navigation::EventData ev = navigation::poll();
            if (ev.type == navigation::Event::Back) {
                return;
            }
            if (dead) {
                if (ev.type == navigation::Event::Select) {
                    break;
                }
                delay(20);
                continue;
            }

            const bool space = navigation::pressed(' ');
            const bool flap = ev.type == navigation::Event::Select || (space && !spaceHeld);
            spaceHeld = space;

            if (!started) {
                if (flap) {
                    started = true;
                    vy = kFlap;
                    lastFrame = millis();
                } else {
                    display::clear(display::kBackground);
                    display::text(6, 3, "FLAPPY", display::kAccent, 2);
                    display::text(92, 5, "PUNTI 0", display::kText, 0);
                    display::text(174, 5, "RECORD " + std::to_string(best),
                                  display::kInfo, 0);
                    display::g().fillRect(0, kGroundY, display::width(),
                                          display::height() - kGroundY,
                                          display::kAccentDark);
                    for (const auto& pipe : pipes) {
                        display::g().fillRect(static_cast<int>(pipe.x), 22,
                                              static_cast<int>(kPipeW),
                                              pipe.gapY - 22, display::kNeoGreen);
                        display::g().fillRect(static_cast<int>(pipe.x),
                                              pipe.gapY + static_cast<int>(kPipeGap),
                                              static_cast<int>(kPipeW),
                                              kGroundY - pipe.gapY -
                                                  static_cast<int>(kPipeGap),
                                              display::kNeoGreen);
                    }
                    drawBird(birdY, 0);
                    display::textCentered(92, "INVIO = vola", display::kNeoYellow, 0);
                    display::push();
                    delay(20);
                    continue;
                }
            }

            if (flap) {
                vy = kFlap;
            }

            const uint32_t now = millis();
            float dt = (now - lastFrame) / 1000.0f;
            lastFrame = now;
            dt = std::min(dt, 0.035f);

            vy += kGravity * dt;
            birdY += vy * dt;

            for (auto& pipe : pipes) {
                pipe.x -= kSpeed * dt;
                if (!pipe.scored && pipe.x + kPipeW < kBirdX) {
                    pipe.scored = true;
                    ++score;
                }
            }

            while (pipes.back().x < 260.0f) {
                Pipe next;
                next.x = pipes.back().x + 92.0f;
                next.gapY = 24 + static_cast<int>(esp_random() % 34);
                pipes.push_back(next);
                if (pipes.size() > 8) {
                    break;
                }
            }
            pipes.erase(std::remove_if(pipes.begin(), pipes.end(),
                                       [](const Pipe& p) { return p.x + kPipeW < -8.0f; }),
                        pipes.end());

            if (birdY - kBirdR < 22.0f) {
                birdY = 22.0f + kBirdR;
                vy = 0.0f;
            }
            if (birdY + kBirdR >= kGroundY) {
                dead = true;
            }
            for (const auto& pipe : pipes) {
                if (kBirdX + kBirdR > pipe.x && kBirdX - kBirdR < pipe.x + kPipeW &&
                    (birdY - kBirdR < pipe.gapY ||
                     birdY + kBirdR > pipe.gapY + kPipeGap)) {
                    dead = true;
                }
            }
            if (dead) {
                scores::submit("flappy", static_cast<uint32_t>(score));
                best = std::max(best, static_cast<uint32_t>(score));
            }

            display::clear(display::kBackground);
            display::text(6, 3, "FLAPPY", display::kAccent, 2);
            display::text(92, 5, "PUNTI " + std::to_string(score), display::kText, 0);
            display::text(174, 5, "RECORD " + std::to_string(best), display::kInfo, 0);

            for (const auto& pipe : pipes) {
                const int x = static_cast<int>(pipe.x);
                display::g().fillRect(x, 22, static_cast<int>(kPipeW),
                                      pipe.gapY - 22, display::kNeoGreen);
                display::g().drawRect(x, 22, static_cast<int>(kPipeW),
                                      pipe.gapY - 22, display::kInk);
                display::g().fillRect(x, pipe.gapY - 5, static_cast<int>(kPipeW),
                                      5, display::kAccentDark);
                display::g().fillRect(x, pipe.gapY + static_cast<int>(kPipeGap),
                                      static_cast<int>(kPipeW),
                                      kGroundY - pipe.gapY -
                                          static_cast<int>(kPipeGap),
                                      display::kNeoGreen);
                display::g().drawRect(x, pipe.gapY + static_cast<int>(kPipeGap),
                                      static_cast<int>(kPipeW),
                                      kGroundY - pipe.gapY -
                                          static_cast<int>(kPipeGap),
                                      display::kInk);
                display::g().fillRect(x, pipe.gapY + static_cast<int>(kPipeGap),
                                      static_cast<int>(kPipeW), 5,
                                      display::kAccentDark);
            }

            display::g().fillRect(0, kGroundY, display::width(),
                                  display::height() - kGroundY,
                                  display::kAccentDark);
            display::g().drawFastHLine(0, kGroundY, display::width(), display::kInk);
            drawBird(birdY, vy);
            if (dead) {
                drawOverlay("GAME OVER", "Punti: " + std::to_string(score),
                            display::kDanger);
            } else if (!started) {
                display::textCentered(92, "INVIO = vola", display::kNeoYellow, 0);
            }
            display::text(150, 121, "INVIO = vola", display::kPaperLight, 0);
            display::push();
            delay(16);
        }
        navigation::waitRelease();
    }
}

}  // namespace games

#endif  // ENABLE_MODULE_GAMES
