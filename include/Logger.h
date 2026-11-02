#pragma once

#include <Arduino.h>

// Log seriale leggero (USB CDC). Nessun dato sensibile viene scritto.
namespace logger {

template <typename... Args>
inline void info(const char* fmt, Args... args) {
    Serial.printf("[FW][I] ");
    Serial.printf(fmt, args...);
    Serial.print('\n');
}

template <typename... Args>
inline void warn(const char* fmt, Args... args) {
    Serial.printf("[FW][W] ");
    Serial.printf(fmt, args...);
    Serial.print('\n');
}

template <typename... Args>
inline void error(const char* fmt, Args... args) {
    Serial.printf("[FW][E] ");
    Serial.printf(fmt, args...);
    Serial.print('\n');
}

}  // namespace logger
