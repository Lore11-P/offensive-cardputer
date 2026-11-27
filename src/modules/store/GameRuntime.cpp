#include "GameRuntime.h"

#include "DiagConsole.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "AudioOut.h"
#include "Display.h"
#include "DiagLog.h"
#include "Logger.h"
#include "Navigation.h"
#include "Storage.h"
#include "StoreLogic.h"
#include "Ui.h"
#include "WifiSetup.h"
#include "wren.h"

namespace store {
namespace {

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
#if defined(ORBIT_DIAG_AUTOGAME) || defined(ORBIT_DIAG_BOOTGAME)
uint32_t g_diagFrames = 0;
#endif

void wrenWrite(WrenVM*, const char* text) {
    if (text != nullptr) {
        Serial.print(text);
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
    static int diagFrames = 0;
    if (diagFrames < 5 || diagFrames % 60 == 0) {
        diag::log("present %d heap=%u min=%u stack=%u", diagFrames,
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getMinFreeHeap()),
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    }
    ++diagFrames;
    display::push();
    wrenSetSlotNull(vm, 0);
}

void gameFrame(WrenVM* vm) {
#if defined(ORBIT_DIAG_AUTOGAME) || defined(ORBIT_DIAG_BOOTGAME)
    if (++g_diagFrames > 90) {
        g_exitRequested = true;
        wrenSetSlotBool(vm, 0, false);
        wrenAbortFiber(vm, 0);
        return;
    }
#endif
    navigation::update();
    const navigation::EventData ev = navigation::poll();

    g_up = navigation::pressed(';');
    g_down = navigation::pressed('.');
    g_left = navigation::pressed(',');
    g_right = navigation::pressed('/');
    g_select = ev.type == navigation::Event::Select;

    if (ev.type == navigation::Event::Back || navigation::backPressed()) {
        // Nessuna allocazione in uscita: un'eventuale stringa "exit" puo'
        // fallire proprio quando l'heap e' pieno.
        g_exitRequested = true;
        wrenSetSlotBool(vm, 0, false);
        wrenAbortFiber(vm, 0);
        return;
    }

    delay(30);
    wrenSetSlotNull(vm, 0);
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
    double freq = num(vm, 1);
    if (freq > 0.0) {
        if (freq < 20.0) {
            freq = 20.0;
        }
        if (freq > 20000.0) {
            freq = 20000.0;
        }
        double ms = num(vm, 2);
        if (ms < 1.0) {
            ms = 1.0;  // tone(_,0) in M5Unified significa repeat infinito
        }
        if (ms > 60000.0) {
            ms = 60000.0;
        }
        M5Cardputer.Speaker.tone(static_cast<float>(freq), static_cast<uint32_t>(ms));
    }
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

std::vector<std::string> errorLines() {
    std::vector<std::string> lines;
    const char* p = g_error;
    while (*p != '\0' && lines.size() < 3) {
        const char* end = strchr(p, '\n');
        size_t len = end != nullptr ? static_cast<size_t>(end - p) : strlen(p);
        if (len > 36) {
            len = 36;
        }
        std::string line;
        if (lines.empty()) {
            line += '!';
        }
        line.append(p, len);
        lines.push_back(line);
        if (end == nullptr) {
            break;
        }
        p = end + 1;
    }
    if (lines.empty()) {
        lines.push_back("!Errore sconosciuto.");
    }
    return lines;
}

// Il framebuffer viene ripristinato dal task UI dopo la morte del task Wren,
// quando lo stack da 32 KB e' gia' stato liberato.
}  // namespace

static bool runGameInner(const std::string& path) {
    WrenInterpretResult result = WREN_RESULT_RUNTIME_ERROR;
    diag::log("inner start %s heap=%u", path.c_str(),
              static_cast<unsigned>(ESP.getFreeHeap()));

    bool preferSD = storage::hasSD();
    size_t fileSize = storage::fileSize(path, preferSD);
    if (fileSize == 0 && preferSD) {
        preferSD = false;
        fileSize = storage::fileSize(path, false);
    }
    diag::log("file size=%u sd=%d", static_cast<unsigned>(fileSize), preferSD ? 1 : 0);
    if (fileSize == 0 || fileSize > store::kMaxGameBytes) {
        ui::message("Store", {"!Gioco non leggibile.", path}, "Store");
        return false;
    }

    // BLE, decoder audio e framebuffer full-screen sono gia' stati liberati da
    // runGame() prima di dare il via a questo task: qui resta il gate.
    const uint32_t freeHeap = ESP.getFreeHeap();
    const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    diag::log("gate free=%uKB blk=%uKB", freeHeap / 1024, largest / 1024);
    // Wren non ha un percorso di errore per l'OOM (una realloc NULL diventa
    // un fault): sotto questa soglia il gioco non parte. Misure sul device:
    // servono ~78 KB per la VM e ~37 KB per compilare/eseguire un gioco da
    // 5-6 KB, quindi 136 KB liberi lasciano >=15 KB di margine. Il blocco
    // contiguo minimo e' basso perche' lo stack del task e' gia' stato
    // ritagliato dal blocco piu' grande prima di questo controllo.
    if (freeHeap < 136 * 1024 || largest < 24 * 1024) {
        char detail[64];
        std::snprintf(detail, sizeof(detail), "free=%uKB blk=%uKB", freeHeap / 1024,
                      largest / 1024);
        ui::message("Store", {"!Memoria insufficiente.", detail, "Riavvia il device e riprova."},
                    "Store");
        return false;
    }

    // Il codice resta vivo solo durante l'interpretazione: la capacita' e'
    // esattamente il file + preludio, non il limite massimo di 8 KB.
    {
        diag::log("reserve start size=%u pre=%u", static_cast<unsigned>(fileSize),
                  static_cast<unsigned>(sizeof(kPrelude)));
        std::string code;
        code.reserve(fileSize + sizeof(kPrelude));
        diag::log("reserve ok");
        diag::log("read start");
        if (!storage::readText(path, code, store::kMaxGameBytes, preferSD)) {
#ifdef ORBIT_DIAG_AUTOGAME
            diag::log("read FAILED");
            logger::info("DIAG read FAILED");
            return false;
#else
            ui::message("Store", {"!Gioco non leggibile.", path}, "Store");
            return false;
#endif
        }
        diag::log("read ok %u", static_cast<unsigned>(code.size()));
        code.insert(0, kPrelude);
        diag::log("code ready %u heap=%u", static_cast<unsigned>(code.size()),
                  static_cast<unsigned>(ESP.getFreeHeap()));

        resetError();
        g_exitRequested = false;
#if defined(ORBIT_DIAG_AUTOGAME) || defined(ORBIT_DIAG_BOOTGAME)
        g_diagFrames = 0;
#endif
        g_left = false;
        g_right = false;
        g_up = false;
        g_down = false;
        g_select = false;

        logger::info("Gioco: %s, %u B, heap=%u", path.c_str(),
                     static_cast<unsigned>(fileSize),
                     static_cast<unsigned>(ESP.getFreeHeap()));

        WrenConfiguration config;
        wrenInitConfiguration(&config);
        config.initialHeapSize = 8 * 1024;
        config.minHeapSize = 4 * 1024;
        config.heapGrowthPercent = 10;
        config.writeFn = wrenWrite;
        config.errorFn = wrenError;
        config.bindForeignMethodFn = bindForeignMethod;

        diag::log("vm create heap=%u blk=%u", static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
        WrenVM* vm = wrenNewVM(&config);
        diag::log("vm=%p heap=%u", static_cast<void*>(vm),
                  static_cast<unsigned>(ESP.getFreeHeap()));
        if (vm == nullptr) {
            ui::message("Store", {"!Wren VM non disponibile."}, "Store");
            return false;
        }
        logger::info("Wren VM: heap=%u", static_cast<unsigned>(ESP.getFreeHeap()));
        diag::log("interpret start heap=%u", static_cast<unsigned>(ESP.getFreeHeap()));
        result = wrenInterpret(vm, "main", code.c_str());
        diag::log("interpret end result=%d heap=%u", static_cast<int>(result),
                  static_cast<unsigned>(ESP.getFreeHeap()));
        wrenFreeVM(vm);
        logger::info("Gioco finito: result=%d heap=%u", static_cast<int>(result),
                     static_cast<unsigned>(ESP.getFreeHeap()));
    }

    display::clear();
    navigation::waitRelease();

    if (g_exitRequested) {
        return true;
    }
    if (result != WREN_RESULT_SUCCESS) {
        ui::message("Errore gioco", errorLines(), "Store");
        return false;
    }
    return true;
}

namespace {

SemaphoreHandle_t g_gameDone = nullptr;
SemaphoreHandle_t g_gameGo = nullptr;

struct GameJob {
    const std::string* path = nullptr;
    bool ok = false;
};

void gameTask(void* arg) {
    GameJob* job = static_cast<GameJob*>(arg);
    // Aspetta che il task della UI abbia liberato framebuffer e decoder.
    xSemaphoreTake(g_gameGo, portMAX_DELAY);
    job->ok = runGameInner(*job->path);
    diag::log("task stack_free=%u heap=%u min=%u",
              static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
              static_cast<unsigned>(ESP.getFreeHeap()),
              static_cast<unsigned>(ESP.getMinFreeHeap()));
    xSemaphoreGive(g_gameDone);
    vTaskDelete(nullptr);
}

}  // namespace

bool runGame(const std::string& path) {
    if (g_gameDone != nullptr) {
        // Un gioco alla volta: la UI e' bloccante.
        return false;
    }
    // Il WiFi/TLS dello Store non serve al gioco: spegnerlo dal task della UI
    // (non dal task Wren) prima di allocare stack e VM.
    wifisetup::disconnect();
    delay(50);

    // La console diagnostica tiene uno stack dedicato: via prima del gate, cosi'
    // il gioco vede esattamente la memoria del firmware di produzione.
    diagconsole::pause();

    // Semafori e task vanno creati PRIMA di liberare il framebuffer: una
    // allocazione anche di poche decine di byte dentro il suo blocco da 64 KB
    // lo spezza, e a fine partita il framebuffer non e' piu' riallocabile
    // (heap libero ma nessun blocco contiguo abbastanza grande).
    g_gameDone = xSemaphoreCreateBinary();
    g_gameGo = xSemaphoreCreateBinary();
    GameJob job;
    job.path = &path;
    // Stack dedicato: il compilatore Wren e' ricorsivo (una struct Compiler da
    // ~2 KB per livello: funzione, metodo, blocco) e i 16 KB del loopTask non
    // bastano. Misurato sul device: ~12 KB nel caso peggiore. Il task viene
    // distrutto a fine partita.
    const bool ready = g_gameDone != nullptr && g_gameGo != nullptr &&
                       xTaskCreatePinnedToCore(gameTask, "wren-game", 32 * 1024, &job,
                                               1, nullptr, 1) == pdPASS;
    if (!ready) {
        if (g_gameDone != nullptr) {
            vSemaphoreDelete(g_gameDone);
            g_gameDone = nullptr;
        }
        if (g_gameGo != nullptr) {
            vSemaphoreDelete(g_gameGo);
            g_gameGo = nullptr;
        }
        ui::message("Store", {"!Memoria insufficiente per il gioco."}, "Store");
        diagconsole::resume();
        return false;
    }

    // Libera tutto ciò che non serve al gioco: BLE, decoder audio e
    // framebuffer full-screen.
    if (NimBLEDevice::getInitialized()) {
        NimBLEDevice::deinit(true);
        diag::log("BLE deinit heap=%u", static_cast<unsigned>(ESP.getFreeHeap()));
    }
    audioout::releaseDecoder();
    const bool framebufferReleased = display::releaseFramebuffer();
    diag::log("framebuffer release=%d heap=%u blk8=%u blkDMA=%u",
              framebufferReleased ? 1 : 0, static_cast<unsigned>(ESP.getFreeHeap()),
              static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
              static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
    xSemaphoreGive(g_gameGo);

    xSemaphoreTake(g_gameDone, portMAX_DELAY);
    // Lascia al task il tempo di eseguire vTaskDelete e liberare lo stack da
    // 32 KB; solo allora il framebuffer da 65 KB puo' essere riallocato.
    vTaskDelay(pdMS_TO_TICKS(10));
    diag::log("restore pre free=%u blk8=%u blkDMA=%u",
              static_cast<unsigned>(ESP.getFreeHeap()),
              static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
              static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
    const bool restored = display::restoreFramebuffer();
    diag::log("framebuffer restore=%d heap=%u", restored ? 1 : 0,
              static_cast<unsigned>(ESP.getFreeHeap()));
    diag::log("task done ok=%d heap=%u", job.ok ? 1 : 0,
              static_cast<unsigned>(ESP.getFreeHeap()));
    vSemaphoreDelete(g_gameDone);
    g_gameDone = nullptr;
    vSemaphoreDelete(g_gameGo);
    g_gameGo = nullptr;
    diagconsole::resume();
    return job.ok;
}

}  // namespace store
