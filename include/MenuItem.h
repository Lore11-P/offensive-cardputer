#pragma once

#include <functional>
#include <string>

// Voce di menu: testo, icona (id logico), callbacks e stato.
// L'icona viene cercata in /icons/<id>.bmp; se manca, viene disegnata a runtime.
struct MenuItem {
    std::string label;
    std::string icon;
    std::function<void()> action;
    bool enabled = true;

    MenuItem() = default;
    MenuItem(std::string label_, std::string icon_, std::function<void()> action_, bool enabled_ = true)
        : label(std::move(label_)), icon(std::move(icon_)), action(std::move(action_)), enabled(enabled_) {
    }
};
