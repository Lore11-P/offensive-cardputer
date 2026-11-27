#pragma once

#include <cstdint>

// Interfaccia tra il loop della mascotte e il disegno procedurale.
// `speech` puo' contenere due righe separate da '|'. `event` puo' essere "".
namespace mascot {

struct View {
    uint32_t score = 0;
    uint8_t affection = 0;  // 0..100
    uint8_t hearts = 0;     // intensita' coccola, decade ogni frame
    bool blink = false;
    bool happy = false;
    const char* speech = "";
    const char* event = "";
    uint32_t nowMs = 0;
};

// Disegna l'intera schermata (header, alieno, fumetto, statistiche, footer)
// e chiama display::push().
void drawScene(const View& view);

}  // namespace mascot
