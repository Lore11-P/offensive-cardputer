#include "Minesweeper.h"

#include <Arduino.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "Display.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "Scores.h"

#if ENABLE_MODULE_GAMES

namespace games {
namespace {

constexpr int kCols = 9;
constexpr int kRows = 7;
constexpr int kCells = kCols * kRows;
constexpr int kMines = 10;
constexpr int kCell = 13;
constexpr int kBoardX = 61;
constexpr int kBoardY = 28;

struct Board {
    int value[kCells] = {0};  // -1 mina, 0..8 mine adiacenti
    bool open[kCells] = {false};
    bool flag[kCells] = {false};
    bool placed = false;
    int revealed = 0;
    int flags = 0;
};

int indexOf(int col, int row) {
    return row * kCols + col;
}

bool inBounds(int col, int row) {
    return col >= 0 && col < kCols && row >= 0 && row < kRows;
}

void plantMines(Board& board, int safeCol, int safeRow) {
    for (int i = 0; i < kCells; ++i) {
        board.value[i] = 0;
    }
    int placed = 0;
    while (placed < kMines) {
        const int col = static_cast<int>(esp_random() % kCols);
        const int row = static_cast<int>(esp_random() % kRows);
        const int idx = indexOf(col, row);
        if (board.value[idx] == -1) {
            continue;
        }
        if (std::abs(col - safeCol) <= 1 && std::abs(row - safeRow) <= 1) {
            continue;
        }
        board.value[idx] = -1;
        ++placed;
    }
    for (int row = 0; row < kRows; ++row) {
        for (int col = 0; col < kCols; ++col) {
            const int idx = indexOf(col, row);
            if (board.value[idx] == -1) {
                continue;
            }
            int count = 0;
            for (int dr = -1; dr <= 1; ++dr) {
                for (int dc = -1; dc <= 1; ++dc) {
                    if ((dr != 0 || dc != 0) && inBounds(col + dc, row + dr) &&
                        board.value[indexOf(col + dc, row + dr)] == -1) {
                        ++count;
                    }
                }
            }
            board.value[idx] = count;
        }
    }
    board.placed = true;
}

bool reveal(Board& board, int col, int row) {
    const int start = indexOf(col, row);
    if (board.open[start] || board.flag[start]) {
        return false;
    }

    std::vector<int> stack;
    stack.push_back(start);
    while (!stack.empty()) {
        const int idx = stack.back();
        stack.pop_back();
        if (board.open[idx] || board.flag[idx]) {
            continue;
        }
        board.open[idx] = true;
        ++board.revealed;
        if (board.value[idx] == -1) {
            return true;
        }
        if (board.value[idx] != 0) {
            continue;
        }
        const int col2 = idx % kCols;
        const int row2 = idx / kCols;
        for (int dr = -1; dr <= 1; ++dr) {
            for (int dc = -1; dc <= 1; ++dc) {
                if ((dr != 0 || dc != 0) && inBounds(col2 + dc, row2 + dr)) {
                    const int next = indexOf(col2 + dc, row2 + dr);
                    if (!board.open[next] && !board.flag[next]) {
                        stack.push_back(next);
                    }
                }
            }
        }
    }
    return false;
}

void drawCell(const Board& board, int col, int row, bool cursor, bool revealMines) {
    const int idx = indexOf(col, row);
    const int x = kBoardX + col * kCell;
    const int y = kBoardY + row * kCell;
    auto& d = display::g();

    d.fillRect(x, y, kCell, kCell,
               board.open[idx] ? display::kPaperLight : display::kNeoYellow);
    d.drawRect(x, y, kCell, kCell, display::kMuted);

    if (board.flag[idx]) {
        d.fillTriangle(x + 4, y + 3, x + 9, y + 6, x + 4, y + 9, display::kNeoRed);
        d.drawLine(x + 3, y + 3, x + 3, y + 11, display::kInk);
        d.drawLine(x + 3, y + 11, x + 9, y + 11, display::kInk);
    } else if (board.open[idx] && board.value[idx] == -1) {
        d.fillCircle(x + kCell / 2, y + kCell / 2, 4, display::kInk);
        d.drawLine(x + 3, y + 3, x + 10, y + 10, display::kDanger);
        d.drawLine(x + 10, y + 3, x + 3, y + 10, display::kDanger);
    } else if (board.open[idx] && board.value[idx] > 0) {
        static const uint16_t kNumberColors[9] = {
            display::kInfo,   display::kNeoGreen, display::kNeoRed,
            display::kNeoPink, display::kNeoOrange, display::kInfo,
            display::kInk,    display::kMuted,    display::kDanger,
        };
        display::text(x + 4, y + 3, std::to_string(board.value[idx]),
                      kNumberColors[board.value[idx]], 0);
    } else if (!board.open[idx] && revealMines && board.value[idx] == -1) {
        d.fillCircle(x + kCell / 2, y + kCell / 2, 3, display::kDanger);
    }

    if (cursor) {
        d.drawRect(x - 1, y - 1, kCell + 2, kCell + 2, display::kNeoPink);
        d.drawRect(x + 1, y + 1, kCell - 2, kCell - 2, display::kInk);
    }
}

void drawBoard(const Board& board, int cursorCol, int cursorRow, uint32_t seconds,
               uint32_t wins, const std::string& status) {
    display::clear(display::kPaper);
    display::text(4, 3, "MINES", display::kInk, 2);
    display::text(52, 5,
                  "MINE " + std::to_string(kMines) + "  TEMPO " +
                      std::to_string(seconds) + "s  WIN " + std::to_string(wins),
                  display::kMuted, 0);
    for (int row = 0; row < kRows; ++row) {
        for (int col = 0; col < kCols; ++col) {
            drawCell(board, col, row, col == cursorCol && row == cursorRow, false);
        }
    }
    display::textCentered(17, status, display::kNeoPink, 0);
    display::footer("frecce=muovi  INVIO=apri  F=bandiera");
    display::push();
}

void drawGameOver(bool won, uint32_t seconds, uint32_t wins) {
    auto& d = display::g();
    const int w = 174;
    const int h = 50;
    const int x = (display::width() - w) / 2;
    const int y = (display::height() - h) / 2;
    d.fillRect(x + 3, y + 3, w, h, display::kInk);
    d.fillRect(x, y, w, h, display::kPaper);
    d.drawRect(x, y, w, h, won ? display::kNeoGreen : display::kDanger);
    display::textCentered(y + 6, won ? "CAMPO LIBERATO!" : "BOOM!",
                          won ? display::kNeoGreen : display::kDanger, 2);
    display::textCentered(y + 23,
                          "Tempo " + std::to_string(seconds) + "s  Vittorie " +
                              std::to_string(wins),
                          display::kInk, 0);
    display::textCentered(y + 35, "INVIO=riprova  `=esci", display::kMuted, 0);
}

}  // namespace

void runMinesweeper() {
    uint32_t wins = scores::best("mines");
    navigation::waitRelease();

    for (;;) {
        Board board;
        int cursorCol = kCols / 2;
        int cursorRow = kRows / 2;
        bool dead = false;
        bool won = false;
        const uint32_t startedAt = millis();

        while (!dead && !won) {
            const uint32_t seconds = (millis() - startedAt) / 1000;
            drawBoard(board, cursorCol, cursorRow, seconds, wins,
                      board.placed ? "Trova le mine" : "Primo tasto = campo sicuro");

            navigation::update();
            const navigation::EventData ev = navigation::poll();
            if (ev.type == navigation::Event::Back) {
                return;
            }
            if (ev.type == navigation::Event::Left) {
                cursorCol = (cursorCol - 1 + kCols) % kCols;
            } else if (ev.type == navigation::Event::Right) {
                cursorCol = (cursorCol + 1) % kCols;
            } else if (ev.type == navigation::Event::Up) {
                cursorRow = (cursorRow - 1 + kRows) % kRows;
            } else if (ev.type == navigation::Event::Down) {
                cursorRow = (cursorRow + 1) % kRows;
            } else if (ev.type == navigation::Event::Select) {
                const int idx = indexOf(cursorCol, cursorRow);
                if (!board.placed) {
                    plantMines(board, cursorCol, cursorRow);
                    reveal(board, cursorCol, cursorRow);
                } else if (!board.open[idx]) {
                    if (board.flag[idx]) {
                        board.flag[idx] = false;
                        --board.flags;
                    } else {
                        dead = reveal(board, cursorCol, cursorRow);
                    }
                }
                if (board.revealed == kCells - kMines) {
                    won = true;
                }
            } else if (ev.type == navigation::Event::Text &&
                       (ev.ch == 'f' || ev.ch == 'F')) {
                if (board.placed) {
                    const int idx = indexOf(cursorCol, cursorRow);
                    if (!board.open[idx]) {
                        board.flag[idx] = !board.flag[idx];
                        board.flags += board.flag[idx] ? 1 : -1;
                    }
                }
            }
            delay(8);
        }

        const uint32_t seconds = (millis() - startedAt) / 1000;
        if (won) {
            ++wins;
            scores::submit("mines", wins);
        }
        drawBoard(board, cursorCol, cursorRow, seconds, wins,
                  won ? "Vittoria!" : "Mina colpita");
        for (int row = 0; row < kRows; ++row) {
            for (int col = 0; col < kCols; ++col) {
                drawCell(board, col, row, false, true);
            }
        }
        drawGameOver(won, seconds, wins);
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
