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
// Caratteri nuovi non ancora consegnati: un poll restituisce un solo evento.
std::vector<char> s_pending;
// Ultimo istante in cui un tasto tenuto premuto ha contato come attivita'.
uint32_t s_heldAt = 0;
bool s_heldActive = false;
constexpr uint32_t kHeldActivityMs = 10000;

bool contains(const std::vector<char>& word, char c) {
    return std::find(word.begin(), word.end(), c) != word.end();
}

bool isPrintable(char c) {
    return c >= 0x20 && c < 0x7F;
}

bool isArrow(char c) {
    return c == ';' || c == '.' || c == ',' || c == '/';
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
    auto& state = M5Cardputer.Keyboard.keysState();
    s_lastWord = state.word;
    s_lastEnter = state.enter;
    s_lastBack = contains(s_lastWord, '`') || state.del;
    s_pending.clear();
    s_heldActive = false;
}

void update() {
    M5Cardputer.update();
}

EventData poll() {
    M5Cardputer.update();

    // Un tasto tenuto premuto e' attivita' continua (scorrimento, giochi).
    // Isteresi: si segnala al massimo ogni 10 s per non azzerare il timer
    // a ogni poll.
    if (M5Cardputer.Keyboard.isPressed() != 0) {
        const uint32_t now = millis();
        if (!s_heldActive || now - s_heldAt >= kHeldActivityMs) {
            s_heldActive = true;
            s_heldAt = now;
            powertimer::activity();
        }
    } else {
        s_heldActive = false;
    }

    // Spegnimento automatico. Il controllo gira qui perche' ogni loop della UI
    // passa da poll() per leggere la tastiera: un solo punto di aggancio.
    powertimer::tick();

    EventData ev;

    // Nota: non ci si puo' basare su Keyboard.isChange(), che confronta solo il
    // numero di tasti premuti. Se tra due poll un tasto viene rilasciato e un
    // altro premuto il conteggio resta identico e l'evento andrebbe perso.
    auto& state = M5Cardputer.Keyboard.keysState();
    const std::vector<char>& word = state.word;
    const bool enter = state.enter;
    const bool back = contains(word, '`') || state.del;

    const bool newEnter = enter && !s_lastEnter;
    const bool newBack = back && !s_lastBack;
    const std::vector<char> fresh = freshChars(word, s_lastWord);

    // Qualsiasi variazione reale della tastiera conta come attivita' utente.
    if (newEnter || newBack || !fresh.empty()) {
        powertimer::activity();
    }

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
        // Piu' caratteri nello stesso poll: accodali e consegnane uno per volta.
        for (char c : fresh) {
            s_pending.push_back(c);
        }
    }

    // Un evento freccia non deve far perdere i caratteri freschi arrivati nello
    // stesso campione: verranno consegnati come Text ai poll successivi.
    if (ev.type == Event::Up || ev.type == Event::Down || ev.type == Event::Left ||
        ev.type == Event::Right) {
        for (char c : fresh) {
            if (!isArrow(c)) {
                s_pending.push_back(c);
            }
        }
    }

    if (ev.type == Event::None && !s_pending.empty()) {
        ev.type = Event::Text;
        ev.ch = s_pending.front();
        s_pending.erase(s_pending.begin());
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
    s_lastBack = contains(s_lastWord, '`') || state.del;
    s_pending.clear();
}

}  // namespace navigation
