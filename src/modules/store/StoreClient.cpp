#include "StoreClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

#include "Display.h"
#include "Navigation.h"
#include "OtaUpdate.h"
#include "Storage.h"
#include "StoreConfig.h"
#include "Ui.h"
#include "WifiSetup.h"

namespace store {
namespace {

constexpr uint32_t kHttpTimeoutMs = 20000;
// Nessun dato per 20 s: la connessione e' da considerare morta.
constexpr uint32_t kStallTimeoutMs = 20000;
constexpr uint32_t kDrawIntervalMs = 100;
constexpr size_t kReadChunk = 1024;
constexpr int kMaxRedirects = 3;

bool isHttps(const std::string& url) {
    return url.rfind("https://", 0) == 0;
}

// Regole comuni a catalogo e download: niente voci ambigue o oversized.
bool validInfo(const GameInfo& game) {
    return validId(game.id) && !game.name.empty() && game.size > 0 &&
           game.size <= kMaxGameBytes && isSha256Hex(game.sha256) &&
           ota::githubUrlAllowed(game.url);
}

// GET solo verso URL https in allowlist, con redirect manuali (max 3).
// Ritorna il codice HTTP e lascia http aperto; -1 se un hop non e' ammesso.
int getAllowlisted(HTTPClient& http, WiFiClientSecure& client, const std::string& url) {
    std::string current = url;
    for (int hop = 0; hop <= kMaxRedirects; ++hop) {
        if (!isHttps(current) || !ota::githubUrlAllowed(current)) {
            return -1;
        }
        http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
        http.setReuse(false);  // connection close: utile se manca Content-Length
        if (!http.begin(client, current.c_str())) {
            return -1;
        }
        const int code = http.GET();
        if (code != 301 && code != 302 && code != 303 && code != 307 &&
            code != 308) {
            return code;
        }
        const String location = http.getLocation();
        http.end();
        if (location.length() == 0) {
            return -1;
        }
        current = location.c_str();
    }
    return -1;
}

// Legge il corpo con un tetto massimo. Con Content-Length assente accumula
// fino a maxBytes + 1 byte: quel byte in piu' segnala il corpo oversized.
bool readCapped(HTTPClient& http, size_t maxBytes, std::string& out) {
    const int declared = http.getSize();
    if (declared > static_cast<int>(maxBytes)) {
        return false;
    }
    out.clear();
    if (declared > 0) {
        out.reserve(static_cast<size_t>(declared));
    }
    const size_t target = declared >= 0 ? static_cast<size_t>(declared) : maxBytes + 1;
    WiFiClient* stream = http.getStreamPtr();
    uint32_t lastData = millis();
    while (out.size() < target) {
        const size_t available = stream->available();
        if (available == 0) {
            if (!http.connected() || millis() - lastData > kStallTimeoutMs) {
                break;
            }
            delay(2);
            continue;
        }
        lastData = millis();
        char buffer[512];
        const size_t read = stream->readBytes(buffer, std::min(available, sizeof(buffer)));
        if (read == 0) {
            break;
        }
        const size_t room = target - out.size();
        out.append(buffer, std::min(read, room));
    }
    if (out.size() > maxBytes) {
        return false;
    }
    return declared < 0 || out.size() == static_cast<size_t>(declared);
}

void drawDownload(const std::string& name, size_t done, size_t total) {
    float fraction = 0.0f;
    if (total > 0) {
        fraction = std::min(1.0f, static_cast<float>(done) / static_cast<float>(total));
    }
    display::clear(display::C(display::T_bg));
    display::header("Download", "Store");
    display::text(10, 32, name, display::C(display::T_text), 2);
    display::progressBar(12, 58, display::width() - 24, 8, fraction,
                         display::C(display::T_acc));
    char percent[8];
    std::snprintf(percent, sizeof(percent), "%d%%",
                  static_cast<int>(fraction * 100.0f + 0.5f));
    display::textCentered(76, percent, display::C(display::T_hi), 2);
    display::footer("ESC = annulla");
    display::push();
}

}  // namespace

bool fetchCatalog(std::vector<GameInfo>& out, const std::string& parent) {
    out.clear();
    if (!wifisetup::ensureConnected(true, parent)) {
        ui::message("Store",
                    {"!Serve una connessione WiFi.", "Configura la rete da",
                     "Impostazioni > Rete WiFi."},
                    parent);
        return false;
    }
    if (!ota::ensureClock(parent)) {
        ui::message("Store",
                    {"!Orologio non sincronizzato.", "Serve NTP per validare il",
                     "certificato TLS."},
                    parent);
        return false;
    }

    WiFiClientSecure client;
    client.setCACert(ota::rootCaPem());
    HTTPClient http;
    http.setTimeout(kHttpTimeoutMs);
    http.setUserAgent("ORBIT-OS-STORE");

    const int code = getAllowlisted(http, client, kCatalogUrl);
    if (code != HTTP_CODE_OK) {
        http.end();
        if (code < 0) {
            ui::message("Store", {"!Catalogo non raggiungibile.", "Controlla la rete."}, parent);
        } else {
            ui::message("Store",
                        {"!Catalogo non raggiungibile.", "HTTP " + std::to_string(code) + "."},
                        parent);
        }
        return false;
    }

    std::string body;
    const bool bodyOk = readCapped(http, kMaxCatalogBytes, body);
    http.end();
    if (!bodyOk) {
        ui::message("Store", {"!Catalogo troppo grande o", "download interrotto."}, parent);
        return false;
    }

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, body);
    if (error) {
        ui::message("Store", {"!Catalogo JSON non valido.", error.c_str()}, parent);
        return false;
    }
    if ((doc["version"] | 0) != 1 || !doc["games"].is<JsonArray>()) {
        ui::message("Store", {"!Formato catalogo non", "supportato."}, parent);
        return false;
    }

    for (JsonObject entry : doc["games"].as<JsonArray>()) {
        if (out.size() >= kMaxGames) {
            break;
        }
        GameInfo game;
        game.id = static_cast<const char*>(entry["id"] | "");
        game.name = static_cast<const char*>(entry["name"] | "");
        game.version = static_cast<const char*>(entry["version"] | "");
        game.desc = static_cast<const char*>(entry["desc"] | "");
        game.url = static_cast<const char*>(entry["url"] | "");
        game.sha256 = static_cast<const char*>(entry["sha256"] | "");
        game.size = entry["size"] | 0U;
        if (validInfo(game)) {
            out.push_back(game);
        }
    }
    return true;
}

bool downloadGame(const GameInfo& info, const std::string& parent) {
    const std::string path = gamePath(info.id);
    if (path.empty() || !validInfo(info)) {
        ui::message("Store", {"!Voce di catalogo non valida."}, parent);
        return false;
    }

    WiFiClientSecure client;
    client.setCACert(ota::rootCaPem());
    HTTPClient http;
    http.setTimeout(kHttpTimeoutMs);
    http.setUserAgent("ORBIT-OS-STORE");

    const int code = getAllowlisted(http, client, info.url);
    if (code != HTTP_CODE_OK) {
        http.end();
        if (code < 0) {
            ui::message("Store", {"!Download non riuscito.", "Controlla la rete."}, parent);
        } else {
            ui::message("Store",
                        {"!Download non riuscito.", "HTTP " + std::to_string(code) + "."}, parent);
        }
        return false;
    }
    const int declared = http.getSize();
    if (declared > static_cast<int>(kMaxGameBytes)) {
        http.end();
        ui::message("Store", {"!File troppo grande.", "Download interrotto."}, parent);
        return false;
    }
    const size_t total = declared > 0 ? static_cast<size_t>(declared) : info.size;

    std::string body;
    body.reserve(total);
    WiFiClient* stream = http.getStreamPtr();
    uint32_t lastData = millis();
    uint32_t lastDraw = millis();
    bool cancelled = false;
    bool tooLarge = false;

    navigation::waitRelease();
    drawDownload(info.name, 0, total);
    while (declared < 0 || body.size() < static_cast<size_t>(declared)) {
        const size_t available = stream->available();
        if (available == 0) {
            if (!http.connected() || millis() - lastData > kStallTimeoutMs) {
                break;
            }
            if (navigation::poll().type == navigation::Event::Back) {
                cancelled = true;
                break;
            }
            delay(2);
        } else {
            lastData = millis();
            char buffer[kReadChunk];
            const size_t read = stream->readBytes(buffer, std::min(available, sizeof(buffer)));
            if (read == 0) {
                break;
            }
            if (body.size() + read > kMaxGameBytes) {
                tooLarge = true;
                break;
            }
            body.append(buffer, read);
            if (navigation::poll().type == navigation::Event::Back) {
                cancelled = true;
                break;
            }
        }
        const uint32_t now = millis();
        if (now - lastDraw >= kDrawIntervalMs) {
            lastDraw = now;
            drawDownload(info.name, body.size(), total);
        }
    }
    http.end();

    if (cancelled) {
        return false;
    }
    if (tooLarge) {
        ui::message("Store", {"!File troppo grande.", "Download interrotto."}, parent);
        return false;
    }
    if (declared >= 0 && body.size() != static_cast<size_t>(declared)) {
        ui::message("Store", {"!Download incompleto."}, parent);
        return false;
    }

    // SHA-256 prima di toccare il filesystem: un file corrotto non viene mai
    // scritto sulla microSD.
    uint8_t digest[32];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts_ret(&sha, 0);
    mbedtls_sha256_update_ret(
        &sha, reinterpret_cast<const unsigned char*>(body.data()), body.size());
    mbedtls_sha256_finish_ret(&sha, digest);
    mbedtls_sha256_free(&sha);

    char hex[65];
    constexpr char kDigits[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); ++i) {
        hex[i * 2] = kDigits[digest[i] >> 4];
        hex[i * 2 + 1] = kDigits[digest[i] & 0x0F];
    }
    hex[64] = '\0';
    if (info.sha256 != hex) {
        ui::message("Store", {"!SHA-256 non valido.", "File scartato."}, parent);
        return false;
    }

    if (!storage::mkdir(kGamesDir, true) && !storage::exists(kGamesDir, true)) {
        ui::message("Store", {"!Cartella /games non", "creabile sulla microSD."}, parent);
        return false;
    }
    if (!storage::writeText(path, body, true)) {
        ui::message("Store", {"!Scrittura su microSD", "non riuscita."}, parent);
        return false;
    }
    return true;
}

bool gameInstalled(const GameInfo& info) {
    const std::string path = gamePath(info.id);
    return !path.empty() && storage::exists(path, true) &&
           storage::fileSize(path, true) == info.size;
}

}  // namespace store
