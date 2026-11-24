#include "modules/usb/UsbTransfer.h"

#include <Arduino.h>
#include <SD.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "Display.h"
#include "Logger.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "PowerTimer.h"
#include "Storage.h"
#include "Ui.h"
#include "UsbLogic.h"
#include "WifiSetup.h"

#if ENABLE_MODULE_SD

namespace usb {
namespace {

constexpr uint32_t kHeaderTimeoutMs = 15000;
constexpr uint32_t kChunkTimeoutMs = 10000;
constexpr char kMagic[8] = {'O', 'R', 'B', 'I', 'T', 'P', 'U', 'T'};

// Il buffer da 8 KB vive nell'heap solo durante il trasferimento: la RAM
// statica resta libera per framebuffer/WiFi/Wren.
enum class Rx { Ok, Timeout, Cancel };

// Lettura non bloccante con timeout di inattivita' e poll della tastiera.
Rx readBytes(uint8_t* dst, size_t len, uint32_t timeoutMs) {
    size_t got = 0;
    uint32_t last = millis();
    while (got < len) {
        while (got < len && Serial.available() > 0) {
            dst[got++] = static_cast<uint8_t>(Serial.read());
            last = millis();
        }
        if (got == len) {
            return Rx::Ok;
        }
        if (navigation::poll().type == navigation::Event::Back) {
            return Rx::Cancel;
        }
        if (millis() - last >= timeoutMs) {
            return Rx::Timeout;
        }
        delay(1);
    }
    return Rx::Ok;
}

void sendLine(const char* line) {
    Serial.print(line);
}

void drainInput() {
    while (Serial.available() > 0) {
        Serial.read();
    }
}

void drawIdle(const std::string& status = "", const std::string& detail = "",
              bool ok = true) {
    display::clear(display::kPaper);
    display::header("USB file", "Sistema");
    const int y = 32;
    if (status.empty()) {
        display::text(6, y, "Collega il PC e avvia", display::kInk, 2);
        display::text(6, y + 13, "tools/usb_put.py", display::kInk, 2);
        display::text(6, y + 34, "Solo microSD. Un file alla volta.", display::kMuted, 0);
    } else {
        display::text(6, y, status, ok ? display::kNeoGreen : display::kDanger, 2);
        if (!detail.empty()) {
            display::text(6, y + 15, detail, display::kInk, 0);
        }
        display::text(6, y + 34, "Pronto per un altro file.", display::kMuted, 0);
    }
    display::footer("ESC = esci");
    display::push();
}

void drawProgress(const std::string& path, uint32_t written, uint32_t total) {
    const float fraction =
        total > 0 ? static_cast<float>(written) / static_cast<float>(total) : 0.0f;
    display::clear(display::kPaper);
    display::header("USB file", "Sistema");
    display::text(6, 30, path, display::kInk, 2);

    char text[64];
    std::snprintf(text, sizeof(text), "%lu / %lu byte",
                  static_cast<unsigned long>(written), static_cast<unsigned long>(total));
    display::text(6, 48, text, display::kMuted, 0);
    std::snprintf(text, sizeof(text), "%d%%", static_cast<int>(fraction * 100.0f + 0.5f));
    display::text(6, 64, text, display::kInk, 4);
    display::progressBar(6, 86, display::width() - 12, 9, fraction, display::kAccent);
    display::footer("ESC = esci");
    display::push();
}

void ensureDirs(const std::string& path) {
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '/') {
            const std::string dir = "/" + path.substr(0, i);
            SD.mkdir(dir.c_str());
        }
    }
}

}  // namespace

void runTransfer() {
    if (!storage::hasSD()) {
        ui::message("USB file", {"!microSD non montata.", "Inserisci una card e riavvia."},
                    "Sistema");
        return;
    }

    navigation::waitRelease();
    drawIdle();
    sendLine("ORBITPUT READY\n");

    uint8_t* chunk = static_cast<uint8_t*>(malloc(kChunkBytes));
    if (chunk == nullptr) {
        ui::message("USB file", {"!Memoria insufficiente."}, "Sistema");
        return;
    }

    for (;;) {
        uint8_t raw[19];
        const Rx hr = readBytes(raw, sizeof(raw), kHeaderTimeoutMs);
        if (hr == Rx::Cancel) {
            sendLine("ERR cancel\n");
            break;
        }
        if (hr == Rx::Timeout) {
            sendLine("ERR timeout\n");
            continue;
        }

        if (std::memcmp(raw, kMagic, sizeof(kMagic)) != 0) {
            sendLine("ERR magic\n");
            drainInput();
            drawIdle("Errore: header non valido", "", false);
            continue;
        }
        const uint8_t version = raw[8];
        const uint16_t pathLen = static_cast<uint16_t>(raw[9] | (raw[10] << 8));
        const uint32_t size = static_cast<uint32_t>(raw[11]) |
                              (static_cast<uint32_t>(raw[12]) << 8) |
                              (static_cast<uint32_t>(raw[13]) << 16) |
                              (static_cast<uint32_t>(raw[14]) << 24);
        const uint32_t expectedCrc = static_cast<uint32_t>(raw[15]) |
                                     (static_cast<uint32_t>(raw[16]) << 8) |
                                     (static_cast<uint32_t>(raw[17]) << 16) |
                                     (static_cast<uint32_t>(raw[18]) << 24);

        if (version != 1) {
            sendLine("ERR version\n");
            drainInput();
            drawIdle("Errore: versione protocollo", "", false);
            continue;
        }
        if (pathLen == 0 || pathLen > kMaxPathBytes) {
            sendLine("ERR path\n");
            drainInput();
            drawIdle("Errore: path non valido", "", false);
            continue;
        }

        char rawPath[kMaxPathBytes];
        const Rx pr = readBytes(reinterpret_cast<uint8_t*>(rawPath), pathLen,
                                kHeaderTimeoutMs);
        if (pr == Rx::Cancel) {
            sendLine("ERR cancel\n");
            break;
        }
        if (pr == Rx::Timeout) {
            sendLine("ERR timeout\n");
            drainInput();
            continue;
        }

        std::string clean;
        if (!sanitizePath(std::string(rawPath, pathLen), clean)) {
            sendLine("ERR path\n");
            drawIdle("Errore: path non valido", "", false);
            continue;
        }
        if (size == 0 || size > kMaxFileBytes) {
            sendLine("ERR size\n");
            drawIdle("Errore: dimensione non valida", clean, false);
            continue;
        }
        if (!storage::hasSD()) {
            sendLine("ERR sd\n");
            drawIdle("Errore: microSD assente", clean, false);
            continue;
        }

        // Un trasferimento lungo non deve far scattare lo spegnimento auto.
        powertimer::Hold hold;
        const std::string finalPath = "/" + clean;
        const std::string tempPath = finalPath + ".part";
        ensureDirs(clean);
        File out = SD.open(tempPath.c_str(), FILE_WRITE);
        if (!out) {
            sendLine("ERR write\n");
            drawIdle("Errore: scrittura", clean, false);
            continue;
        }

        drawProgress(clean, 0, size);
        sendLine("OK\n");

        uint32_t written = 0;
        uint32_t crc = 0;
        bool failed = false;
        bool cancelled = false;
        while (written < size) {
            const size_t want =
                std::min(kChunkBytes, static_cast<size_t>(size - written));
            const Rx rr = readBytes(chunk, want, kChunkTimeoutMs);
            if (rr != Rx::Ok) {
                out.close();
                SD.remove(tempPath.c_str());
                if (rr == Rx::Cancel) {
                    sendLine("ERR cancel\n");
                    cancelled = true;
                    drawIdle("Annullato", clean, false);
                } else {
                    sendLine("ERR timeout\n");
                    drawIdle("Errore: timeout", clean, false);
                }
                failed = true;
                break;
            }
            if (out.write(chunk, want) != want) {
                out.close();
                SD.remove(tempPath.c_str());
                sendLine("ERR write\n");
                drawIdle("Errore: scrittura", clean, false);
                failed = true;
                break;
            }
            crc = usb::crc32(chunk, want, crc);
            written += want;
            // Ridisegna prima dell'ACK: il PC non invia il chunk successivo
            // finche' non lo riceve e il buffer CDC non va in overflow.
            drawProgress(clean, written, size);
            sendLine("ACK\n");
        }
        if (cancelled) {
            break;
        }
        if (failed) {
            continue;
        }
        if (crc != expectedCrc) {
            out.close();
            SD.remove(tempPath.c_str());
            sendLine("ERR crc\n");
            drawIdle("Errore: CRC non valido", clean, false);
            continue;
        }

        out.flush();
        out.close();
        // Il file finale resta intatto fino al rename; in caso di rename
        // fallito il vecchio file viene ripristinato dal .bak.
        const std::string backupPath = finalPath + ".bak";
        const bool hadFinal = SD.exists(finalPath.c_str());
        if (hadFinal) {
            SD.remove(backupPath.c_str());
            if (!SD.rename(finalPath.c_str(), backupPath.c_str())) {
                SD.remove(tempPath.c_str());
                sendLine("ERR write\n");
                drawIdle("Errore: rename", clean, false);
                continue;
            }
        }
        if (!SD.rename(tempPath.c_str(), finalPath.c_str())) {
            if (hadFinal) {
                SD.rename(backupPath.c_str(), finalPath.c_str());
            }
            SD.remove(tempPath.c_str());
            sendLine("ERR write\n");
            drawIdle("Errore: rename", clean, false);
            continue;
        }
        if (hadFinal) {
            SD.remove(backupPath.c_str());
        }
        sendLine("DONE\n");
        drawIdle("File ricevuto", clean, true);
    }

    free(chunk);
    navigation::waitRelease();
}

}  // namespace usb

#endif  // ENABLE_MODULE_SD
