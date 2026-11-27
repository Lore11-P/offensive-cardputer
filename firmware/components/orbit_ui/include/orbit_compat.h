#pragma once

// Shim minimale per riusare il codice UI del firmware 2.x sotto ESP-IDF.
// Fornisce solo cio' che la UI usava di Arduino/M5: tempo, canvas, batteria,
// tastiera. Il display e' il pannello LovyanGFX di orbit_hal.

#include <cstdint>

#include <lgfx/v1/LGFXBase.hpp>
#include <lgfx/v1/LGFX_Sprite.hpp>

#include "Navigation.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "orbit_hal/battery.h"
#include "orbit_hal/display.h"

inline uint32_t millis() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

inline void delay(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

class OrbitCanvas : public lgfx::LGFX_Sprite {
 public:
    explicit OrbitCanvas(lgfx::LGFXBase* parent)
        : lgfx::LGFX_Sprite(static_cast<lgfx::LovyanGFX*>(parent)) {}
};
using M5Canvas = OrbitCanvas;

struct OrbitPower {
    int getBatteryLevel() const { return orbit_hal::batteryPercent(); }
    bool isCharging() const { return false; }
    int16_t getVBUSVoltage() const { return -1; }
    int16_t getBatteryVoltage() const { return static_cast<int16_t>(orbit_hal::batteryMillivolts()); }
};

struct OrbitSpeaker {
    void tone(float, uint32_t) {}
};

struct OrbitKeyboard {
    bool isPressed() const { return navigation::anyPressed(); }
    bool isKeyPressed(char c) const { return navigation::pressed(c); }
};

struct OrbitM5Cardputer {
    lgfx::LGFX_Device& Display;
    OrbitPower Power;
    OrbitSpeaker Speaker;
    OrbitKeyboard Keyboard;
    OrbitM5Cardputer() : Display(orbit_hal::displayDevice()) {}
};

inline OrbitM5Cardputer& m5cardputer() {
    static OrbitM5Cardputer instance;
    return instance;
}
#define M5Cardputer m5cardputer()
