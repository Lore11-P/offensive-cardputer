#include "Orbit.h"

#include "orbit_compat.h"
#include <cmath>

#include "Display.h"

namespace orbit {
namespace {

inline uint32_t hash32(uint32_t v) {
    v ^= v >> 16;
    v *= 0x7FEB352Du;
    v ^= v >> 15;
    v *= 0x846CA68Bu;
    v ^= v >> 16;
    return v;
}

struct Rgb {
    int r, g, b;
};

inline Rgb unpack(uint16_t c) {
    Rgb o;
    o.r = (c >> 11) & 0x1F;
    o.g = (c >> 5) & 0x3F;
    o.b = c & 0x1F;
    return o;
}

inline uint16_t pack(int r, int g, int b) {
    if (r < 0) r = 0;
    if (r > 31) r = 31;
    if (g < 0) g = 0;
    if (g > 63) g = 63;
    if (b < 0) b = 0;
    if (b > 31) b = 31;
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

}  // namespace

const uint8_t kBayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

uint8_t bayer(int x, int y) {
    return kBayer[((y & 3) << 2) | (x & 3)];
}

uint16_t blend(uint16_t a, uint16_t b, uint8_t t) {
    const Rgb ca = unpack(a);
    const Rgb cb = unpack(b);
    const int w = t;
    const int iw = 255 - t;
    return pack((ca.r * iw + cb.r * w) / 255, (ca.g * iw + cb.g * w) / 255,
                (ca.b * iw + cb.b * w) / 255);
}

namespace {

// Riga di appoggio per il dithering: una pushImage per riga invece di
// centinaia di HLine. Buffer statico, nessuna allocazione per frame.
constexpr int kRowMax = 240;
uint16_t s_row[kRowMax];

void ditherSpan(lgfx::LGFXBase& d, int x, int y, int w, int level, uint16_t a, uint16_t b) {
    int done = 0;
    while (done < w) {
        const int chunk = std::min(kRowMax, w - done);
        for (int i = 0; i < chunk; ++i) {
            s_row[i] = (bayer(x + done + i, y) < level) ? b : a;
        }
        // Il cast forza M5GFX a convertire da RGB565 nativo al depth reale
        // della destinazione: il template pushImage(uint16_t*) assume invece
        // un buffer gia' in ordine pannello (swap565) e invertirebbe i byte.
        d.pushImage(x + done, y, chunk, 1,
                    reinterpret_cast<const lgfx::rgb565_t*>(s_row));
        done += chunk;
    }
}

}  // namespace

void ditherRect(int x, int y, int w, int h, uint16_t a, uint16_t b, int level) {
    if (level <= 0) {
        display::g().fillRect(x, y, w, h, a);
        return;
    }
    if (level >= 16) {
        display::g().fillRect(x, y, w, h, b);
        return;
    }
    auto& d = display::g();
    d.startWrite();
    for (int yy = 0; yy < h; ++yy) {
        ditherSpan(d, x, y + yy, w, level, a, b);
    }
    d.endWrite();
}

void stars(int x, int y, int w, int h, uint32_t seed, uint8_t count, uint8_t phase) {
    auto& d = display::g();
    for (uint8_t i = 0; i < count; ++i) {
        const uint32_t a = hash32(seed + i * 2654435761u);
        const uint32_t b = hash32(a ^ 0x9E3779B9u);
        const int sx = x + static_cast<int>(a % static_cast<uint32_t>(w));
        const int sy = y + static_cast<int>(b % static_cast<uint32_t>(h));
        const uint8_t bright = static_cast<uint8_t>((b >> 16) & 3);
        const bool twinkle = ((i + phase) % 7) == 0;
        uint16_t c = display::C(display::T_star);
        if (bright == 0) {
            c = display::C(display::T_dim);
        } else if (bright >= 2) {
            c = display::C(display::T_hi);
        }
        d.drawPixel(sx, sy, c);
        if (twinkle || bright == 3) {
            d.drawPixel(sx - 1, sy, display::C(display::T_dim));
            d.drawPixel(sx + 1, sy, display::C(display::T_dim));
            d.drawPixel(sx, sy - 1, display::C(display::T_dim));
            d.drawPixel(sx, sy + 1, display::C(display::T_dim));
        }
    }
}

void sky(int x, int y, int w, int h, uint32_t seed) {
    const uint16_t top = display::C(display::T_sky0);
    const uint16_t bottom = display::C(display::T_sky1);
    auto& d = display::g();
    d.startWrite();
    for (int yy = 0; yy < h; ++yy) {
        const int level = (yy * 16) / (h > 0 ? h : 1);
        ditherSpan(d, x, y + yy, w, level, top, bottom);
    }
    d.endWrite();
    stars(x, y, w, h, seed, static_cast<uint8_t>(std::max(6, (w * h) / 700)));
}

namespace {

// Il pianeta e' statico per tema/raggio/posizione: il carosello lo ridisegna
// a ogni frame, quindi si rasterizza una volta in una sprite con sfondo
// trasparente. r>32 (splash) resta procedurale per non allocare 12 KB.
constexpr uint16_t kPlanetTransparent = 0x0000;
constexpr int kPlanetCacheMaxR = 32;

M5Canvas s_planetCanvas(&M5Cardputer.Display);
bool s_planetValid = false;
bool s_planetReady = false;
int s_planetX = 0;
int s_planetY = 0;
int s_planetR = 0;
int s_planetLx = 0;
int s_planetLy = 0;
bool s_planetCity = false;
uint8_t s_planetTheme = 0;

// noinline: evita due copie della rasterizzazione (una per la cache, una per
// il fallback) e tiene il binario piu' piccolo.
__attribute__((noinline)) void planetInto(lgfx::LGFXBase& d, int cx, int cy, int r, float lightX,
                                          float lightY, bool cityLights, int phaseX, int phaseY) {
    const uint16_t base[5] = {display::C(display::T_pl0), display::C(display::T_pl1),
                              display::C(display::T_pl2), display::C(display::T_pl3),
                              display::C(display::T_pl4)};
    const uint16_t city = display::C(display::T_city);
    const uint16_t atm = display::C(display::T_atm);
    const float r2 = static_cast<float>(r * r);
    // Luce normalizzata: punta verso la sorgente (in alto a sinistra).
    const float lx = lightX < -0.99f ? -0.99f : (lightX > 0.99f ? 0.99f : lightX);
    const float ly = lightY < -0.99f ? -0.99f : (lightY > 0.99f ? 0.99f : lightY);

    for (int dy = -r + 1; dy <= r - 1; ++dy) {
        const int span = static_cast<int>(sqrtf(r2 - static_cast<float>(dy * dy)));
        for (int dx = -span; dx <= span; ++dx) {
            const float dist = sqrtf(static_cast<float>(dx * dx + dy * dy));
            if (dist > r) {
                continue;
            }
            // Illuminazione sferica: prodotto scalare tra normale e luce.
            const float nx = static_cast<float>(dx) / r;
            const float ny = static_cast<float>(dy) / r;
            const float lambert = nx * lx + ny * ly;  // -1..1
            float lit = (lambert + 0.55f) / 1.55f;
            if (lit < 0.0f) lit = 0.0f;
            if (lit > 1.0f) lit = 1.0f;
            // Fasce del gigante gassoso: modulano la luminosità.
            const float bands = sinf(ny * 6.0f + 0.6f);
            float v = 0.10f + 0.90f * lit + 0.055f * bands;
            // Limb darkening: il bordo perde luce.
            const float limb = dist / static_cast<float>(r);
            if (limb > 0.86f) {
                v -= (limb - 0.86f) * 0.55f;
            }
            int idx = static_cast<int>(v * 5.0f);
            if (idx < 0) idx = 0;
            if (idx > 4) idx = 4;
            const float frac = v * 5.0f - static_cast<float>(idx);
            uint16_t c = base[idx];
            if (idx < 4 && frac > 0.0f &&
                static_cast<int>(bayer(phaseX + dx, phaseY + dy)) <
                    static_cast<int>(frac * 16.0f)) {
                c = base[idx + 1];
            }
            // Bordo: riflesso atmosferico sul lato illuminato, silhouettes sul lato buio.
            if (dist > r - 1.3f) {
                c = (lambert > 0.0f) ? blend(base[4], atm, 105) : base[0];
            }
            // Luci delle citta' sul terminatore notturno.
            if (cityLights && lambert < -0.15f && idx <= 2) {
                const uint32_t hsh = hash32(static_cast<uint32_t>((phaseX + dx) * 73856093) ^
                                            static_cast<uint32_t>((phaseY + dy) * 19349663));
                if ((hsh & 0xFF) == 0x5A && dist < r - 2) {
                    c = city;
                }
            }
            d.drawPixel(cx + dx, cy + dy, c);
        }
    }
    // Atmosfera: due anelli con dither.
    const uint16_t bg = display::C(display::T_bg2);
    for (int a = 0; a < 360; a += 2) {
        const float rad = a * 0.0174533f;
        const int x1 = cx + static_cast<int>(cosf(rad) * (r + 1));
        const int y1 = cy + static_cast<int>(sinf(rad) * (r + 1));
        const int x2 = cx + static_cast<int>(cosf(rad) * (r + 2));
        const int y2 = cy + static_cast<int>(sinf(rad) * (r + 2));
        const bool litRim = (cosf(rad) * lx + sinf(rad) * ly) > -0.2f;
        d.drawPixel(x1, y1, litRim ? atm : blend(atm, bg, 160));
        d.drawPixel(x2, y2, litRim ? blend(atm, bg, 90) : bg);
    }
}

// Costruisce/riusa la cache. false = disegno procedurale.
bool preparePlanetCache(int cx, int cy, int r, float lightX, float lightY, bool cityLights) {
    if (r > kPlanetCacheMaxR) {
        return false;
    }
    const int lx = static_cast<int>(lightX * 100.0f);
    const int ly = static_cast<int>(lightY * 100.0f);
    const uint8_t theme = display::themeId();
    if (s_planetValid && s_planetX == cx && s_planetY == cy && s_planetR == r &&
        s_planetLx == lx && s_planetLy == ly && s_planetCity == cityLights &&
        s_planetTheme == theme) {
        return s_planetReady;
    }

    // La sprite precedente non e' piu' in lettura: il blit usa pushImage
    // sincrono, quindi liberarla non rischia DMA in volo.
    s_planetCanvas.deleteSprite();
    const int side = 2 * r + 7;
    s_planetCanvas.setColorDepth(16);
    s_planetValid = true;
    s_planetReady = false;
    s_planetX = cx;
    s_planetY = cy;
    s_planetR = r;
    s_planetLx = lx;
    s_planetLy = ly;
    s_planetCity = cityLights;
    s_planetTheme = theme;
    if (s_planetCanvas.createSprite(side, side) == nullptr) {
        return false;
    }
    s_planetCanvas.fillSprite(kPlanetTransparent);
    planetInto(s_planetCanvas, side / 2, side / 2, r, lightX, lightY, cityLights, cx, cy);
    // Lo sfondo deve restare trasparente: se un pixel coincide, niente cache.
    const uint16_t* buf = static_cast<const uint16_t*>(s_planetCanvas.getBuffer());
    for (int i = 0; i < side * side; ++i) {
        if (buf[i] == kPlanetTransparent) {
            s_planetCanvas.deleteSprite();
            return false;
        }
    }
    s_planetReady = true;
    return true;
}

}  // namespace

void planet(int cx, int cy, int r, float lightX, float lightY, bool cityLights) {
    if (r <= 1) {
        return;
    }
    if (preparePlanetCache(cx, cy, r, lightX, lightY, cityLights)) {
        const int side = 2 * r + 7;
        auto& dst = display::g();
        lgfx::pixelcopy_t pc(s_planetCanvas.getBuffer(), dst.getColorDepth(),
                             s_planetCanvas.getColorDepth(), dst.hasPalette(), nullptr,
                             kPlanetTransparent);
        dst.pushImage(cx - side / 2, cy - side / 2, side, side, &pc);
        return;
    }
    planetInto(display::g(), cx, cy, r, lightX, lightY, cityLights, cx, cy);
}

void orbitPath(int cx, int cy, int rx, int ry, uint16_t color, uint16_t gapColor, int phase) {
    auto& d = display::g();
    for (int a = 0; a < 360; a += 2) {
        const bool gap = ((a / 2 + phase) % 6) >= 4;
        if (gap) {
            continue;
        }
        const float rad = a * 0.0174533f;
        const int x = cx + static_cast<int>(sinf(rad) * rx);
        const int y = cy + static_cast<int>(cosf(rad) * ry);
        d.drawPixel(x, y, color);
        if (a % 12 == 0) {
            d.drawPixel(x, y + 1, gapColor);
        }
    }
}

void rocket(int x, int y, int size, float angle, bool flame, uint8_t frame) {
    auto& d = display::g();
    const float s = static_cast<float>(std::max(6, size));
    const float dx = sinf(angle);
    const float dy = -cosf(angle);
    const float px = -dy;
    const float py = dx;
    const float nose = s * 0.55f;
    const float tail = s * 0.30f;
    const float halfW = s * 0.17f;

    auto pt = [&](float a, float b, int& ox, int& oy) {
        ox = x + static_cast<int>(dx * a + px * b);
        oy = y + static_cast<int>(dy * a + py * b);
    };

    int nx, ny, lx, ly, rx, ry;
    pt(-nose, 0, nx, ny);
    pt(tail, -halfW, lx, ly);
    pt(tail, halfW, rx, ry);
    const uint16_t body = display::C(display::T_hi);
    const uint16_t shade = display::C(display::T_text);
    const uint16_t accent = display::C(display::T_acc);
    d.fillTriangle(nx, ny, lx, ly, rx, ry, body);
    int blx, bly, brx, bry, tlx, tly, trx, try_;
    pt(-nose * 0.2f, -halfW * 0.95f, blx, bly);
    pt(-nose * 0.2f, halfW * 0.95f, brx, bry);
    pt(tail, -halfW, tlx, tly);
    pt(tail, halfW, trx, try_);
    d.fillTriangle(blx, bly, tlx, tly, trx, try_, shade);
    d.fillTriangle(blx, bly, trx, try_, brx, bry, shade);
    // Pinne.
    int flx, fly, frx, fry;
    pt(tail, -halfW * 2.0f, flx, fly);
    pt(tail, halfW * 2.0f, frx, fry);
    d.fillTriangle(tlx, tly, flx, fly, tlx + static_cast<int>(dx * 2), tly + static_cast<int>(dy * 2),
                   accent);
    d.fillTriangle(trx, try_, frx, fry, trx + static_cast<int>(dx * 2), try_ + static_cast<int>(dy * 2),
                   accent);
    // Oblò.
    int wx, wy;
    pt(-nose * 0.15f, 0, wx, wy);
    d.drawPixel(wx, wy, display::C(display::T_sec));
    d.drawPixel(wx + 1, wy, display::C(display::T_secD));

    if (flame) {
        const uint16_t f0 = display::C(display::T_flame0);
        const uint16_t f1 = display::C(display::T_flame1);
        const uint16_t f2 = display::C(display::T_flame2);
        const float flick = 1.0f + 0.18f * static_cast<float>((frame % 5) - 2);
        int fx, fy, gx, gy, hx, hy;
        pt(tail, -halfW * 0.6f, fx, fy);
        pt(tail, halfW * 0.6f, gx, gy);
        pt(tail + s * 0.45f * flick, 0, hx, hy);
        d.fillTriangle(fx, fy, gx, gy, hx, hy, f0);
        int ix, iy, jx, jy, kx, ky;
        pt(tail, -halfW * 0.35f, ix, iy);
        pt(tail, halfW * 0.35f, jx, jy);
        pt(tail + s * 0.28f * flick, 0, kx, ky);
        d.fillTriangle(ix, iy, jx, jy, kx, ky, f1);
        int mx, my;
        pt(tail + s * 0.12f, 0, mx, my);
        d.drawPixel(mx, my, f2);
    }
}

void satellite(int x, int y, int size, uint16_t body, uint16_t panel) {
    auto& d = display::g();
    const int s = std::max(6, size);
    const int bw = std::max(3, s / 5);
    const int bh = std::max(5, s / 3);
    const int cx = x + s / 2;
    const int cy = y + s / 2;
    const int pw = std::max(2, s / 4);
    d.fillRect(cx - bw / 2, cy - bh / 2, bw, bh, body);
    d.drawRect(cx - bw / 2 - 1, cy - bh / 2 - 1, bw + 2, bh + 2, panel);
    d.fillRect(cx - bw / 2 - 1 - pw, cy - bh / 4, pw, bh / 2, panel);
    d.fillRect(cx + bw / 2 + 1, cy - bh / 4, pw, bh / 2, panel);
    d.drawLine(cx - bw / 2 - 1 - pw, cy, cx - bw / 2 - 1, cy, body);
    d.drawLine(cx + bw / 2 + 1, cy, cx + bw / 2 + 1 + pw, cy, body);
    d.drawPixel(cx, cy - bh / 2 - 2, body);
    d.drawPixel(cx, cy - bh / 2 - 3, panel);
}

void radar(int cx, int cy, int radius, float angle, const int16_t* blips, int count, uint32_t now) {
    auto& d = display::g();
    const uint16_t grid = display::C(display::T_line);
    const uint16_t dim = display::C(display::T_line2);
    const uint16_t sweep = display::C(display::T_acc);
    const uint16_t blip = display::C(display::T_hi);

    d.fillCircle(cx, cy, radius, display::C(display::T_sky0));
    d.drawCircle(cx, cy, radius, grid);
    d.drawCircle(cx, cy, (radius * 2) / 3, dim);
    d.drawCircle(cx, cy, radius / 3, dim);
    d.drawFastHLine(cx - radius, cy, radius * 2 + 1, dim);
    d.drawFastVLine(cx, cy - radius, radius * 2 + 1, dim);

    // Scia del raggio: quattro settori sempre più tenui.
    for (int i = 0; i < 4; ++i) {
        const float a0 = angle - i * 0.16f;
        const uint16_t c = (i == 0) ? sweep : blend(sweep, display::C(display::T_sky0), 60 + i * 55);
        for (int rr = 2; rr <= radius; rr += 2) {
            const int x0 = cx + static_cast<int>(sinf(a0) * rr);
            const int y0 = cy - static_cast<int>(cosf(a0) * rr);
            d.drawPixel(x0, y0, c);
            d.drawPixel(x0 + 1, y0, blend(c, display::C(display::T_sky0), 70));
        }
    }
    const int sx = cx + static_cast<int>(sinf(angle) * radius);
    const int sy = cy - static_cast<int>(cosf(angle) * radius);
    d.drawLine(cx, cy, sx, sy, sweep);
    d.fillCircle(cx, cy, 2, sweep);

    // Blips: posizioni relative, pulsano in base al tempo.
    for (int i = 0; i < count && i < 32; ++i) {
        const int bx = cx + blips[i * 2];
        const int by = cy + blips[i * 2 + 1];
        const bool hot = ((now / 220 + static_cast<uint32_t>(i)) % 4) == 0;
        d.fillCircle(bx, by, hot ? 2 : 1, hot ? blip : sweep);
        if (hot) {
            d.drawCircle(bx, by, 3, blend(blip, display::C(display::T_sky0), 120));
        }
    }
}

void signalBars(int x, int y, int level, uint16_t color, uint16_t off) {
    auto& d = display::g();
    for (int i = 0; i < 4; ++i) {
        const int h = 2 + i * 2;
        d.fillRect(x + i * 3, y + (7 - h), 2, h, i < level ? color : off);
    }
}

}  // namespace orbit
