#pragma once

#include <cstdint>

namespace games {
namespace scores {

// Record per gioco, salvati in /scores.txt come "nome=valore".
// L'API usa const char*: le chiamate passano tutti string literal e cosi'
// non serve costruire std::string per ogni lettura/scrittura.
uint32_t best(const char* game);
// false solo se la scrittura fallisce (file illeggibile o filesystem pieno).
bool submit(const char* game, uint32_t score);
// Backend usato per leggere e scrivere /scores.txt (SD se montata).
bool preferSD();
// Unisce su SD i record rimasti su LittleFS, se la SD e' montata.
void mergeBackends();

}  // namespace scores
}  // namespace games
