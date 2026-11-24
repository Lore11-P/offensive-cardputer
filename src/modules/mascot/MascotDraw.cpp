#include "MascotDraw.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "Display.h"

// Schermata di Zorbo: tutto disegnato a primitive sul framebuffer.
// Le stringhe passano solo da buffer locali (niente std::string nel disegno).
namespace mascot {
namespace {

using display::C;

constexpr int kStatsY = 104;
constexpr int kStatsH = 19;
constexpr int kBubbleX = 122;
constexpr int kBubbleY = 30;
constexpr int kBubbleW = 112;
constexpr int kBubbleH = 50;
constexpr int kBubbleTextX = kBubbleX + 7;
constexpr int kBubbleTextW = kBubbleW - 14;
constexpr int kSpeechChars = (kBubbleTextW + 1) / 6;

// Cuore miniaturizzato: due cerchi e un triangolo.
void heart(int cx, int cy, int r, uint16_t color) {
    auto& d = display::g();
    d.fillCircle(cx - r, cy - r, r, color);
    d.fillCircle(cx + r, cy - r, r, color);
    d.fillTriangle(cx - 2 * r, cy - r, cx + 2 * r, cy - r, cx, cy + 2 * r, color);
}

// Cielo sobrio: stelle fisse, nessuna animazione (niente flicker).
void drawSky() {
    auto& d = display::g();
    for (int i = 0; i < 16; ++i) {
        const int x = (i * 73 + 17) % display::width();
        const int y = 26 + (i * 37 + 11) % 88;
        d.drawPixel(x, y, i % 3 == 0 ? C(display::T_star) : C(display::T_dim));
        if (i % 5 == 0) {
            d.drawPixel(x + 1, y, C(display::T_dim));
            d.drawPixel(x, y + 1, C(display::T_dim));
        }
    }
}

void drawPlanet() {
    auto& d = display::g();
    constexpr int cx = 27;
    constexpr int cy = 39;
    // Anello dietro al disco, terminatore e luce in alto a sinistra.
    d.drawEllipse(cx, cy, 14, 5, C(display::T_atm));
    d.fillCircle(cx, cy, 8, C(display::T_pl2));
    d.drawArc(cx, cy, 6, 8, 15, 165, C(display::T_pl1));
    d.fillCircle(cx - 3, cy - 3, 2, C(display::T_pl4));
    d.drawPixel(cx + 3, cy + 1, C(display::T_pl3));
    d.drawPixel(cx + 5, cy - 2, C(display::T_pl3));
}

void drawAlien(const View& view) {
    auto& d = display::g();
    constexpr int ax = 72;
    const int ay = 80 + static_cast<int>(std::sin(view.nowMs / 280.0f) * 2.0f);
    const uint16_t skin = display::kNeoGreen;
    const uint16_t shade = C(display::T_okD);
    const uint16_t shine = C(display::T_okL);
    const bool festive = view.hearts > 0;

    // Corpo dietro la testa.
    d.drawEllipse(ax, ay + 7, 14, 11, shade);
    d.fillEllipse(ax, ay + 7, 13, 10, skin);

    // Braccia: distese a riposo, alzate e oscillanti durante le coccole.
    if (festive) {
        const int wave = static_cast<int>(std::sin(view.nowMs / 150.0f) * 3.0f);
        d.drawLine(ax - 13, ay + 4, ax - 27, ay - 4 + wave, skin);
        d.drawLine(ax - 13, ay + 5, ax - 27, ay - 3 + wave, skin);
        d.drawCircle(ax - 27, ay - 6 + wave, 3, shade);
        d.fillCircle(ax - 27, ay - 6 + wave, 2, skin);
        d.drawLine(ax + 13, ay + 4, ax + 27, ay - 4 - wave, skin);
        d.drawLine(ax + 13, ay + 5, ax + 27, ay - 3 - wave, skin);
        d.drawCircle(ax + 27, ay - 6 - wave, 3, shade);
        d.fillCircle(ax + 27, ay - 6 - wave, 2, skin);
    } else {
        d.drawLine(ax - 13, ay + 6, ax - 20, ay + 12, skin);
        d.drawLine(ax - 13, ay + 7, ax - 20, ay + 13, skin);
        d.drawCircle(ax - 20, ay + 12, 3, shade);
        d.fillCircle(ax - 20, ay + 12, 2, skin);
        d.drawLine(ax + 13, ay + 6, ax + 20, ay + 12, skin);
        d.drawLine(ax + 13, ay + 7, ax + 20, ay + 13, skin);
        d.drawCircle(ax + 20, ay + 12, 3, shade);
        d.fillCircle(ax + 20, ay + 12, 2, skin);
    }

    // Testa ovale con riflesso.
    d.drawEllipse(ax, ay - 14, 22, 20, shade);
    d.fillEllipse(ax, ay - 14, 21, 19, skin);
    d.drawArc(ax, ay - 14, 16, 18, 195, 262, shine);

    // Antenne con bulbetti.
    const int bulbY = ay - 44;
    d.drawLine(ax - 10, ay - 30, ax - 17, bulbY + 3, shade);
    d.drawLine(ax + 10, ay - 30, ax + 17, bulbY + 3, shade);
    d.drawCircle(ax - 17, bulbY, 4, shade);
    d.fillCircle(ax - 17, bulbY, 3, display::kAccent);
    d.drawCircle(ax + 17, bulbY, 4, shade);
    d.fillCircle(ax + 17, bulbY, 3, display::kAccent);
    d.fillCircle(ax - 18, bulbY - 1, 1, display::kWhite);
    d.fillCircle(ax + 16, bulbY - 1, 1, display::kWhite);

    // Grandi occhi con pupille e riflesso; al blink diventano due linee.
    const int eyeY = ay - 18;
    if (view.blink) {
        d.fillRect(ax - 15, eyeY, 13, 2, shade);
        d.fillRect(ax + 3, eyeY, 13, 2, shade);
        d.drawPixel(ax - 16, eyeY, shade);
        d.drawPixel(ax + 16, eyeY, shade);
    } else {
        d.fillEllipse(ax - 9, eyeY, 7, 7, display::kWhite);
        d.fillEllipse(ax + 9, eyeY, 7, 7, display::kWhite);
        d.fillCircle(ax - 8, eyeY, 3, display::kBlack);
        d.fillCircle(ax + 10, eyeY, 3, display::kBlack);
        d.fillCircle(ax - 9, eyeY - 2, 1, display::kWhite);
        d.fillCircle(ax + 9, eyeY - 2, 1, display::kWhite);
    }

    // Bocca: sorriso se felice, linea neutra altrimenti.
    if (view.happy) {
        d.drawArc(ax, ay - 12, 6, 8, 25, 155, shade);
    } else {
        d.fillRect(ax - 5, ay - 6, 11, 2, shade);
    }

    // Guance rosate quando l'affetto e' alto.
    if (view.affection >= 60) {
        d.fillCircle(ax - 17, ay - 9, 3, display::kNeoPink);
        d.fillCircle(ax + 17, ay - 9, 3, display::kNeoPink);
    }

    // Cuoricini che salgono: 2..4 in base all'intensita'.
    const int count = 2 + (view.hearts >= 5 ? 1 : 0) + (view.hearts >= 8 ? 1 : 0);
    for (int i = 0; i < count; ++i) {
        const int rise = static_cast<int>((view.nowMs / 14 + i * 29) % 46);
        const int hx =
            48 + i * 14 + static_cast<int>(std::sin((view.nowMs + i * 500) / 230.0f) * 2.0f);
        const int hy = 73 - rise;
        const int r = ((view.nowMs / 220 + i) & 1) ? 3 : 2;
        heart(hx, hy, r, display::kNeoPink);
    }
}

// Riga di fumetto troncata a ".." senza modificare la stringa sorgente.
void drawSpeechLine(int x, int y, const char* s, size_t len, uint16_t color) {
    const size_t visible = len > static_cast<size_t>(kSpeechChars) ? kSpeechChars - 2 : len;
    char buf[kSpeechChars + 1];
    std::memcpy(buf, s, visible);
    size_t n = visible;
    if (len > static_cast<size_t>(kSpeechChars)) {
        buf[n++] = '.';
        buf[n++] = '.';
    }
    buf[n] = '\0';
    display::text(x, y, buf, color, 0);
}

void drawSpeech(const View& view) {
    auto& d = display::g();
    d.fillRoundRect(kBubbleX, kBubbleY, kBubbleW, kBubbleH, 6, display::kPanel);
    d.drawRoundRect(kBubbleX, kBubbleY, kBubbleW, kBubbleH, 6, C(display::T_line));

    // Coda verso la testa: il riempimento apre il bordo sinistro del fumetto,
    // le due diagonali chiudono il profilo.
    d.fillTriangle(kBubbleX, 61, kBubbleX, 72, 104, 67, display::kPanel);
    d.fillRect(kBubbleX, 61, 1, 12, display::kPanel);
    d.drawLine(kBubbleX, 61, 104, 67, C(display::T_line));
    d.drawLine(kBubbleX, 72, 104, 67, C(display::T_line));

    const char* speech = view.speech != nullptr ? view.speech : "";
    if (speech[0] == '\0') {
        display::text(kBubbleTextX, kBubbleY + 21, "...", C(display::T_dim), 0);
        return;
    }
    const char* split = std::strchr(speech, '|');
    if (split == nullptr) {
        drawSpeechLine(kBubbleTextX, kBubbleY + 21, speech, std::strlen(speech), display::kText);
        return;
    }
    drawSpeechLine(kBubbleTextX, kBubbleY + 15, speech, static_cast<size_t>(split - speech),
                   display::kText);
    drawSpeechLine(kBubbleTextX, kBubbleY + 25, split + 1, std::strlen(split + 1),
                   display::kText);
}

// Testo generico troncato con ".." dentro maxWidth (font 0).
void drawClipped(int x, int y, const char* s, int maxWidth, uint16_t color) {
    if (s == nullptr || maxWidth <= 0) {
        return;
    }
    if (display::textWidth(s, 0) <= maxWidth) {
        display::text(x, y, s, color, 0);
        return;
    }
    int keep = (maxWidth + 1) / 6 - 2;
    if (keep < 0) {
        keep = 0;
    }
    if (keep > 60) {
        keep = 60;
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*s..", keep, s);
    display::text(x, y, buf, color, 0);
}

void drawStats(const View& view) {
    auto& d = display::g();
    const int w = display::width();

    // Evento sopra la barra, a destra della mascotte.
    if (view.event != nullptr && view.event[0] != '\0') {
        int ex = w - 6 - display::textWidth(view.event, 0);
        if (ex < 100) {
            ex = 100;
        }
        drawClipped(ex, 96, view.event, w - 6 - ex, display::kAccent);
    }

    d.fillRect(0, kStatsY, w, kStatsH, C(display::T_bg2));
    d.drawFastHLine(0, kStatsY, w, C(display::T_line));
    d.fillRect(0, kStatsY, 14, 1, display::kAccent);

    char buf[24];
    display::text(6, 106, "PTS", C(display::T_dim), 0);
    std::snprintf(buf, sizeof buf, "%lu", static_cast<unsigned long>(view.score));
    display::text(29, 106, buf, display::kText, 0);

    std::snprintf(buf, sizeof buf, "%u%%", static_cast<unsigned>(view.affection));
    const int pctW = display::textWidth(buf, 0);
    const int pctX = w - 6 - pctW;
    display::text(pctX, 106, buf, display::kText, 0);
    display::text(pctX - 4 - display::textWidth("AFF", 0), 106, "AFF", C(display::T_dim), 0);

    // Mini barra dell'affetto, sotto il valore percentuale.
    constexpr int kBarX = 166;
    constexpr int kBarY = 116;
    constexpr int kBarH = 5;
    const int barW = w - 6 - kBarX;
    d.fillRect(kBarX, kBarY, barW, kBarH, C(display::T_bg3));
    const int fillW = (static_cast<int>(view.affection) * (barW - 2)) / 100;
    if (fillW > 0) {
        d.fillRect(kBarX + 1, kBarY + 1, fillW, kBarH - 2, display::kAccent);
    }
    d.drawRect(kBarX, kBarY, barW, kBarH, C(display::T_line));

    // Nota discreta in basso a sinistra.
    display::text(6, 114, "solo lab autorizzato", C(display::T_dim), 0);
}

}  // namespace

void drawScene(const View& view) {
    display::clear(C(display::T_bg));
    drawSky();
    drawPlanet();
    display::header("Zorbo", "Mascot");
    drawAlien(view);
    drawSpeech(view);
    drawStats(view);
    display::footer("ENTER=coccola  ESC=esci");
    display::push();
}

}  // namespace mascot
