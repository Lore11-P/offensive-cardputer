#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <esp_timer.h>

#include "Display.h"
#include "Navigation.h"
#include "ToolsInternal.h"
#include "orbit_compat.h"

namespace orbit_tools {
namespace {

// 2020-01-01 00:00:00 UTC: sotto questa soglia l'ora non e' considerata
// sincronizzata (NTP mai eseguito o orologio non impostato).
constexpr time_t kEpoch2020 = 1577836800;
constexpr const char* kTimezone = "CET-1CEST,M3.5.0,M10.5.0/3";

void ensureTimezone() {
    static bool done = false;
    if (!done) {
        setenv("TZ", kTimezone, 1);
        tzset();
        done = true;
    }
}

void formatElapsed(uint64_t ms, char* out, size_t size) {
    const uint64_t hundredths = (ms / 10) % 100;
    const uint64_t seconds = (ms / 1000) % 60;
    const uint64_t minutes = (ms / 60000) % 60;
    const uint64_t hours = ms / 3600000;
    if (hours > 0) {
        std::snprintf(out, size, "%llu:%02llu:%02llu",
                      static_cast<unsigned long long>(hours),
                      static_cast<unsigned long long>(minutes),
                      static_cast<unsigned long long>(seconds));
    } else {
        std::snprintf(out, size, "%02llu:%02llu.%02llu",
                      static_cast<unsigned long long>(minutes),
                      static_cast<unsigned long long>(seconds),
                      static_cast<unsigned long long>(hundredths));
    }
}

struct Stopwatch {
    uint64_t elapsedMs = 0;
    uint32_t startedMs = 0;
    uint64_t lapMs = 0;
    uint64_t lastLapTotalMs = 0;
    uint32_t lapCount = 0;
    bool running = false;

    uint64_t current() const {
        if (!running) {
            return elapsedMs;
        }
        return elapsedMs + static_cast<uint32_t>(millis() - startedMs);
    }

    void toggle() {
        if (running) {
            elapsedMs = current();
            running = false;
        } else {
            startedMs = millis();
            running = true;
        }
    }

    void lap() {
        if (!running) {
            return;
        }
        const uint64_t now = current();
        lapMs = now - lastLapTotalMs;
        lastLapTotalMs = now;
        ++lapCount;
    }

    void reset() {
        running = false;
        elapsedMs = 0;
        lapMs = 0;
        lastLapTotalMs = 0;
        lapCount = 0;
    }
};

void drawClock(const Stopwatch& watch) {
    display::clear(display::C(display::T_bg));
    display::header("Orologio", "Strumenti");

    const time_t now = time(nullptr);
    const bool valid = now >= kEpoch2020;
    struct tm local = {};
    const bool haveLocal = valid && localtime_r(&now, &local) != nullptr;

    if (haveLocal) {
        char clock[16];
        char date[48];
        std::snprintf(clock, sizeof(clock), "%02d:%02d:%02d", local.tm_hour, local.tm_min,
                      local.tm_sec);
        std::snprintf(date, sizeof(date), "%02d/%02d/%04d  CET/CEST", local.tm_mday,
                      local.tm_mon + 1, local.tm_year + 1900);
        display::textCentered(27, clock, display::C(display::T_acc), 4);
        display::textCentered(45, date, display::C(display::T_text), 0);
    } else {
        display::textCentered(28, "NON SINCRONIZZATO", display::C(display::T_warn), 2);
        display::textCentered(44, "NTP non eseguito o ora non valida",
                              display::C(display::T_dim), 0);
    }

    const uint64_t up = static_cast<uint64_t>(esp_timer_get_time()) / 1000000ULL;
    char uptime[40];
    std::snprintf(uptime, sizeof(uptime), "UPTIME %llud %02llu:%02llu:%02llu",
                  static_cast<unsigned long long>(up / 86400),
                  static_cast<unsigned long long>((up / 3600) % 24),
                  static_cast<unsigned long long>((up / 60) % 60),
                  static_cast<unsigned long long>(up % 60));
    display::text(8, 59, uptime, display::C(display::T_dim), 0);

    display::g().drawFastHLine(8, 69, display::width() - 16, display::C(display::T_line));

    display::text(8, 73, "CRONOMETRO", display::C(display::T_dim), 0);
    display::text(display::width() - 40, 73, watch.running ? "RUN" : "STOP",
                  watch.running ? display::C(display::T_ok) : display::C(display::T_dim), 0);

    char elapsed[24];
    formatElapsed(watch.current(), elapsed, sizeof(elapsed));
    display::text(8, 83, elapsed, watch.running ? display::C(display::T_acc)
                                                : display::C(display::T_text), 2);

    char lap[48];
    if (watch.lapCount == 0) {
        std::snprintf(lap, sizeof(lap), "L = registra un giro");
    } else {
        char lapTime[24];
        formatElapsed(watch.lapMs, lapTime, sizeof(lapTime));
        std::snprintf(lap, sizeof(lap), "GIRO %u: %s",
                      static_cast<unsigned>(watch.lapCount), lapTime);
    }
    display::text(8, 97, lap, display::C(display::T_dim), 0);

    display::text(8, 109, "ENTER avvia/stop  L giro  R reset",
                  display::C(display::T_dim), 0);
    display::footer("`=esci");
    display::push();
}

}  // namespace

void clockScreen() {
    ensureTimezone();
    navigation::waitRelease();
    Stopwatch watch;
    uint32_t lastDraw = 0;
    uint32_t lastStatus = millis();
    uint16_t lastBattery = display::batterySignature();

    for (;;) {
        const uint32_t now = millis();
        if (lastDraw == 0 || now - lastDraw >= 100) {
            lastDraw = now;
            drawClock(watch);
            lastBattery = display::batterySignature();
        }
        if (now - lastStatus >= 500) {
            lastStatus = now;
            refreshHeader("Orologio", "Strumenti", lastBattery);
        }

        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            break;
        }
        if (ev.type == navigation::Event::Select) {
            watch.toggle();
        } else if (ev.type == navigation::Event::Text) {
            if (ev.ch == 'l' || ev.ch == 'L') {
                watch.lap();
            } else if (ev.ch == 'r' || ev.ch == 'R') {
                watch.reset();
            }
        }
        delay(20);
    }
    navigation::waitRelease();
}

}  // namespace orbit_tools
