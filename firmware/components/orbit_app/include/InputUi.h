#pragma once

#include <cstddef>
#include <string>

namespace orbit_app {

// Input testuale con lettura diretta del TCA8418: '.' ',' '/' ';' restano
// caratteri e non diventano frecce. ` annulla, Backspace cancella.
bool textInput(const std::string& title, const std::string& parent, std::string& out,
               size_t maxLen);

}  // namespace orbit_app
