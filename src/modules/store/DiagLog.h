#pragma once

#include <Arduino.h>

#include <cstdarg>
#include <cstdio>

// Log diagnostico su seriale: attivo nella build ORBIT_DIAG_AUTOGAME (harness)
// e nella build ORBIT_DIAG_LOG (firmware normale con log su USB CDC).
namespace diag {

inline void begin() {}

inline void log(const char* fmt, ...) {
#if defined(ORBIT_DIAG_AUTOGAME) || defined(ORBIT_DIAG_LOG)
    char buffer[192];
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (n > 0) {
        Serial.println(buffer);
    }
#else
    (void)fmt;
#endif
}

}  // namespace diag
