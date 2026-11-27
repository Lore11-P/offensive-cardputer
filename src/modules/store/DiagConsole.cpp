#include "modules/store/DiagConsole.h"

#if ORBIT_DIAG_CONSOLE

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstring>
#include <string>
#include <vector>

#include "Display.h"
#include "Storage.h"
#include "modules/store/GameRuntime.h"

namespace diagconsole {
namespace {

constexpr size_t kLineMax = 96;
char g_line[kLineMax];
size_t g_lineLen = 0;
volatile bool g_busy = false;
volatile bool g_stop = false;
SemaphoreHandle_t g_gone = nullptr;
TaskHandle_t g_task = nullptr;
std::string g_runPath;

void heapLine(const char* tag) {
    Serial.printf("[DIAG] %s free=%u min=%u blk8=%u blkDMA=%u\n", tag,
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getMinFreeHeap()),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
    Serial.flush();
}

void fbLine() {
    Serial.printf("[DIAG] fb active=%d bpp=%u bytes=%u fallback=%d\n",
                  display::framebufferActive() ? 1 : 0,
                  static_cast<unsigned>(display::framebufferBpp()),
                  static_cast<unsigned>(display::framebufferBytes()),
                  display::framebufferFallback() ? 1 : 0);
    Serial.flush();
}

void runTask(void*) {
    const bool ok = store::runGame(g_runPath);
    Serial.printf("[DIAG] run done ok=%d\n", ok ? 1 : 0);
    Serial.flush();
    heapLine("run post");
    g_busy = false;
    vTaskDelete(nullptr);
}

void startRun(const char* arg) {
    if (g_busy) {
        Serial.println("[DIAG] gia' in esecuzione (ESC sul device per uscire)");
        return;
    }
    if (arg == nullptr || *arg == 0) {
        Serial.println("[DIAG] uso: run <nome|/games/x.wren>");
        return;
    }
    std::string path(arg);
    if (path[0] != '/') {
        path = "/games/" + path;
        if (path.size() < 6 || path.compare(path.size() - 6, 6, ".wren") != 0) {
            path += ".wren";
        }
    }
    g_runPath = path;
    g_busy = true;
    Serial.printf("[DIAG] run %s\n", path.c_str());
    heapLine("run pre");
    if (xTaskCreatePinnedToCore(runTask, "diag-run", 4 * 1024, nullptr, 1, nullptr, 1) !=
        pdPASS) {
        g_busy = false;
        Serial.println("[DIAG] task non creato");
    }
}

void taskLine(const char* name) {
    TaskHandle_t handle = xTaskGetHandle(name);
    if (handle == nullptr) {
        Serial.printf("[DIAG]   %-14s -\n", name);
        return;
    }
    Serial.printf("[DIAG]   %-14s stack_free=%u\n", name,
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(handle)));
    Serial.flush();
}

void tasks() {
    // uxTaskGetSystemState non e' compilato in questa configurazione FreeRTOS:
    // si interrogano i task noti per nome.
    Serial.printf("[DIAG] tasks=%u\n", static_cast<unsigned>(uxTaskGetNumberOfTasks()));
    Serial.flush();
    taskLine("loopTask");
    taskLine("wren-game");
    taskLine("diag-console");
    taskLine("diag-run");
}

void handle(char* line) {
    char* arg = std::strchr(line, ' ');
    if (arg != nullptr) {
        *arg++ = 0;
        while (*arg == ' ') {
            ++arg;
        }
    }

    if (std::strcmp(line, "help") == 0) {
        Serial.println("[DIAG] heap | fb [rel|res] | games | run <nome> | tasks | wifi | reset");
    } else if (std::strcmp(line, "heap") == 0 || std::strcmp(line, "mem") == 0) {
        heapLine("heap");
        fbLine();
    } else if (std::strcmp(line, "fb") == 0) {
        if (arg != nullptr && std::strcmp(arg, "rel") == 0) {
            const bool ok = display::releaseFramebuffer();
            Serial.printf("[DIAG] fb release=%d\n", ok ? 1 : 0);
            heapLine("fb rel");
        } else if (arg != nullptr && std::strcmp(arg, "res") == 0) {
            const bool ok = display::restoreFramebuffer();
            Serial.printf("[DIAG] fb restore=%d\n", ok ? 1 : 0);
            fbLine();
        } else {
            fbLine();
        }
    } else if (std::strcmp(line, "games") == 0) {
        const std::vector<std::string> files = storage::list("/games", false, true);
        Serial.printf("[DIAG] games=%u\n", static_cast<unsigned>(files.size()));
        for (const std::string& file : files) {
            const std::string path = "/games/" + file;
            Serial.printf("[DIAG]   %s %u\n", file.c_str(),
                          static_cast<unsigned>(storage::fileSize(path, true)));
            Serial.flush();
        }
    } else if (std::strcmp(line, "run") == 0) {
        startRun(arg);
    } else if (std::strcmp(line, "tasks") == 0) {
        tasks();
    } else if (std::strcmp(line, "wifi") == 0) {
        Serial.printf("[DIAG] wifi status=%d ip=%s\n", static_cast<int>(WiFi.status()),
                      WiFi.localIP().toString().c_str());
        heapLine("wifi");
    } else if (std::strcmp(line, "reset") == 0) {
        Serial.println("[DIAG] reset");
        Serial.flush();
        delay(50);
        esp_restart();
    } else if (*line != 0) {
        Serial.printf("[DIAG] sconosciuto: %s (help)\n", line);
    }
    // La CDC perde caratteri se si accoda piu' di una riga senza drenare.
    Serial.flush();
}

void consoleTask(void*) {
    for (;;) {
        if (g_stop) {
            xSemaphoreGive(g_gone);
            vTaskDelete(nullptr);
        }
        while (Serial.available() > 0) {
            const char c = static_cast<char>(Serial.read());
            if (c == '\r' || c == '\n') {
                if (g_lineLen > 0) {
                    g_line[g_lineLen] = 0;
                    handle(g_line);
                    g_lineLen = 0;
                }
            } else if (g_lineLen + 1 < sizeof(g_line)) {
                g_line[g_lineLen++] = c;
            } else {
                g_lineLen = 0;  // riga troppo lunga: scartata
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

}  // namespace

void begin() {
    if (g_gone == nullptr) {
        g_gone = xSemaphoreCreateBinary();
    }
    Serial.println("[DIAG] console pronta: help");
    Serial.flush();
    if (xTaskCreatePinnedToCore(consoleTask, "diag-console", 4 * 1024, nullptr, 1, &g_task,
                                0) != pdPASS) {
        g_task = nullptr;
    }
}

void pause() {
    if (g_task == nullptr || g_gone == nullptr) {
        return;
    }
    g_stop = true;
    if (xSemaphoreTake(g_gone, pdMS_TO_TICKS(500)) == pdTRUE) {
        // Il task si e' cancellato: lascia all'idle il tempo di liberare lo stack.
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    g_task = nullptr;
    g_stop = false;
}

void resume() {
    if (g_task != nullptr) {
        return;
    }
    if (xTaskCreatePinnedToCore(consoleTask, "diag-console", 4 * 1024, nullptr, 1, &g_task,
                                0) != pdPASS) {
        g_task = nullptr;
    }
}

}  // namespace diagconsole

#endif  // ORBIT_DIAG_CONSOLE
