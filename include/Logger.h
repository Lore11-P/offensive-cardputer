#pragma once

#include <Arduino.h>

// Log seriale leggero (USB CDC). Nessun dato sensibile viene scritto.
//
// La formattazione vive in utils/Logger.cpp: una sola copia nel binario
// invece di un template inline per ogni chiamata. Ogni riga viene composta in
// un buffer fisso e scritta con una sola Serial.write(): prefisso, messaggio e
// '\n' non possono interlacciarsi con i log di un altro task (es. callback
// NimBLE della chat).
namespace logger {

namespace detail {

constexpr size_t kLineLen = 256;

// Attributo printf sul punto unico di formattazione (la verifica piena dei
// formati resta limitata dal template, vedi OPT_CORE.md LOG-2).
void writeLine(char level, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));

}  // namespace detail

template <typename... Args>
inline void info(const char* fmt, const Args&... args) {
    detail::writeLine('I', fmt, args...);
}

template <typename... Args>
inline void warn(const char* fmt, const Args&... args) {
    detail::writeLine('W', fmt, args...);
}

template <typename... Args>
inline void error(const char* fmt, const Args&... args) {
    detail::writeLine('E', fmt, args...);
}

}  // namespace logger
