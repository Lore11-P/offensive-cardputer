#include "Snake.h"

#include <Arduino.h>

#include <algorithm>
#include <deque>

#include "Display.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "Scores.h"

#if ENABLE_MODULE_GAMES

namespace games {
namespace {

constexpr int kCell = 8;
constexpr int kCols = 28;
constexpr int kRows = 13;
constexpr int kFieldX = (240 - kCols * kCell) / 2;
constexpr int kFieldY = 18;

struct Point {
    int x = 0;
    int y = 0;
};

enum class Dir { Up, Down, Left, Right };

bool same(const Point& a, const Point& b) {
    return a.x == b.x && a.y == b.y;
}

void drawField(const std::deque<Point>& body, const Point& food, int score,
               uint32_t best) {
    display::clear(display::kBackground);
    display::g().drawRect(kFieldX - 1, kFieldY - 1, kCols * kCell + 2,
                          kRows * kCell + 2, display::kAccent);
    display::text(4, 3, "SNAKE", display::kAccent, 2);
    display::text(70, 5, "PUNTI " + std::to_string(score), display::kText, 0);
    display::text(160, 5, "RECORD " + std::to_string(best), display::kInfo, 0);

    display::g().fillCircle(kFieldX + food.x * kCell + kCell / 2,
                            kFieldY + food.y * kCell + kCell / 2, kCell / 2 - 1,
                            display::kDanger);
    bool head = true;
    for (const auto& p : body) {
        const uint16_t color = head ? display::kAccent : display::kInfo;
        display::g().fillRect(kFieldX + p.x * kCell + 1,
                              kFieldY + p.y * kCell + 1, kCell - 2, kCell - 2,
                              color);
        head = false;
    }
}

void drawOverlay(const char* title, uint16_t color) {
    const int w = 170;
    const int h = 44;
    const int x = (display::width() - w) / 2;
    const int y = (display::height() - h) / 2;
    display::g().fillRect(x, y, w, h, display::kBlack);
    display::g().drawRect(x, y, w, h, color);
    display::textCentered(y + 8, title, color, 2);
    display::textCentered(y + 27, "INVIO = riprova   ` = esci", display::kTextDim, 0);
}

}  // namespace

void runSnake() {
    const uint32_t best = scores::best("snake");
    navigation::waitRelease();

    for (;;) {
        std::deque<Point> body;
        body.push_back({kCols / 2, kRows / 2});
        body.push_back({kCols / 2 - 1, kRows / 2});
        body.push_back({kCols / 2 - 2, kRows / 2});
        Dir dir = Dir::Right;
        Dir pending = dir;
        Point food{static_cast<int>(esp_random() % kCols),
                   static_cast<int>(esp_random() % kRows)};
        int score = 0;
        bool dead = false;
        bool running = true;
        uint32_t nextTick = millis() + 120;

        while (running) {
            navigation::update();
            const navigation::EventData ev = navigation::poll();
            if (ev.type == navigation::Event::Back) {
                return;
            }
            if (ev.type == navigation::Event::Up && dir != Dir::Down) {
                pending = Dir::Up;
            } else if (ev.type == navigation::Event::Down && dir != Dir::Up) {
                pending = Dir::Down;
            } else if (ev.type == navigation::Event::Left && dir != Dir::Right) {
                pending = Dir::Left;
            } else if (ev.type == navigation::Event::Right && dir != Dir::Left) {
                pending = Dir::Right;
            }

            if (dead) {
                if (ev.type == navigation::Event::Select) {
                    break;
                }
                delay(20);
                continue;
            }

            const uint32_t now = millis();
            if (now < nextTick) {
                delay(5);
                continue;
            }
            nextTick = now + std::max<uint32_t>(55, 130 - score / 5);
            dir = pending;

            Point head = body.front();
            if (dir == Dir::Up) {
                --head.y;
            } else if (dir == Dir::Down) {
                ++head.y;
            } else if (dir == Dir::Left) {
                --head.x;
            } else {
                ++head.x;
            }

            if (head.x < 0 || head.x >= kCols || head.y < 0 || head.y >= kRows) {
                dead = true;
            }
            for (const auto& p : body) {
                if (same(head, p)) {
                    dead = true;
                    break;
                }
            }
            if (dead) {
                scores::submit("snake", static_cast<uint32_t>(score));
                drawField(body, food, score, std::max(best, static_cast<uint32_t>(score)));
                drawOverlay("GAME OVER", display::kDanger);
                display::push();
                continue;
            }

            body.push_front(head);
            if (same(head, food)) {
                score += 10;
                do {
                    food = {static_cast<int>(esp_random() % kCols),
                            static_cast<int>(esp_random() % kRows)};
                } while (std::any_of(body.begin(), body.end(),
                                     [&](const Point& p) { return same(p, food); }));
            } else {
                body.pop_back();
            }

            drawField(body, food, score, std::max(best, static_cast<uint32_t>(score)));
            display::push();
        }
        navigation::waitRelease();
    }
}

}  // namespace games

#endif  // ENABLE_MODULE_GAMES
