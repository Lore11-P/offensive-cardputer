#include "Navigation.h"

#include <Arduino.h>
#include <algorithm>
#include <cstring>
#include <vector>

#include "M5Cardputer.h"
#include "PowerTimer.h"

namespace navigation {
namespace {
std::vector<char> s_lastWord;
bool s_lastEnter = false;
bool s_lastBack = false;

bool contains(const std::vector<char>& word, char c) {
    return std::find(word.begin(), word.end(), c) != word.end();
}

bool isPrintable(char c) {
    return c >= 0x20 && c < 0x7F;
}

// Restituisce i caratteri presenti ora ma non nello stato precedente.
std::vector<char> freshChars(const std::vector<char>& now, const std::vector<char>& before) {
    std::vector<char> fresh;
    for (char c : now) {
        if (isPrintable(c) && !contains(before, c)) {
            fresh.push_back(c);
        }
    }
    return fresh;
}
}  // namespace

void begin() {
    M5Cardputer.update();
    s_lastWord = M5Cardputer.Keyboard.keysState().word;
    s_lastEnter = M5Cardputer.Keyboard.keysState().enter;
    s_lastBack = contains(s_lastWord, '`') || contains(s_lastWord, 0x1B);
}

void update() {
    M5Cardputer.update();
}

EventData poll() {
    M5Cardputer.update();

    // Spegnimento automatico. Il controllo gira qui perche' ogni loop della UI
    // passa da poll() per leggere la tastiera: un solo punto di aggancio.
    powertimer::tick();

    EventData ev;
    if (!M5Cardputer.Keyboard.isChange()) {
        return ev;
    }

    // Qualsiasi variazione della tastiera conta come attivita' dell'utente.
    powertimer::activity();

    auto& state = M5Cardputer.Keyboard.keysState();
    const std::vector<char>& word = state.word;
    const bool enter = state.enter;
    const bool back = contains(word, '`') || contains(word, 0x1B);

    const bool newEnter = enter && !s_lastEnter;
    const bool newBack = back && !s_lastBack;
    const std::vector<char> fresh = freshChars(word, s_lastWord);

    if (newBack) {
        ev.type = Event::Back;
    } else if (newEnter) {
        ev.type = Event::Select;
    } else if (contains(fresh, ';')) {
        ev.type = Event::Up;
    } else if (contains(fresh, '.')) {
        ev.type = Event::Down;
    } else if (contains(fresh, ',')) {
        ev.type = Event::Left;
    } else if (contains(fresh, '/')) {
        ev.type = Event::Right;
    } else if (!fresh.empty()) {
        ev.type = Event::Text;
        ev.ch = fresh.front();
    }

    s_lastWord = word;
    s_lastEnter = enter;
    s_lastBack = back;
    return ev;
}

bool pressed(char c) {
    return M5Cardputer.Keyboard.isKeyPressed(c);
}

bool enterPressed() {
    return M5Cardputer.Keyboard.keysState().enter;
}

bool backPressed() {
    return M5Cardputer.Keyboard.isKeyPressed('`') || M5Cardputer.Keyboard.keysState().del;
}

void waitRelease() {
    const uint32_t start = millis();
    for (;;) {
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isPressed() == 0) {
            break;
        }
        if (millis() - start > 3000) {
            break;  // sicurezza: non bloccare il firmware per un tasto guasto
        }
        delay(10);
    }
    // Riallinea lo stato interno all'ultimo campionamento.
    auto& state = M5Cardputer.Keyboard.keysState();
    s_lastWord = state.word;
    s_lastEnter = state.enter;
    s_lastBack = contains(s_lastWord, '`') || contains(s_lastWord, 0x1B);
}

}  // namespace navigation
