#pragma once

#include <stdint.h>
#include <string>

// Arte procedurale Orbit: cielo, stelle, pianeta, razzo, satellite e radar.
// Tutto viene disegnato a runtime sul framebuffer di display::g().
namespace orbit {

uint8_t bayer(int x, int y);
uint16_t blend(uint16_t a, uint16_t b, uint8_t t);

// Riempimento con dithering Bayer tra due colori (level 0..16).
void ditherRect(int x, int y, int w, int h, uint16_t a, uint16_t b, int level);

// Cielo verticale (T_sky0 -> T_sky1) con stelle fisse.
void sky(int x, int y, int w, int h, uint32_t seed = 0x51ED);
void stars(int x, int y, int w, int h, uint32_t seed, uint8_t count, uint8_t phase = 0);

// Pianeta procedurale con fasce, terminatore e atmosfera.
void planet(int cx, int cy, int r, float lightX = -0.55f, float lightY = -0.45f,
            bool cityLights = true);

// Ellisse orbitale puntinata (traiettoria).
void orbitPath(int cx, int cy, int rx, int ry, uint16_t color, uint16_t gapColor, int phase = 0);

// Razzo stilizzato. angle in radianti (0 = verso l'alto, orario).
void rocket(int x, int y, int size, float angle, bool flame, uint8_t frame = 0);
// Satellite a pannelli.
void satellite(int x, int y, int size, uint16_t body, uint16_t panel);

// Radar con raggio rotante; blips = coppie (x, y) in pixel relativi al centro.
void radar(int cx, int cy, int radius, float angle, const int16_t* blips, int count,
           uint32_t now);

// Piccolo indicatore a barre (segnale).
void signalBars(int x, int y, int level, uint16_t color, uint16_t off);

}  // namespace orbit
