#include "orbit_debug/Console.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "driver/usb_serial_jtag.h"
#include "esp_vfs_fat.h"
#include "orbit_hal/battery.h"
#include "orbit_store/Store.h"
#include "orbit_wren/GameRuntime.h"

namespace orbit_debug {
namespace {

constexpr size_t kLineMax = 96;

// Prefisso fisso: le risposte si distinguono dai log sullo stesso canale.
void reply(const char* fmt, ...) {
    std::printf("[DBG] ");
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
}

void cmdHelp() {
    reply("comandi: help heap wifi battery sd games run <nome|path> reboot");
    reply("run blocca la console fino all'uscita: usarlo a menu UI fermo");
}

void cmdHeap() {
    reply("heap libero=%u minimo=%u",
          static_cast<unsigned>(esp_get_free_heap_size()),
          static_cast<unsigned>(esp_get_minimum_free_heap_size()));
    reply("blocco 8bit=%u dma=%u",
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
}

void cmdBattery() {
    reply("batteria: %d mV %d%%", orbit_hal::batteryMillivolts(),
          orbit_hal::batteryPercent());
}

void cmdSd() {
    uint64_t total = 0;
    uint64_t freeBytes = 0;
    if (!orbit_store::mountSd()) {
        reply("sd non montata");
        return;
    }
    if (esp_vfs_fat_info("/sd", &total, &freeBytes) != ESP_OK) {
        reply("sd: info non disponibile");
        return;
    }
    reply("sd totale=%lluMB liberi=%lluMB",
          static_cast<unsigned long long>(total / (1024 * 1024)),
          static_cast<unsigned long long>(freeBytes / (1024 * 1024)));
}

void cmdWifi() {
    reply("connessione WiFi (puo' richiedere ~45 s)...");
    std::string error;
    if (orbit_store::ensureWifi(&error)) {
        reply("wifi ok");
    } else {
        reply("wifi errore: %s", error.c_str());
    }
    std::string ssid;
    if (orbit_store::currentSsid(ssid)) {
        reply("ssid=%s", ssid.c_str());
    } else {
        reply("ssid assente");
    }
}

void cmdGames() {
    if (!orbit_store::mountLittleFs()) {
        reply("archivio giochi non disponibile");
        return;
    }
    const std::vector<std::string> games = orbit_store::installedGames();
    reply("giochi=%u", static_cast<unsigned>(games.size()));
    for (const std::string& name : games) {
        reply("  %s", name.c_str());
    }
}

void cmdRun(const char* arg) {
    while (*arg == ' ') {
        ++arg;
    }
    if (*arg == '\0') {
        reply("uso: run <nome|path>");
        return;
    }
    if (!orbit_store::mountLittleFs()) {
        reply("archivio giochi non disponibile");
        return;
    }
    std::string path = arg;
    // Nome semplice -> /littlefs/games/<nome>.wren; un percorso resta invariato.
    if (path.find('/') == std::string::npos) {
        path = std::string(orbit_store::kGamesDir) + "/" + path;
        if (path.size() < 5 ||
            path.compare(path.size() - 5, 5, ".wren") != 0) {
            path += ".wren";
        }
    }
    reply("eseguo %s; console bloccata fino all'uscita", path.c_str());
    reply("esito: %s", orbit_wren::runGame(path) ? "ok" : "errore");
}

void handleLine(const char* line) {
    if (line[0] == '\0') {
        return;
    }
    if (std::strcmp(line, "help") == 0) {
        cmdHelp();
    } else if (std::strcmp(line, "heap") == 0) {
        cmdHeap();
    } else if (std::strcmp(line, "battery") == 0) {
        cmdBattery();
    } else if (std::strcmp(line, "sd") == 0) {
        cmdSd();
    } else if (std::strcmp(line, "wifi") == 0) {
        cmdWifi();
    } else if (std::strcmp(line, "games") == 0) {
        cmdGames();
    } else if (std::strncmp(line, "run ", 4) == 0) {
        cmdRun(line + 4);
    } else if (std::strcmp(line, "run") == 0) {
        reply("uso: run <nome|path>");
    } else if (std::strcmp(line, "reboot") == 0) {
        reply("riavvio");
        esp_restart();
    } else {
        reply("comando sconosciuto (help)");
    }
}

// Con il driver installato legge a blocchi con timeout 20 ms. Senza driver la
// VFS del console e' non bloccante: si interroga stdin e si cede la CPU.
int readInput(char* buf, size_t size) {
    if (usb_serial_jtag_is_driver_installed()) {
        const int n = usb_serial_jtag_read_bytes(buf, size, pdMS_TO_TICKS(20));
        return n > 0 ? n : 0;
    }
    const int c = std::fgetc(stdin);
    if (c == EOF) {
        vTaskDelay(pdMS_TO_TICKS(10));
        return 0;
    }
    buf[0] = static_cast<char>(c);
    return 1;
}

void consoleTask(void*) {
    reply("console pronta: 'help' per i comandi");
    char line[kLineMax] = {};
    size_t len = 0;
    for (;;) {
        char chunk[32];
        const int got = readInput(chunk, sizeof(chunk));
        for (int i = 0; i < got; ++i) {
            const char c = chunk[i];
            if (c == '\r' || c == '\n') {
                line[len] = '\0';
                handleLine(line);
                len = 0;
            } else if (len < sizeof(line) - 1) {
                line[len++] = c;
            }
        }
    }
}

}  // namespace

void begin() {
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    xTaskCreatePinnedToCore(consoleTask, "orbit_debug", 4096, nullptr, 1,
                            nullptr, 0);
}

}  // namespace orbit_debug
