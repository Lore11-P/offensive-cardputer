#include "orbit_wren/GameRuntime.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "Display.h"
#include "Navigation.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "orbit_audio/Audio.h"
#include "orbit_compat.h"
#include "orbit_store/Store.h"
#include "wren.h"

namespace orbit_wren {
namespace {

const char* TAG = "orbit_wren";

// Stesso limite dello Store: oltre non e' un gioco.
constexpr size_t kMaxGameBytes = 8192;

// Preludio fisso: l'unica superficie che un gioco puo' usare.
const char kPrelude[] =
    "class Game {\n"
    "  foreign static width\n"
    "  foreign static height\n"
    "  foreign static clear(c)\n"
    "  foreign static rect(x, y, w, h, c)\n"
    "  foreign static circle(x, y, r, c)\n"
    "  foreign static line(x1, y1, x2, y2, c)\n"
    "  foreign static text(x, y, s, c, size)\n"
    "  foreign static present()\n"
    "  foreign static frame()\n"
    "  foreign static left\n"
    "  foreign static right\n"
    "  foreign static up\n"
    "  foreign static down\n"
    "  foreign static select\n"
    "  foreign static millis\n"
    "  foreign static random(n)\n"
    "  foreign static rgb(r, g, b)\n"
    "  foreign static beep(freq, ms)\n"
    "  foreign static exit()\n"
    "}\n";

char g_error[193];
size_t g_errorLen = 0;

bool g_left = false;
bool g_right = false;
bool g_up = false;
bool g_down = false;
bool g_select = false;
bool g_exitRequested = false;

void wrenWrite(WrenVM*, const char* text) {
    if (text != nullptr) {
        ESP_LOGD(TAG, "%s", text);
    }
}

void wrenError(WrenVM*, WrenErrorType type, const char*, int, const char* message) {
    if (type == WREN_ERROR_STACK_TRACE || message == nullptr) {
        return;
    }
    const size_t cap = sizeof(g_error) - 1;
    if (g_errorLen > 0 && g_errorLen < cap) {
        g_error[g_errorLen++] = '\n';
    }
    size_t len = strlen(message);
    const size_t room = cap - g_errorLen;
    if (len > room) {
        len = room;
    }
    if (len > 0) {
        memcpy(g_error + g_errorLen, message, len);
        g_errorLen += len;
        g_error[g_errorLen] = '\0';
    }
}

void resetError() {
    g_error[0] = '\0';
    g_errorLen = 0;
}

// Le firme Wren non tipizzano gli argomenti: valida prima di leggerli.
double num(WrenVM* vm, int slot) {
    return wrenGetSlotType(vm, slot) == WREN_TYPE_NUM ? wrenGetSlotDouble(vm, slot) : 0.0;
}

int coord(WrenVM* vm, int slot) {
    const double v = num(vm, slot);
    if (!(v > -100000.0)) {
        return -100000;
    }
    if (v > 100000.0) {
        return 100000;
    }
    return static_cast<int>(v);
}

uint16_t color(WrenVM* vm, int slot) {
    const double v = num(vm, slot);
    if (!(v > 0.0)) {
        return 0;
    }
    return v > 65535.0 ? 0xFFFF : static_cast<uint16_t>(v);
}

uint8_t channel(WrenVM* vm, int slot) {
    const double v = num(vm, slot);
    if (!(v > 0.0)) {
        return 0;
    }
    return v > 255.0 ? 255 : static_cast<uint8_t>(v);
}

// Stessa formula di M5GFX: RGB565 nativo del pannello.
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void gameWidth(WrenVM* vm) { wrenSetSlotDouble(vm, 0, display::width()); }
void gameHeight(WrenVM* vm) { wrenSetSlotDouble(vm, 0, display::height()); }

void gameLeft(WrenVM* vm) { wrenSetSlotBool(vm, 0, g_left); }
void gameRight(WrenVM* vm) { wrenSetSlotBool(vm, 0, g_right); }
void gameUp(WrenVM* vm) { wrenSetSlotBool(vm, 0, g_up); }
void gameDown(WrenVM* vm) { wrenSetSlotBool(vm, 0, g_down); }

void gameSelect(WrenVM* vm) {
    wrenSetSlotBool(vm, 0, g_select);
    g_select = false;
}

void gameMillis(WrenVM* vm) { wrenSetSlotDouble(vm, 0, static_cast<double>(millis())); }

void gameClear(WrenVM* vm) {
    display::clear(color(vm, 1));
    wrenSetSlotNull(vm, 0);
}

void gameRect(WrenVM* vm) {
    int w = coord(vm, 3);
    int h = coord(vm, 4);
    if (w < 0) {
        w = 0;
    }
    if (h < 0) {
        h = 0;
    }
    display::g().fillRect(coord(vm, 1), coord(vm, 2), w, h, color(vm, 5));
    wrenSetSlotNull(vm, 0);
}

void gameCircle(WrenVM* vm) {
    int r = coord(vm, 3);
    if (r < 0) {
        r = 0;
    }
    display::g().fillCircle(coord(vm, 1), coord(vm, 2), r, color(vm, 4));
    wrenSetSlotNull(vm, 0);
}

void gameLine(WrenVM* vm) {
    display::g().drawLine(coord(vm, 1), coord(vm, 2), coord(vm, 3), coord(vm, 4),
                          color(vm, 5));
    wrenSetSlotNull(vm, 0);
}

void gameText(WrenVM* vm) {
    if (wrenGetSlotType(vm, 3) == WREN_TYPE_STRING) {
        const int size = coord(vm, 5);
        const uint8_t font = (size == 0 || size == 4) ? static_cast<uint8_t>(size) : 2;
        display::text(coord(vm, 1), coord(vm, 2), wrenGetSlotString(vm, 3), color(vm, 4),
                      font);
    }
    wrenSetSlotNull(vm, 0);
}

void gamePresent(WrenVM* vm) {
    display::push();
    wrenSetSlotNull(vm, 0);
}

void gameFrame(WrenVM* vm) {
    navigation::update();
    const navigation::EventData ev = navigation::poll();

    g_up = navigation::pressed(';');
    g_down = navigation::pressed('.');
    g_left = navigation::pressed(',');
    g_right = navigation::pressed('/');
    g_select = ev.type == navigation::Event::Select;

    if (ev.type == navigation::Event::Back || navigation::backPressed() || g_exitRequested) {
        // I giochi girano in un ciclo infinito: senza abort il ciclo non
        // uscirebbe mai. Nessuna allocazione qui: l'heap puo' essere pieno.
        g_exitRequested = true;
        wrenSetSlotBool(vm, 0, false);
        wrenAbortFiber(vm, 0);
        return;
    }

    delay(30);
    wrenSetSlotBool(vm, 0, true);
}

void gameRandom(WrenVM* vm) {
    const double n = num(vm, 1);
    uint32_t mod = 0;
    if (n > 0.0) {
        mod = n > 4294967295.0 ? 0xFFFFFFFFu : static_cast<uint32_t>(n);
    }
    wrenSetSlotDouble(vm, 0, mod > 0 ? static_cast<double>(esp_random() % mod) : 0.0);
}

void gameRgb(WrenVM* vm) {
    wrenSetSlotDouble(vm, 0, rgb565(channel(vm, 1), channel(vm, 2), channel(vm, 3)));
}

void gameBeep(WrenVM* vm) {
    // Il codec potrebbe non essere inizializzato: beep() e' un no-op sicuro.
    const int hz = static_cast<int>(num(vm, 1));
    const int ms = static_cast<int>(num(vm, 2));
    orbit_audio::beep(static_cast<uint16_t>(hz > 0 ? hz : 440),
                      static_cast<uint16_t>(ms > 0 ? ms : 40));
    wrenSetSlotNull(vm, 0);
}

void gameExit(WrenVM* vm) {
    g_exitRequested = true;
    wrenSetSlotBool(vm, 0, false);
    wrenAbortFiber(vm, 0);
}

struct Binding {
    const char* signature;
    WrenForeignMethodFn fn;
};

const Binding kBindings[] = {
    {"width", gameWidth},         {"height", gameHeight},     {"left", gameLeft},
    {"right", gameRight},         {"up", gameUp},             {"down", gameDown},
    {"select", gameSelect},       {"millis", gameMillis},     {"clear(_)", gameClear},
    {"rect(_,_,_,_,_)", gameRect},  {"circle(_,_,_,_)", gameCircle},
    {"line(_,_,_,_,_)", gameLine},  {"text(_,_,_,_,_)", gameText},
    {"present()", gamePresent},   {"frame()", gameFrame},     {"random(_)", gameRandom},
    {"rgb(_,_,_)", gameRgb},      {"beep(_,_)", gameBeep},    {"exit()", gameExit},
};

WrenForeignMethodFn bindForeignMethod(WrenVM*, const char* module, const char* className,
                                      bool isStatic, const char* signature) {
    if (!isStatic || strcmp(module, "main") != 0 || strcmp(className, "Game") != 0) {
        return nullptr;
    }
    for (const Binding& binding : kBindings) {
        if (strcmp(binding.signature, signature) == 0) {
            return binding.fn;
        }
    }
    return nullptr;
}

// Legge il gioco con fopen/fread: il path e' gia' VFS (es. "/sd/...").
bool readGameText(const std::string& path, std::string* out) {
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        ESP_LOGE(TAG, "fopen %s: %s", path.c_str(), strerror(errno));
        return false;
    }

    bool ok = false;
    const long size = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1;
    if (size <= 0 || static_cast<size_t>(size) > kMaxGameBytes) {
        ESP_LOGE(TAG, "gioco non leggibile: %ld B (max %u)", size,
                 static_cast<unsigned>(kMaxGameBytes));
    } else if (fseek(file, 0, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "rewind %s fallita", path.c_str());
    } else {
        out->assign(static_cast<size_t>(size), '\0');
        const size_t got = fread(&(*out)[0], 1, out->size(), file);
        if (got != out->size()) {
            ESP_LOGE(TAG, "lettura %s incompleta: %u/%u", path.c_str(),
                     static_cast<unsigned>(got), static_cast<unsigned>(out->size()));
        } else {
            ok = true;
        }
    }
    fclose(file);
    return ok;
}

bool runGameInner(const std::string& path) {
    ESP_LOGI(TAG, "start %s heap=%u", path.c_str(),
             static_cast<unsigned>(esp_get_free_heap_size()));

    // Wren non ha un percorso di errore per l'OOM: sotto questa soglia non parte.
    const uint32_t freeHeap = esp_get_free_heap_size();
    if (freeHeap < 120 * 1024) {
        ESP_LOGE(TAG, "memoria insufficiente: free=%uKB, servono 120KB",
                 static_cast<unsigned>(freeHeap / 1024));
        return false;
    }

    std::string code;
    if (!readGameText(path, &code)) {
        return false;
    }
    code.reserve(code.size() + sizeof(kPrelude));
    code.insert(0, kPrelude);

    resetError();
    g_exitRequested = false;
    g_left = false;
    g_right = false;
    g_up = false;
    g_down = false;
    g_select = false;

    WrenConfiguration config;
    wrenInitConfiguration(&config);
    config.initialHeapSize = 8 * 1024;
    config.minHeapSize = 4 * 1024;
    config.heapGrowthPercent = 10;
    config.writeFn = wrenWrite;
    config.errorFn = wrenError;
    config.bindForeignMethodFn = bindForeignMethod;

    WrenVM* vm = wrenNewVM(&config);
    if (vm == nullptr) {
        ESP_LOGE(TAG, "Wren VM non disponibile");
        return false;
    }

    const WrenInterpretResult result = wrenInterpret(vm, "main", code.c_str());
    wrenFreeVM(vm);
    ESP_LOGI(TAG, "fine: result=%d heap=%u stack=%u", static_cast<int>(result),
             static_cast<unsigned>(esp_get_free_heap_size()),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

    display::clear();
    navigation::waitRelease();

    if (g_exitRequested) {
        return true;
    }
    if (result != WREN_RESULT_SUCCESS) {
        ESP_LOGE(TAG, "errore gioco: %s", g_error[0] != '\0' ? g_error : "sconosciuto");
        return false;
    }
    return true;
}

SemaphoreHandle_t g_gameDone = nullptr;

struct GameJob {
    const std::string* path = nullptr;
    bool ok = false;
};

void gameTask(void* arg) {
    GameJob* job = static_cast<GameJob*>(arg);
    job->ok = runGameInner(*job->path);
    xSemaphoreGive(g_gameDone);
    vTaskDelete(nullptr);
}

}  // namespace

bool runGame(const std::string& path) {
    if (g_gameDone != nullptr) {
        // Un gioco alla volta: la UI e' bloccante.
        return false;
    }

    // Wren costa ~115 KB: senza liberare framebuffer (64 KB) e radio il gate
    // da 120 KB non passa e i giochi dello Store non si aprono.
    const bool hadFramebuffer = display::framebufferActive();
    if (hadFramebuffer) {
        display::releaseFramebuffer();
    }
    orbit_store::wifiShutdownDriver();

    g_gameDone = xSemaphoreCreateBinary();
    if (g_gameDone == nullptr) {
        if (hadFramebuffer) {
            display::restoreFramebuffer();
        }
        ESP_LOGE(TAG, "semaphore non creato");
        return false;
    }

    GameJob job;
    job.path = &path;
    // Stack dedicato: il compilatore Wren e' ricorsivo e i 16 KB del task UI
    // non bastano. Il task muore a fine partita e lo stack torna all'idle.
    if (xTaskCreatePinnedToCore(gameTask, "wren-game", 32 * 1024, &job, 1, nullptr, 1) !=
        pdPASS) {
        vSemaphoreDelete(g_gameDone);
        g_gameDone = nullptr;
        if (hadFramebuffer) {
            display::restoreFramebuffer();
        }
        ESP_LOGE(TAG, "task wren-game non creato");
        return false;
    }

    xSemaphoreTake(g_gameDone, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(10));
    vSemaphoreDelete(g_gameDone);
    g_gameDone = nullptr;
    if (hadFramebuffer) {
        display::restoreFramebuffer();
    }
    return job.ok;
}

}  // namespace orbit_wren
