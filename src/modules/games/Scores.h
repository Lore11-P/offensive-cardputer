#pragma once

#include <cstdint>
#include <string>

namespace games {
namespace scores {

uint32_t best(const std::string& game);
void submit(const std::string& game, uint32_t score);
std::string all();

}  // namespace scores
}  // namespace games
