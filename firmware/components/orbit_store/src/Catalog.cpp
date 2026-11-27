#include "orbit_store/Store.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <cJSON.h>
#include <esp_log.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>

#include "Http.h"

namespace orbit_store {
namespace {

constexpr const char* kTag = "store-catalog";
constexpr const char* kCatalogUrl =
    "https://raw.githubusercontent.com/Lore11-P/offensive-cardputer/main/store/games.json";
constexpr const char* kCatalogSigUrl =
    "https://raw.githubusercontent.com/Lore11-P/offensive-cardputer/main/store/games.sig";
constexpr const char* kGameBaseUrl =
    "https://raw.githubusercontent.com/Lore11-P/offensive-cardputer/main/store/games/";
constexpr size_t kMaxCatalogBytes = 32 * 1024;
constexpr size_t kMaxGameBytes = 8192;
constexpr size_t kMaxGames = 24;
constexpr size_t kSignatureSize = 64;

// Chiave pubblica ECDSA P-256 dello store/OTA (tools/sign_ota.py).
constexpr uint8_t kPublicKey[65] = {
    0x04, 0xD9, 0x29, 0xB0, 0xDE, 0x2F, 0xB8, 0x4C, 0x01, 0x4B, 0x5B, 0x59,
    0xFD, 0xF5, 0x3D, 0x51, 0x27, 0x7B, 0x38, 0x34, 0x43, 0x05, 0xD2, 0xC9,
    0xB2, 0x61, 0x08, 0xD2, 0xAF, 0xEE, 0x3D, 0x46, 0x0E, 0x0A, 0x6B, 0xF3,
    0x30, 0x7E, 0x59, 0xB0, 0xEF, 0xEC, 0x95, 0xC0, 0x29, 0x32, 0x42, 0xEC,
    0xD4, 0x46, 0x90, 0x59, 0xDB, 0xA6, 0xB4, 0x32, 0x20, 0x77, 0x1C, 0x92,
    0xB9, 0xD8, 0x69, 0x76, 0xC7,
};

bool verifySignature(const uint8_t hash[32], const uint8_t signature[kSignatureSize]) {
    mbedtls_ecp_group group;
    mbedtls_ecp_point point;
    mbedtls_mpi r;
    mbedtls_mpi s;
    mbedtls_ecp_group_init(&group);
    mbedtls_ecp_point_init(&point);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    bool ok = false;
    if (mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
        mbedtls_ecp_point_read_binary(&group, &point, kPublicKey, sizeof(kPublicKey)) == 0 &&
        mbedtls_ecp_check_pubkey(&group, &point) == 0 &&
        mbedtls_mpi_read_binary(&r, signature, 32) == 0 &&
        mbedtls_mpi_read_binary(&s, signature + 32, 32) == 0) {
        ok = mbedtls_ecdsa_verify(&group, hash, 32, &point, &r, &s) == 0;
    }

    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    mbedtls_ecp_point_free(&point);
    mbedtls_ecp_group_free(&group);
    return ok;
}

bool verifyBlob(const uint8_t* data, size_t len, const uint8_t* signature,
                size_t signatureLen) {
    if (data == nullptr || signature == nullptr || signatureLen != kSignatureSize) {
        return false;
    }
    uint8_t hash[32] = {};
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    const bool ok = mbedtls_sha256_starts(&sha, 0) == 0 &&
                    mbedtls_sha256_update(&sha, data, len) == 0 &&
                    mbedtls_sha256_finish(&sha, hash) == 0;
    mbedtls_sha256_free(&sha);
    return ok && verifySignature(hash, signature);
}

bool validId(const std::string& id) {
    if (id.empty() || id.size() > 24) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
               c == '-';
    });
}

bool isSha256Hex(const std::string& value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

bool validInfo(const GameInfo& game) {
    return validId(game.id) && !game.name.empty() && game.name.size() <= 24 &&
           game.version.size() <= 8 && game.desc.size() <= 64 && game.size > 0 &&
           game.size <= kMaxGameBytes && isSha256Hex(game.sha256) &&
           game.url == std::string(kGameBaseUrl) + game.id + ".wren";
}

std::string jsonString(const cJSON* item) {
    return cJSON_IsString(item) ? std::string(item->valuestring) : std::string();
}

}  // namespace

bool fetchCatalog(std::vector<GameInfo>& out, std::string* error) {
    std::string body;
    if (!httpGet(kCatalogUrl, body, kMaxCatalogBytes, error)) {
        return false;
    }
    if (body.empty()) {
        if (error != nullptr) {
            *error = "Catalogo vuoto.";
        }
        return false;
    }

    std::string signature;
    if (!httpGet(kCatalogSigUrl, signature, kSignatureSize, error) ||
        signature.size() != kSignatureSize) {
        if (error != nullptr) {
            *error = "Firma catalogo assente.";
        }
        return false;
    }
    if (!verifyBlob(reinterpret_cast<const uint8_t*>(body.data()), body.size(),
                    reinterpret_cast<const uint8_t*>(signature.data()),
                    signature.size())) {
        if (error != nullptr) {
            *error = "Firma catalogo non valida.";
        }
        return false;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        if (error != nullptr) {
            *error = "Catalogo JSON non valido.";
        }
        return false;
    }
    const cJSON* version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON* games = cJSON_GetObjectItemCaseSensitive(root, "games");
    if (!cJSON_IsNumber(version) || version->valueint != 1 || !cJSON_IsArray(games)) {
        cJSON_Delete(root);
        if (error != nullptr) {
            *error = "Formato catalogo non supportato.";
        }
        return false;
    }

    out.clear();
    const cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, games) {
        if (out.size() >= kMaxGames) {
            break;
        }
        GameInfo game;
        game.id = jsonString(cJSON_GetObjectItemCaseSensitive(entry, "id"));
        game.name = jsonString(cJSON_GetObjectItemCaseSensitive(entry, "name"));
        game.version = jsonString(cJSON_GetObjectItemCaseSensitive(entry, "version"));
        game.desc = jsonString(cJSON_GetObjectItemCaseSensitive(entry, "desc"));
        game.url = jsonString(cJSON_GetObjectItemCaseSensitive(entry, "url"));
        game.sha256 = jsonString(cJSON_GetObjectItemCaseSensitive(entry, "sha256"));
        const cJSON* size = cJSON_GetObjectItemCaseSensitive(entry, "size");
        game.size = cJSON_IsNumber(size) ? static_cast<uint32_t>(size->valuedouble) : 0;
        if (!validInfo(game)) {
            continue;
        }
        const bool duplicate = std::any_of(out.begin(), out.end(),
                                           [&](const GameInfo& existing) {
                                               return existing.id == game.id;
                                           });
        if (!duplicate) {
            out.push_back(game);
        }
    }
    cJSON_Delete(root);
    return true;
}

}  // namespace orbit_store
