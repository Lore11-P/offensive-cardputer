#pragma once

#include <cstdint>
#include <cstring>

#include <nvs.h>

// Record dei giochi: NVS namespace "orbit-scores", una chiave per gioco.
// Il formato legacy 2.x era /scores.txt; la migrazione avviene al primo
// submit se il file esiste (vedi ScoresMigration nel firmware 2.x).
namespace games {
namespace scores {
namespace detail {

inline void makeKey(const char* game, char* out, size_t cap) {
    size_t n = 0;
    for (const char* p = game; p != nullptr && *p != '\0' && n + 1 < cap; ++p) {
        const char c = *p;
        out[n++] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '_'
                       ? c
                       : '_';
    }
    out[n] = '\0';
}

}  // namespace detail

inline uint32_t best(const char* game) {
    char key[16] = {};
    detail::makeKey(game, key, sizeof(key));
    nvs_handle_t handle = 0;
    if (nvs_open("orbit-scores", NVS_READONLY, &handle) != ESP_OK) {
        return 0;
    }
    uint32_t value = 0;
    nvs_get_u32(handle, key, &value);
    nvs_close(handle);
    return value;
}

inline bool submit(const char* game, uint32_t score) {
    if (score <= best(game)) {
        return true;
    }
    char key[16] = {};
    detail::makeKey(game, key, sizeof(key));
    nvs_handle_t handle = 0;
    if (nvs_open("orbit-scores", NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    const bool ok = nvs_set_u32(handle, key, score) == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
}

inline bool preferSD() {
    return false;
}

inline void mergeBackends() {}

}  // namespace scores
}  // namespace games
