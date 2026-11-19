#include "Lander.h"

#include <Arduino.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Display.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "Scores.h"

#if ENABLE_MODULE_GAMES

namespace games {
namespace {

constexpr int kScreenW = 240;
constexpr int kScreenH = 135;
constexpr int kHudH = 18;
constexpr int kPlayTop = 24;
constexpr int kRadius = 4;

// Terreno: una quota ogni 2 px, cosi' 3 piattaforme restano definite.
constexpr int kCols = 120;
constexpr int kColStep = 2;
constexpr int kPadCount = 3;
constexpr float kWallMargin = 12.0f;

constexpr float kGravity = 52.0f;
constexpr float kThrust = 130.0f;
constexpr float kRotSpeed = 2.6f;
constexpr float kFuelMax = 100.0f;
constexpr float kBurnRate = 18.0f;
constexpr float kMaxSpeed = 110.0f;
constexpr float kLandVy = 30.0f;
constexpr float kLandVx = 16.0f;
constexpr float kLandAngle = 0.25f;
constexpr float kPi = 3.14159265f;
constexpr float kDeg = 57.29578f;

struct Pad {
    int start = 0;  // prima colonna del pianale
    int width = 0;  // colonne piatte
    int y = 110;    // quota della superficie
};

struct Ship {
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    float angle = 0.0f;  // 0 = muso in alto, positivo = orario
    float fuel = kFuelMax;
};

float clampf(float value, float lo, float hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

int iround(float value) {
    return static_cast<int>(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

uint8_t limitStep(uint8_t value, int reference) {
    return static_cast<uint8_t>(
        std::max(reference - 4, std::min(reference + 4, static_cast<int>(value))));
}

// Un solo tono ogni 100 ms: vale per atterraggio e schianto.
void beep(uint32_t& lastMs, uint32_t now, float freq, uint32_t ms) {
    if (now - lastMs < 100) {
        return;
    }
    M5Cardputer.Speaker.tone(freq, ms);
    lastMs = now;
}

void drawHud(const Ship& ship, int attemptsLeft, uint32_t best) {
    char buf[20];

    display::text(4, 1, "LANDER", display::kAccent, 0);
    display::text(46, 1, "FUEL", display::kTextDim, 0);

    constexpr int kBarX = 72;
    constexpr int kBarY = 2;
    constexpr int kBarW = 48;
    constexpr int kBarH = 8;
    display::g().drawRect(kBarX, kBarY, kBarW, kBarH, display::kTextDim);
    const uint16_t fuelColor = ship.fuel < 20.0f
                                   ? display::kDanger
                                   : (ship.fuel < 45.0f ? display::kWarning : display::kAccent);
    const int fillW = iround((kBarW - 4) * clampf(ship.fuel / kFuelMax, 0.0f, 1.0f));
    if (fillW > 0) {
        display::g().fillRect(kBarX + 2, kBarY + 2, fillW, kBarH - 4, fuelColor);
    }

    std::snprintf(buf, sizeof(buf), "%d%%", iround(ship.fuel));
    display::text(kBarX + kBarW + 3, 1, buf, fuelColor, 0);

    std::snprintf(buf, sizeof(buf), "T %d/3", attemptsLeft);
    display::text(152, 1, buf, display::kText, 0);

    std::snprintf(buf, sizeof(buf), "BEST %u",
                  static_cast<unsigned>(std::min<uint32_t>(best, 9999u)));
    display::text(kScreenW - 3 - display::textWidth(buf, 0), 1, buf, display::kInfo, 0);

    const bool risky = fabsf(ship.vx) > kLandVx || fabsf(ship.vy) > kLandVy ||
                       fabsf(ship.angle) > kLandAngle;
    uint16_t statColor = display::kTextDim;
    if (ship.y > 70.0f) {
        statColor = risky ? display::kDanger : display::kInfo;
    }
    std::snprintf(buf, sizeof(buf), "VY %+d", iround(ship.vy));
    display::text(4, 9, buf, statColor, 0);
    std::snprintf(buf, sizeof(buf), "VX %+d", iround(ship.vx));
    display::text(58, 9, buf, statColor, 0);
    std::snprintf(buf, sizeof(buf), "ANG %+d", iround(ship.angle * kDeg));
    display::text(112, 9, buf, statColor, 0);

    display::g().fillRect(0, kHudH - 1, kScreenW, 1, display::kPanel);
}

void drawTerrain(const uint8_t* heights, const uint8_t* padId, const Pad* pads, int target,
                 uint32_t now) {
    const bool blink = ((now / 320) & 1u) == 0;
    for (int i = 0; i < kCols; ++i) {
        const int x = i * kColStep;
        const int y = heights[i];
        const bool targetPad = padId[i] == target + 1;
        const bool anyPad = padId[i] != 0;

        uint16_t fill = display::kPanel;
        uint16_t top = display::kTextDim;
        if (targetPad) {
            fill = display::kAccentDark;
            top = blink ? display::kAccent : display::kAccentDark;
        } else if (anyPad) {
            top = display::kInfo;
        }
        display::g().fillRect(x, y, kColStep, kScreenH - y, fill);
        display::g().fillRect(x, y, kColStep, 1, top);
    }

    const Pad& pad = pads[target];
    const int cx = (pad.start + pad.width / 2) * kColStep;
    display::text(cx - display::textWidth("TARGET", 0) / 2, pad.y - 20, "TARGET",
                  blink ? display::kAccent : display::kTextDim, 0);
    display::g().fillRect(cx - 2, pad.y - 10, 4, 4,
                          blink ? display::kAccent : display::kAccentDark);
}

void drawShip(const Ship& ship, bool flame, uint32_t now) {
    const int cx = iround(ship.x);
    const int cy = iround(ship.y);
    const float sa = sinf(ship.angle);
    const float ca = cosf(ship.angle);
    const float dx = -sa;  // asse coda
    const float dy = ca;

    if (flame) {
        const float len = 7.0f + static_cast<float>((now / 70) % 3);
        display::g().drawLine(cx + iround(dx * 3.0f), cy + iround(dy * 3.0f),
                              cx + iround(dx * len), cy + iround(dy * len),
                              display::kWarning);
    }

    display::g().drawLine(cx - iround(ca * 2.0f), cy - iround(sa * 2.0f),
                          cx - iround(ca * 6.0f) + iround(dx * 4.0f),
                          cy - iround(sa * 6.0f) + iround(dy * 4.0f),
                          display::kTextDim);
    display::g().drawLine(cx + iround(ca * 2.0f), cy + iround(sa * 2.0f),
                          cx + iround(ca * 6.0f) + iround(dx * 4.0f),
                          cy + iround(sa * 6.0f) + iround(dy * 4.0f),
                          display::kTextDim);
    display::g().fillCircle(cx, cy, 3, display::kText);
    display::g().fillCircle(cx + iround(sa * 6.0f), cy - iround(ca * 6.0f), 1,
                            display::kAccent);
}

void drawExplosion(float x, float y, uint32_t now) {
    const int cx = iround(x);
    const int cy = iround(y);
    const int radius = 5 + static_cast<int>((now / 90) % 4);
    display::g().fillCircle(cx, cy, radius, display::kWarning);
    display::g().fillCircle(cx, cy, radius / 2, display::kDanger);
    for (int i = 0; i < 4; ++i) {
        const float a = (static_cast<float>(i) * kPi) / 2.0f + 0.35f;
        display::g().drawLine(
            cx + iround(cosf(a) * static_cast<float>(radius + 1)),
            cy + iround(sinf(a) * static_cast<float>(radius + 1)),
            cx + iround(cosf(a) * static_cast<float>(radius + 3)),
            cy + iround(sinf(a) * static_cast<float>(radius + 3)), display::kWarning);
    }
}

void drawOverlay(const char* title, uint16_t color, const char* line1, const char* line2) {
    const int w = 200;
    const int h = 74;
    const int x = (kScreenW - w) / 2;
    const int y = (kScreenH - h) / 2;
    display::g().fillRect(x, y, w, h, display::kBlack);
    display::g().drawRect(x, y, w, h, color);
    display::textCentered(y + 6, title, color, 4);
    display::textCentered(y + 28, line1, display::kText, 0);
    display::textCentered(y + 38, line2, display::kTextDim, 0);
    display::textCentered(y + 52, "INVIO=riprova", display::kText, 0);
    display::textCentered(y + 61, "ESC=esci", display::kTextDim, 0);
}

}  // namespace

void runLander() {
    static uint8_t heights[kCols];
    static uint8_t padId[kCols];
    static Pad pads[kPadCount];

    uint32_t best = scores::best("lander");
    navigation::waitRelease();

    for (;;) {
        // Profilo del suolo: passeggiata casuale dolce, poi 3 pianali piatti.
        int level = 112;
        for (int i = 0; i < kCols; ++i) {
            level += static_cast<int>(esp_random() % 5) - 2;
            level = std::max(100, std::min(126, level));
            heights[i] = static_cast<uint8_t>(level);
            padId[i] = 0;
        }

        constexpr int zoneStart[kPadCount] = {6, 44, 86};
        constexpr int zoneWidth[kPadCount] = {28, 34, 28};
        for (int p = 0; p < kPadCount; ++p) {
            const int width = 12 + static_cast<int>(esp_random() % 5);
            const int start =
                zoneStart[p] + static_cast<int>(esp_random() % (zoneWidth[p] - width));
            const int y = 104 + static_cast<int>(esp_random() % 15);
            pads[p] = {start, width, y};
            for (int c = start; c < start + width; ++c) {
                heights[c] = static_cast<uint8_t>(y);
                padId[c] = static_cast<uint8_t>(p + 1);
            }
        }

        // Raccorda i bordi dei pianali senza toccarli: niente pareti verticali.
        for (int i = 1; i < kCols; ++i) {
            if (padId[i] == 0) {
                heights[i] = limitStep(heights[i], heights[i - 1]);
            }
        }
        for (int i = kCols - 2; i >= 0; --i) {
            if (padId[i] == 0) {
                heights[i] = limitStep(heights[i], heights[i + 1]);
            }
        }

        const int target = static_cast<int>(esp_random() % kPadCount);
        const Pad& targetPad = pads[target];
        const int targetX = (targetPad.start + targetPad.width / 2) * kColStep;

        enum class Phase { Fly, Crash, Over, Win };
        Phase phase = Phase::Fly;
        int attemptsLeft = 3;
        uint32_t score = 0;
        uint32_t lastTone = 0;
        const uint32_t gameStart = millis();

        Ship ship;
        const auto resetShip = [&] {
            const int jitter = static_cast<int>(esp_random() % 81) - 40;
            ship.x = clampf(static_cast<float>(targetX + jitter), 20.0f, 220.0f);
            ship.y = 34.0f;
            ship.vx = (static_cast<float>(esp_random() % 1201) - 600.0f) * 0.01f;
            ship.vy = 6.0f;
            ship.angle = (static_cast<float>(esp_random() % 2001) - 1000.0f) * 0.0001f;
            ship.fuel = kFuelMax;
        };
        resetShip();

        uint32_t lastFrame = millis();
        for (;;) {
            navigation::update();
            const navigation::EventData ev = navigation::poll();
            if (ev.type == navigation::Event::Back) {
                return;
            }
            const uint32_t now = millis();
            bool thrusting = false;

            if (phase == Phase::Fly) {
                float dt = static_cast<float>(now - lastFrame) * 0.001f;
                lastFrame = now;
                dt = clampf(dt, 0.0f, 0.05f);

                float turn = 0.0f;
                if (ev.type == navigation::Event::Left) {
                    turn -= 0.08f;  // scatto per tap, oltre alla rotazione continua
                } else if (ev.type == navigation::Event::Right) {
                    turn += 0.08f;
                }
                const bool left = navigation::pressed(',');
                const bool right = navigation::pressed('/');
                if (left != right) {
                    turn += (right ? kRotSpeed : -kRotSpeed) * dt;
                }
                ship.angle += turn;
                if (ship.angle > kPi) {
                    ship.angle -= 2.0f * kPi;
                } else if (ship.angle < -kPi) {
                    ship.angle += 2.0f * kPi;
                }

                thrusting = (navigation::pressed(';') || ev.type == navigation::Event::Up) &&
                            ship.fuel > 0.0f;
                if (thrusting) {
                    ship.fuel = std::max(0.0f, ship.fuel - kBurnRate * dt);
                    ship.vx += sinf(ship.angle) * kThrust * dt;
                    ship.vy -= cosf(ship.angle) * kThrust * dt;
                }

                ship.vy += kGravity * dt;
                ship.vx = clampf(ship.vx, -kMaxSpeed, kMaxSpeed);
                ship.vy = clampf(ship.vy, -kMaxSpeed, kMaxSpeed);

                ship.x += ship.vx * dt;
                ship.y += ship.vy * dt;

                if (ship.x < kWallMargin) {
                    ship.x = kWallMargin;
                    ship.vx = fabsf(ship.vx) * 0.3f;
                } else if (ship.x > kScreenW - 1.0f - kWallMargin) {
                    ship.x = kScreenW - 1.0f - kWallMargin;
                    ship.vx = -fabsf(ship.vx) * 0.3f;
                }
                if (ship.y < kPlayTop) {
                    ship.y = kPlayTop;
                    ship.vy = fabsf(ship.vy) * 0.3f;
                }

                int col = static_cast<int>(ship.x) / kColStep;
                col = std::max(0, std::min(kCols - 1, col));
                if (ship.y + kRadius >= static_cast<float>(heights[col])) {
                    ship.y = static_cast<float>(heights[col] - kRadius);
                    const bool onTarget =
                        padId[col] == target + 1 && fabsf(ship.vx) <= kLandVx &&
                        fabsf(ship.vy) <= kLandVy && fabsf(ship.angle) <= kLandAngle;
                    if (onTarget) {
                        const float quality =
                            0.45f * (1.0f - std::max(fabsf(ship.vx) / kLandVx,
                                                     fabsf(ship.vy) / kLandVy)) +
                            0.35f * (1.0f - fabsf(ship.angle) / kLandAngle) +
                            0.20f * (1.0f - std::min(1.0f,
                                                     fabsf(ship.x - targetX) /
                                                         static_cast<float>(targetPad.width)));
                        score = static_cast<uint32_t>(
                            1000 + iround(ship.fuel * 3.0f) +
                            iround(500.0f * clampf(quality, 0.0f, 1.0f)));
                        scores::submit("lander", score);
                        best = std::max(best, score);
                        ship.vx = 0.0f;
                        ship.vy = 0.0f;
                        phase = Phase::Win;
                        beep(lastTone, now, 880.0f, 160);
                    } else {
                        --attemptsLeft;
                        phase = attemptsLeft > 0 ? Phase::Crash : Phase::Over;
                        beep(lastTone, now, 90.0f, 240);
                    }
                }
            } else {
                lastFrame = now;
                if (ev.type == navigation::Event::Select) {
                    navigation::waitRelease();
                    if (phase == Phase::Crash) {
                        resetShip();
                        phase = Phase::Fly;
                        lastFrame = millis();
                    } else {
                        break;  // nuova partita, nuovo terreno
                    }
                }
            }

            display::clear(display::kBackground);
            drawHud(ship, attemptsLeft, best);
            drawTerrain(heights, padId, pads, target, now);
            if (phase == Phase::Crash || phase == Phase::Over) {
                drawExplosion(ship.x, ship.y, now);
            } else if (phase == Phase::Fly) {
                drawShip(ship, thrusting, now);
            } else {
                drawShip(ship, false, now);
            }

            if (phase == Phase::Fly) {
                if (now - gameStart < 3000) {
                    display::text(6, 19, "SPINTA ;", display::kTextDim, 0);
                    display::text(60, 19, "RUOTA , /", display::kTextDim, 0);
                }
            } else {
                char line1[24];
                char line2[24];
                const char* title = "";
                uint16_t color = display::kDanger;
                if (phase == Phase::Win) {
                    title = "ATTERRATO!";
                    color = display::kAccent;
                    std::snprintf(line1, sizeof(line1), "PUNTI %u",
                                  static_cast<unsigned>(score));
                    std::snprintf(line2, sizeof(line2), "BONUS %u",
                                  static_cast<unsigned>(score - 1000u));
                } else if (phase == Phase::Crash) {
                    title = "SCHIANTO!";
                    std::snprintf(line1, sizeof(line1), "TENTATIVI: %d", attemptsLeft);
                    std::snprintf(line2, sizeof(line2), "VAI AL TARGET");
                } else {
                    title = "GAME OVER";
                    std::snprintf(line1, sizeof(line1), "TENTATIVI: 0");
                    std::snprintf(line2, sizeof(line2), "PUNTI 0");
                }
                drawOverlay(title, color, line1, line2);
            }

            display::push();
            delay(phase == Phase::Fly ? 10 : 20);
        }
    }
}

}  // namespace games

#endif  // ENABLE_MODULE_GAMES
