#include "Navigation.h"

#include <cstring>

#include "NavCore.h"
#include "Logger.h"
#include "orbit_compat.h"
#include "orbit_hal/keyboard.h"

namespace navigation {
namespace {

KeyState s_last;
PendingKeys s_pending;
char s_held[kMaxKeys] = {};
int s_heldCount = 0;
bool s_enter = false;
bool s_back = false;
bool s_backspace = false;
bool s_backtick = false;

bool isHeld(char c) {
    for (int i = 0; i < s_heldCount; ++i) {
        if (s_held[i] == c) {
            return true;
        }
    }
    return false;
}

void addHeld(char c) {
    if (s_heldCount < kMaxKeys && !isHeld(c)) {
        s_held[s_heldCount++] = c;
    }
}

void removeHeld(char c) {
    for (int i = 0; i < s_heldCount; ++i) {
        if (s_held[i] == c) {
            for (int j = i; j + 1 < s_heldCount; ++j) {
                s_held[j] = s_held[j + 1];
            }
            --s_heldCount;
            return;
        }
    }
}

void drain() {
    orbit_hal::KeyEvent ev;
    while (orbit_hal::keyboardPoll(&ev)) {
        if (ev.key == '\n') {
            s_enter = ev.pressed;
            continue;
        }
        if (ev.key == '\b') {
            s_backspace = ev.pressed;
            s_back = s_backspace || s_backtick;
            continue;
        }
        if (ev.key == '`') {
            s_backtick = ev.pressed;
            s_back = s_backspace || s_backtick;
            continue;
        }
        if (ev.key >= 0x20 && ev.key < 0x7F) {
            if (ev.pressed) {
                addHeld(ev.key);
            } else {
                removeHeld(ev.key);
            }
        }
    }
}

KeyState sample() {
    KeyState state;
    state.count = s_heldCount;
    for (int i = 0; i < s_heldCount; ++i) {
        state.word[i] = s_held[i];
    }
    state.enter = s_enter;
    state.back = s_back;
    return state;
}

}  // namespace

void begin() {
    if (!orbit_hal::keyboardBegin()) {
        logger::error("tastiera non inizializzata");    }
    drain();
    s_last = sample();
    s_pending = PendingKeys{};
}

void update() {
    drain();
}

EventData poll() {
    drain();
    const KeyState cur = sample();
    const EventData ev = stepKeys(s_last, cur, s_pending);
    s_last = cur;
    return ev;
}

bool pressed(char c) {
    return isHeld(c);
}

bool enterPressed() {
    return s_enter;
}

bool backPressed() {
    return s_back;
}

bool anyPressed() {
    return s_heldCount > 0 || s_enter || s_back;
}

bool backspacePressed() {
    return s_backspace;
}

bool backtickPressed() {
    return s_backtick;
}

void waitRelease() {
    const uint32_t start = millis();
    for (;;) {
        drain();
        if (!anyPressed()) {
            return;
        }
        if (millis() - start > 3000) {
            return;
        }
        delay(10);
    }
}

}  // namespace navigation
