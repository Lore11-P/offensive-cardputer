#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>

// Regole del gioco degli scacchi, indipendenti da Arduino/display, cosi' da
// poter essere verificate anche su host (perft).
namespace chess {

constexpr int kSquares = 64;

inline int rowOf(int sq) { return sq >> 3; }
inline int colOf(int sq) { return sq & 7; }
inline int sqOf(int r, int c) { return r * 8 + c; }
inline bool onBoard(int r, int c) { return r >= 0 && r < 8 && c >= 0 && c < 8; }
inline bool isWhitePiece(char p) { return p >= 'A' && p <= 'Z'; }
inline bool isBlackPiece(char p) { return p >= 'a' && p <= 'z'; }
inline int colorOf(char p) {
    return p == '.' ? 0 : (isWhitePiece(p) ? 1 : -1);
}
inline char upperOf(char p) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(p)));
}

struct Move {
    int8_t from = -1;
    int8_t to = -1;
};

struct Position {
    char board[kSquares];
    int8_t side = 1;      // 1 = bianco, -1 = nero
    uint8_t castle = 0;   // 1 WK, 2 WQ, 4 BK, 8 BQ
    int8_t ep = -1;       // casa di en passant
    uint16_t halfmove = 0;
};

inline Move mkMove(int from, int to) {
    Move m;
    m.from = static_cast<int8_t>(from);
    m.to = static_cast<int8_t>(to);
    return m;
}

inline Position initialPosition() {
    static const char* const kInitial[8] = {
        "rnbqkbnr", "pppppppp", "........", "........",
        "........", "........", "PPPPPPPP", "RNBQKBNR",
    };
    Position pos;
    pos.side = 1;
    pos.castle = 0x0F;
    pos.ep = -1;
    pos.halfmove = 0;
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            pos.board[sqOf(r, c)] = kInitial[r][c];
        }
    }
    return pos;
}

inline int kingSquare(const Position& pos, int side) {
    const char king = (side == 1) ? 'K' : 'k';
    for (int i = 0; i < kSquares; ++i) {
        if (pos.board[i] == king) {
            return i;
        }
    }
    return -1;
}

inline bool squareAttacked(const Position& pos, int sq, int bySide) {
    const int r = rowOf(sq);
    const int c = colOf(sq);

    const int pawnRow = (bySide == 1) ? r + 1 : r - 1;
    if (pawnRow >= 0 && pawnRow < 8) {
        const char pawn = (bySide == 1) ? 'P' : 'p';
        if (c > 0 && pos.board[sqOf(pawnRow, c - 1)] == pawn) {
            return true;
        }
        if (c < 7 && pos.board[sqOf(pawnRow, c + 1)] == pawn) {
            return true;
        }
    }

    static const int kKnight[8][2] = {{-2, -1}, {-2, 1}, {-1, -2}, {-1, 2},
                                      {1, -2},  {1, 2},  {2, -1},  {2, 1}};
    const char knight = (bySide == 1) ? 'N' : 'n';
    for (const auto& d : kKnight) {
        const int nr = r + d[0];
        const int nc = c + d[1];
        if (onBoard(nr, nc) && pos.board[sqOf(nr, nc)] == knight) {
            return true;
        }
    }

    static const int kKing[8][2] = {{-1, -1}, {-1, 0}, {-1, 1}, {0, -1},
                                    {0, 1},   {1, -1}, {1, 0},  {1, 1}};
    const char king = (bySide == 1) ? 'K' : 'k';
    for (const auto& d : kKing) {
        const int nr = r + d[0];
        const int nc = c + d[1];
        if (onBoard(nr, nc) && pos.board[sqOf(nr, nc)] == king) {
            return true;
        }
    }

    static const int kRookDirs[4][2] = {
        {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    const char rook = (bySide == 1) ? 'R' : 'r';
    const char queen = (bySide == 1) ? 'Q' : 'q';
    for (const auto& d : kRookDirs) {
        int nr = r + d[0];
        int nc = c + d[1];
        while (onBoard(nr, nc)) {
            const char t = pos.board[sqOf(nr, nc)];
            if (t != '.') {
                if (t == rook || t == queen) {
                    return true;
                }
                break;
            }
            nr += d[0];
            nc += d[1];
        }
    }

    static const int kBishopDirs[4][2] = {
        {-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
    const char bishop = (bySide == 1) ? 'B' : 'b';
    for (const auto& d : kBishopDirs) {
        int nr = r + d[0];
        int nc = c + d[1];
        while (onBoard(nr, nc)) {
            const char t = pos.board[sqOf(nr, nc)];
            if (t != '.') {
                if (t == bishop || t == queen) {
                    return true;
                }
                break;
            }
            nr += d[0];
            nc += d[1];
        }
    }
    return false;
}

inline bool inCheck(const Position& pos, int side) {
    const int king = kingSquare(pos, side);
    return king >= 0 && squareAttacked(pos, king, -side);
}

inline void makeMove(Position& pos, const Move& mv) {
    const int from = mv.from;
    const int to = mv.to;
    const char piece = pos.board[from];
    char captured = pos.board[to];
    const bool white = isWhitePiece(piece);
    const char type = upperOf(piece);
    const bool pawnMove = (type == 'P');
    bool capture = (captured != '.');

    // Cattura en passant: il pedone catturato non e' sulla casa di arrivo.
    if (pawnMove && captured == '.' && to == pos.ep &&
        colOf(from) != colOf(to)) {
        const int capSq = (pos.side == 1) ? (to + 8) : (to - 8);
        captured = pos.board[capSq];
        pos.board[capSq] = '.';
        capture = true;
    }

    pos.board[to] = piece;
    pos.board[from] = '.';

    // Arrocco: il re si sposta di due colonne, la torre lo segue.
    if (type == 'K' && (to - from == 2 || from - to == 2)) {
        if (to == 62) {
            pos.board[61] = pos.board[63];
            pos.board[63] = '.';
        } else if (to == 58) {
            pos.board[59] = pos.board[56];
            pos.board[56] = '.';
        } else if (to == 6) {
            pos.board[5] = pos.board[7];
            pos.board[7] = '.';
        } else if (to == 2) {
            pos.board[3] = pos.board[0];
            pos.board[0] = '.';
        }
    }

    // Promozione automatica a donna.
    const int toRow = rowOf(to);
    if (pawnMove && (toRow == 0 || toRow == 7)) {
        pos.board[to] = white ? 'Q' : 'q';
    }

    auto clearRights = [&](uint8_t bits) {
        pos.castle &= static_cast<uint8_t>(~bits);
    };
    if (type == 'K') {
        clearRights(white ? 0x03 : 0x0C);
    }
    if (type == 'R') {
        if (from == 63) {
            clearRights(0x01);
        } else if (from == 56) {
            clearRights(0x02);
        } else if (from == 7) {
            clearRights(0x04);
        } else if (from == 0) {
            clearRights(0x08);
        }
    }
    if (capture && upperOf(captured) == 'R') {
        if (to == 63) {
            clearRights(0x01);
        } else if (to == 56) {
            clearRights(0x02);
        } else if (to == 7) {
            clearRights(0x04);
        } else if (to == 0) {
            clearRights(0x08);
        }
    }

    pos.ep = -1;
    if (pawnMove && (to - from == 16 || from - to == 16)) {
        pos.ep = static_cast<int8_t>((from + to) / 2);
    }

    if (pawnMove || capture) {
        pos.halfmove = 0;
    } else {
        ++pos.halfmove;
    }
    pos.side = static_cast<int8_t>(-pos.side);
}

inline int genPseudo(const Position& pos, Move* out) {
    int n = 0;
    const int side = pos.side;
    for (int sq = 0; sq < kSquares; ++sq) {
        const char p = pos.board[sq];
        if (p == '.' || colorOf(p) != side) {
            continue;
        }
        const int r = rowOf(sq);
        const int c = colOf(sq);
        const char type = upperOf(p);

        if (type == 'P') {
            const int dir = (side == 1) ? -8 : 8;
            const int startRow = (side == 1) ? 6 : 1;
            const int one = sq + dir;
            if (one >= 0 && one < kSquares && pos.board[one] == '.') {
                out[n++] = mkMove(sq, one);
                if (r == startRow) {
                    const int two = sq + 2 * dir;
                    if (two >= 0 && two < kSquares && pos.board[two] == '.') {
                        out[n++] = mkMove(sq, two);
                    }
                }
            }
            for (int dc = -1; dc <= 1; dc += 2) {
                const int nc = c + dc;
                if (nc < 0 || nc >= 8) {
                    continue;
                }
                const int to = sq + dir + dc;
                if (to < 0 || to >= kSquares) {
                    continue;
                }
                const char target = pos.board[to];
                if (colorOf(target) == -side) {
                    out[n++] = mkMove(sq, to);
                } else if (target == '.' && to == pos.ep) {
                    out[n++] = mkMove(sq, to);
                }
            }
        } else if (type == 'N') {
            static const int kOff[8][2] = {{-2, -1}, {-2, 1}, {-1, -2}, {-1, 2},
                                           {1, -2},  {1, 2},  {2, -1},  {2, 1}};
            for (const auto& d : kOff) {
                const int nr = r + d[0];
                const int nc = c + d[1];
                if (!onBoard(nr, nc)) {
                    continue;
                }
                const int to = sqOf(nr, nc);
                if (colorOf(pos.board[to]) != side) {
                    out[n++] = mkMove(sq, to);
                }
            }
        } else if (type == 'B' || type == 'R' || type == 'Q') {
            static const int kDirs[8][2] = {
                {-1, -1}, {-1, 1}, {1, -1}, {1, 1},
                {-1, 0},  {1, 0},  {0, -1}, {0, 1}};
            const int begin = (type == 'B') ? 0 : ((type == 'R') ? 4 : 0);
            const int end = (type == 'B') ? 4 : 8;
            for (int di = begin; di < end; ++di) {
                int nr = r + kDirs[di][0];
                int nc = c + kDirs[di][1];
                while (onBoard(nr, nc)) {
                    const int to = sqOf(nr, nc);
                    const char target = pos.board[to];
                    if (colorOf(target) == side) {
                        break;
                    }
                    out[n++] = mkMove(sq, to);
                    if (target != '.') {
                        break;
                    }
                    nr += kDirs[di][0];
                    nc += kDirs[di][1];
                }
            }
        } else if (type == 'K') {
            static const int kKing[8][2] = {
                {-1, -1}, {-1, 0}, {-1, 1}, {0, -1},
                {0, 1},   {1, -1}, {1, 0},  {1, 1}};
            for (const auto& d : kKing) {
                const int nr = r + d[0];
                const int nc = c + d[1];
                if (!onBoard(nr, nc)) {
                    continue;
                }
                const int to = sqOf(nr, nc);
                if (colorOf(pos.board[to]) != side) {
                    out[n++] = mkMove(sq, to);
                }
            }
            // Arrocco: si verifica lo scacco simulando il re sulle case
            // di partenza, transito e arrivo (evita il caso del re che fa
            // da schermo a una torre avversaria).
            auto safeOn = [&](int square) {
                Position copy = pos;
                copy.board[sq] = '.';
                copy.board[square] = (side == 1) ? 'K' : 'k';
                return !squareAttacked(copy, square, -side);
            };
            if (side == 1 && sq == 60) {
                if ((pos.castle & 0x01) && pos.board[63] == 'R' &&
                    pos.board[61] == '.' && pos.board[62] == '.' &&
                    safeOn(60) && safeOn(61) && safeOn(62)) {
                    out[n++] = mkMove(60, 62);
                }
                if ((pos.castle & 0x02) && pos.board[56] == 'R' &&
                    pos.board[57] == '.' && pos.board[58] == '.' &&
                    pos.board[59] == '.' && safeOn(60) && safeOn(59) &&
                    safeOn(58)) {
                    out[n++] = mkMove(60, 58);
                }
            } else if (side == -1 && sq == 4) {
                if ((pos.castle & 0x04) && pos.board[7] == 'r' &&
                    pos.board[5] == '.' && pos.board[6] == '.' &&
                    safeOn(4) && safeOn(5) && safeOn(6)) {
                    out[n++] = mkMove(4, 6);
                }
                if ((pos.castle & 0x08) && pos.board[0] == 'r' &&
                    pos.board[1] == '.' && pos.board[2] == '.' &&
                    pos.board[3] == '.' && safeOn(4) && safeOn(3) &&
                    safeOn(2)) {
                    out[n++] = mkMove(4, 2);
                }
            }
        }
    }
    return n;
}

inline int genLegal(const Position& pos, Move* out) {
    Move pseudo[256];
    const int pn = genPseudo(pos, pseudo);
    const int mover = pos.side;
    int n = 0;
    for (int i = 0; i < pn; ++i) {
        Position copy = pos;
        makeMove(copy, pseudo[i]);
        if (!inCheck(copy, mover)) {
            out[n++] = pseudo[i];
        }
    }
    return n;
}

inline int pieceValue(char p) {
    switch (upperOf(p)) {
        case 'P':
            return 100;
        case 'N':
            return 320;
        case 'B':
            return 330;
        case 'R':
            return 500;
        case 'Q':
            return 900;
        default:
            return 0;
    }
}

inline int evaluateWhite(const Position& pos) {
    int score = 0;
    for (int sq = 0; sq < kSquares; ++sq) {
        const char p = pos.board[sq];
        if (p == '.') {
            continue;
        }
        const int sign = isWhitePiece(p) ? 1 : -1;
        const char type = upperOf(p);
        int value = pieceValue(p);
        const int r = rowOf(sq);
        const int c = colOf(sq);
        if (type == 'P') {
            const int advance = isWhitePiece(p) ? (6 - r) : (r - 1);
            value += std::max(0, advance) * 8;
            if (c >= 2 && c <= 5) {
                value += 5;
            }
        } else if (type == 'N' || type == 'B') {
            if (c >= 2 && c <= 5 && r >= 2 && r <= 5) {
                value += 10;
            }
        }
        score += sign * value;
    }
    return score;
}

inline int moveScore(const Position& pos, const Move& mv) {
    const char target = pos.board[mv.to];
    const char mover = pos.board[mv.from];
    int value = 0;
    if (target != '.') {
        value = 10 * pieceValue(target) - pieceValue(mover) / 10;
    } else if (upperOf(mover) == 'P' && mv.to == pos.ep) {
        value = 900;
    }
    return value;
}

inline int negamax(const Position& pos, int depth, int alpha, int beta,
                   int* nodes = nullptr) {
    if (nodes && ++(*nodes) > 60000) {
        return evaluateWhite(pos) * pos.side;
    }
    if (depth <= 0) {
        return evaluateWhite(pos) * pos.side;
    }
    Move moves[256];
    const int n = genLegal(pos, moves);
    if (n == 0) {
        if (inCheck(pos, pos.side)) {
            return -100000 - depth;
        }
        return 0;
    }
    std::sort(moves, moves + n, [&](const Move& a, const Move& b) {
        return moveScore(pos, a) > moveScore(pos, b);
    });
    int best = -2000000;
    for (int i = 0; i < n; ++i) {
        Position copy = pos;
        makeMove(copy, moves[i]);
        const int score = -negamax(copy, depth - 1, -beta, -alpha, nodes);
        if (score > best) {
            best = score;
        }
        if (best > alpha) {
            alpha = best;
        }
        if (alpha >= beta) {
            break;
        }
    }
    return best;
}

}  // namespace chess
