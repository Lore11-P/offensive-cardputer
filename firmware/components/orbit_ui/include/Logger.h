#pragma once

#include <cstdarg>
#include <cstdio>

#include <esp_log.h>

// Adapter dei log del firmware 2.x (logger::info/warn/error) su ESP_LOG.
// Scrittura con vsnprintf: i format string arrivano dal codice 2.x e non
// vogliamo i controlli -Wformat del compilatore su ogni messaggio.
namespace logger {

inline void vline(esp_log_level_t level, const char* fmt, va_list args) {
    char buffer[192];
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    esp_log_write(level, "orbit-ui", "%s\n", buffer);
}

inline void info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vline(ESP_LOG_INFO, fmt, args);
    va_end(args);
}

inline void warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vline(ESP_LOG_WARN, fmt, args);
    va_end(args);
}

inline void error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vline(ESP_LOG_ERROR, fmt, args);
    va_end(args);
}

}  // namespace logger
