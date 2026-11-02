#include "Tetris.h"

#include <Arduino.h>

#include <algorithm>

#include "Display.h"
#include "GamesMenu.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "Scores.h"

#if ENABLE_MODULE_GAMES

namespace games {
namespace {

constexpr int kBoardW = 10;
constexpr int kBoardH = 20;
constexpr int kCell = 6;
constexpr int kBoardX = 5;
constexpr int kBoardY = 12;

constexpr int kPieceCount = 7;
constexpr int kRotations = 4;

// Maschere 4x4: bit y*4+x. I pezzi classici entrano in un 3x3 (tranne I).
constexpr uint16_t kBaseShapes[kPieceCount] = {
    0x00F0,  // I
    0x0660,  // O
    0x0072,  // T
    0x0036,  // S
    0x0063,  // Z
    0x0071,  // J
    0x0074,  // L
};

constexpr uint16_t kColors[kPieceCount] = {
    display::kInfo,    // I ciano
    display::kWarning, // O giallo
    0xA81F,            // T viola
    display::kAccent,  // S verde
    display::kDanger,  // Z rosso
    0x2D7F,            // J blu
    0xFC60,            // L arancio
};

struct Piece {
    int type = 0;
    int rot = 0;
    int x = 3;
    int y = 0;
};

uint16_t g_shapes[kPieceCount][kRotations];
uint8_t g_board[kBoardH][kBoardW];
uint32_t g_best = 0;

bool g_shapesReady = false;

uint16_t rotate3x3(uint16_t mask) {
    uint16_t out = 0;
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) {
            if (mask & (1u << (y * 4 + x))) {
                const int nx = 2 - y;
                const int ny = x;
                out |= static_cast<uint16_t>(1u << (ny * 4 + nx));
            }
        }
    }
    return out;
}

uint16_t rotate4x4(uint16_t mask) {
    uint16_t out = 0;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            if (mask & (1u << (y * 4 + x))) {
                const int nx = 3 - y;
                const int ny = x;
                out |= static_cast<uint16_t>(1u << (ny * 4 + nx));
            }
        }
    }
    return out;
}

void prepareShapes() {
    if (g_shapesReady) {
        return;
    }
    for (int type = 0; type < kPieceCount; ++type) {
        g_shapes[type][0] = kBaseShapes[type];
        for (int r = 1; r < kRotations; ++r) {
            g_shapes[type][r] = (type == 0) ? rotate4x4(g_shapes[type][r - 1])
                                            : rotate3x3(g_shapes[type][r - 1]);
        }
    }
    g_shapesReady = true;
}

bool collides(const Piece& piece) {
    const uint16_t mask = g_shapes[piece.type][piece.rot];
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            if (!(mask & (1u << (y * 4 + x)))) {
                continue;
            }
            const int bx = piece.x + x;
            const int by = piece.y + y;
            if (bx < 0 || bx >= kBoardW || by >= kBoardH) {
                return true;
            }
            if (by >= 0 && g_board[by][bx] != 0) {
                return true;
            }
        }
    }
    return false;
}

void lockPiece(const Piece& piece) {
    const uint16_t mask = g_shapes[piece.type][piece.rot];
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            if (!(mask & (1u << (y * 4 + x)))) {
                continue;
            }
            const int bx = piece.x + x;
            const int by = piece.y + y;
            if (by >= 0 && by < kBoardH && bx >= 0 && bx < kBoardW) {
                g_board[by][bx] = static_cast<uint8_t>(piece.type + 1);
            }
        }
    }
}

int clearLines() {
    int cleared = 0;
    for (int y = kBoardH - 1; y >= 0; --y) {
        bool full = true;
        for (int x = 0; x < kBoardW; ++x) {
            if (g_board[y][x] == 0) {
                full = false;
                break;
            }
        }
        if (!full) {
            continue;
        }
        ++cleared;
        for (int row = y; row > 0; --row) {
            for (int x = 0; x < kBoardW; ++x) {
                g_board[row][x] = g_board[row - 1][x];
            }
        }
        for (int x = 0; x < kBoardW; ++x) {
            g_board[0][x] = 0;
        }
        ++y;  // ricontrolla la stessa riga dopo lo shift
    }
    return cleared;
}

int dropDistance(const Piece& piece) {
    Piece probe = piece;
    int distance = 0;
    while (true) {
        ++probe.y;
        if (collides(probe)) {
            break;
        }
        ++distance;
    }
    return distance;
}

Piece randomPiece() {
    Piece piece;
    piece.type = static_cast<int>(esp_random() % kPieceCount);
    piece.rot = 0;
    piece.x = 3;
    piece.y = 0;
    return piece;
}

void drawBlock(int x, int y, int cell, uint16_t color, bool hollow = false) {
    if (hollow) {
        display::g().drawRect(x, y, cell, cell, color);
    } else {
        display::g().fillRect(x, y, cell, cell, color);
        if (cell >= 5) {
            display::g().drawRect(x, y, cell, cell, display::kBlack);
        }
    }
}

void drawNext(const Piece& next, int x, int y, int cell) {
    const uint16_t mask = g_shapes[next.type][0];
    for (int py = 0; py < 4; ++py) {
        for (int px = 0; px < 4; ++px) {
            if (mask & (1u << (py * 4 + px))) {
                drawBlock(x + px * cell, y + py * cell, cell - 1,
                          kColors[next.type]);
            }
        }
    }
}

void drawBoard(const Piece& piece, uint32_t score, uint32_t lines, int level,
               const Piece& next, bool gameOver, uint32_t best) {
    display::clear();

    // Area di gioco.
    display::g().fillRect(kBoardX - 2, kBoardY - 2,
                          kBoardW * kCell + 4, kBoardH * kCell + 4,
                          display::kPanel);
    display::g().drawRect(kBoardX - 2, kBoardY - 2,
                          kBoardW * kCell + 4, kBoardH * kCell + 4,
                          display::kAccentDark);

    for (int y = 0; y < kBoardH; ++y) {
        for (int x = 0; x < kBoardW; ++x) {
            if (g_board[y][x] != 0) {
                drawBlock(kBoardX + x * kCell, kBoardY + y * kCell, kCell - 1,
                          kColors[g_board[y][x] - 1]);
            }
        }
    }

    // Ghost piece (posizione di caduta).
    Piece ghost = piece;
    ghost.y += dropDistance(piece);
    const uint16_t ghostMask = g_shapes[ghost.type][ghost.rot];
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            if (ghostMask & (1u << (y * 4 + x))) {
                const int bx = ghost.x + x;
                const int by = ghost.y + y;
                if (by >= 0 && by < kBoardH && bx >= 0 && bx < kBoardW) {
                    drawBlock(kBoardX + bx * kCell, kBoardY + by * kCell,
                              kCell - 1, display::kTextDim, true);
                }
            }
        }
    }

    const uint16_t mask = g_shapes[piece.type][piece.rot];
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            if (mask & (1u << (y * 4 + x))) {
                const int bx = piece.x + x;
                const int by = piece.y + y;
                if (by >= 0 && by < kBoardH && bx >= 0 && bx < kBoardW) {
                    drawBlock(kBoardX + bx * kCell, kBoardY + by * kCell,
                              kCell - 1, kColors[piece.type]);
                }
            }
        }
    }

    // Pannello laterale.
    const int sx = kBoardX + kBoardW * kCell + 10;
    display::text(sx, 8, "TETRIS", display::kAccent, 2);
    display::text(sx, 30, "PUNTI", display::kTextDim, 0);
    display::text(sx, 40, std::to_string(score), display::kText, 2);
    display::text(sx, 58, "RECORD", display::kTextDim, 0);
    display::text(sx, 68, std::to_string(std::max<uint32_t>(best, score)),
                  display::kInfo, 0);
    display::text(sx, 82, "LIVELLO " + std::to_string(level) + "   LINEE " +
                              std::to_string(lines),
                  display::kTextDim, 0);

    display::text(sx, 96, "PROSSIMO", display::kTextDim, 0);
    drawNext(next, sx + 4, 106, 4);

    display::text(sx + 46, 106, "; ruota", display::kTextDim, 0);
    display::text(sx + 46, 116, ", / muovi", display::kTextDim, 0);
    display::text(sx + 46, 126, ". giu  SPC drop", display::kTextDim, 0);

    if (gameOver) {
        const int w = 138;
        const int h = 44;
        const int x = (display::width() - w) / 2;
        const int y = (display::height() - h) / 2;
        display::g().fillRect(x, y, w, h, display::kBlack);
        display::g().drawRect(x, y, w, h, display::kDanger);
        display::textCentered(y + 8, "GAME OVER", display::kDanger, 2);
        display::textCentered(y + 26, "INVIO = riprova  ` = esci",
                              display::kTextDim, 0);
    }

    display::push();
}

void resetGame(Piece& piece, Piece& next, uint32_t& score, uint32_t& lines,
               int& level) {
    memset(g_board, 0, sizeof(g_board));
    score = 0;
    lines = 0;
    level = 1;
    piece = randomPiece();
    next = randomPiece();
}

}  // namespace

void runTetris() {
    prepareShapes();
    memset(g_board, 0, sizeof(g_board));
    g_best = std::max(g_best, scores::best("tetris"));

    Piece piece = randomPiece();
    Piece next = randomPiece();
    uint32_t score = 0;
    uint32_t lines = 0;
    int level = 1;
    bool gameOver = false;
    uint32_t dropTimer = millis();
    uint32_t leftRepeat = 0;
    uint32_t rightRepeat = 0;
    uint32_t softRepeat = 0;
    bool spaceHeld = false;
    uint32_t renderedAt = 0;

    navigation::waitRelease();

    while (true) {
        navigation::update();
        const navigation::EventData ev = navigation::poll();
        const uint32_t now = millis();

        if (ev.type == navigation::Event::Back) {
            break;
        }

        if (gameOver) {
            if (ev.type == navigation::Event::Select) {
                resetGame(piece, next, score, lines, level);
                gameOver = false;
                dropTimer = now;
                spaceHeld = navigation::pressed(' ');
            }
            if (now - renderedAt > 50) {
                drawBoard(piece, score, lines, level, next, gameOver, g_best);
                renderedAt = now;
            }
            delay(10);
            continue;
        }

        bool rotate = (ev.type == navigation::Event::Up) ||
                      (ev.type == navigation::Event::Text && ev.ch == 'w');

        if (rotate) {
            Piece rotated = piece;
            rotated.rot = (rotated.rot + 1) % kRotations;
            if (!collides(rotated)) {
                piece = rotated;
            } else {
                for (int kick : {-1, 1, -2, 2}) {
                    rotated.x = piece.x + kick;
                    if (!collides(rotated)) {
                        piece = rotated;
                        break;
                    }
                }
            }
        }

        const bool leftHeld = navigation::pressed(',');
        const bool rightHeld = navigation::pressed('/');
        const bool downHeld = navigation::pressed('.');

        if (ev.type == navigation::Event::Left || (leftHeld && now >= leftRepeat)) {
            Piece moved = piece;
            --moved.x;
            if (!collides(moved)) {
                piece = moved;
            }
            leftRepeat = now + (ev.type == navigation::Event::Left ? 180 : 110);
        }
        if (ev.type == navigation::Event::Right ||
            (rightHeld && now >= rightRepeat)) {
            Piece moved = piece;
            ++moved.x;
            if (!collides(moved)) {
                piece = moved;
            }
            rightRepeat = now + (ev.type == navigation::Event::Right ? 180 : 110);
        }
        if (leftHeld && ev.type != navigation::Event::Left) {
            // il timer parte solo se il tasto e' gia' tenuto
            if (leftRepeat == 0) {
                leftRepeat = now + 180;
            }
        }
        if (rightHeld && ev.type != navigation::Event::Right) {
            if (rightRepeat == 0) {
                rightRepeat = now + 180;
            }
        }

        if (ev.type == navigation::Event::Down ||
            (downHeld && now >= softRepeat)) {
            Piece moved = piece;
            ++moved.y;
            if (!collides(moved)) {
                piece = moved;
                score += 1;
            }
            softRepeat = now + (ev.type == navigation::Event::Down ? 140 : 55);
            dropTimer = now;
        }

        const bool spaceNow = navigation::pressed(' ');
        if (spaceNow && !spaceHeld) {
            const int distance = dropDistance(piece);
            piece.y += distance;
            score += static_cast<uint32_t>(distance) * 2;
            dropTimer = now;
            spaceHeld = true;
        } else if (!spaceNow) {
            spaceHeld = false;
        }

        if (now - dropTimer >=
            static_cast<uint32_t>(std::max(90, 720 - (level - 1) * 65))) {
            dropTimer = now;
            Piece moved = piece;
            ++moved.y;
            if (!collides(moved)) {
                piece = moved;
            } else {
                lockPiece(piece);
                const int cleared = clearLines();
                if (cleared > 0) {
                    static const uint32_t kScoreTable[5] = {0, 100, 300, 500,
                                                            800};
                    score +=
                        kScoreTable[std::min(cleared, 4)] * level;
                    lines += static_cast<uint32_t>(cleared);
                    level = 1 + static_cast<int>(lines / 10);
                }
                piece = next;
                next = randomPiece();
                if (collides(piece)) {
                    gameOver = true;
                    g_best = std::max(g_best, score);
                    scores::submit("tetris", score);
                }
                dropTimer = now;
                leftRepeat = 0;
                rightRepeat = 0;
                softRepeat = 0;
            }
        }

        if (now - renderedAt > 33) {
            drawBoard(piece, score, lines, level, next, gameOver, g_best);
            renderedAt = now;
        }

        delay(8);
    }

    g_best = std::max(g_best, score);
}

}  // namespace games

#endif  // ENABLE_MODULE_GAMES
