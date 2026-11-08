#include "Chess.h"

#include <Arduino.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "ChessLogic.h"
#include "Display.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "Scores.h"

#if ENABLE_MODULE_GAMES

namespace games {
namespace {

using namespace chess;

constexpr int kCell = 13;
constexpr int kBoardX = (240 - 8 * kCell) / 2;  // 68
constexpr int kBoardY = 19;
constexpr int kMaxHistory = 256;
constexpr int kHistoryVisible = 7;

enum Difficulty { kEasy = 0, kMedium = 1, kHard = 2 };

int g_difficulty = kMedium;

struct HistoryEntry {
    Position pos;
    Move move;
    char text[8];
};

HistoryEntry g_history[kMaxHistory];
int g_historyCount = 0;
int g_historyScroll = 0;
bool g_showHistory = false;

int squareX(int sq) { return kBoardX + colOf(sq) * kCell; }
int squareY(int sq) { return kBoardY + rowOf(sq) * kCell; }

const char* difficultyShort(int difficulty) {
    return difficulty == kEasy ? "D:FAC"
           : difficulty == kMedium ? "D:MED"
                                   : "D:DIFF";
}

const char* difficultyName(int difficulty) {
    return difficulty == kEasy ? "Facile"
           : difficulty == kMedium ? "Medio"
                                   : "Difficile";
}

void moveToText(const Position& before, const Move& mv, char* out, size_t size) {
    const char piece = before.board[mv.from];
    const int fromCol = colOf(mv.from);
    const int toCol = colOf(mv.to);
    const int toRow = rowOf(mv.to);
    if (upperOf(piece) == 'K' &&
        (toCol - fromCol == 2 || fromCol - toCol == 2)) {
        std::snprintf(out, size, "%s", (toCol > fromCol) ? "O-O" : "O-O-O");
        return;
    }
    const bool capture = (before.board[mv.to] != '.') ||
                         (upperOf(piece) == 'P' && mv.to == before.ep);
    const char promo =
        (upperOf(piece) == 'P' && (toRow == 0 || toRow == 7)) ? 'Q' : 0;
    const char fromFile = static_cast<char>('a' + fromCol);
    const char fromRank = static_cast<char>('8' - rowOf(mv.from));
    const char toFile = static_cast<char>('a' + toCol);
    const char toRank = static_cast<char>('8' - toRow);
    if (promo != 0) {
        std::snprintf(out, size, "%c%c%c%c%c%c", fromFile, fromRank,
                      capture ? 'x' : '-', toFile, toRank, promo);
    } else {
        std::snprintf(out, size, "%c%c%c%c%c", fromFile, fromRank,
                      capture ? 'x' : '-', toFile, toRank);
    }
}

void pushHistory(const Position& before, const Move& mv) {
    if (g_historyCount >= kMaxHistory) {
        for (int i = 1; i < kMaxHistory; ++i) {
            g_history[i - 1] = g_history[i];
        }
        --g_historyCount;
    }
    HistoryEntry& entry = g_history[g_historyCount++];
    entry.pos = before;
    entry.move = mv;
    moveToText(before, mv, entry.text, sizeof(entry.text));
}

constexpr int kSpriteSize = 11;

constexpr uint16_t kPieceSprites[6][kSpriteSize] = {
    {
        0x0000, 0x0070, 0x00F8, 0x0070, 0x0020, 0x0070,
        0x00F8, 0x00F8, 0x01FC, 0x01FC, 0x0000,
    },
    {
        0x0000, 0x0154, 0x01FC, 0x01FC, 0x01FC, 0x00F8,
        0x00F8, 0x00F8, 0x01FC, 0x01FC, 0x0000,
    },
    {
        0x0000, 0x0050, 0x0070, 0x00F8, 0x00DC, 0x01FC,
        0x02FC, 0x02F8, 0x01FC, 0x01FC, 0x0000,
    },
    {
        0x0000, 0x0020, 0x0070, 0x00F8, 0x01FC, 0x01FC,
        0x00F8, 0x0070, 0x01FC, 0x01FC, 0x0000,
    },
    {
        0x0000, 0x0124, 0x01DC, 0x01FC, 0x00F8, 0x00F8,
        0x00F8, 0x00F8, 0x01FC, 0x01FC, 0x0000,
    },
    {
        0x0000, 0x0020, 0x0070, 0x0020, 0x0070, 0x00F8,
        0x00F8, 0x00F8, 0x01FC, 0x01FC, 0x0000,
    },
};

const uint16_t* pieceSprite(char piece) {
    switch (upperOf(piece)) {
        case 'P': return kPieceSprites[0];
        case 'R': return kPieceSprites[1];
        case 'N': return kPieceSprites[2];
        case 'B': return kPieceSprites[3];
        case 'Q': return kPieceSprites[4];
        case 'K': return kPieceSprites[5];
        default: return kPieceSprites[0];
    }
}

void drawPiece(char piece, int cx, int cy) {
    auto& d = display::g();
    const bool white = isWhitePiece(piece);
    const uint16_t fill = white ? display::kWhite : display::kHeader;
    const uint16_t ink = white ? display::kInk : display::kWhite;
    const uint16_t* sprite = pieceSprite(piece);
    const int x0 = cx - kSpriteSize / 2;
    const int y0 = cy - kSpriteSize / 2;
    for (int r = 0; r < kSpriteSize; ++r) {
        const uint16_t row = sprite[r];
        for (int c = 0; c < kSpriteSize; ++c) {
            const bool solid = (row & (1u << c)) != 0;
            bool edge = false;
            if (!solid) {
                if (r > 0 && (sprite[r - 1] & (1u << c))) edge = true;
                if (r + 1 < kSpriteSize && (sprite[r + 1] & (1u << c))) {
                    edge = true;
                }
                if (c > 0 && (row & (1u << (c - 1)))) edge = true;
                if (c + 1 < kSpriteSize && (row & (1u << (c + 1)))) {
                    edge = true;
                }
            }
            if (solid) {
                d.drawPixel(x0 + c, y0 + r, fill);
            } else if (edge) {
                d.drawPixel(x0 + c, y0 + r, ink);
            }
        }
    }
}

void drawDifficulty(int selected) {
    display::clear(display::kBlack);
    auto& d = display::g();
    d.fillRect(20, 12, 200, 111, display::kPaper);
    d.drawRect(20, 12, 200, 111, display::kInk);
    display::textCentered(18, "SCACCHI", display::kInk, 2);
    display::textCentered(38, "Scegli la difficolta' del bot", display::kMuted, 0);
    static const char* const kNames[3] = {"Facile", "Medio", "Difficile"};
    for (int i = 0; i < 3; ++i) {
        const int y = 54 + i * 20;
        const uint16_t fill =
            (i == selected) ? display::kNeoYellow : display::kPaperLight;
        d.fillRect(60, y, 120, 16, fill);
        d.drawRect(60, y, 120, 16, display::kInk);
        if (i == selected) {
            d.drawRect(61, y + 1, 118, 14, display::kInk);
        }
        display::textCentered(y + 4, kNames[i], display::kInk, 0);
    }
    display::textCentered(112, "frecce = scegli   INVIO = avvia   ` = esci",
                          display::kMuted, 0);
    display::push();
}

void drawBoard(const Position& pos, int cursor, int selected,
               const Move* legal, int legalCount, int lastFrom, int lastTo,
               uint32_t wins, int difficulty, bool thinking) {
    display::clear(display::kBlack);
    auto& d = display::g();

    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            const uint16_t color = ((r + c) % 2 == 0) ? 0xE71C : 0x6B4D;
            d.fillRect(kBoardX + c * kCell, kBoardY + r * kCell, kCell, kCell,
                       color);
        }
    }

    if (lastFrom >= 0 && lastTo >= 0) {
        d.drawRect(squareX(lastFrom) + 1, squareY(lastFrom) + 1, kCell - 2,
                   kCell - 2, display::kInfo);
        d.drawRect(squareX(lastTo) + 1, squareY(lastTo) + 1, kCell - 2,
                   kCell - 2, display::kInfo);
    }

    if (selected >= 0) {
        d.drawRect(squareX(selected), squareY(selected), kCell, kCell,
                   display::kAccent);
        d.drawRect(squareX(selected) + 1, squareY(selected) + 1, kCell - 2,
                   kCell - 2, display::kAccent);
    }

    for (int i = 0; i < legalCount; ++i) {
        const int cx = squareX(legal[i].to) + kCell / 2;
        const int cy = squareY(legal[i].to) + kCell / 2;
        if (pos.board[legal[i].to] == '.') {
            d.fillCircle(cx, cy, 2, display::kAccent);
        } else {
            d.drawCircle(cx, cy, 4, display::kDanger);
        }
    }

    for (int sq = 0; sq < 64; ++sq) {
        if (pos.board[sq] != '.') {
            drawPiece(pos.board[sq], squareX(sq) + kCell / 2,
                      squareY(sq) + kCell / 2);
        }
    }

    const bool check = inCheck(pos, pos.side);
    if (check) {
        const int king = kingSquare(pos, pos.side);
        if (king >= 0) {
            d.drawRect(squareX(king), squareY(king), kCell, kCell,
                       display::kDanger);
            d.drawRect(squareX(king) + 1, squareY(king) + 1, kCell - 2,
                       kCell - 2, display::kDanger);
        }
    }

    d.drawRect(squareX(cursor), squareY(cursor), kCell, kCell,
               display::kNeoYellow);
    d.drawRect(squareX(cursor) + 1, squareY(cursor) + 1, kCell - 2, kCell - 2,
               display::kNeoYellow);
    d.drawRect(kBoardX - 1, kBoardY - 1, 8 * kCell + 2, 8 * kCell + 2,
               display::kInk);

    display::text(4, 2, "SCACCHI", display::kPaperLight, 2);
    display::text(66, 5, pos.side == 1 ? "T:BIANCO" : "T:NERO",
                  pos.side == 1 ? display::kPaperLight : display::kNeoOrange,
                  0);
    display::text(118, 5, "V:" + std::to_string(wins), display::kInfo, 0);
    display::text(146, 5, difficultyShort(difficulty), display::kTextDim, 0);
    if (check) {
        display::text(190, 5, "SCACCO", display::kDanger, 0);
    }

    display::text(
        4, 125,
        thinking ? "IA sta pensando..."
                 : "frecce INVIO  H mosse  U undo  ` esci",
        display::kTextDim, 0);
}

void drawHistoryOverlay() {
    auto& d = display::g();
    const int x = 14;
    const int y = 18;
    const int w = 212;
    const int h = 104;
    d.fillRect(x, y, w, h, display::kBlack);
    d.drawRect(x, y, w, h, display::kInfo);
    display::textCentered(y + 5, "MOSSE GIOCATE", display::kInfo, 2);
    if (g_historyCount == 0) {
        display::textCentered(y + 44, "Nessuna mossa", display::kTextDim, 0);
    } else {
        const int first = std::max(0, std::min(g_historyScroll,
                                               g_historyCount - 1));
        for (int row = 0; row < kHistoryVisible; ++row) {
            const int idx = first + row;
            if (idx >= g_historyCount) {
                break;
            }
            const int number = idx / 2 + 1;
            char line[24];
            if (idx % 2 == 0) {
                std::snprintf(line, sizeof(line), "%d.  %s", number,
                              g_history[idx].text);
            } else {
                std::snprintf(line, sizeof(line), "%d... %s", number,
                              g_history[idx].text);
            }
            display::text(x + 10, y + 24 + row * 10, line,
                          (idx % 2 == 0) ? display::kText : display::kTextDim,
                          0);
        }
    }
    display::textCentered(y + h - 11, "su/giu scorri   H/INVIO chiudi",
                          display::kTextDim, 0);
}

void drawResultOverlay(int result, uint32_t wins, int difficulty) {
    auto& d = display::g();
    const int w = 190;
    const int h = 58;
    const int x = (240 - w) / 2;
    const int y = 34;
    d.fillRect(x + 3, y + 3, w, h, display::kInk);
    d.fillRect(x, y, w, h, display::kPaper);
    const uint16_t color = (result == 1) ? display::kNeoGreen
                            : (result == -1) ? display::kDanger
                                             : display::kMuted;
    d.drawRect(x, y, w, h, color);
    const char* title = (result == 1) ? "HAI VINTO!"
                        : (result == -1) ? "IL BOT VINCE"
                                         : "PAREGGIO";
    display::textCentered(y + 6, title, color, 2);
    display::textCentered(
        y + 24,
        "Vittorie: " + std::to_string(wins) + "   (" +
            difficultyName(difficulty) + ")",
        display::kInk, 0);
    display::textCentered(y + 39, "INVIO = nuova partita   ` = esci",
                          display::kMuted, 0);
}

int chooseDifficulty() {
    int selected = g_difficulty;
    while (true) {
        drawDifficulty(selected);
        navigation::update();
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            return -1;
        }
        if (ev.type == navigation::Event::Up ||
            ev.type == navigation::Event::Left) {
            selected = (selected + 2) % 3;
        } else if (ev.type == navigation::Event::Down ||
                   ev.type == navigation::Event::Right) {
            selected = (selected + 1) % 3;
        } else if (ev.type == navigation::Event::Select) {
            return selected;
        }
        delay(10);
    }
}

int legalFrom(const Position& pos, int from, Move* out) {
    Move all[256];
    const int n = genLegal(pos, all);
    int m = 0;
    for (int i = 0; i < n; ++i) {
        if (all[i].from == from) {
            out[m++] = all[i];
        }
    }
    return m;
}

Move chooseAiMove(const Position& pos, int difficulty) {
    Move moves[256];
    const int n = genLegal(pos, moves);
    if (n == 0) {
        return mkMove(-1, -1);
    }
    if (difficulty == kEasy && (esp_random() % 100) < 35) {
        return moves[esp_random() % n];
    }
    const int depth = (difficulty == kEasy) ? 1
                      : (difficulty == kMedium) ? 2
                                                : 3;
    std::sort(moves, moves + n, [&](const Move& a, const Move& b) {
        return moveScore(pos, a) > moveScore(pos, b);
    });
    int nodes = 0;
    int* const counter = (difficulty == kHard) ? &nodes : nullptr;
    int bestScore = -2000000;
    int bestIdx[256];
    int bestCount = 0;
    for (int i = 0; i < n; ++i) {
        Position copy = pos;
        makeMove(copy, moves[i]);
        const int score = -negamax(copy, depth, -2000000, 2000000, counter);
        if (score > bestScore) {
            bestScore = score;
            bestCount = 0;
            bestIdx[bestCount++] = i;
        } else if (score == bestScore && bestCount < 256) {
            bestIdx[bestCount++] = i;
        }
    }
    if (bestCount == 0) {
        return moves[esp_random() % n];
    }
    return moves[bestIdx[esp_random() % bestCount]];
}

bool insufficientMaterial(const Position& pos) {
    int whiteMinors = 0;
    int blackMinors = 0;
    for (int sq = 0; sq < 64; ++sq) {
        const char p = pos.board[sq];
        if (p == '.') {
            continue;
        }
        const char type = upperOf(p);
        if (type == 'P' || type == 'R' || type == 'Q') {
            return false;
        }
        if (type == 'N' || type == 'B') {
            if (isWhitePiece(p)) {
                ++whiteMinors;
            } else {
                ++blackMinors;
            }
        }
    }
    return whiteMinors <= 1 && blackMinors <= 1;
}

bool positionEnded(const Position& pos, int& result) {
    Move moves[256];
    const int n = genLegal(pos, moves);
    if (n == 0) {
        if (inCheck(pos, pos.side)) {
            result = (pos.side == 1) ? -1 : 1;
        } else {
            result = 2;
        }
        return true;
    }
    if (pos.halfmove >= 100 || insufficientMaterial(pos)) {
        result = 2;
        return true;
    }
    return false;
}

}  // namespace

void runChess() {
    uint32_t wins = scores::best("chess");
    navigation::waitRelease();

    for (;;) {
        const int difficulty = chooseDifficulty();
        if (difficulty < 0) {
            return;
        }
        navigation::waitRelease();
        g_difficulty = difficulty;

        Position pos = initialPosition();
        int cursor = 52;  // e2
        int selected = -1;
        Move legal[256];
        int legalCount = 0;
        int lastFrom = -1;
        int lastTo = -1;
        bool thinking = false;
        uint32_t thinkAt = 0;
        bool finished = false;
        int result = 0;
        g_historyCount = 0;
        g_historyScroll = 0;
        g_showHistory = false;

        auto syncLegal = [&] {
            legalCount = (selected >= 0) ? legalFrom(pos, selected, legal) : 0;
        };

        auto finishIfOver = [&] {
            if (positionEnded(pos, result)) {
                finished = true;
                if (result == 1) {
                    ++wins;
                    scores::submit("chess", wins);
                }
                return true;
            }
            return false;
        };

        while (!finished) {
            syncLegal();
            drawBoard(pos, cursor, selected, legal, legalCount, lastFrom,
                      lastTo, wins, difficulty, thinking);
            if (g_showHistory) {
                drawHistoryOverlay();
            }
            display::push();

            navigation::update();
            const navigation::EventData ev = navigation::poll();
            const uint32_t now = millis();

            if (ev.type == navigation::Event::Back) {
                if (g_showHistory) {
                    g_showHistory = false;
                    continue;
                }
                return;
            }

            if (g_showHistory) {
                if (ev.type == navigation::Event::Up) {
                    if (g_historyScroll > 0) {
                        --g_historyScroll;
                    }
                } else if (ev.type == navigation::Event::Down) {
                    if (g_historyScroll + kHistoryVisible < g_historyCount) {
                        ++g_historyScroll;
                    }
                } else if (ev.type == navigation::Event::Select ||
                           (ev.type == navigation::Event::Text &&
                            (ev.ch == 'h' || ev.ch == 'H'))) {
                    g_showHistory = false;
                }
                delay(8);
                continue;
            }

            if (thinking) {
                if (now >= thinkAt) {
                    const Move ai = chooseAiMove(pos, difficulty);
                    if (ai.from >= 0) {
                        pushHistory(pos, ai);
                        makeMove(pos, ai);
                        lastFrom = ai.from;
                        lastTo = ai.to;
                    }
                    thinking = false;
                    finishIfOver();
                }
                delay(8);
                continue;
            }

            if (ev.type == navigation::Event::Text) {
                if (ev.ch == 'h' || ev.ch == 'H') {
                    g_showHistory = true;
                    g_historyScroll =
                        std::max(0, g_historyCount - kHistoryVisible);
                    delay(8);
                    continue;
                }
                if (ev.ch == 'u' || ev.ch == 'U') {
                    if (g_historyCount >= 2) {
                        g_historyCount -= 2;
                        pos = g_history[g_historyCount].pos;
                        selected = -1;
                        if (g_historyCount > 0) {
                            lastFrom = g_history[g_historyCount - 1].move.from;
                            lastTo = g_history[g_historyCount - 1].move.to;
                        } else {
                            lastFrom = -1;
                            lastTo = -1;
                        }
                    }
                    delay(8);
                    continue;
                }
            }

            if (ev.type == navigation::Event::Left) {
                cursor = (cursor % 8 == 0) ? cursor + 7 : cursor - 1;
            } else if (ev.type == navigation::Event::Right) {
                cursor = (cursor % 8 == 7) ? cursor - 7 : cursor + 1;
            } else if (ev.type == navigation::Event::Up) {
                cursor = (cursor / 8 == 0) ? cursor + 56 : cursor - 8;
            } else if (ev.type == navigation::Event::Down) {
                cursor = (cursor / 8 == 7) ? cursor - 56 : cursor + 8;
            } else if (ev.type == navigation::Event::Select) {
                bool moved = false;
                if (selected >= 0) {
                    for (int i = 0; i < legalCount; ++i) {
                        if (legal[i].to == cursor) {
                            const Move chosen = legal[i];
                            pushHistory(pos, chosen);
                            makeMove(pos, chosen);
                            lastFrom = chosen.from;
                            lastTo = chosen.to;
                            selected = -1;
                            moved = true;
                            break;
                        }
                    }
                }
                if (moved) {
                    if (!finishIfOver()) {
                        thinking = true;
                        thinkAt = now + 320;
                    }
                } else if (selected == cursor) {
                    selected = -1;
                } else {
                    const char piece = pos.board[cursor];
                    selected = (piece != '.' && colorOf(piece) == pos.side)
                                   ? cursor
                                   : -1;
                }
            }
            delay(8);
        }

        syncLegal();
        drawBoard(pos, cursor, selected, legal, 0, lastFrom, lastTo, wins,
                  difficulty, false);
        drawResultOverlay(result, wins, difficulty);
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
