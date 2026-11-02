#include "Game2048.h"

#include <Arduino.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <utility>
#include <vector>

#include "Display.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "Scores.h"

#if ENABLE_MODULE_GAMES

namespace games {
namespace {

constexpr int kBoardX = 6;
constexpr int kBoardY = 18;
constexpr int kTile = 26;
constexpr int kGap = 4;
constexpr int kBoardSize = 4 * kTile + 3 * kGap;

using Board = std::array<std::array<uint32_t, 4>, 4>;

uint16_t tileColor(uint32_t value) {
    switch (value) {
        case 0:
            return display::kPaper;
        case 2:
            return display::kNeoYellow;
        case 4:
            return display::kNeoOrange;
        case 8:
            return 0xFB40;
        case 16:
            return display::kNeoPink;
        case 32:
            return display::kNeoRed;
        case 64:
            return display::kNeoCyan;
        case 128:
            return display::kNeoGreen;
        case 256:
            return 0x2D7F;
        case 512:
            return 0xA81F;
        default:
            return display::kInk;
    }
}

uint16_t tileTextColor(uint32_t value) {
    if (value == 2 || value == 4 || value == 64 || value == 128 || value == 256) {
        return display::kInk;
    }
    return display::kPaperLight;
}

void drawTile(int x, int y, uint32_t value) {
    display::g().fillRect(x, y, kTile, kTile, tileColor(value));
    display::g().drawRect(x, y, kTile, kTile, display::kInk);
    if (value == 0) {
        return;
    }
    const std::string text = std::to_string(value);
    const uint8_t font = text.size() >= 4 ? 0 : 2;
    if (font == 0) {
        display::g().setFont(&fonts::Font0);
    } else {
        display::g().setFont(&fonts::Font2);
    }
    const int tw = display::g().textWidth(text.c_str());
    const int th = display::g().fontHeight();
    display::text(x + (kTile - tw) / 2, y + (kTile - th) / 2, text,
                  tileTextColor(value), font);
}

void draw(const Board& board, uint32_t score, uint32_t best, bool finished,
          bool won) {
    display::clear(display::kPaper);
    display::text(6, 3, "2048", display::kInk, 2);
    display::text(58, 5, "PUNTI " + std::to_string(score), display::kInk, 0);

    display::g().fillRect(kBoardX - 2, kBoardY - 2, kBoardSize + 4, kBoardSize + 4,
                          display::kPaperLight);
    display::g().drawRect(kBoardX - 2, kBoardY - 2, kBoardSize + 4, kBoardSize + 4,
                          display::kInk);
    display::g().drawRect(kBoardX - 1, kBoardY - 1, kBoardSize + 2, kBoardSize + 2,
                          display::kInk);

    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            drawTile(kBoardX + x * (kTile + kGap), kBoardY + y * (kTile + kGap),
                     board[y][x]);
        }
    }

    display::text(132, 5, "RECORD", display::kMuted, 0);
    display::text(132, 16, std::to_string(best), display::kInk, 2);
    display::text(132, 42, "PUNTI", display::kMuted, 0);
    display::text(132, 53, std::to_string(score), display::kInk, 2);
    display::text(132, 80, "; . = su/giu", display::kInk, 0);
    display::text(132, 90, ", / = sx/dx", display::kInk, 0);
    display::text(132, 104, "` = esci", display::kMuted, 0);

    if (finished) {
        const int w = 176;
        const int h = 46;
        const int x = (display::width() - w) / 2;
        const int y = (display::height() - h) / 2;
        display::g().fillRect(x, y, w, h, display::kPaperLight);
        display::g().drawRect(x, y, w, h, display::kInk);
        display::g().drawRect(x + 1, y + 1, w - 2, h - 2, display::kInk);
        display::textCentered(y + 8, won ? "2048!" : "GAME OVER",
                              won ? display::kNeoGreen : display::kNeoRed, 2);
        display::textCentered(y + 28, "INVIO = riprova   ` = esci", display::kMuted, 0);
    }
}

void spawn(Board& board) {
    std::vector<std::pair<int, int>> empty;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            if (board[y][x] == 0) {
                empty.emplace_back(x, y);
            }
        }
    }
    if (empty.empty()) {
        return;
    }
    const auto pick = empty[esp_random() % empty.size()];
    board[pick.second][pick.first] = (esp_random() % 10 == 0) ? 4 : 2;
}

bool slideLine(std::array<uint32_t, 4>& line, uint32_t& gained) {
    std::array<uint32_t, 4> out{};
    bool merged[4] = {false, false, false, false};
    int pos = 0;
    for (uint32_t value : line) {
        if (value == 0) {
            continue;
        }
        if (pos > 0 && out[pos - 1] == value && !merged[pos - 1]) {
            out[pos - 1] *= 2;
            gained += out[pos - 1];
            merged[pos - 1] = true;
        } else {
            out[pos++] = value;
        }
    }
    const bool changed = out != line;
    line = out;
    return changed;
}

bool moveBoard(Board& board, navigation::Event dir, uint32_t& gained) {
    Board before = board;
    gained = 0;

    for (int i = 0; i < 4; ++i) {
        std::array<uint32_t, 4> line{};
        if (dir == navigation::Event::Left) {
            for (int j = 0; j < 4; ++j) line[j] = board[i][j];
            slideLine(line, gained);
            for (int j = 0; j < 4; ++j) board[i][j] = line[j];
        } else if (dir == navigation::Event::Right) {
            for (int j = 0; j < 4; ++j) line[j] = board[i][3 - j];
            slideLine(line, gained);
            for (int j = 0; j < 4; ++j) board[i][3 - j] = line[j];
        } else if (dir == navigation::Event::Up) {
            for (int j = 0; j < 4; ++j) line[j] = board[j][i];
            slideLine(line, gained);
            for (int j = 0; j < 4; ++j) board[j][i] = line[j];
        } else if (dir == navigation::Event::Down) {
            for (int j = 0; j < 4; ++j) line[j] = board[3 - j][i];
            slideLine(line, gained);
            for (int j = 0; j < 4; ++j) board[3 - j][i] = line[j];
        }
    }
    return board != before;
}

bool canMove(const Board& board) {
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            if (board[y][x] == 0) {
                return true;
            }
            if (x < 3 && board[y][x] == board[y][x + 1]) {
                return true;
            }
            if (y < 3 && board[y][x] == board[y + 1][x]) {
                return true;
            }
        }
    }
    return false;
}

bool has2048(const Board& board) {
    for (const auto& row : board) {
        for (uint32_t value : row) {
            if (value >= 2048) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

void run2048() {
    navigation::waitRelease();
    uint32_t best = scores::best("2048");

    for (;;) {
        Board board{};
        spawn(board);
        spawn(board);
        uint32_t score = 0;
        bool finished = false;
        bool won = false;
        bool submitted = false;
        uint32_t renderedAt = 0;

        while (true) {
            navigation::update();
            const navigation::EventData ev = navigation::poll();
            if (ev.type == navigation::Event::Back) {
                return;
            }

            if (finished) {
                if (!submitted) {
                    scores::submit("2048", score);
                    best = std::max(best, score);
                    submitted = true;
                }
                if (ev.type == navigation::Event::Select) {
                    break;
                }
                if (millis() - renderedAt > 60) {
                    draw(board, score, std::max(best, score), true, won);
                    display::push();
                    renderedAt = millis();
                }
                delay(10);
                continue;
            }

            const bool isMove = ev.type == navigation::Event::Up ||
                                ev.type == navigation::Event::Down ||
                                ev.type == navigation::Event::Left ||
                                ev.type == navigation::Event::Right;
            if (isMove) {
                uint32_t gained = 0;
                if (moveBoard(board, ev.type, gained)) {
                    score += gained;
                    spawn(board);
                    if (has2048(board)) {
                        finished = true;
                        won = true;
                    } else if (!canMove(board)) {
                        finished = true;
                        won = false;
                    }
                }
            }

            if (millis() - renderedAt > 33) {
                draw(board, score, std::max(best, score), finished, won);
                display::push();
                renderedAt = millis();
            }
            delay(12);
        }
        navigation::waitRelease();
    }
}

}  // namespace games

#endif  // ENABLE_MODULE_GAMES
