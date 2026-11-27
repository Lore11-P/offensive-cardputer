#include "orbit_store/Store.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <esp_log.h>
#include <mbedtls/sha256.h>

#include "Http.h"

namespace orbit_store {
namespace {

constexpr const char* kTag = "store";
constexpr size_t kMaxGameBytes = 8192;

std::string sha256Hex(const std::string& data) {
    uint8_t digest[32] = {};
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    const bool ok = mbedtls_sha256_starts(&sha, 0) == 0 &&
                    mbedtls_sha256_update(
                        &sha, reinterpret_cast<const uint8_t*>(data.data()),
                        data.size()) == 0 &&
                    mbedtls_sha256_finish(&sha, digest) == 0;
    mbedtls_sha256_free(&sha);
    if (!ok) {
        return std::string();
    }
    constexpr char kDigits[] = "0123456789abcdef";
    char hex[65] = {};
    for (size_t i = 0; i < sizeof(digest); ++i) {
        hex[i * 2] = kDigits[digest[i] >> 4];
        hex[i * 2 + 1] = kDigits[digest[i] & 0x0F];
    }
    return std::string(hex, 64);
}

}  // namespace

bool downloadGame(const GameInfo& game, std::string* error) {
    // Il runtime Wren spegne il WiFi prima di ogni gioco: senza questo,
    // dopo una partita l'installazione di un altro gioco falliva.
    if (!ensureWifi(error)) {
        return false;
    }
    std::string body;
    if (!httpGet(game.url, body, kMaxGameBytes, error)) {
        return false;
    }
    if (body.size() != game.size) {
        if (error != nullptr) {
            *error = "Dimensione non valida.";
        }
        return false;
    }
    if (sha256Hex(body) != game.sha256) {
        if (error != nullptr) {
            *error = "SHA-256 non valido.";
        }
        return false;
    }

    const std::string path = gamePath(game.id);
    const std::string temp = path + ".tmp";
    FILE* file = std::fopen(temp.c_str(), "wb");
    if (file == nullptr) {
        if (error != nullptr) {
            *error = "Scrittura su microSD non riuscita.";
        }
        return false;
    }
    const size_t written = std::fwrite(body.data(), 1, body.size(), file);
    std::fclose(file);
    if (written != body.size()) {
        std::remove(temp.c_str());
        if (error != nullptr) {
            *error = "Scrittura incompleta.";
        }
        return false;
    }
    std::remove(path.c_str());
    if (std::rename(temp.c_str(), path.c_str()) != 0) {
        std::remove(temp.c_str());
        if (error != nullptr) {
            *error = "Rename su microSD non riuscito.";
        }
        return false;
    }
    ESP_LOGI(kTag, "installato %s (%u B)", game.id.c_str(),
             static_cast<unsigned>(body.size()));
    return true;
}

}  // namespace orbit_store
