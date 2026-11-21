#include "Navigation.h"

#include <Arduino.h>
#include <algorithm>
#include <cstring>
#include <vector>

#include "M5Cardputer.h"
#include "PowerTimer.h"

namespace navigation {
namespace {

// Stato della tastiera senza allocazioni: un poll gira ogni ~2 ms nei menu,
// quindi i vector dinamici di prima creavano churn di heap continuo.
constexpr int kMaxWord = 16;
constexpr int kMaxPending = 16;

char s_lastWord[kMaxWord];
int s_lastWordCount = 0;
bool s_lastEnter = false;
bool s_lastBack = false;
// Caratteri nuovi non ancora consegnati: un poll restituisce un solo evento.
char s_pending[kMaxPending];
int s_pendingCount = 0;
// Ultimo istante in cui un tasto tenuto premuto ha contato come attivita'.
uint32_t s_heldAt = 0;
bool s_heldActive = false;
constexpr uint32_t kHeldActivityMs = 10000;

bool contains(const char* word, int count, char c) {
    for (int i = 0; i < count; ++i) {
        if (word[i] == c) {
            return true;
        }
    }
    return false;
}

bool lastContains(char c) {
    return contains(s_lastWord, s_lastWordCount, c);
}

bool isPrintable(char c) {
    return c >= 0x20 && c < 0x7F;
}

bool isArrow(char c) {
    return c == ';' || c == '.' || c == ',' || c == '/';
}

void storeWord(const std::vector<char>& word) {
    const int count = static_cast<int>(word.size());
    s_lastWordCount = count < kMaxWord ? count : kMaxWord;
    for (int i = 0; i < s_lastWordCount; ++i) {
        s_lastWord[i] = word[i];
    }
}

void pushPending(char c) {
    // shortcut: la coda tiene 16 caratteri, abbastanza per un poll di una
    // tastiera a matrice; se piena il carattere viene scartato.
    if (s_pendingCount < kMaxPending) {
        s_pending[s_pendingCount++] = c;
    }
}

void popPending() {
    if (s_pendingCount > 1) {
        std::memmove(s_pending, s_pending + 1, static_cast<size_t>(s_pendingCount - 1));
    }
    --s_pendingCount;
}

}  // namespace

void begin() {
    M5Cardputer.update();
    auto& state = M5Cardputer.Keyboard.keysState();
    storeWord(state.word);
    s_lastEnter = state.enter;
    s_lastBack = lastContains('`') || state.del;
    s_pendingCount = 0;
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
    const bool back = contains(word.data(), static_cast<int>(word.size()), '`') || state.del;

    const bool newEnter = enter && !s_lastEnter;
    const bool newBack = back && !s_lastBack;

    // Restituisce i caratteri presenti ora ma non nello stato precedente.
    char fresh[kMaxWord];
    int freshCount = 0;
    for (char c : word) {
        if (isPrintable(c) && !lastContains(c) && freshCount < kMaxWord) {
            fresh[freshCount++] = c;
        }
    }

    // Qualsiasi variazione reale della tastiera conta come attivita' utente.
    if (newEnter || newBack || freshCount > 0) {
        powertimer::activity();
    }

    if (newBack) {
        ev.type = Event::Back;
    } else if (newEnter) {
        ev.type = Event::Select;
    } else if (contains(fresh, freshCount, ';')) {
        ev.type = Event::Up;
    } else if (contains(fresh, freshCount, '.')) {
        ev.type = Event::Down;
    } else if (contains(fresh, freshCount, ',')) {
        ev.type = Event::Left;
    } else if (contains(fresh, freshCount, '/')) {
        ev.type = Event::Right;
    } else {
        // Piu' caratteri nello stesso poll: accodali e consegnane uno per volta.
        for (int i = 0; i < freshCount; ++i) {
            pushPending(fresh[i]);
        }
    }

    // Un evento freccia non deve far perdere i caratteri freschi arrivati nello
    // stesso campione: verranno consegnati come Text ai poll successivi.
    if (ev.type == Event::Up || ev.type == Event::Down || ev.type == Event::Left ||
        ev.type == Event::Right) {
        for (int i = 0; i < freshCount; ++i) {
            if (!isArrow(fresh[i])) {
                pushPending(fresh[i]);
            }
        }
    }

    if (ev.type == Event::None && s_pendingCount > 0) {
        ev.type = Event::Text;
        ev.ch = s_pending[0];
        popPending();
    }

    storeWord(word);
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
    storeWord(state.word);
    s_lastEnter = state.enter;
    s_lastBack = lastContains('`') || state.del;
    s_pendingCount = 0;
}

}  // namespace navigation
