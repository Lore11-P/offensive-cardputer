#include "Orbital.h"

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

constexpr float kPi = 3.14159265f;
constexpr int kHudH = 15;
constexpr int kPlanetR = 18;
constexpr int kMaxHp = 5;
constexpr int kMaxAsteroids = 14;
constexpr int kMaxBullets = 10;
constexpr float kTurnSpeed = 3.2f;      // rad/s
constexpr float kBulletSpeed = 200.0f;  // px/s
constexpr uint32_t kFireCooldownMs = 200;
constexpr uint32_t kWaveMs = 20000;

struct Asteroid {
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    float r = 4.0f;
    int8_t hits = 1;
    int8_t points = 10;
    bool alive = false;
};

struct Bullet {
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    bool alive = false;
};

float planetX() { return display::width() * 0.5f; }

float planetY() { return kHudH + (display::height() - kHudH) * 0.5f; }

float rndRange(float a, float b) {
    return a + (b - a) * static_cast<float>(esp_random() % 1000) / 1000.0f;
}

void beep(float freq, uint32_t ms, uint32_t now, uint32_t& lastSound) {
    if (now - lastSound >= 80) {
        M5Cardputer.Speaker.tone(freq, ms);
        lastSound = now;
    }
}

void spawnAsteroid(Asteroid* rocks, int wave) {
    int slot = -1;
    for (int i = 0; i < kMaxAsteroids; ++i) {
        if (!rocks[i].alive) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return;
    }

    const float w = static_cast<float>(display::width());
    const float h = static_cast<float>(display::height());
    Asteroid& a = rocks[slot];
    a.r = static_cast<float>(4 + esp_random() % 5);

    // Punto di partenza sul bordo, appena dentro lo schermo.
    const int edge = static_cast<int>(esp_random() % 4);
    if (edge == 0) {
        a.x = rndRange(a.r, w - a.r);
        a.y = kHudH + a.r;
    } else if (edge == 1) {
        a.x = rndRange(a.r, w - a.r);
        a.y = h - a.r;
    } else if (edge == 2) {
        a.x = a.r;
        a.y = rndRange(kHudH + a.r, h - a.r);
    } else {
        a.x = w - a.r;
        a.y = rndRange(kHudH + a.r, h - a.r);
    }

    // Direzione verso il pianeta con un piccolo scarto, cosi' non arriva
    // sempre esattamente al centro.
    const float dx = planetX() + rndRange(-16.0f, 16.0f) - a.x;
    const float dy = planetY() + rndRange(-16.0f, 16.0f) - a.y;
    const float len = std::max(1.0f, sqrtf(dx * dx + dy * dy));
    const float speed = (26.0f + 6.0f * wave) * rndRange(0.8f, 1.25f);
    a.vx = dx / len * speed;
    a.vy = dy / len * speed;
    a.hits = a.r >= 6.5f ? 2 : 1;
    a.points = a.hits == 2 ? 20 : 10;
    a.alive = true;
}

void drawHud(int hp, int wave, uint32_t score) {
    const int w = display::width();
    display::text(4, 4, "HP", display::kTextDim, 0);
    for (int i = 0; i < kMaxHp; ++i) {
        display::g().fillRect(20 + i * 7, 4, 5, 5,
                              i < hp ? display::kDanger : display::kBlack);
    }
    display::textCentered(4, "ONDA " + std::to_string(wave), display::kInfo, 0);
    const std::string s = "PUNTI " + std::to_string(score);
    display::text(w - 4 - display::textWidth(s, 0), 4, s, display::kText, 0);
    display::g().drawLine(0, kHudH - 1, w - 1, kHudH - 1, display::kAccent);
}

void drawScene(const Asteroid* rocks, const Bullet* shots, float angle, int hp, int wave,
               uint32_t score) {
    display::clear(display::kBackground);

    for (int i = 0; i < kMaxBullets; ++i) {
        if (!shots[i].alive) {
            continue;
        }
        display::g().fillRect(static_cast<int>(shots[i].x) - 1,
                              static_cast<int>(shots[i].y) - 1, 3, 3, display::kAccent);
    }

    for (int i = 0; i < kMaxAsteroids; ++i) {
        const Asteroid& a = rocks[i];
        if (!a.alive) {
            continue;
        }
        // I sassi grossi (2 colpi) restano piu' chiari.
        display::g().fillCircle(static_cast<int>(a.x), static_cast<int>(a.y),
                                static_cast<int>(a.r),
                                a.hits > 1 ? display::kText : display::kTextDim);
    }

    const float px = planetX();
    const float py = planetY();
    display::g().fillCircle(static_cast<int>(px), static_cast<int>(py), kPlanetR,
                            display::kInfo);
    display::g().fillCircle(static_cast<int>(px - 5.0f), static_cast<int>(py - 4.0f),
                            kPlanetR / 2, display::kAccent);

    const float c = cosf(angle);
    const float s = sinf(angle);
    display::g().drawLine(static_cast<int>(px + c * (kPlanetR - 6)),
                          static_cast<int>(py + s * (kPlanetR - 6)),
                          static_cast<int>(px + c * (kPlanetR + 4)),
                          static_cast<int>(py + s * (kPlanetR + 4)), display::kText);
    display::g().fillCircle(static_cast<int>(px + c * (kPlanetR + 4)),
                            static_cast<int>(py + s * (kPlanetR + 4)), 2, display::kText);

    drawHud(hp, wave, score);
}

void drawOverlay(uint32_t score, uint32_t best) {
    static const std::string kHint = "INVIO = riprova   ` = esci";
    const int w = 180;
    const int h = 58;
    const int x = (display::width() - w) / 2;
    const int y = (display::height() - h) / 2;
    display::g().fillRect(x, y, w, h, display::kBlack);
    display::g().drawRect(x, y, w, h, display::kDanger);
    display::textCentered(y + 6, "GAME OVER", display::kDanger, 4);
    display::textCentered(y + 24, "PUNTI " + std::to_string(score), display::kText, 2);
    display::textCentered(y + 36, "RECORD " + std::to_string(std::max(best, score)),
                          display::kInfo, 0);
    display::textCentered(y + 47, kHint, display::kTextDim, 0);
}

}  // namespace

void runOrbital() {
    const uint32_t best = scores::best("orbital");
    navigation::waitRelease();

    for (;;) {
        Asteroid rocks[kMaxAsteroids];
        Bullet shots[kMaxBullets];
        int hp = kMaxHp;
        uint32_t score = 0;
        int wave = 1;
        float angle = -kPi * 0.5f;
        bool dead = false;
        const uint32_t startMs = millis();
        uint32_t lastFrame = startMs;
        uint32_t lastSpawn = startMs;
        uint32_t lastShot = 0;
        uint32_t lastSound = 0;

        for (;;) {
            navigation::update();
            const navigation::EventData ev = navigation::poll();
            if (ev.type == navigation::Event::Back) {
                return;
            }

            const uint32_t now = millis();
            if (dead) {
                drawScene(rocks, shots, angle, hp, wave, score);
                drawOverlay(score, std::max(best, score));
                display::push();
                if (ev.type == navigation::Event::Select) {
                    navigation::waitRelease();
                    break;
                }
                delay(20);
                continue;
            }

            const int w = display::width();
            const int h = display::height();
            float dt = (now - lastFrame) / 1000.0f;
            lastFrame = now;
            dt = std::min(dt, 0.05f);
            wave = 1 + static_cast<int>((now - startMs) / kWaveMs);

            if (navigation::pressed(',')) {
                angle -= kTurnSpeed * dt;
            }
            if (navigation::pressed('/')) {
                angle += kTurnSpeed * dt;
            }
            // Un tocco breve puo' cadere tra due frame: l'evento lo recupera.
            if (ev.type == navigation::Event::Left) {
                angle -= 0.15f;
            } else if (ev.type == navigation::Event::Right) {
                angle += 0.15f;
            }

            if (ev.type == navigation::Event::Select && now - lastShot >= kFireCooldownMs) {
                for (int i = 0; i < kMaxBullets; ++i) {
                    if (shots[i].alive) {
                        continue;
                    }
                    const float c = cosf(angle);
                    const float s = sinf(angle);
                    Bullet& b = shots[i];
                    b.x = planetX() + c * (kPlanetR + 3);
                    b.y = planetY() + s * (kPlanetR + 3);
                    b.vx = c * kBulletSpeed;
                    b.vy = s * kBulletSpeed;
                    b.alive = true;
                    lastShot = now;
                    beep(880.0f, 12, now, lastSound);
                    break;
                }
            }

            const uint32_t spawnEvery =
                static_cast<uint32_t>(std::max(340, 1000 - (wave - 1) * 80));
            if (now - lastSpawn >= spawnEvery) {
                spawnAsteroid(rocks, wave);
                lastSpawn = now;
            }

            // Proiettili: avanzano, escono dallo schermo o colpiscono un sasso.
            for (int i = 0; i < kMaxBullets; ++i) {
                Bullet& b = shots[i];
                if (!b.alive) {
                    continue;
                }
                b.x += b.vx * dt;
                b.y += b.vy * dt;
                if (b.x < -4.0f || b.x > w + 4.0f || b.y < -4.0f || b.y > h + 4.0f) {
                    b.alive = false;
                    continue;
                }
                for (int j = 0; j < kMaxAsteroids; ++j) {
                    Asteroid& a = rocks[j];
                    if (!a.alive) {
                        continue;
                    }
                    const float dx = b.x - a.x;
                    const float dy = b.y - a.y;
                    const float rr = a.r + 2.0f;
                    if (dx * dx + dy * dy > rr * rr) {
                        continue;
                    }
                    b.alive = false;
                    if (--a.hits <= 0) {
                        a.alive = false;
                        score += static_cast<uint32_t>(a.points);
                    }
                    beep(220.0f, 22, now, lastSound);
                    break;
                }
            }

            // Asteroidi: avanzano verso il pianeta e tolgono 1 HP all'impatto.
            for (int i = 0; i < kMaxAsteroids && !dead; ++i) {
                Asteroid& a = rocks[i];
                if (!a.alive) {
                    continue;
                }
                a.x += a.vx * dt;
                a.y += a.vy * dt;
                if (a.x < -a.r - 8.0f || a.x > w + a.r + 8.0f || a.y < -a.r - 8.0f ||
                    a.y > h + a.r + 8.0f) {
                    a.alive = false;
                    continue;
                }
                const float dx = a.x - planetX();
                const float dy = a.y - planetY();
                const float rr = a.r + kPlanetR;
                if (dx * dx + dy * dy > rr * rr) {
                    continue;
                }
                a.alive = false;
                if (--hp <= 0) {
                    hp = 0;
                    dead = true;
                    scores::submit("orbital", score);
                }
                beep(140.0f, 35, now, lastSound);
            }

            drawScene(rocks, shots, angle, hp, wave, score);
            display::push();
            delay(10);
        }
    }
}

}  // namespace games

#endif  // ENABLE_MODULE_GAMES
