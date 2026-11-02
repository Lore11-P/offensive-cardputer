#include "Simon.h"

#include <Arduino.h>

#include <algorithm>
#include <string>
#include <vector>

#include "Display.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "Scores.h"

#if ENABLE_MODULE_GAMES

namespace games {
namespace {

constexpr int kPadX = 7;
constexpr int kPadY = 33;
constexpr int kPadW = 110;
constexpr int kPadH = 38;
constexpr int kGapX = 6;
constexpr int kGapY = 4;
constexpr const char* kPadLabels[4] = {"SU", "DX", "GIU", "SX"};

uint16_t padColor(int pad) {
    static const uint16_t kColors[4] = {
        display::kNeoGreen,
        display::kNeoRed,
        display::kNeoYellow,
        display::kNeoCyan,
    };
    return kColors[pad & 3];
}

uint16_t padDarkColor(int pad) {
    static const uint16_t kColors[4] = {0x0320, 0x7800, 0x7BE0, 0x0293};
    return kColors[pad & 3];
}

void padRect(int pad, int& x, int& y) {
    x = kPadX + (pad & 1) * (kPadW + kGapX);
    y = kPadY + (pad >> 1) * (kPadH + kGapY);
}

void drawBoard(int level, uint32_t best, int active, const std::string& status) {
    display::clear(display::kPaper);
    display::header("SIMON", "Giochi");
    display::text(6, 23, "LIVELLO " + std::to_string(level), display::kInk, 0);
    display::text(150, 23, "RECORD " + std::to_string(best), display::kMuted, 0);

    for (int pad = 0; pad < 4; ++pad) {
        int x = 0;
        int y = 0;
        padRect(pad, x, y);
        const bool lit = (pad == active);
        const uint16_t fill = lit ? padColor(pad) : padDarkColor(pad);
        if (lit) {
            display::g().fillRect(x + 2, y + 2, kPadW, kPadH, display::kInk);
        }
        display::g().fillRect(x, y, kPadW, kPadH, fill);
        display::g().drawRect(x, y, kPadW, kPadH, display::kInk);
        display::g().drawRect(x + 1, y + 1, kPadW - 2, kPadH - 2,
                              lit ? display::kPaperLight : display::kMuted);
        display::text(x + 4, y + 3, kPadLabels[pad],
                      lit ? display::kInk : display::kPaperLight, 0);
        display::text(x + kPadW - 12, y + kPadH - 11,
                      std::to_string(pad + 1),
                      lit ? display::kInk : display::kPaperLight, 0);
    }

    display::textCentered(114, status, display::kNeoPink, 0);
    display::footer("frecce = ripeti  ` = esci");
    display::push();
}

bool pauseWithBackCheck(uint32_t ms) {
    const uint32_t until = millis() + ms;
    while (millis() < until) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            return false;
        }
        delay(4);
    }
    return true;
}

bool showSequence(const std::vector<uint8_t>& sequence, uint32_t best) {
    if (!pauseWithBackCheck(260)) {
        return false;
    }
    for (uint8_t pad : sequence) {
        drawBoard(static_cast<int>(sequence.size()), best, pad, "OSSERVA");
        if (!pauseWithBackCheck(330)) {
            return false;
        }
        drawBoard(static_cast<int>(sequence.size()), best, -1, "OSSERVA");
        if (!pauseWithBackCheck(130)) {
            return false;
        }
    }
    return true;
}

void drawGameOver(int score, uint32_t best) {
    auto& d = display::g();
    const int w = 170;
    const int h = 50;
    const int x = (display::width() - w) / 2;
    const int y = (display::height() - h) / 2;
    d.fillRect(x + 3, y + 3, w, h, display::kInk);
    d.fillRect(x, y, w, h, display::kPaper);
    d.drawRect(x, y, w, h, display::kDanger);
    display::textCentered(y + 6, "GAME OVER", display::kDanger, 2);
    display::textCentered(y + 23,
                          "Livello " + std::to_string(score) +
                              "  Record " + std::to_string(best),
                          display::kInk, 0);
    display::textCentered(y + 35, "INVIO=riprova  `=esci", display::kMuted, 0);
}

}  // namespace

void runSimon() {
    uint32_t best = scores::best("simon");
    navigation::waitRelease();

    for (;;) {
        std::vector<uint8_t> sequence;
        sequence.push_back(static_cast<uint8_t>(esp_random() % 4));
        int completed = 0;
        bool dead = false;

        while (!dead) {
            if (!showSequence(sequence, best)) {
                return;
            }

            int position = 0;
            bool turnDone = false;
            while (!turnDone) {
                drawBoard(static_cast<int>(sequence.size()), best, -1,
                          "RIPETI LA SEQUENZA");
                navigation::update();
                const navigation::EventData ev = navigation::poll();
                if (ev.type == navigation::Event::Back) {
                    return;
                }
                int pad = -1;
                if (ev.type == navigation::Event::Up) {
                    pad = 0;
                } else if (ev.type == navigation::Event::Right) {
                    pad = 1;
                } else if (ev.type == navigation::Event::Down) {
                    pad = 2;
                } else if (ev.type == navigation::Event::Left) {
                    pad = 3;
                }
                if (pad < 0) {
                    delay(8);
                    continue;
                }

                drawBoard(static_cast<int>(sequence.size()), best, pad, "");
                delay(130);
                if (pad != sequence[position]) {
                    dead = true;
                    break;
                }
                ++position;
                if (position >= static_cast<int>(sequence.size())) {
                    turnDone = true;
                }
            }

            if (dead) {
                scores::submit("simon", static_cast<uint32_t>(std::max(0, completed)));
                best = std::max(best, static_cast<uint32_t>(std::max(0, completed)));
                break;
            }

            completed = static_cast<int>(sequence.size());
            scores::submit("simon", static_cast<uint32_t>(completed));
            best = std::max(best, static_cast<uint32_t>(completed));
            sequence.push_back(static_cast<uint8_t>(esp_random() % 4));
            if (!pauseWithBackCheck(320)) {
                return;
            }
        }

        drawBoard(static_cast<int>(sequence.size()) - 1, best, -1, "");
        drawGameOver(completed, best);
        display::push();
        for (;;) {
            navigation::update();
            const navigation::EventData ev = navigation::poll();
            if (ev.type == navigation::Event::Back) {
                return;
            }
            if (ev.type == navigation::Event::Select) {
                break;
            }
            delay(20);
        }
        navigation::waitRelease();
    }
}

}  // namespace games

#endif  // ENABLE_MODULE_GAMES
