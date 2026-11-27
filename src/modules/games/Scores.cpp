#include "Scores.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "ModuleFlags.h"
#include "Storage.h"

#if GAMES_ANY

namespace games {
namespace scores {
namespace {

constexpr const char* kPath = "/scores.txt";
// Stesso limite della versione precedente: un file oltre 4 KB non viene
// sovrascritto, cosi' nessun record viene troncato o perso.
constexpr size_t kMaxBytes = 4096;

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

// Riconosce una riga "chiave=valore" in [start, end), ignorando un eventuale
// '\r'. Serve sia alle chiavi note sia al merge tra i due backend.
bool parseEntry(const std::string& text, size_t start, size_t end, size_t& keyStart,
                size_t& keyLen, uint32_t& value) {
    if (end > start && text[end - 1] == '\r') {
        --end;
    }
    const size_t eq = text.find('=', start);
    if (eq == std::string::npos || eq >= end || eq == start) {
        return false;
    }
    const char* number = text.c_str() + eq + 1;
    char* stop = nullptr;
    const unsigned long parsed = std::strtoul(number, &stop, 10);
    if (stop == number) {
        return false;
    }
    keyStart = start;
    keyLen = eq - start;
    value = static_cast<uint32_t>(parsed);
    return true;
}

// Riconosce "game=valore" in [start, end).
bool parseLine(const std::string& text, size_t start, size_t end, const char* game,
               uint32_t& value) {
    size_t keyStart = 0;
    size_t keyLen = 0;
    uint32_t parsed = 0;
    if (!parseEntry(text, start, end, keyStart, keyLen, parsed)) {
        return false;
    }
    const size_t gameLen = std::strlen(game);
    if (keyLen != gameLen || std::memcmp(text.c_str() + keyStart, game, gameLen) != 0) {
        return false;
    }
    value = parsed;
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

// Sostituisce tutte le righe del gioco, o la aggiunge se manca.
void setEntry(std::string& text, const char* game, uint32_t score) {
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
    text = std::move(out);
}

}  // namespace

// I record vanno sulla SD solo se la SD c'e': altrimenti restano su LittleFS,
// altrimenti su un dispositivo senza card non verrebbe salvato nulla.
bool preferSD() {
    return storage::hasSD();
}

// Al primo mount unisce su SD i record scritti su LittleFS senza card, cosi'
// non restano invisibili a chi poi legge solo il backend attivo.
void mergeBackends() {
    if (!storage::hasSD()) {
        return;
    }
    if (!storage::exists(kPath, false)) {
        return;
    }
    std::string lfs;
    if (!storage::readText(kPath, lfs, kMaxBytes, false)) {
        return;
    }
    std::string sd;
    if (storage::exists(kPath, true) && !storage::readText(kPath, sd, kMaxBytes, true)) {
        return;  // SD illeggibile: non sovrascrivere
    }
    bool changed = false;
    for (size_t start = 0; start < lfs.size();) {
        const size_t end = lineEnd(lfs, start);
        size_t keyStart = 0;
        size_t keyLen = 0;
        uint32_t value = 0;
        if (parseEntry(lfs, start, end, keyStart, keyLen, value)) {
            const std::string key = lfs.substr(keyStart, keyLen);
            uint32_t old = 0;
            if (!find(sd, key.c_str(), old) || value > old) {
                setEntry(sd, key.c_str(), value);
                changed = true;
            }
        }
        start = end + 1;
    }
    if (changed) {
        storage::writeText(kPath, sd, true);
    }
}

uint32_t best(const char* game) {
    mergeBackends();
    std::string text;
    uint32_t value = 0;
    if (!readAll(text) || !find(text, game, value)) {
        return 0;
    }
    return value;
}

bool submit(const char* game, uint32_t score) {
    if (game == nullptr || *game == '\0') {
        return false;
    }
    std::string text;
    // Se il file esiste ma non e' leggibile per intero non va sovrascritto:
    // si perderebbero i record che non sono stati letti.
    if (!readAll(text)) {
        return false;
    }
    uint32_t old = 0;
    if (find(text, game, old) && score <= old) {
        return true;  // record invariato: niente da scrivere
    }
    setEntry(text, game, score);
    return storage::writeText(kPath, text, preferSD());
}

}  // namespace scores
}  // namespace games

#endif  // ENABLE_MODULE_GAMES
