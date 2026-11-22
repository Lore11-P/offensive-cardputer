#include "BleHidScript.h"

#include "ModuleFlags.h"

#if ENABLE_MODULE_BLEHID

#include <Arduino.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

#include "BleHidKeyboard.h"
#include "Display.h"
#include "Logger.h"
#include "Navigation.h"
#include "PowerTimer.h"
#include "Storage.h"

namespace blehid {
namespace {

constexpr size_t kMaxLineBytes = 256;
constexpr uint32_t kMaxLines = 5000;
constexpr uint32_t kMaxRepeat = 1000;
constexpr uint32_t kKeyHoldMs = 6;
constexpr uint32_t kPollSliceMs = 10;
constexpr uint32_t kDrawIntervalMs = 90;

// Modificatori nel formato del report HID.
constexpr uint8_t kModCtrl = 0x01;
constexpr uint8_t kModShift = 0x02;
constexpr uint8_t kModAlt = 0x04;
constexpr uint8_t kModGui = 0x08;

struct Line {
    char data[kMaxLineBytes + 1] = {};
    size_t length = 0;
    bool truncated = false;
};

bool readLine(File& file, Line& out) {
    out.length = 0;
    out.truncated = false;
    bool any = false;
    for (;;) {
        const int c = file.read();
        if (c < 0) {
            break;
        }
        any = true;
        if (c == '\n') {
            break;
        }
        if (c == '\r') {
            continue;
        }
        if (out.length < kMaxLineBytes) {
            out.data[out.length++] = static_cast<char>(c);
        } else {
            out.truncated = true;
        }
    }
    out.data[out.length] = '\0';
    return any;
}

enum class ActionType { None, Delay, DefaultDelay, Text, Key, Chord, Repeat };

struct Action {
    ActionType type = ActionType::None;
    uint32_t arg = 0;
    std::string text;
    uint8_t key = 0;  // ASCII oppure blehid::Special
    uint8_t mods = 0;

    void reset() {
        type = ActionType::None;
        arg = 0;
        text.clear();
        key = 0;
        mods = 0;
    }
};

struct RunContext {
    uint32_t defaultDelayMs = 0;
    uint32_t dropsAtStart = 0;
    bool aborted = false;
    bool lost = false;
    bool flow = false;
};

bool isSpace(char c) {
    return c == ' ' || c == '\t';
}

std::string trim(const char* data, size_t length) {
    size_t begin = 0;
    size_t end = length;
    while (begin < end && isSpace(data[begin])) {
        ++begin;
    }
    while (end > begin && isSpace(data[end - 1])) {
        --end;
    }
    return std::string(data + begin, end - begin);
}

std::string upper(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return value;
}

std::string lower(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

bool startsWith(const std::string& text, const char* prefix) {
    const size_t length = std::strlen(prefix);
    return text.size() >= length && text.compare(0, length, prefix) == 0;
}

void skipSpaces(const std::string& text, size_t& pos) {
    while (pos < text.size() && isSpace(text[pos])) {
        ++pos;
    }
}

std::string nextToken(const std::string& text, size_t& pos) {
    skipSpaces(text, pos);
    const size_t start = pos;
    while (pos < text.size() && !isSpace(text[pos])) {
        ++pos;
    }
    return text.substr(start, pos - start);
}

bool restIsSpaces(const std::string& text, size_t pos) {
    while (pos < text.size()) {
        if (!isSpace(text[pos])) {
            return false;
        }
        ++pos;
    }
    return true;
}

bool parseU32(const std::string& text, size_t pos, uint32_t& out) {
    skipSpaces(text, pos);
    if (pos >= text.size() || text[pos] < '0' || text[pos] > '9') {
        return false;
    }
    uint64_t value = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        value = value * 10 + static_cast<uint64_t>(text[pos] - '0');
        if (value > 0xFFFFFFFFULL) {
            return false;
        }
        ++pos;
    }
    if (!restIsSpaces(text, pos)) {
        return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
}

bool isMetadata(const std::string& lowerLine) {
    return startsWith(lowerLine, "id:") || startsWith(lowerLine, "name:") ||
           startsWith(lowerLine, "author:") || startsWith(lowerLine, "version:") ||
           startsWith(lowerLine, "description:");
}

uint8_t keyCode(const std::string& upperToken) {
    if (upperToken == "ENTER" || upperToken == "RETURN") return kEnter;
    if (upperToken == "TAB") return kTab;
    if (upperToken == "ESC" || upperToken == "ESCAPE") return kEsc;
    if (upperToken == "SPACE") return ' ';
    if (upperToken == "BACKSPACE") return kBackspace;
    if (upperToken == "DELETE" || upperToken == "DEL") return kDelete;
    if (upperToken == "CAPSLOCK" || upperToken == "CAPS_LOCK") return kCapsLock;
    if (upperToken == "INSERT") return kInsert;
    if (upperToken == "HOME") return kHome;
    if (upperToken == "END") return kEnd;
    if (upperToken == "PAGEUP" || upperToken == "PAGE_UP" || upperToken == "PGUP") return kPageUp;
    if (upperToken == "PAGEDOWN" || upperToken == "PAGE_DOWN" || upperToken == "PGDN") return kPageDown;
    if (upperToken == "UP" || upperToken == "ARROW_UP") return kUp;
    if (upperToken == "DOWN" || upperToken == "ARROW_DOWN") return kDown;
    if (upperToken == "LEFT" || upperToken == "ARROW_LEFT") return kLeft;
    if (upperToken == "RIGHT" || upperToken == "ARROW_RIGHT") return kRight;
    if (upperToken == "F1") return kF1;
    if (upperToken == "F2") return kF2;
    if (upperToken == "F3") return kF3;
    if (upperToken == "F4") return kF4;
    if (upperToken == "F5") return kF5;
    if (upperToken == "F6") return kF6;
    if (upperToken == "F7") return kF7;
    if (upperToken == "F8") return kF8;
    if (upperToken == "F9") return kF9;
    if (upperToken == "F10") return kF10;
    if (upperToken == "F11") return kF11;
    if (upperToken == "F12") return kF12;
    return 0;
}

uint8_t modifierBit(const std::string& upperToken) {
    if (upperToken == "CTRL" || upperToken == "CONTROL") return kModCtrl;
    if (upperToken == "SHIFT") return kModShift;
    if (upperToken == "ALT") return kModAlt;
    if (upperToken == "GUI" || upperToken == "WINDOWS" || upperToken == "WIN") return kModGui;
    return 0;
}

bool pollAbort(RunContext& ctx) {
    if (ctx.aborted) {
        return true;
    }
    if (!hid::connected()) {
        ctx.lost = true;
        ctx.aborted = true;
        return true;
    }
    if (hid::reportDrops() != ctx.dropsAtStart) {
        // La coda host non regge: fermarsi e dirlo, non fingere l'invio.
        ctx.flow = true;
        ctx.aborted = true;
        return true;
    }
    const navigation::EventData event = navigation::poll();
    if (event.type == navigation::Event::Back) {
        ctx.aborted = true;
    }
    return ctx.aborted;
}

bool waitPollable(uint32_t ms, RunContext& ctx) {
    while (ms > 0) {
        if (pollAbort(ctx)) {
            return false;
        }
        const uint32_t slice = ms < kPollSliceMs ? ms : kPollSliceMs;
        delay(slice);
        ms -= slice;
    }
    return !pollAbort(ctx);
}

// Testo con escape \n \t \\ e UTF-8: ogni carattere e' interrompibile.
uint32_t typeText(const std::string& text, RunContext& ctx) {
    uint32_t sent = 0;
    for (size_t i = 0; i < text.size();) {
        if (pollAbort(ctx)) {
            return sent;
        }
        const uint8_t c = static_cast<uint8_t>(text[i]);
        uint32_t codepoint = c;
        size_t length = 1;
        if (c == '\\' && i + 1 < text.size()) {
            const char next = text[i + 1];
            if (next == 'n' || next == 'N') {
                codepoint = '\n';
                length = 2;
            } else if (next == 't' || next == 'T') {
                codepoint = '\t';
                length = 2;
            } else if (next == '\\') {
                codepoint = '\\';
                length = 2;
            }
        } else if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
            codepoint = ((c & 0x1FU) << 6) | (static_cast<uint8_t>(text[i + 1]) & 0x3FU);
            length = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
            codepoint = ((c & 0x0FU) << 12) |
                        ((static_cast<uint8_t>(text[i + 1]) & 0x3FU) << 6) |
                        (static_cast<uint8_t>(text[i + 2]) & 0x3FU);
            length = 3;
        }
        i += length;
        if (hid::tapCodepoint(codepoint)) {
            ++sent;
        }
        if (ctx.defaultDelayMs > 0 && !waitPollable(ctx.defaultDelayMs, ctx)) {
            return sent;
        }
    }
    return sent;
}

bool executeAction(const Action& action, RunContext& ctx) {
    switch (action.type) {
        case ActionType::None:
            return !ctx.aborted;
        case ActionType::DefaultDelay:
            ctx.defaultDelayMs = action.arg;
            return true;
        case ActionType::Delay:
            return waitPollable(action.arg, ctx);
        case ActionType::Text:
            typeText(action.text, ctx);
            return !ctx.aborted;
        case ActionType::Key:
            if (pollAbort(ctx)) {
                return false;
            }
            hid::tapScriptKey(action.key, 0);
            if (ctx.defaultDelayMs > 0 && !waitPollable(ctx.defaultDelayMs, ctx)) {
                return false;
            }
            return true;
        case ActionType::Chord:
            if (pollAbort(ctx)) {
                return false;
            }
            hid::tapScriptKey(action.key, action.mods);
            if (ctx.defaultDelayMs > 0 && !waitPollable(ctx.defaultDelayMs, ctx)) {
                return false;
            }
            return true;
        case ActionType::Repeat:
            return !ctx.aborted;
    }
    return true;
}

bool parseLine(const std::string& line, Action& out) {
    out.reset();
    if (line.empty()) {
        return true;
    }
    if (line[0] == '#' || (line.size() >= 2 && line[0] == '/' && line[1] == '/')) {
        return true;
    }
    if (isMetadata(lower(line))) {
        return true;
    }

    size_t pos = 0;
    const std::string first = nextToken(line, pos);
    if (first.empty()) {
        return true;
    }
    const std::string firstUp = upper(first);

    if (firstUp == "REM") {
        return true;
    }
    if (firstUp == "DEFAULT_DELAY" || firstUp == "DEFAULTDELAY") {
        uint32_t value = 0;
        if (!parseU32(line, pos, value)) {
            return false;
        }
        out.type = ActionType::DefaultDelay;
        out.arg = value;
        return true;
    }
    if (firstUp == "DELAY") {
        uint32_t value = 0;
        if (!parseU32(line, pos, value)) {
            return false;
        }
        out.type = ActionType::Delay;
        out.arg = value;
        return true;
    }
    if (firstUp == "STRING") {
        size_t start = pos;
        while (start < line.size() && isSpace(line[start])) {
            ++start;
        }
        out.type = ActionType::Text;
        out.text = line.substr(start);
        return true;
    }
    if (firstUp == "REPEAT") {
        uint32_t value = 0;
        if (!parseU32(line, pos, value)) {
            return false;
        }
        out.type = ActionType::Repeat;
        out.arg = value;
        return true;
    }

    const uint8_t namedKey = keyCode(firstUp);
    if (namedKey != 0) {
        if (!restIsSpaces(line, pos)) {
            return false;
        }
        out.type = ActionType::Key;
        out.key = namedKey;
        return true;
    }

    const uint8_t firstModifier = modifierBit(firstUp);
    if (firstModifier != 0) {
        out.type = ActionType::Chord;
        out.mods = firstModifier;
        for (;;) {
            const std::string token = nextToken(line, pos);
            if (token.empty()) {
                logger::warn("Bad BLE: comando ignorato: solo modificatori");
                return false;
            }
            const uint8_t modifier = modifierBit(upper(token));
            if (modifier != 0) {
                out.mods |= modifier;
                continue;
            }
            if (!restIsSpaces(line, pos)) {
                return false;
            }
            uint8_t key = 0;
            if (token.size() == 1) {
                unsigned char c = static_cast<unsigned char>(token[0]);
                // Ducky: CTRL C / GUI R non aggiungono Shift. Il caso
                // maiuscolo del token non deve diventare un tasto Shift.
                if (c >= 'A' && c <= 'Z') {
                    c = static_cast<unsigned char>(c - 'A' + 'a');
                }
                if (c >= 0x20 && c < 0x7F) {
                    key = c;
                }
            } else {
                key = keyCode(upper(token));
            }
            if (key == 0) {
                return false;
            }
            out.key = key;
            return true;
        }
    }

    if (line.size() == 1) {
        const unsigned char c = static_cast<unsigned char>(line[0]);
        if (c >= 0x20 && c < 0x7F) {
            out.type = ActionType::Text;
            out.text.assign(1, static_cast<char>(c));
            return true;
        }
    }
    return false;
}

// Self-check del parser: CTRL C / GUI R non devono aggiungere Shift.
bool parserSanity() {
    Action action;
    if (!parseLine("CTRL C", action) || action.type != ActionType::Chord ||
        action.key != 'c' || action.mods != kModCtrl) {
        return false;
    }
    action.reset();
    if (!parseLine("GUI R", action) || action.type != ActionType::Chord ||
        action.key != 'r' || action.mods != kModGui) {
        return false;
    }
    action.reset();
    if (!parseLine("STRING hello", action) || action.type != ActionType::Text ||
        action.text != "hello") {
        return false;
    }
    return true;
}

std::string shorten(const std::string& text, size_t maxChars) {
    if (text.size() <= maxChars || maxChars < 2) {
        return text;
    }
    return text.substr(0, maxChars - 1) + "~";
}

void drawRunScreen(const std::string& name, uint32_t line, uint32_t total,
                   const std::string& preview) {
    display::clear(display::kBackground);
    display::header("Bad BLE", "BLE HID");
    display::textCentered(28, shorten(name, 30), display::kAccent, 2);

    const std::string counter =
        "Riga " + std::to_string(line) + " / " + std::to_string(total);
    display::text(8, 48, counter, display::kText, 0);
    const float fraction =
        total > 0 ? static_cast<float>(line) / static_cast<float>(total) : 0.0f;
    display::progressBar(8, 60, display::width() - 16, 6, fraction);

    const size_t maxChars = static_cast<size_t>(display::width() - 16) / 6;
    display::text(8, 72, shorten(preview, maxChars), display::kTextDim, 0);
    display::text(8, 86, "Tasti inviati: " + std::to_string(hid::sentKeys()),
                  display::kText, 0);
    if (!hid::connected()) {
        display::text(8, 100, "Host disconnesso", display::kDanger, 0);
    }
    display::footer("ESC = annulla");
    display::push();
}

}  // namespace

std::vector<std::string> listScripts(bool& preferSd) {
    preferSd = true;
    std::vector<std::string> scripts;
    if (storage::hasSD()) {
        scripts = storage::list("/badusb", false, true);
    }
    if (scripts.empty()) {
        preferSd = false;
        scripts = storage::list("/badusb", false, false);
    }
    return scripts;
}

ScriptResult runScript(const std::string& path, const std::string& name,
                       bool preferSd) {
    ScriptResult result;

    if (!parserSanity()) {
        logger::error("Bad BLE: self-check parser fallito");
        return result;
    }

    File file = storage::openRead(path, preferSd);
    if (!file) {
        logger::warn("Bad BLE: impossibile aprire %s", path.c_str());
        return result;
    }

    // Primo passaggio: conteggio righe con lo stesso limite dell'esecuzione.
    bool capped = false;
    Line line;
    while (result.total < kMaxLines && readLine(file, line)) {
        ++result.total;
        if ((result.total & 0x3F) == 0) {
            navigation::update();
            if (!hid::connected()) {
                file.close();
                result.aborted = true;
                result.disconnected = true;
                return result;
            }
            if (navigation::poll().type == navigation::Event::Back) {
                file.close();
                result.aborted = true;
                return result;
            }
        }
    }
    if (result.total >= kMaxLines) {
        capped = readLine(file, line);
    }
    file.close();

    if (result.total == 0) {
        return result;
    }

    file = storage::openRead(path, preferSd);
    if (!file) {
        return result;
    }

    powertimer::Hold hold;
    navigation::waitRelease();

    RunContext ctx;
    ctx.dropsAtStart = hid::reportDrops();
    Action action;
    Action last;
    bool hasLast = false;
    uint32_t lastDraw = 0;

    while (result.lines < result.total && readLine(file, line)) {
        ++result.lines;
        const std::string text = trim(line.data, line.length);
        const uint32_t now = millis();
        if (lastDraw == 0 || now - lastDraw >= kDrawIntervalMs) {
            drawRunScreen(name, result.lines, result.total, text);
            lastDraw = now;
        }

        if (line.truncated) {
            logger::warn("Bad BLE: riga %u oltre %u byte, ignorata",
                         static_cast<unsigned>(result.lines),
                         static_cast<unsigned>(kMaxLineBytes));
            continue;
        }
        if (pollAbort(ctx)) {
            break;
        }
        if (!parseLine(text, action)) {
            logger::warn("Bad BLE: riga %u non riconosciuta: %s",
                         static_cast<unsigned>(result.lines), text.c_str());
            continue;
        }

        if (action.type == ActionType::Repeat) {
            if (action.arg == 0) {
                continue;
            }
            if (!hasLast) {
                logger::warn("Bad BLE: REPEAT senza comando precedente (riga %u)",
                             static_cast<unsigned>(result.lines));
                continue;
            }
            const uint32_t times = action.arg > kMaxRepeat ? kMaxRepeat : action.arg;
            bool stop = false;
            for (uint32_t i = 0; i < times; ++i) {
                if (!executeAction(last, ctx)) {
                    stop = true;
                    break;
                }
            }
            if (stop) {
                break;
            }
        } else {
            if (!executeAction(action, ctx)) {
                break;
            }
            if (action.type != ActionType::None) {
                last = action;
                hasLast = true;
            }
        }
    }

    hid::releaseAll();
    file.close();

    result.aborted = ctx.aborted;
    result.disconnected = ctx.lost;
    result.flowError = ctx.flow || hid::reportDrops() != ctx.dropsAtStart;
    result.completed =
        !ctx.aborted && !result.flowError && result.lines >= result.total;
    if (capped) {
        logger::warn("Bad BLE: limite di %u righe raggiunto",
                     static_cast<unsigned>(kMaxLines));
    }
    if (result.disconnected) {
        logger::warn("Bad BLE: script interrotto, host disconnesso");
    }
    if (result.flowError) {
        logger::warn("Bad BLE: report HID scartati, script interrotto");
    }
    return result;
}

}  // namespace blehid

#endif  // ENABLE_MODULE_BLEHID
