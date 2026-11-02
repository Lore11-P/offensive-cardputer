#include "Scores.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <sstream>

#include "ModuleFlags.h"
#include "Storage.h"

#if ENABLE_MODULE_GAMES

namespace games {
namespace scores {
namespace {

constexpr const char* kPath = "/scores.txt";
constexpr size_t kMaxBytes = 4096;

std::map<std::string, uint32_t> load() {
    std::map<std::string, uint32_t> out;
    std::string text;
    if (!storage::readText(kPath, text, kMaxBytes, true)) {
        return out;
    }
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) {
            continue;
        }
        const std::string name = line.substr(0, eq);
        const unsigned long value = std::strtoul(line.c_str() + eq + 1, nullptr, 10);
        out[name] = static_cast<uint32_t>(value);
    }
    return out;
}

void store(const std::map<std::string, uint32_t>& values) {
    std::string text;
    for (const auto& item : values) {
        text += item.first;
        text += '=';
        text += std::to_string(item.second);
        text += '\n';
    }
    storage::writeText(kPath, text, true);
}

}  // namespace

uint32_t best(const std::string& game) {
    const auto values = load();
    const auto it = values.find(game);
    return it == values.end() ? 0 : it->second;
}

void submit(const std::string& game, uint32_t score) {
    if (game.empty()) {
        return;
    }
    auto values = load();
    auto& slot = values[game];
    if (score > slot) {
        slot = score;
        store(values);
    }
}

std::string all() {
    const auto values = load();
    if (values.empty()) {
        return "";
    }
    std::string text;
    for (const auto& item : values) {
        text += item.first;
        text += ": ";
        text += std::to_string(item.second);
        text += '\n';
    }
    return text;
}

}  // namespace scores
}  // namespace games

#endif  // ENABLE_MODULE_GAMES
