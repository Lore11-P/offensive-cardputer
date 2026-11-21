#include "GamesUi.h"

#include <Arduino.h>

#include "ModuleFlags.h"
#include "Navigation.h"

#if ENABLE_MODULE_GAMES

namespace games {
namespace ui {

bool waitRetry() {
    for (;;) {
        navigation::update();
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            return false;
        }
        if (ev.type == navigation::Event::Select) {
            return true;
        }
        delay(20);
    }
}

}  // namespace ui
}  // namespace games

#endif  // ENABLE_MODULE_GAMES
