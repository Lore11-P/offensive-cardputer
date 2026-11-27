#pragma once

#include <cstdint>

#include <lgfx/v1/LGFXBase.hpp>

namespace orbit_hal {

// Display ST7789V2 del Cardputer ADV (240x135 in landscape, rotation 1).
bool displayBegin();
lgfx::LGFXBase& display();
// Stessa istanza come LGFX_Device (setBrightness/getBrightness sono qui).
lgfx::LGFX_Device& displayDevice();
void displayBrightness(uint8_t value);
void displayClear(uint16_t color);
void displayText(int x, int y, const char* text, uint16_t fg, uint16_t bg);
void displayPresent();

}  // namespace orbit_hal
