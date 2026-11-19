#include "TextInput.h"

#include <Arduino.h>
#include <M5Cardputer.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "Display.h"
#include "Navigation.h"
#include "PowerTimer.h"

namespace textinput {
namespace {

bool wordContains(const std::vector<char>& word, char c) {
    return std::find(word.begin(), word.end(), c) != word.end();
}

// Testo piu' largo del campo: ellissi + coda, cosi' cursore e backspace
// restano sempre nella parte visibile.
std::string tailWindow(const std::string& text, int maxWidth) {
    if (display::textWidth(text, 2) <= maxWidth) {
        return text;
    }
    const std::string dots = "...";
    std::string out = dots;
    for (size_t i = text.size(); i > 0; --i) {
        const std::string candidate = dots + text.substr(i - 1);
        if (display::textWidth(candidate, 2) > maxWidth) {
            break;
        }
        out = candidate;
    }
    return out;
}

void drawEditor(const std::string& title, const std::string& parent,
                const std::vector<Field>& fields, size_t active) {
    display::clear();
    display::header(title, parent);

    int y = 28;
    for (size_t i = 0; i < fields.size(); ++i) {
        const bool selected = (i == active);
        if (selected) {
            display::g().fillRoundRect(2, y - 3, display::width() - 6, 22, 3,
                                       display::kAccentDark);
            display::g().fillRect(2, y + 1, 2, 14, display::kAccent);
        }
        display::text(8, y - 1, fields[i].label, display::kTextDim, 0);
        // Contatore caratteri accanto all'etichetta, solo se c'e' spazio.
        char count[16];
        std::snprintf(count, sizeof(count), "%u/%u",
                      static_cast<unsigned>(fields[i].value.size()),
                      static_cast<unsigned>(fields[i].maxLen));
        const int countX = display::width() - 8 - display::textWidth(count, 0);
        if (countX > 8 + display::textWidth(fields[i].label, 0) + 6) {
            display::text(countX, y - 1, count,
                          selected ? display::kAccent : display::kTextDim, 0);
        }
        std::string shown = fields[i].secret ? std::string(fields[i].value.size(), '*')
                                             : fields[i].value;
        if (selected) {
            shown += '_';
        }
        shown = tailWindow(shown, display::width() - 16);
        display::text(8, y + 8, shown.empty() ? std::string("-") : shown,
                      display::kText, 2);
        y += 28;
    }
    display::footer("ESC=annulla  DEL=cancella  ENTER=ok");
    display::push();
}

}  // namespace

bool edit(const std::string& title, const std::string& parent,
          std::vector<Field>& fields) {
    if (fields.empty()) {
        return false;
    }
    size_t active = 0;

    navigation::waitRelease();
    M5Cardputer.update();
    std::vector<char> lastWord = M5Cardputer.Keyboard.keysState().word;
    bool lastEnter = M5Cardputer.Keyboard.keysState().enter;
    // Auto-repeat del tasto DEL: ~400 ms prima ripetizione, poi ogni 60 ms.
    uint32_t nextDel = 0;
    bool delHeld = false;

    for (;;) {
        powertimer::tick();
        drawEditor(title, parent, fields, active);
        M5Cardputer.update();
        auto& state = M5Cardputer.Keyboard.keysState();
        const std::vector<char>& word = state.word;
        Field& field = fields[active];

        for (char c : word) {
            if (c < 0x20 || c >= 0x7F || wordContains(lastWord, c)) {
                continue;
            }
            powertimer::activity();
            if (c == '`') {
                // ESC annulla sempre: la cancellazione e' solo su DEL.
                navigation::waitRelease();
                return false;
            }
            if (field.value.size() < field.maxLen) {
                if (field.numeric) {
                    if ((c >= '0' && c <= '9') || c == '.' || c == '-') {
                        field.value.push_back(c);
                    }
                } else {
                    field.value.push_back(c);
                }
            }
        }
        if (state.del) {
            if (!delHeld) {
                delHeld = true;
                nextDel = millis() + 400;
                powertimer::activity();
                if (!field.value.empty()) {
                    field.value.pop_back();
                }
            } else if (millis() >= nextDel) {
                nextDel = millis() + 60;
                powertimer::activity();
                if (!field.value.empty()) {
                    field.value.pop_back();
                }
            }
        } else {
            delHeld = false;
        }
        if (state.enter && !lastEnter) {
            powertimer::activity();
            ++active;
            if (active >= fields.size()) {
                break;
            }
        }
        lastWord = word;
        lastEnter = state.enter;
        delay(18);
    }
    navigation::waitRelease();
    return true;
}

bool parseFloat(const std::string& text, float& out) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const float value = std::strtof(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0') {
        return false;
    }
    out = value;
    return true;
}

bool parseInt(const std::string& text, long& out) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') {
        return false;
    }
    out = value;
    return true;
}

}  // namespace textinput
