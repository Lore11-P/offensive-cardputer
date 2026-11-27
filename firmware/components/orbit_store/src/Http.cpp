#include "orbit_store/Store.h"

#include <cstdio>
#include <string>

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>

#include "Http.h"

namespace orbit_store {
namespace {

constexpr const char* kTag = "store-http";
constexpr const char* kAllowedPrefix =
    "https://raw.githubusercontent.com/Lore11-P/offensive-cardputer/main/store/";

}  // namespace

// GET con TLS verificato (CA bundle) e tetto di dimensione. Solo URL dello
// store ufficiale: il catalogo non puo' puntare altrove.
bool httpGet(const std::string& url, std::string& out, size_t maxBytes,
             std::string* error) {
    if (url.rfind(kAllowedPrefix, 0) != 0) {
        if (error != nullptr) {
            *error = "URL non autorizzato.";
        }
        return false;
    }

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = 20000;
    config.buffer_size = 4096;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.disable_auto_redirect = true;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        if (error != nullptr) {
            *error = "Client HTTP non disponibile.";
        }
        return false;
    }
    esp_http_client_set_header(client, "User-Agent", "ORBIT-OS-STORE");

    bool ok = false;
    do {
        if (esp_http_client_open(client, 0) != ESP_OK) {
            if (error != nullptr) {
                *error = "Connessione non riuscita.";
            }
            break;
        }
        const int contentLen = esp_http_client_fetch_headers(client);
        if (contentLen > static_cast<int>(maxBytes)) {
            if (error != nullptr) {
                *error = "Risposta troppo grande.";
            }
            break;
        }
        if (esp_http_client_get_status_code(client) != 200) {
            if (error != nullptr) {
                *error = "HTTP " + std::to_string(esp_http_client_get_status_code(client));
            }
            break;
        }

        out.clear();
        char buffer[1024];
        for (;;) {
            const int read = esp_http_client_read(client, buffer, sizeof(buffer));
            if (read < 0) {
                if (error != nullptr) {
                    *error = "Lettura interrotta.";
                }
                break;
            }
            if (read == 0) {
                ok = true;
                break;
            }
            if (out.size() + static_cast<size_t>(read) > maxBytes) {
                if (error != nullptr) {
                    *error = "Risposta troppo grande.";
                }
                break;
            }
            out.append(buffer, static_cast<size_t>(read));
        }
    } while (false);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

}  // namespace orbit_store
