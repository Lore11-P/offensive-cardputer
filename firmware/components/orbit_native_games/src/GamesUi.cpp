#include "GamesUi.h"

#include "Navigation.h"
#include "orbit_compat.h"

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
