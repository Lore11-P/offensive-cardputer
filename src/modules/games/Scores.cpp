#include "Scores.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include "ModuleFlags.h"
#include "Storage.h"

#if ENABLE_MODULE_GAMES

namespace games {
namespace scores {
namespace {

constexpr const char* kPath = "/scores.txt";
// Stesso limite della versione precedente: un file oltre 4 KB non viene
// sovrascritto, cosi' nessun record viene troncato o perso.
constexpr size_t kMaxBytes = 4096;

// I record vanno sulla SD solo se la SD c'e': altrimenti restano su LittleFS,
// altrimenti su un dispositivo senza card non verrebbe salvato nulla.
bool preferSD() {
    return storage::hasSD();
}

// Legge tutto il file; true anche se il file non esiste ancora.
bool readAll(std::string& text) {
    if (!storage::exists(kPath, preferSD())) {
        return true;
    }
    return storage::readText(kPath, text, kMaxBytes, preferSD());
}

size_t lineEnd(const std::string& text, size_t start) {
    const size_t end = text.find('\n', start);
    return end == std::string::npos ? text.size() : end;
}

// Riconosce "game=valore" in [start, end), ignorando un eventuale '\r'.
bool parseLine(const std::string& text, size_t start, size_t end, const char* game,
               uint32_t& value) {
    if (end > start && text[end - 1] == '\r') {
        --end;
    }
    const size_t keyLen = std::strlen(game);
    if (end <= start + keyLen || text[start + keyLen] != '=') {
        return false;
    }
    if (std::memcmp(text.c_str() + start, game, keyLen) != 0) {
        return false;
    }
    const char* number = text.c_str() + start + keyLen + 1;
    char* stop = nullptr;
    const unsigned long parsed = std::strtoul(number, &stop, 10);
    if (stop == number) {
        return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

// Ultima occorrenza vince, come la vecchia mappa nome->valore.
bool find(const std::string& text, const char* game, uint32_t& out) {
    bool found = false;
    for (size_t start = 0; start < text.size();) {
        const size_t end = lineEnd(text, start);
        uint32_t value = 0;
        if (parseLine(text, start, end, game, value)) {
            out = value;
            found = true;
        }
        start = end + 1;
    }
    return found;
}

}  // namespace

uint32_t best(const char* game) {
    std::string text;
    uint32_t value = 0;
    if (!readAll(text) || !find(text, game, value)) {
        return 0;
    }
    return value;
}

void submit(const char* game, uint32_t score) {
    if (game == nullptr || *game == '\0') {
        return;
    }
    std::string text;
    // Se il file esiste ma non e' leggibile per intero non va sovrascritto:
    // si perderebbero i record che non sono stati letti.
    if (!readAll(text)) {
        return;
    }
    uint32_t old = 0;
    if (find(text, game, old) && score <= old) {
        return;
    }

    std::string out;
    out.reserve(text.size() + 16);
    bool replaced = false;
    for (size_t start = 0; start < text.size();) {
        const size_t end = lineEnd(text, start);
        uint32_t ignored = 0;
        if (parseLine(text, start, end, game, ignored)) {
            out += game;
            out += '=';
            out += std::to_string(score);
            out += '\n';
            replaced = true;
        } else {
            out.append(text, start, end - start);
            out += '\n';
        }
        start = end + 1;
    }
    if (!replaced) {
        out += game;
        out += '=';
        out += std::to_string(score);
        out += '\n';
    }
    storage::writeText(kPath, out, preferSD());
}

}  // namespace scores
}  // namespace games

#endif  // ENABLE_MODULE_GAMES
