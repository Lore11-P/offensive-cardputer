#include "OtaUpdate.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "Display.h"
#include "Logger.h"
#include "Navigation.h"
#include "PowerTimer.h"
#include "Ui.h"
#include "WifiSetup.h"

namespace ota {

const char* const kFirmwareUrl =
    "https://raw.githubusercontent.com/Lore11-P/offensive-cardputer/main/flasher/bin/firmware.bin";

namespace {

constexpr uint32_t kHttpTimeoutMs = 20000;
constexpr size_t kChunkSize = 2048;
constexpr uint32_t kDrawIntervalMs = 120;

void drawProgress(const std::string& status, float fraction, const std::string& detail,
                  const std::string& parent) {
    display::clear();
    display::header("Aggiornamento", parent);
    display::textCentered(38, status, display::kAccent, 2);
    display::progressBar(20, 62, display::width() - 40, 10, fraction, display::kInfo);
    if (!detail.empty()) {
        display::textCentered(82, detail, display::kTextDim, 0);
    }
    display::footer("` = annulla");
    display::push();
}

std::string humanSize(size_t bytes) {
    char buf[24];
    if (bytes >= 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        std::snprintf(buf, sizeof(buf), "%u KB", static_cast<unsigned>((bytes + 1023) / 1024));
    }
    return std::string(buf);
}

// Mostra un errore leggibile e riaccende la radio per non lasciarla attiva.
void fail(const std::string& title, const std::vector<std::string>& lines,
          const std::string& parent) {
    ui::message(title, lines, parent);
}

}  // namespace

void run(const std::string& parent) {
    if (!wifisetup::ensureConnected(true, parent)) {
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
        return;
    }

    drawProgress("Connessione a GitHub", 0.0f, "", parent);

    WiFiClientSecure client;
    // Il firmware non e' firmato: il canale e' cifrato (TLS) ma senza pinning
    // di una CA. L'URL e' fisso nel repository del progetto.
    client.setInsecure();

    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(kHttpTimeoutMs);
    http.setUserAgent("CardputerFirmware-OTA");

    if (!http.begin(client, kFirmwareUrl)) {
        fail("Aggiornamento", {"!URL firmware non valido.", "Verifica OtaUpdate.cpp."}, parent);
        return;
    }

    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        http.end();
        fail("Aggiornamento",
             {"!Download non riuscito.", "HTTP " + std::to_string(code) + ".",
              "Riprova piu' tardi."},
             parent);
        return;
    }

    const int contentLength = http.getSize();
    if (contentLength <= 0) {
        http.end();
        fail("Aggiornamento", {"!Dimensione firmware sconosciuta.", "Riprova piu' tardi."},
             parent);
        return;
    }

    const size_t total = static_cast<size_t>(contentLength);

    // Verifica esplicita del doppio slot: con il layout a slot singolo Update
    // non deve mai poter scrivere sulla partizione in esecuzione.
    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (target == nullptr || running == nullptr || target == running || target->size < total) {
        http.end();
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

    navigation::waitRelease();
    while (written < total) {
        const size_t available = stream->available();
        if (available == 0) {
            if (!http.connected()) {
                break;  // connessione chiusa prima della fine
            }
            navigation::update();
            if (navigation::poll().type == navigation::Event::Back) {
                cancelled = true;
                break;
            }
            delay(2);
            continue;
        }

        const size_t toRead = std::min(available, kChunkSize);
        const size_t read = stream->readBytes(buffer, toRead);
        if (read == 0) {
            break;
        }
        if (Update.write(buffer, read) != read) {
            writeFailed = true;
            break;
        }
        written += read;

        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            cancelled = true;
            break;
        }

        if (millis() - lastDraw > kDrawIntervalMs || written == total) {
            lastDraw = millis();
            const float fraction = static_cast<float>(written) / static_cast<float>(total);
            drawProgress("Download firmware", fraction,
                         std::to_string(static_cast<int>(fraction * 100.0f)) + "%  " +
                             humanSize(written) + " / " + humanSize(total),
                         parent);
        }
    }

    http.end();

    if (cancelled) {
        Update.abort();
        fail("Aggiornamento",
             {"Aggiornamento annullato.", "Il firmware attuale non", "e' stato modificato."},
             parent);
        return;
    }

    if (writeFailed || written != total || !Update.end(true)) {
        const std::string err = Update.errorString();
        Update.abort();
        fail("Aggiornamento",
             {"!Scrittura non riuscita.", err, "Il firmware attuale resta attivo."}, parent);
        return;
    }

    logger::info("OTA: firmware %u byte scritto, riavvio", static_cast<unsigned>(written));
    drawProgress("Completato", 1.0f, "Riavvio in corso...", parent);
    delay(1200);
    ESP.restart();
}

}  // namespace ota
