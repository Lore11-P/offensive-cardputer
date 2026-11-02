#include "TextInput.h"

#include <Arduino.h>
#include <M5Cardputer.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "Display.h"
#include "Navigation.h"

namespace textinput {
namespace {

bool wordContains(const std::vector<char>& word, char c) {
    return std::find(word.begin(), word.end(), c) != word.end();
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
        std::string shown = fields[i].secret ? std::string(fields[i].value.size(), '*')
                                             : fields[i].value;
        if (selected) {
            shown += '_';
        }
        display::text(8, y + 8, shown.empty() ? std::string("-") : shown,
                      display::kText, 2);
        y += 28;
    }
    display::footer("` = cancella/annulla   ENTER = conferma");
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
    bool lastDel = M5Cardputer.Keyboard.keysState().del;

    for (;;) {
        drawEditor(title, parent, fields, active);
        M5Cardputer.update();
        auto& state = M5Cardputer.Keyboard.keysState();
        const std::vector<char>& word = state.word;
        Field& field = fields[active];

        for (char c : word) {
            if (c < 0x20 || c >= 0x7F || wordContains(lastWord, c)) {
                continue;
            }
            if (c == '`') {
                if (!field.value.empty()) {
                    field.value.pop_back();
                } else {
                    navigation::waitRelease();
                    return false;
                }
            } else if (field.value.size() < field.maxLen) {
                if (field.numeric) {
                    if ((c >= '0' && c <= '9') || c == '.' || c == '-') {
                        field.value.push_back(c);
                    }
                } else {
                    field.value.push_back(c);
                }
            }
        }
        if (state.del && !lastDel && !field.value.empty()) {
            field.value.pop_back();
        }
        if (state.enter && !lastEnter) {
            ++active;
            if (active >= fields.size()) {
                break;
            }
        }
        lastWord = word;
        lastEnter = state.enter;
        lastDel = state.del;
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
