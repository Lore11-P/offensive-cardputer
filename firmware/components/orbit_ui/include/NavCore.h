#pragma once

#include <cstring>

#include "Navigation.h"

namespace navigation {

// Stato tastiera e coda eventi senza hardware: la logica di differenza si
// prova sull'host (test/test_core) e resta una sola implementazione.
constexpr int kMaxKeys = 16;
constexpr int kMaxPending = 16;

struct KeyState {
    char word[kMaxKeys] = {};
    int count = 0;
    bool enter = false;
    bool back = false;
};

struct PendingKeys {
    char chars[kMaxPending] = {};
    int count = 0;
};

inline bool anyKey(const char* keys, int count, char c) {
    for (int i = 0; i < count; ++i) {
        if (keys[i] == c) {
            return true;
        }
    }
    return false;
}

inline bool arrowKey(char c) {
    return c == ';' || c == '.' || c == ',' || c == '/';
}

inline void pushKey(PendingKeys& pending, char c) {
    // shortcut: coda da 16, sufficiente per un campione della matrice; se
    // piena il tasto viene scartato. Alzare se la tastiera cambia.
    if (pending.count < kMaxPending) {
        pending.chars[pending.count++] = c;
    }
}

// Calcola l'evento del campione e accoda i tasti che non possono essere
// consegnati subito. I caratteri non-freccia entrano in coda PRIMA della
// scelta: un carattere che arriva con ENTER/ESC non si perde.
inline EventData stepKeys(const KeyState& prev, const KeyState& cur, PendingKeys& pending) {
    EventData ev;
    const bool newEnter = cur.enter && !prev.enter;
    const bool newBack = cur.back && !prev.back;

    char fresh[kMaxKeys];
    int freshCount = 0;
    for (int i = 0; i < cur.count; ++i) {
        const char c = cur.word[i];
        if (c >= 0x20 && c < 0x7F && !anyKey(prev.word, prev.count, c) &&
            freshCount < kMaxKeys) {
            fresh[freshCount++] = c;
        }
    }

    for (int i = 0; i < freshCount; ++i) {
        // '`' e' il tasto ESC fisico: non deve uscire come Text.
        if (!arrowKey(fresh[i]) && fresh[i] != '`') {
            pushKey(pending, fresh[i]);
        }
    }

    char chosenArrow = 0;
    if (newBack) {
        ev.type = Event::Back;
    } else if (newEnter) {
        ev.type = Event::Select;
    } else if (anyKey(fresh, freshCount, ';')) {
        ev.type = Event::Up;
        chosenArrow = ';';
    } else if (anyKey(fresh, freshCount, '.')) {
        ev.type = Event::Down;
        chosenArrow = '.';
    } else if (anyKey(fresh, freshCount, ',')) {
        ev.type = Event::Left;
        chosenArrow = ',';
    } else if (anyKey(fresh, freshCount, '/')) {
        ev.type = Event::Right;
        chosenArrow = '/';
    }

    // Le frecce fresche non consumate da questo evento restano in coda e
    // verranno consegnate ai poll successivi invece di essere scartate.
    for (int i = 0; i < freshCount; ++i) {
        if (arrowKey(fresh[i]) && fresh[i] != chosenArrow) {
            pushKey(pending, fresh[i]);
        }
    }

    if (ev.type == Event::None && pending.count > 0) {
        const char c = pending.chars[0];
        if (pending.count > 1) {
            std::memmove(pending.chars, pending.chars + 1,
                         static_cast<size_t>(pending.count - 1));
        }
        --pending.count;
        if (c == ';') {
            ev.type = Event::Up;
        } else if (c == '.') {
            ev.type = Event::Down;
        } else if (c == ',') {
            ev.type = Event::Left;
        } else if (c == '/') {
            ev.type = Event::Right;
        } else {
            ev.type = Event::Text;
            ev.ch = c;
        }
    }
    return ev;
}

}  // namespace navigation
