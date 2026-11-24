#include "Display.h"

#include <algorithm>
#include <cmath>

// Icone procedurali Orbit: griglie 16x16 disegnate con rettangoli e pixel.
// Nessun BMP, nessun font: cambiano colore insieme al tema.
namespace display {
namespace {

struct Pen {
    int ox, oy, u;
    uint16_t main, alt, hot;
    int w, h;

    Pen(int x, int y, int size, uint16_t tint) {
        u = std::max(1, size / 16);
        w = 16 * u;
        h = 16 * u;
        ox = x + (size - w) / 2;
        oy = y + (size - h) / 2;
        main = tint ? tint : C(T_sec);
        alt = tint ? tint : C(T_acc);
        hot = tint ? tint : C(T_gold);
    }

    inline int X(int gx) const { return ox + gx * u; }
    inline int Y(int gy) const { return oy + gy * u; }
    void rect(int gx, int gy, int gw, int gh, uint16_t c) const {
        g().fillRect(X(gx), Y(gy), gw * u, gh * u, c);
    }
    void frame(int gx, int gy, int gw, int gh, uint16_t c) const {
        auto& d = g();
        d.drawRect(X(gx), Y(gy), gw * u, gh * u, c);
        if (u > 1) {
            d.drawRect(X(gx) + 1, Y(gy) + 1, gw * u - 2, gh * u - 2, c);
        }
    }
    void pix(int gx, int gy, uint16_t c) const { rect(gx, gy, 1, 1, c); }
    void vline(int gx, int gy, int gh, uint16_t c) const { rect(gx, gy, 1, gh, c); }
    void hline(int gx, int gy, int gw, uint16_t c) const { rect(gx, gy, gw, 1, c); }
    void circle(int gx, int gy, int gr, uint16_t c) const {
        auto& d = g();
        d.drawCircle(X(gx), Y(gy), gr * u, c);
        if (u > 1) {
            d.drawCircle(X(gx), Y(gy), gr * u - 1, c);
        }
    }
    void disc(int gx, int gy, int gr, uint16_t c) const {
        g().fillCircle(X(gx), Y(gy), gr * u, c);
    }
    void dot(int gx, int gy, uint16_t c) const { disc(gx, gy, 1, c); }
    void line(int x0, int y0, int x1, int y1, uint16_t c) const {
        g().drawLine(X(x0), Y(y0), X(x1), Y(y1), c);
        if (u > 1) {
            g().drawLine(X(x0), Y(y0) + 1, X(x1), Y(y1) + 1, c);
        }
    }
};

}  // namespace

bool drawOrbitIcon(const std::string& id, int x, int y, int size, uint16_t tint) {
    Pen p(x, y, size, tint);
    const uint16_t main = p.main;
    const uint16_t alt = p.alt;
    const uint16_t hot = p.hot;
    const uint16_t bg = C(T_bg2);

    if (id == "ble") {
        p.vline(7, 2, 12, main);
        p.line(7, 2, 12, 6, main);
        p.line(12, 6, 5, 11, main);
        p.line(5, 5, 12, 10, main);
        p.line(12, 10, 7, 14, main);
        p.dot(7, 1, alt);
        return true;
    }
    if (id == "hid") {
        // Tastiera con antenna: BLE HID.
        p.dot(5, 2, alt);
        p.dot(8, 2, hot);
        p.dot(11, 2, main);
        p.frame(2, 5, 12, 8, main);
        p.rect(3, 6, 10, 6, C(T_bg3));
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 4; ++col) {
                p.pix(4 + col * 2, 7 + row * 2, alt);
            }
        }
        p.hline(4, 11, 8, C(T_line));
        return true;
    }
    if (id == "chat") {
        p.frame(2, 3, 12, 8, main);
        p.rect(3, 4, 10, 6, C(T_bg3));
        p.rect(4, 9, 3, 1, C(T_bg3));
        p.pix(4, 10, C(T_bg3));
        p.pix(5, 11, C(T_bg3));
        p.dot(5, 7, alt);
        p.dot(8, 7, alt);
        p.dot(11, 7, alt);
        p.dot(13, 2, hot);
        return true;
    }
    if (id == "espnow") {
        // Mesh di nodi collegati: ESP-NOW.
        p.line(3, 4, 12, 5, C(T_line));
        p.line(3, 4, 8, 12, C(T_line));
        p.line(12, 5, 8, 12, C(T_line));
        p.disc(3, 4, 2, main);
        p.disc(12, 5, 2, alt);
        p.disc(8, 12, 2, hot);
        return true;
    }
    if (id == "wifi") {
        auto& d = g();
        const int cx = p.X(8);
        const int cy = p.Y(13);
        for (int i = 0; i < 3; ++i) {
            const int r = (4 + i * 4) * p.u;
            d.drawArc(cx, cy, r, r + p.u, 220, 320, i == 2 ? alt : main);
        }
        d.fillCircle(cx, cy, std::max(1, p.u + 1), hot);
        return true;
    }
    if (id == "wifiplus") {
        // Onde WiFi con badge "+".
        auto& d = g();
        const int cx = p.X(8);
        const int cy = p.Y(13);
        for (int i = 0; i < 2; ++i) {
            const int r = (4 + i * 4) * p.u;
            d.drawArc(cx, cy, r, r + p.u, 220, 320, main);
        }
        d.fillCircle(cx, cy, std::max(1, p.u + 1), hot);
        d.fillCircle(p.X(12), p.Y(4), 3 * p.u, C(T_bg2));
        p.hline(10, 4, 5, alt);
        p.vline(12, 2, 5, alt);
        return true;
    }
    if (id == "wifilab") {
        // Beuta da laboratorio: WiFi Lab.
        p.hline(6, 2, 5, main);
        p.line(6, 3, 6, 6, main);
        p.line(10, 3, 10, 6, main);
        p.line(6, 6, 3, 13, main);
        p.line(10, 6, 13, 13, main);
        p.hline(3, 13, 11, main);
        p.hline(5, 10, 7, alt);
        p.dot(7, 8, hot);
        p.dot(9, 6, hot);
        return true;
    }
    if (id == "wpasec") {
        // Scudo con toppa: catture e chiave WPA-SEC.
        p.line(8, 1, 3, 4, main);
        p.line(8, 1, 13, 4, main);
        p.line(3, 4, 3, 9, main);
        p.line(13, 4, 13, 9, main);
        p.line(3, 9, 8, 14, main);
        p.line(13, 9, 8, 14, main);
        p.disc(8, 7, 1, alt);
        p.vline(8, 8, 2, alt);
        return true;
    }
    if (id == "radar") {
        p.circle(8, 8, 7, main);
        p.circle(8, 8, 4, C(T_line));
        p.line(8, 8, 14, 3, alt);
        p.dot(11, 6, hot);
        p.dot(5, 10, main);
        p.dot(8, 8, alt);
        return true;
    }
    if (id == "tv") {
        p.frame(2, 4, 12, 8, main);
        p.rect(3, 5, 10, 6, C(T_bg3));
        p.rect(5, 7, 4, 1, alt);
        p.rect(5, 9, 6, 1, C(T_line));
        p.line(8, 4, 5, 1, main);
        p.line(8, 4, 12, 1, main);
        p.hline(6, 13, 4, main);
        return true;
    }
    if (id == "music") {
        p.vline(5, 5, 8, main);
        p.vline(12, 2, 8, main);
        p.rect(5, 3, 8, 2, main);
        p.disc(4, 13, 2, alt);
        p.disc(11, 10, 2, alt);
        p.dot(13, 1, hot);
        return true;
    }
    if (id == "audio") {
        // Forma d'onda: microfono, spettro e registrazioni.
        const int bars[7] = {3, 6, 10, 13, 9, 5, 2};
        for (int i = 0; i < 7; ++i) {
            p.vline(1 + i * 2, 8 - bars[i] / 2, bars[i],
                    i == 3 ? hot : (i % 2 ? main : alt));
        }
        return true;
    }
    if (id == "capture") {
        p.frame(1, 2, 9, 12, main);
        p.hline(3, 5, 5, alt);
        p.hline(3, 8, 5, C(T_line));
        p.hline(3, 11, 3, C(T_line));
        auto& d = g();
        const int ax = p.X(13);
        const int ay = p.Y(8);
        d.drawArc(ax, ay, 2 * p.u, 3 * p.u, 250, 290, hot);
        d.drawArc(ax, ay, 4 * p.u, 5 * p.u, 250, 290, hot);
        d.fillCircle(ax, ay, std::max(1, p.u), hot);
        return true;
    }
    if (id == "usb") {
        p.vline(8, 3, 11, main);
        p.line(8, 3, 6, 1, main);
        p.line(8, 3, 10, 1, main);
        p.line(8, 8, 4, 5, main);
        p.line(8, 11, 12, 8, main);
        p.disc(4, 4, 1, alt);
        p.disc(12, 7, 1, alt);
        p.disc(8, 14, 1, hot);
        return true;
    }
    if (id == "ir" || id == "learn") {
        p.frame(2, 3, 6, 11, main);
        p.rect(3, 4, 4, 3, alt);
        p.dot(4, 9, hot);
        p.dot(6, 9, C(T_line));
        p.dot(4, 12, C(T_line));
        p.line(10, 5, 14, 5, C(T_line));
        p.line(10, 8, 15, 8, C(T_line));
        p.line(10, 11, 14, 11, C(T_line));
        p.pix(15, 8, hot);
        return true;
    }
    if (id == "games") {
        p.rect(2, 5, 12, 7, C(T_bg3));
        p.frame(1, 4, 14, 9, main);
        p.rect(4, 7, 3, 1, alt);
        p.rect(5, 6, 1, 3, alt);
        p.dot(11, 6, hot);
        p.dot(13, 8, hot);
        p.pix(12, 10, main);
        p.pix(3, 3, main);
        return true;
    }
    if (id == "sd") {
        p.rect(4, 2, 8, 12, C(T_bg3));
        p.frame(4, 2, 8, 12, main);
        p.rect(5, 3, 3, 2, alt);
        p.hline(6, 8, 4, alt);
        p.hline(6, 11, 4, C(T_line));
        return true;
    }
    if (id == "settings") {
        p.circle(8, 8, 5, main);
        p.disc(8, 8, 2, alt);
        for (int i = 0; i < 8; ++i) {
            const float a = i * 0.7853982f;
            const int x0 = 8 + static_cast<int>(cosf(a) * 10.0f);
            const int y0 = 8 + static_cast<int>(sinf(a) * 10.0f);
            p.pix(x0 / 2 + 4, y0 / 2 + 4, main);
        }
        p.dot(8, 1, hot);
        return true;
    }
    if (id == "tools") {
        // Cassetta attrezzi: Utility (clock, note, calcolatrice...).
        p.line(6, 5, 6, 2, main);
        p.line(10, 5, 10, 2, main);
        p.hline(6, 2, 5, main);
        p.rect(2, 5, 12, 9, C(T_bg3));
        p.frame(2, 5, 12, 9, main);
        p.hline(3, 8, 10, C(T_line));
        p.rect(7, 8, 2, 4, hot);
        p.dot(4, 6, alt);
        p.dot(12, 6, alt);
        return true;
    }
    if (id == "sensors") {
        // Livella a bolla: sensori IMU e strumenti di misura.
        p.rect(1, 5, 14, 9, C(T_bg3));
        p.frame(1, 5, 14, 9, main);
        p.hline(3, 9, 10, C(T_line));
        p.vline(11, 7, 5, C(T_line2));
        p.disc(6, 9, 2, alt);
        p.dot(6, 9, hot);
        return true;
    }
    if (id == "2048") {
        p.frame(1, 1, 14, 14, main);
        p.rect(2, 2, 6, 6, C(T_bg3));
        p.rect(8, 2, 6, 6, C(T_bg3));
        p.rect(2, 8, 6, 6, C(T_bg3));
        p.rect(8, 8, 6, 6, alt);
        p.rect(4, 4, 2, 2, main);
        p.rect(10, 10, 2, 2, hot);
        return true;
    }
    if (id == "scan") {
        p.circle(7, 7, 5, main);
        p.disc(6, 6, 1, alt);
        p.line(10, 11, 14, 15, hot);
        p.pix(7, 4, alt);
        return true;
    }
    if (id == "backup") {
        p.frame(2, 4, 12, 9, main);
        p.vline(4, 6, 5, C(T_line));
        p.vline(11, 6, 5, C(T_line));
        p.line(8, 2, 8, 9, alt);
        p.line(8, 9, 5, 6, alt);
        p.line(8, 9, 11, 6, alt);
        return true;
    }
    if (id == "connect4") {
        p.frame(2, 2, 12, 12, main);
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 2; ++col) {
                p.disc(5 + col * 6, 5 + row * 6, 2, (row + col) % 2 ? hot : alt);
            }
        }
        return true;
    }
    if (id == "doom") {
        p.circle(8, 8, 6, main);
        p.circle(8, 8, 3, alt);
        p.line(8, 1, 8, 5, main);
        p.line(8, 11, 8, 15, main);
        p.line(1, 8, 5, 8, main);
        p.line(11, 8, 15, 8, main);
        p.dot(8, 8, hot);
        return true;
    }
    if (id == "flappy") {
        p.rect(12, 1, 3, 6, main);
        p.rect(11, 6, 4, 6, main);
        p.rect(11, 0, 5, 2, main);
        p.disc(7, 8, 3, hot);
        p.pix(8, 7, C(T_bg));
        p.pix(11, 9, alt);
        return true;
    }
    if (id == "file") {
        p.rect(4, 1, 8, 14, C(T_bg3));
        p.frame(4, 1, 8, 14, main);
        p.rect(9, 1, 3, 3, main);
        p.hline(6, 6, 4, alt);
        p.hline(6, 8, 4, C(T_line));
        p.hline(6, 10, 3, C(T_line));
        return true;
    }
    if (id == "info") {
        p.circle(8, 8, 7, main);
        p.disc(8, 4, 1, alt);
        p.vline(8, 7, 5, alt);
        return true;
    }
    if (id == "jammer") {
        p.disc(4, 13, 2, main);
        p.vline(4, 4, 7, main);
        p.line(4, 4, 1, 1, main);
        p.line(4, 4, 8, 1, main);
        p.line(9, 6, 14, 11, alt);
        p.line(14, 6, 9, 11, alt);
        p.pix(2, 12, hot);
        return true;
    }
    if (id == "lock") {
        p.rect(3, 7, 10, 8, main);
        p.frame(3, 7, 10, 8, hot);
        p.circle(8, 6, 3, main);
        p.rect(5, 6, 6, 6, C(T_bg2));
        p.disc(8, 10, 1, C(T_bg));
        p.vline(8, 10, 2, C(T_bg));
        return true;
    }
    if (id == "mines") {
        p.frame(1, 1, 14, 14, main);
        p.hline(1, 6, 14, C(T_line));
        p.hline(1, 11, 14, C(T_line));
        p.vline(6, 1, 14, C(T_line));
        p.vline(11, 1, 14, C(T_line));
        p.disc(8, 8, 2, hot);
        p.line(8, 3, 8, 5, hot);
        return true;
    }
    if (id == "minecraft") {
        p.rect(2, 2, 12, 12, C(T_bg3));
        p.frame(2, 2, 12, 12, main);
        p.rect(2, 2, 12, 4, alt);
        p.pix(5, 8, main);
        p.pix(10, 10, alt);
        p.pix(8, 6, hot);
        return true;
    }
    if (id == "pacman") {
        p.disc(7, 8, 6, hot);
        g().fillTriangle(p.X(7), p.Y(8), p.X(15), p.Y(3), p.X(15), p.Y(9), bg);
        p.pix(6, 5, bg);
        p.dot(13, 3, main);
        return true;
    }
    if (id == "pong") {
        p.rect(1, 5, 2, 6, main);
        p.rect(13, 3, 2, 6, alt);
        p.rect(7, 7, 2, 2, hot);
        p.hline(3, 8, 10, C(T_line));
        return true;
    }
    if (id == "portal") {
        p.frame(2, 2, 12, 12, main);
        p.rect(4, 4, 8, 8, C(T_bg3));
        p.rect(6, 6, 4, 4, alt);
        p.dot(3, 2, hot);
        p.dot(12, 13, hot);
        return true;
    }
    if (id == "fav" || id == "trophy") {
        if (id == "trophy") {
            p.rect(5, 2, 6, 2, main);
            p.rect(4, 4, 8, 4, main);
            p.rect(6, 8, 4, 2, hot);
            p.rect(5, 11, 6, 2, hot);
            p.line(4, 4, 1, 6, alt);
            p.line(12, 4, 15, 6, alt);
            return true;
        }
        // Stella a cinque punte.
        const int cx = 8;
        const int cy = 8;
        const int pts[10][2] = {{8, 1}, {10, 6}, {15, 6}, {11, 9}, {13, 14},
                                {8, 11}, {3, 14}, {5, 9}, {1, 6}, {6, 6}};
        for (int i = 0; i < 10; ++i) {
            const int j = (i + 1) % 10;
            g().fillTriangle(p.X(cx), p.Y(cy), p.X(pts[i][0]), p.Y(pts[i][1]),
                             p.X(pts[j][0]), p.Y(pts[j][1]),
                             i % 2 ? main : hot);
        }
        return true;
    }
    if (id == "folder") {
        p.rect(1, 4, 14, 10, C(T_bg3));
        p.frame(1, 4, 14, 10, main);
        p.rect(1, 2, 6, 3, main);
        p.hline(3, 8, 10, C(T_line));
        p.hline(3, 11, 6, C(T_line));
        return true;
    }
    if (id == "simon") {
        p.disc(5, 5, 3, main);
        p.disc(11, 5, 3, alt);
        p.disc(5, 11, 3, hot);
        p.disc(11, 11, 3, C(T_ok));
        p.disc(8, 8, 1, C(T_bg));
        return true;
    }
    if (id == "snake") {
        const int seg[6][2] = {{2, 12}, {5, 12}, {8, 12}, {8, 9}, {8, 6}, {11, 6}};
        for (int i = 0; i < 6; ++i) {
            p.rect(seg[i][0], seg[i][1], 3, 2, i == 5 ? hot : main);
        }
        p.dot(13, 12, alt);
        return true;
    }
    if (id == "chess") {
        p.rect(3, 13, 10, 2, main);
        p.rect(5, 10, 6, 3, main);
        p.rect(6, 8, 4, 2, main);
        p.disc(8, 5, 2, hot);
        p.pix(8, 2, hot);
        return true;
    }
    if (id == "camera") {
        p.rect(1, 5, 14, 9, C(T_bg3));
        p.frame(1, 5, 14, 9, main);
        p.rect(5, 3, 6, 2, main);
        p.circle(8, 9, 3, alt);
        p.disc(8, 9, 1, alt);
        p.dot(13, 7, hot);
        return true;
    }
    if (id == "rocket") {
        p.line(8, 2, 8, 12, main);
        p.line(8, 2, 5, 6, main);
        p.line(8, 2, 11, 6, main);
        p.line(5, 9, 3, 13, alt);
        p.line(11, 9, 13, 13, alt);
        p.line(6, 13, 8, 15, hot);
        p.line(10, 13, 8, 15, hot);
        p.dot(8, 7, C(T_sec));
        return true;
    }
    if (id == "sat") {
        p.rect(6, 6, 4, 4, C(T_hi));
        p.frame(5, 5, 6, 6, main);
        p.rect(1, 6, 4, 3, alt);
        p.rect(11, 6, 4, 3, alt);
        p.hline(1, 7, 4, C(T_line));
        p.vline(8, 1, 3, main);
        p.dot(8, 1, hot);
        return true;
    }
    if (id == "planet") {
        p.disc(8, 8, 5, main);
        p.circle(8, 8, 6, C(T_atm));
        p.hline(2, 8, 12, alt);
        p.dot(6, 6, C(T_pl4));
        return true;
    }
    if (id == "alien") {
        // Zorbo: testa tonda, occhi grandi e antenne con bulbetti.
        p.line(6, 3, 4, 1, main);
        p.line(10, 3, 12, 1, main);
        p.dot(4, 1, hot);
        p.dot(12, 1, hot);
        p.disc(8, 8, 5, main);
        p.disc(6, 7, 2, bg);
        p.disc(10, 7, 2, bg);
        p.dot(6, 7, hot);
        p.dot(10, 7, hot);
        p.hline(7, 11, 3, alt);
        return true;
    }
    if (id == "power") {
        p.circle(8, 8, 6, main);
        p.rect(7, 1, 2, 6, main);
        return true;
    }
    if (id == "chip") {
        p.frame(3, 3, 10, 10, main);
        p.rect(6, 6, 4, 4, alt);
        for (int i = 0; i < 3; ++i) {
            p.vline(2, 4 + i * 3, 2, main);
            p.vline(13, 4 + i * 3, 2, main);
            p.hline(4 + i * 3, 2, 2, main);
            p.hline(4 + i * 3, 13, 2, main);
        }
        return true;
    }
    if (id == "key") {
        p.circle(5, 8, 3, main);
        p.disc(5, 8, 1, main);
        p.hline(8, 8, 6, main);
        p.vline(12, 8, 3, main);
        p.vline(14, 8, 2, main);
        return true;
    }
    if (id == "globe") {
        p.circle(8, 8, 6, main);
        p.vline(8, 2, 12, C(T_line));
        p.hline(2, 8, 12, C(T_line));
        p.line(4, 4, 12, 12, C(T_line));
        p.dot(5, 5, alt);
        return true;
    }
    if (id == "bright") {
        p.disc(8, 8, 3, hot);
        for (int i = 0; i < 8; ++i) {
            const float a = i * 0.7853982f;
            const int x0 = 8 + static_cast<int>(cosf(a) * 5.0f);
            const int y0 = 8 + static_cast<int>(sinf(a) * 5.0f);
            const int x1 = 8 + static_cast<int>(cosf(a) * 7.0f);
            const int y1 = 8 + static_cast<int>(sinf(a) * 7.0f);
            p.line(x0, y0, x1, y1, main);
        }
        return true;
    }
    if (id == "rotate") {
        auto& d = g();
        const int cx = p.X(8);
        const int cy = p.Y(8);
        const int r = 5 * p.u;
        d.drawArc(cx, cy, r, r + p.u, 30, 300, main);
        d.fillTriangle(cx + r, cy - 2 * p.u, cx + r + 3 * p.u, cy, cx + r, cy + 2 * p.u, alt);
        return true;
    }
    return false;
}

}  // namespace display
