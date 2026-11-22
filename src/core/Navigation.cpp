#include "Navigation.h"

#include <Arduino.h>

#include "M5Cardputer.h"
#include "NavCore.h"
#include "PowerTimer.h"

namespace navigation {
namespace {

// Stato della tastiera senza allocazioni: un poll gira ogni ~2 ms nei menu,
// quindi i vector dinamici di prima creavano churn di heap continuo.
KeyState s_last;
// Eventi non ancora consegnati: un poll restituisce un solo evento.
PendingKeys s_pending;
// Ultimo istante in cui un tasto tenuto premuto ha contato come attivita'.
uint32_t s_heldAt = 0;
bool s_heldActive = false;
constexpr uint32_t kHeldActivityMs = 10000;

KeyState sampleKeyboard() {
    KeyState sample;
    auto& state = M5Cardputer.Keyboard.keysState();
    const int count = static_cast<int>(state.word.size());
    sample.count = count < kMaxKeys ? count : kMaxKeys;
    for (int i = 0; i < sample.count; ++i) {
        sample.word[i] = state.word[i];
    }
    sample.enter = state.enter;
    sample.back = state.del;
    for (char c : state.word) {
        if (c == '`') {
            sample.back = true;
            break;
        }
    }
    return sample;
}

}  // namespace

void begin() {
    M5Cardputer.update();
    s_last = sampleKeyboard();
    s_pending = PendingKeys{};
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

    const KeyState cur = sampleKeyboard();
    const EventData ev = stepKeys(s_last, cur, s_pending);

    // Qualsiasi variazione reale della tastiera conta come attivita' utente.
    if (ev.type != Event::None) {
        powertimer::activity();
    }

    s_last = cur;
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
    s_last = sampleKeyboard();
    s_pending = PendingKeys{};
}

}  // namespace navigation
