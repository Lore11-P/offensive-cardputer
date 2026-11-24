#include "OtaUpdate.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include "Display.h"
#include "Logger.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "Orbit.h"
#include "OtaPublicKey.h"
#include "PowerTimer.h"
#include "Ui.h"
#include "WifiSetup.h"

namespace ota {

const char* const kFirmwareUrl =
    "https://raw.githubusercontent.com/Lore11-P/offensive-cardputer/main/flasher/bin/firmware.bin";
const char* const kSignatureUrl =
    "https://raw.githubusercontent.com/Lore11-P/offensive-cardputer/main/flasher/bin/firmware.sig";

namespace {

// Manifest pubblicato accanto al binario. Contiene versione, dimensione e
// SHA-256: blocca il downgrade e il caso "main" che serve una release vecchia.
constexpr const char* kManifestUrl =
    "https://raw.githubusercontent.com/Lore11-P/offensive-cardputer/main/flasher/bin/firmware.json";

constexpr uint32_t kHttpTimeoutMs = 20000;
constexpr uint32_t kStallTimeoutMs = 60000;
constexpr size_t kChunkSize = 2048;
constexpr uint32_t kDrawIntervalMs = 120;
constexpr size_t kSignatureSize = 64;

// CA pinnata: Let's Encrypt ISRG Root X1. Il certificato del server puo'
// cambiare: in quel caso va aggiornata questa PEM, ma la firma ECDSA del
// firmware resta il controllo di autenticita' che non dipende dal CDN.
const char kRootCaPem[] = R"PEM(-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)PEM";

void drawProgress(const std::string& status, float fraction, const std::string& detail,
                  const std::string& parent);

// Solo HTTPS e solo host del repository/CDN GitHub: i redirect fuori da
// questa lista non vengono seguiti.
bool allowedUrl(const std::string& url) {
    return url.rfind("https://raw.githubusercontent.com/", 0) == 0 ||
           url.rfind("https://objects.githubusercontent.com/", 0) == 0;
}

// GET con redirect manuali (max 3) verso host in allowlist. Ritorna il codice
// HTTP (200 = ok) e lascia http aperto; -1 se un hop non e' ammesso.
int getAllowlisted(HTTPClient& http, WiFiClientSecure& client, std::string url) {
    for (int hop = 0; hop < 3; ++hop) {
        if (!allowedUrl(url)) {
            return -1;
        }
        http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
        if (!http.begin(client, url.c_str())) {
            return -1;
        }
        const int code = http.GET();
        if (code != 301 && code != 302 && code != 303 && code != 307 &&
            code != 308) {
            return code;
        }
        const String location = http.getLocation();
        http.end();
        if (!location.startsWith("https://")) {
            return -1;
        }
        url = location.c_str();
    }
    return -1;
}

// Con la CA pinnata mbedtls controlla la validita' temporale del certificato:
// senza orologio il TLS fallirebbe sempre con "not yet valid".
bool syncClock(const std::string& parent) {
    drawProgress("Sincronizzazione orologio", 0.0f, "NTP", parent);
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
    navigation::waitRelease();
    for (int i = 0; i < 100; ++i) {
        if (std::time(nullptr) > 1700000000) {
            return true;
        }
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            return false;
        }
        delay(100);
    }
    return false;
}

struct Manifest {
    unsigned major = 0;
    unsigned minor = 0;
    unsigned patch = 0;
    size_t size = 0;
    uint8_t sha256[32] = {};
};

bool parseVersion(const std::string& text, Manifest& manifest) {
    char extra = 0;
    return std::sscanf(text.c_str(), "%u.%u.%u%c", &manifest.major,
                       &manifest.minor, &manifest.patch, &extra) == 3;
}

int compareVersion(unsigned major, unsigned minor, unsigned patch,
                   const Manifest& remote) {
    if (remote.major != major) return remote.major > major ? 1 : -1;
    if (remote.minor != minor) return remote.minor > minor ? 1 : -1;
    if (remote.patch != patch) return remote.patch > patch ? 1 : -1;
    return 0;
}

bool parseHex32(const std::string& text, uint8_t out[32]) {
    if (text.size() != 64) {
        return false;
    }
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (int i = 0; i < 32; ++i) {
        const int hi = nibble(text[i * 2]);
        const int lo = nibble(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

bool parseManifest(const std::string& text, Manifest& manifest) {
    bool hasVersion = false;
    bool hasSize = false;
    bool hasSha = false;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.rfind("version=", 0) == 0) {
            hasVersion = parseVersion(line.substr(8), manifest);
        } else if (line.rfind("size=", 0) == 0) {
            char* endPtr = nullptr;
            const unsigned long value = std::strtoul(line.c_str() + 5, &endPtr, 10);
            hasSize = endPtr != nullptr && *endPtr == '\0' && value > 0;
            manifest.size = static_cast<size_t>(value);
        } else if (line.rfind("sha256=", 0) == 0) {
            hasSha = parseHex32(line.substr(7), manifest.sha256);
        }
    }
    return hasVersion && hasSize && hasSha;
}

// Scarica e valida il manifest. Il corpo e' piccolo: il limite evita che un
// server compromesso faccia allocare memoria a piacere.
bool fetchManifest(WiFiClientSecure& client, Manifest& manifest) {
    HTTPClient http;
    http.setTimeout(kHttpTimeoutMs);
    http.setUserAgent("ORBIT-OS-OTA");
    const int code = getAllowlisted(http, client, kManifestUrl);
    const int size = http.getSize();
    if (code != HTTP_CODE_OK || size <= 0 || size > 512) {
        http.end();
        return false;
    }
    std::string text;
    text.reserve(static_cast<size_t>(size));
    WiFiClient* stream = http.getStreamPtr();
    uint32_t lastData = millis();
    while (text.size() < static_cast<size_t>(size)) {
        const size_t available = stream->available();
        if (available == 0) {
            if (!http.connected() || millis() - lastData > kStallTimeoutMs) {
                break;
            }
            delay(2);
            continue;
        }
        lastData = millis();
        char chunk[64];
        const size_t read =
            stream->readBytes(chunk, std::min(available, sizeof(chunk)));
        if (read == 0) {
            break;
        }
        text.append(chunk, read);
    }
    http.end();
    if (text.size() != static_cast<size_t>(size)) {
        return false;
    }
    return parseManifest(text, manifest);
}

struct Sha256 {
    mbedtls_sha256_context ctx;
    Sha256() {
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts_ret(&ctx, 0);
    }
    ~Sha256() { mbedtls_sha256_free(&ctx); }
    void update(const uint8_t* data, size_t len) {
        mbedtls_sha256_update_ret(&ctx, data, len);
    }
    void finish(uint8_t out[32]) { mbedtls_sha256_finish_ret(&ctx, out); }
};

// Firma raw r||s a 64 byte, ECDSA P-256 sullo SHA-256 del binario.
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
        mbedtls_ecp_point_read_binary(&group, &point, kFirmwarePublicKey,
                                      sizeof(kFirmwarePublicKey)) == 0 &&
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

void drawProgress(const std::string& status, float fraction, const std::string& detail,
                  const std::string& parent) {
    display::clear(display::C(display::T_bg));
    orbit::sky(0, 0, display::width(), display::height(), 0x0A17);
    display::header("Aggiornamento", parent);

    char pct[8];
    std::snprintf(pct, sizeof(pct), "%d%%", static_cast<int>(fraction * 100.0f));
    display::text(12, 32, "CARICAMENTO FIRMWARE", display::C(display::T_dim), 0);
    display::text(display::width() - 12 - display::textWidth(pct, 4), 30, pct,
                  display::C(display::T_hi), 4);

    // Traiettoria di volo: il razzo avanza con il download.
    display::trajectory(14, 52, display::width() - 28, 40, fraction, status);
    if (!detail.empty()) {
        display::text(14, 106, detail, display::C(display::T_dim), 0);
    }
    display::footer("ESC = annulla");
    display::push();
}

// Scrive la dimensione in un buffer del chiamante: durante il download non
// vengono create stringhe temporanee a ogni ridisegno.
void formatSize(size_t bytes, char* out, size_t outLen) {
    if (bytes >= 1024 * 1024) {
        std::snprintf(out, outLen, "%.1f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        std::snprintf(out, outLen, "%u KB",
                      static_cast<unsigned>((bytes + 1023) / 1024));
    }
}

// Mostra un errore leggibile.
void fail(const std::string& title, const std::vector<std::string>& lines,
          const std::string& parent) {
    ui::message(title, lines, parent);
}

}  // namespace

void run(const std::string& parent) {
    if (!wifisetup::ensureConnected(true, parent)) {
        wifisetup::disconnect();
        fail("Aggiornamento",
             {"!Serve una connessione WiFi.", "Configura la rete da",
              "Impostazioni > Rete WiFi."},
             parent);
        return;
    }

    if (!ui::confirm("Aggiorna firmware",
                     {"Scaricare firmware.bin dal", "repository GitHub e applicarlo?",
                      "Serve WiFi e il device si", "riavvia al termine."},
                     parent)) {
        wifisetup::disconnect();
        return;
    }

    if (!syncClock(parent)) {
        wifisetup::disconnect();
        fail("Aggiornamento",
             {"!Orologio non sincronizzato.", "Serve NTP per validare il", "certificato TLS."},
             parent);
        return;
    }

    drawProgress("Verifica aggiornamento", 0.0f, "", parent);

    WiFiClientSecure client;
    client.setCACert(kRootCaPem);

    Manifest manifest;
    if (!fetchManifest(client, manifest)) {
        wifisetup::disconnect();
        fail("Aggiornamento",
             {"!Manifest OTA assente o non", "valido. Il server deve", "pubblicare firmware.json."},
             parent);
        return;
    }

    Manifest current;
    if (!parseVersion(fw::kVersion, current)) {
        wifisetup::disconnect();
        fail("Aggiornamento", {"!Versione firmware locale", "non riconosciuta."}, parent);
        return;
    }
    if (compareVersion(current.major, current.minor, current.patch, manifest) < 0) {
        char remote[24];
        std::snprintf(remote, sizeof(remote), "%u.%u.%u", manifest.major,
                      manifest.minor, manifest.patch);
        wifisetup::disconnect();
        fail("Aggiornamento",
             {"!Versione remota " + std::string(remote) + ":", "precedente alla " +
                  std::string(fw::kVersion) + ".",
              "Downgrade bloccato."},
             parent);
        return;
    }

    drawProgress("Connessione a GitHub", 0.0f, "", parent);

    HTTPClient http;
    http.setTimeout(kHttpTimeoutMs);
    http.setUserAgent("ORBIT-OS-OTA");

    const int code = getAllowlisted(http, client, kFirmwareUrl);
    if (code != HTTP_CODE_OK) {
        http.end();
        wifisetup::disconnect();
        fail("Aggiornamento",
             {"!Download non riuscito.", "HTTP " + std::to_string(code) + ".",
              "Riprova piu' tardi."},
             parent);
        return;
    }

    const int contentLength = http.getSize();
    if (contentLength <= 0 || static_cast<size_t>(contentLength) != manifest.size) {
        http.end();
        wifisetup::disconnect();
        fail("Aggiornamento",
             {"!Dimensione diversa dal manifest.", "Download interrotto."}, parent);
        return;
    }

    const size_t total = manifest.size;

    // Verifica esplicita del doppio slot: con il layout a slot singolo Update
    // non deve mai poter scrivere sulla partizione in esecuzione.
    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (target == nullptr || running == nullptr || target == running || target->size < total) {
        http.end();
        wifisetup::disconnect();
        fail("Aggiornamento",
             {"!Layout OTA non disponibile.", "Serve un flash USB con il", "nuovo partitions.csv."},
             parent);
        return;
    }

    // Update.begin sceglie la partizione OTA inattiva e verifica che il
    // firmware entri: se la tabella partizioni e' quella vecchia a slot
    // singolo, qui fallisce senza toccare il firmware in esecuzione.
    if (!Update.begin(total, U_FLASH)) {
        const std::string err = Update.errorString();
        http.end();
        wifisetup::disconnect();
        fail("Aggiornamento",
             {"!Partizione OTA non disponibile.", "Serve un flash USB per", "installare il nuovo layout.",
              err},
             parent);
        return;
    }

    powertimer::Hold hold;  // nessuno spegnimento automatico durante il download
    WiFiClient* stream = http.getStreamPtr();
    uint8_t buffer[kChunkSize];
    size_t written = 0;
    uint32_t lastDraw = 0;
    bool cancelled = false;
    bool writeFailed = false;
    Sha256 sha;

    navigation::waitRelease();
    uint32_t lastDataAt = millis();
    while (written < total) {
        const size_t available = stream->available();
        if (available == 0) {
            // Server che tiene la connessione aperta ma non invia piu' nulla:
            // senza un timeout il loop resterebbe appeso per sempre (con
            // powertimer::Hold attivo, quindi senza spegnimento automatico).
            if (!http.connected() || millis() - lastDataAt > kStallTimeoutMs) {
                break;  // connessione chiusa o download bloccato
            }
            navigation::update();
            if (navigation::poll().type == navigation::Event::Back) {
                cancelled = true;
                break;
            }
            delay(2);
            continue;
        }
        lastDataAt = millis();

        const size_t toRead = std::min(available, kChunkSize);
        const size_t read = stream->readBytes(buffer, toRead);
        if (read == 0) {
            break;
        }
        if (Update.write(buffer, read) != read) {
            writeFailed = true;
            break;
        }
        sha.update(buffer, read);
        written += read;

        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            cancelled = true;
            break;
        }

        if (millis() - lastDraw > kDrawIntervalMs || written == total) {
            lastDraw = millis();
            const float fraction = static_cast<float>(written) / static_cast<float>(total);
            char done[16];
            char full[16];
            char detail[48];
            formatSize(written, done, sizeof(done));
            formatSize(total, full, sizeof(full));
            std::snprintf(detail, sizeof(detail), "%d%%  %s / %s",
                          static_cast<int>(fraction * 100.0f), done, full);
            drawProgress("Download firmware", fraction, detail, parent);
        }
    }

    http.end();

    if (cancelled) {
        Update.abort();
        wifisetup::disconnect();
        fail("Aggiornamento",
             {"Aggiornamento annullato.", "Il firmware attuale non", "e' stato modificato."},
             parent);
        return;
    }

    if (writeFailed || written != total) {
        const std::string err = Update.errorString();
        Update.abort();
        wifisetup::disconnect();
        fail("Aggiornamento",
             {"!Scrittura non riuscita.", err, "Il firmware attuale resta attivo."}, parent);
        return;
    }

    uint8_t digest[32];
    sha.finish(digest);

    if (std::memcmp(digest, manifest.sha256, sizeof(digest)) != 0) {
        Update.abort();
        wifisetup::disconnect();
        fail("Aggiornamento",
             {"!Hash firmware diverso dal", "manifest: aggiornamento bloccato.",
              "Il firmware attuale resta attivo."},
             parent);
        return;
    }

    // Firma scaricata dopo il binario: la partizione inattiva e' gia' scritta,
    // ma il boot non viene spostato finche' la firma non e' valida.
    drawProgress("Verifica firma", 1.0f, "", parent);
    uint8_t signature[kSignatureSize];
    HTTPClient sigHttp;
    sigHttp.setTimeout(kHttpTimeoutMs);
    sigHttp.setUserAgent("ORBIT-OS-OTA");
    const int sigCode = getAllowlisted(sigHttp, client, kSignatureUrl);
    bool sigOk = false;
    if (sigCode == HTTP_CODE_OK && sigHttp.getSize() == static_cast<int>(kSignatureSize)) {
        WiFiClient* sigStream = sigHttp.getStreamPtr();
        size_t got = 0;
        uint32_t lastSigData = millis();
        while (got < kSignatureSize) {
            const size_t available = sigStream->available();
            if (available == 0) {
                if (!sigHttp.connected() || millis() - lastSigData > kStallTimeoutMs) {
                    break;
                }
                delay(2);
                continue;
            }
            lastSigData = millis();
            const size_t read = sigStream->readBytes(
                signature + got, std::min(available, kSignatureSize - got));
            if (read == 0) {
                break;
            }
            got += read;
        }
        sigOk = got == kSignatureSize && verifySignature(digest, signature);
    }
    sigHttp.end();

    if (!sigOk) {
        Update.abort();
        wifisetup::disconnect();
        fail("Aggiornamento",
             {"!Firma firmware assente o non", "valida: aggiornamento bloccato.",
              "Il firmware attuale resta attivo."},
             parent);
        return;
    }
    logger::info("OTA: firma ECDSA verificata (SHA-256 ok)");

    if (!Update.end(true)) {
        const std::string err = Update.errorString();
        Update.abort();
        wifisetup::disconnect();
        fail("Aggiornamento",
             {"!Scrittura non riuscita.", err, "Il firmware attuale resta attivo."}, parent);
        return;
    }

    // Verifica indipendente dell'immagine appena scritta: controlla la
    // struttura e il descrittore dell'app. Se non e' valida si ripristina la
    // partizione di boot corrente, senza riavviare sul firmware nuovo.
    esp_app_desc_t written_desc = {};
    if (esp_ota_get_partition_description(target, &written_desc) != ESP_OK) {
        esp_ota_set_boot_partition(running);
        wifisetup::disconnect();
        fail("Aggiornamento",
             {"!Immagine firmware non valida.", "Il firmware attuale resta attivo."}, parent);
        return;
    }

    logger::info("OTA: %u byte scritti, immagine %.32s %.32s, riavvio",
                 static_cast<unsigned>(written), written_desc.project_name,
                 written_desc.version);
    drawProgress("Completato", 1.0f, "Riavvio in corso...", parent);
    delay(1200);
    ESP.restart();
}

const char* rootCaPem() {
    return kRootCaPem;
}

bool githubUrlAllowed(const std::string& url) {
    return allowedUrl(url);
}

bool ensureClock(const std::string& parent) {
    return syncClock(parent);
}

bool verifyBlob(const uint8_t* data, size_t len, const uint8_t* signature,
                size_t signatureLen) {
    if (data == nullptr || signature == nullptr || signatureLen != kSignatureSize) {
        return false;
    }
    uint8_t hash[32] = {};
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    const bool ok = mbedtls_sha256_starts_ret(&sha, 0) == 0 &&
                    mbedtls_sha256_update_ret(&sha, data, len) == 0 &&
                    mbedtls_sha256_finish_ret(&sha, hash) == 0;
    mbedtls_sha256_free(&sha);
    return ok && verifySignature(hash, signature);
}

}  // namespace ota
