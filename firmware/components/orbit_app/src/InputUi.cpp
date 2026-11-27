#include "InputUi.h"

#include "Display.h"
#include "orbit_hal/keyboard.h"

namespace orbit_app {

bool textInput(const std::string& title, const std::string& parent, std::string& out,
               size_t maxLen) {
    if (out.size() > maxLen) {
        out.resize(maxLen);
    }
    for (;;) {
        display::clear(display::C(display::T_bg));
        display::header(title, parent);
        display::text(8, 34, out.empty() ? std::string("_") : out,
                      display::C(display::T_text), 2);
        display::text(8, 58, "INVIO=ok  `=annulla", display::C(display::T_dim), 0);
        display::push();

        orbit_hal::KeyEvent ev;
        while (orbit_hal::keyboardPoll(&ev)) {
            if (!ev.pressed) {
                continue;
            }
            if (ev.key == '\n') {
                return true;
            }
            if (ev.key == '\b') {
                if (!out.empty()) {
                    out.pop_back();
                }
                continue;
            }
            if (ev.key == '`') {
                return false;
            }
            if (ev.key >= 0x20 && ev.key < 0x7F && out.size() < maxLen) {
                out.push_back(ev.key);
            }
        }
        delay(15);
    }
}

}  // namespace orbit_app
