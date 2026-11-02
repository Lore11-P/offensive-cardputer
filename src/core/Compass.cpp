#include "Compass.h"

#include <Arduino.h>
#include <M5Cardputer.h>

#include <cmath>

#include "Logger.h"

namespace compass {
namespace {

bool s_available = false;
float s_bias = 0.0f;      // deg/s a riposo
float s_heading = 0.0f;   // gradi
uint32_t s_lastMs = 0;

constexpr float kDeadbandDps = 1.2f;  // sotto questa velocita' si considera fermo
constexpr uint32_t kBiasMs = 400;

}  // namespace

bool begin() {
    if (!M5.Imu.isEnabled()) {
        M5.Imu.begin();  // l'esito si verifica con isEnabled()
    }
    s_available = M5.Imu.isEnabled();
    s_heading = 0.0f;
    s_lastMs = millis();

    if (!s_available) {
        logger::warn("Bussola: IMU non disponibile");
        return false;
    }

    // Misura del bias con il dispositivo fermo.
    float sum = 0.0f;
    int samples = 0;
    const uint32_t started = millis();
    while (millis() - started < kBiasMs) {
        M5.Imu.update();
        float gx = 0.0f;
        float gy = 0.0f;
        float gz = 0.0f;
        if (M5.Imu.getGyro(&gx, &gy, &gz)) {
            sum += gz;
            ++samples;
        }
        delay(10);
    }
    s_bias = (samples > 0) ? (sum / static_cast<float>(samples)) : 0.0f;
    s_lastMs = millis();
    logger::info("Bussola: pronta, bias=%.2f dps", static_cast<double>(s_bias));
    return true;
}

void update() {
    if (!s_available) {
        return;
    }
    const uint32_t now = millis();
    const float dt = static_cast<float>(now - s_lastMs) / 1000.0f;
    if (dt < 0.005f) {
        return;  // troppo presto: evita divisioni per dt minuscoli
    }
    s_lastMs = now;

    float gx = 0.0f;
    float gy = 0.0f;
    float gz = 0.0f;
    M5.Imu.update();
    if (!M5.Imu.getGyro(&gx, &gy, &gz)) {
        return;
    }
    float rate = gz - s_bias;
    if (std::fabs(rate) < kDeadbandDps) {
        rate = 0.0f;  // fermo: niente accumulo di deriva
    }
    s_heading += rate * dt;
    while (s_heading < 0.0f) {
        s_heading += 360.0f;
    }
    while (s_heading >= 360.0f) {
        s_heading -= 360.0f;
    }
}

float heading() {
    return s_heading;
}

void reset() {
    s_heading = 0.0f;
    s_lastMs = millis();
}

bool available() {
    return s_available;
}

}  // namespace compass
