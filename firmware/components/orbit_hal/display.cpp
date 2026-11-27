#include "orbit_hal/display.h"

#include <lgfx/v1/LGFXBase.hpp>
#include <lgfx/v1/panel/Panel_ST7789.hpp>
#include <lgfx/v1/platforms/esp32/Bus_SPI.hpp>
#include <lgfx/v1/platforms/esp32/Light_PWM.hpp>

namespace orbit_hal {
namespace {

// Pin e timing del Cardputer ADV (M5GFX board_data: cardputer_adv).
class OrbitDisplay : public lgfx::LGFX_Device {
 public:
    OrbitDisplay() {
        {
            auto cfg = _bus.config();
            cfg.spi_host = SPI3_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = 40000000;
            cfg.freq_read = 16000000;
            cfg.spi_3wire = true;
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = 36;
            cfg.pin_mosi = 35;
            cfg.pin_miso = -1;
            cfg.pin_dc = 34;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs = 37;
            cfg.pin_rst = 33;
            cfg.pin_busy = -1;
            cfg.panel_width = 135;
            cfg.panel_height = 240;
            cfg.offset_x = 52;
            cfg.offset_y = 40;
            cfg.offset_rotation = 1;
            cfg.invert = true;
            cfg.rgb_order = false;
            cfg.readable = true;
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl = 38;
            cfg.freq = 256;
            cfg.pwm_channel = 7;
            cfg.invert = false;
            cfg.offset = 16;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        setPanel(&_panel);
    }

 private:
    lgfx::Panel_ST7789 _panel;
    lgfx::Bus_SPI _bus;
    lgfx::Light_PWM _light;
};

OrbitDisplay s_display;

}  // namespace

bool displayBegin() {
    if (!s_display.init()) {
        return false;
    }
    s_display.setRotation(1);
    s_display.setBrightness(128);
    s_display.fillScreen(0x0000);
    return true;
}

lgfx::LGFXBase& display() {
    return s_display;
}

lgfx::LGFX_Device& displayDevice() {
    return s_display;
}

void displayBrightness(uint8_t value) {
    s_display.setBrightness(value);
}

void displayClear(uint16_t color) {
    s_display.fillScreen(color);
}

void displayText(int x, int y, const char* text, uint16_t fg, uint16_t bg) {
    s_display.setFont(&lgfx::fonts::Font2);
    s_display.setTextColor(fg, bg);
    s_display.drawString(text, x, y);
}

void displayPresent() {
    // Per ora si disegna direttamente sul pannello; lo sprite arriva in M0.1.
}

}  // namespace orbit_hal
