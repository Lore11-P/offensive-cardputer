#include "Logger.h"

#include <cstdarg>
#include <cstdio>

namespace logger {
namespace detail {

void writeLine(char level, const char* fmt, ...) {
#if defined(ORBIT_DISABLE_LOGGER) && ORBIT_DISABLE_LOGGER
    // Build HID: la CDC e' la porta del target, nessun log.
    (void)level;
    (void)fmt;
#else
    char line[kLineLen];
    line[0] = '[';
    line[1] = 'F';
    line[2] = 'W';
    line[3] = ']';
    line[4] = '[';
    line[5] = level;
    line[6] = ']';
    line[7] = ' ';

    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(line + 8, sizeof(line) - 8, fmt, args);
    va_end(args);
    if (n < 0) {
        return;
    }
    size_t len = 8 + static_cast<size_t>(n);
    if (len > sizeof(line) - 2) {
        len = sizeof(line) - 2;  // riga troppo lunga: troncata, mai oltre il buffer
    }
    line[len++] = '\n';
    Serial.write(reinterpret_cast<const uint8_t*>(line), len);
#endif
}

}  // namespace detail
}  // namespace logger
