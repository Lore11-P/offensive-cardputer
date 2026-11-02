#include "Connect4.h"

#include <Arduino.h>

#include <array>
#include <string>
#include <vector>

#include "Display.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "Scores.h"

#if ENABLE_MODULE_GAMES

namespace games {
namespace {

constexpr int kCols = 7;
constexpr int kRows = 6;
constexpr int kCell = 15;
constexpr int kBoardX = (240 - kCols * kCell) / 2;
constexpr int kBoardY = 30;
constexpr int kPlayer = 1;
constexpr int kCpu = 2;

using Board = std::array<int, kCols * kRows>;

int indexOf(int col, int row) {
    return row * kCols + col;
}

bool inBounds(int col, int row) {
    return col >= 0 && col < kCols && row >= 0 && row < kRows;
}

int drop(Board& board, int col, int piece) {
    if (col < 0 || col >= kCols) {
        return -1;
    }
    for (int row = kRows - 1; row >= 0; --row) {
        const int idx = indexOf(col, row);
        if (board[idx] == 0) {
            board[idx] = piece;
            return row;
        }
    }
    return -1;
}

bool hasWon(const Board& board, int piece) {
    static const int kDirs[4][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};
    for (int row = 0; row < kRows; ++row) {
        for (int col = 0; col < kCols; ++col) {
            if (board[indexOf(col, row)] != piece) {
                continue;
            }
            for (const auto& dir : kDirs) {
                int count = 1;
                for (int sign = -1; sign <= 1; sign += 2) {
                    int c = col + dir[0] * sign;
                    int r = row + dir[1] * sign;
                    while (inBounds(c, r) && board[indexOf(c, r)] == piece) {
                        ++count;
                        c += dir[0] * sign;
                        r += dir[1] * sign;
                    }
                }
                if (count >= 4) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool isFull(const Board& board) {
    for (int value : board) {
        if (value == 0) {
            return false;
        }
    }
    return true;
}

bool wouldWin(const Board& board, int col, int piece) {
    Board copy = board;
    return drop(copy, col, piece) >= 0 && hasWon(copy, piece);
}

int chooseAiColumn(const Board& board) {
    for (int col = 0; col < kCols; ++col) {
        if (wouldWin(board, col, kCpu)) {
            return col;
        }
    }
    for (int col = 0; col < kCols; ++col) {
        if (wouldWin(board, col, kPlayer)) {
            return col;
        }
    }
    std::vector<int> valid;
    for (int col = 0; col < kCols; ++col) {
        Board copy = board;
        if (drop(copy, col, kCpu) >= 0) {
            valid.push_back(col);
        }
    }
    if (valid.empty()) {
        return -1;
    }
    constexpr int kCenter = kCols / 2;
    Board centerCopy = board;
    if (drop(centerCopy, kCenter, kCpu) >= 0 && (esp_random() % 3) != 0) {
        return kCenter;
    }
    return valid[esp_random() % valid.size()];
}

void drawBoard(const Board& board, int cursor, int playerScore, int cpuScore,
               uint32_t wins, bool thinking) {
    display::clear(display::kPaper);
    display::text(4, 3, "CONNECT 4", display::kInk, 2);
    display::text(100, 5, "TU " + std::to_string(playerScore), display::kNeoRed, 0);
    display::text(136, 5, std::to_string(cpuScore) + " CPU", display::kNeoYellow, 0);
    display::text(178, 5, "RECORD " + std::to_string(wins), display::kMuted, 0);

    auto& d = display::g();
    d.fillRect(kBoardX - 2, kBoardY - 2, kCols * kCell + 4, kRows * kCell + 4,
               display::kInk);
    d.fillRect(kBoardX, kBoardY, kCols * kCell, kRows * kCell, 0x031F);
    for (int row = 0; row < kRows; ++row) {
        for (int col = 0; col < kCols; ++col) {
            const int cx = kBoardX + col * kCell + kCell / 2;
            const int cy = kBoardY + row * kCell + kCell / 2;
            const int value = board[indexOf(col, row)];
            d.fillCircle(cx, cy, kCell / 2 - 2, display::kPaperLight);
            if (value == kPlayer) {
                d.fillCircle(cx, cy, kCell / 2 - 3, display::kNeoRed);
                d.drawCircle(cx, cy, kCell / 2 - 3, display::kInk);
            } else if (value == kCpu) {
                d.fillCircle(cx, cy, kCell / 2 - 3, display::kNeoYellow);
                d.drawCircle(cx, cy, kCell / 2 - 3, display::kInk);
            }
        }
    }

    const int cursorX = kBoardX + cursor * kCell + kCell / 2;
    d.fillTriangle(cursorX, kBoardY - 7, cursorX - 5, kBoardY - 2, cursorX + 5,
                   kBoardY - 2, display::kNeoPink);
    display::footer(thinking ? "CPU sta pensando..." 
                             : "frecce=colonna  INVIO=gioca  `=esci");
    display::push();
}

void drawGameOver(bool playerWon, bool draw, uint32_t wins) {
    auto& d = display::g();
    const int w = 176;
    const int h = 48;
    const int x = (display::width() - w) / 2;
    const int y = (display::height() - h) / 2;
    d.fillRect(x + 3, y + 3, w, h, display::kInk);
    d.fillRect(x, y, w, h, display::kPaper);
    const uint16_t color = draw ? display::kMuted
                                : (playerWon ? display::kNeoGreen : display::kDanger);
    d.drawRect(x, y, w, h, color);
    display::textCentered(y + 6, draw ? "PAREGGIO" : (playerWon ? "HAI VINTO!" : "CPU VINCE"),
                          color, 2);
    display::textCentered(y + 23, "Vittorie totali: " + std::to_string(wins),
                          display::kInk, 0);
    display::textCentered(y + 35, "INVIO=riprova  `=esci", display::kMuted, 0);
}

}  // namespace

void runConnect4() {
    uint32_t wins = scores::best("connect4");
    navigation::waitRelease();

    for (;;) {
        Board board{};
        int cursor = kCols / 2;
        int playerScore = 0;
        int cpuScore = 0;
        bool playerTurn = true;
        bool waitingAi = false;
        uint32_t aiAt = 0;
        bool finished = false;
        bool playerWon = false;
        bool draw = false;

        while (!finished) {
            drawBoard(board, cursor, playerScore, cpuScore, wins, waitingAi);

            navigation::update();
            const navigation::EventData ev = navigation::poll();
            if (ev.type == navigation::Event::Back) {
                return;
            }

            if (waitingAi) {
                if (millis() >= aiAt) {
                    const int col = chooseAiColumn(board);
                    if (col >= 0) {
                        drop(board, col, kCpu);
                        if (hasWon(board, kCpu)) {
                            ++cpuScore;
                            finished = true;
                        } else if (isFull(board)) {
                            draw = true;
                            finished = true;
                        }
                    }
                    waitingAi = false;
                    playerTurn = true;
                }
                delay(10);
                continue;
            }

            if (!playerTurn) {
                continue;
            }

            if (ev.type == navigation::Event::Left) {
                cursor = (cursor - 1 + kCols) % kCols;
            } else if (ev.type == navigation::Event::Right) {
                cursor = (cursor + 1) % kCols;
            } else if (ev.type == navigation::Event::Select) {
                const int row = drop(board, cursor, kPlayer);
                if (row >= 0) {
                    if (hasWon(board, kPlayer)) {
                        ++playerScore;
                        ++wins;
                        scores::submit("connect4", wins);
                        playerWon = true;
                        finished = true;
                    } else if (isFull(board)) {
                        draw = true;
                        finished = true;
                    } else {
                        playerTurn = false;
                        waitingAi = true;
                        aiAt = millis() + 420;
                    }
                }
            }
            delay(8);
        }

        drawBoard(board, cursor, playerScore, cpuScore, wins, false);
        drawGameOver(playerWon, draw, wins);
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
