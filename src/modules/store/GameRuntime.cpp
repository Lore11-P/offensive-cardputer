#include "GameRuntime.h"

#include <Arduino.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Display.h"
#include "Navigation.h"
#include "Storage.h"
#include "StoreLogic.h"
#include "Ui.h"
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

    if (ev.type == navigation::Event::Back || navigation::backPressed()) {
        wrenSetSlotString(vm, 0, "exit");
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
        if (ms < 0.0) {
            ms = 0.0;
        }
        if (ms > 60000.0) {
            ms = 60000.0;
        }
        M5Cardputer.Speaker.tone(static_cast<float>(freq), static_cast<uint32_t>(ms));
    }
    wrenSetSlotNull(vm, 0);
}

void gameExit(WrenVM* vm) {
    wrenSetSlotString(vm, 0, "exit");
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

}  // namespace

bool runGame(const std::string& path) {
    WrenInterpretResult result = WREN_RESULT_RUNTIME_ERROR;
    // Il codice resta vivo solo durante l'interpretazione: il buffer da 64 KB
    // non deve occupare RAM mentre la schermata d'errore aspetta un tasto.
    {
        std::string code;
        code.reserve(store::kMaxGameBytes + sizeof(kPrelude));
        if (!storage::readText(path, code, store::kMaxGameBytes, true)) {
            ui::message("Store", {"!Gioco non leggibile.", path}, "Store");
            return false;
        }
        code.insert(0, kPrelude);

        resetError();
        g_left = false;
        g_right = false;
        g_up = false;
        g_down = false;
        g_select = false;

        WrenConfiguration config;
        wrenInitConfiguration(&config);
        config.initialHeapSize = 32 * 1024;
        config.minHeapSize = 8 * 1024;
        config.writeFn = wrenWrite;
        config.errorFn = wrenError;
        config.bindForeignMethodFn = bindForeignMethod;

        WrenVM* vm = wrenNewVM(&config);
        if (vm == nullptr) {
            ui::message("Store", {"!Wren VM non disponibile."}, "Store");
            return false;
        }
        result = wrenInterpret(vm, "main", code.c_str());
        wrenFreeVM(vm);
    }

    display::clear();
    navigation::waitRelease();

    if (strcmp(g_error, "exit") == 0) {
        return true;
    }
    if (result != WREN_RESULT_SUCCESS) {
        ui::message("Errore gioco", errorLines(), "Store");
        return false;
    }
    return true;
}

}  // namespace store
