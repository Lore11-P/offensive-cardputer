#pragma once

#include <cstdint>

namespace games {
namespace scores {

// Record per gioco, salvati in /scores.txt come "nome=valore".
// L'API usa const char*: le chiamate passano tutti string literal e cosi'
// non serve costruire std::string per ogni lettura/scrittura.
uint32_t best(const char* game);
void submit(const char* game, uint32_t score);

}  // namespace scores
}  // namespace games
