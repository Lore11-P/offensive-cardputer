#include "BadUsbScript.h"

#include "ModuleFlags.h"

#if ENABLE_MODULE_BADUSB

#include <Arduino.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "BadUsbHid.h"
#include "Display.h"
#include "Logger.h"
#include "Navigation.h"
#include "PowerTimer.h"
#include "Storage.h"
#include "Ui.h"

namespace badusb {
namespace {

constexpr size_t kMaxLineBytes = 256;
constexpr uint32_t kMaxLines = 5000;
constexpr uint32_t kMaxRepeat = 1000;
constexpr uint32_t kKeyHoldMs = 6;
constexpr uint32_t kPollSliceMs = 10;
constexpr uint32_t kDrawIntervalMs = 90;

struct Line {
    char data[kMaxLineBytes + 1] = {};
    size_t length = 0;
    bool truncated = false;
};

// Legge una riga senza allocare: il buffer ha un limite fisso e le righe
// piu' lunghe vengono scartate dal chiamante (truncated = true).
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

// Comando gia' interpretato. Il testo e' limitato dalla riga (max 256 byte).
struct Action {
    ActionType type = ActionType::None;
    uint32_t arg = 0;
    std::string text;
    uint8_t key = 0;  // carattere ASCII o hid::Special
    uint8_t mods[4] = {0, 0, 0, 0};
    uint8_t modCount = 0;

    void reset() {
        type = ActionType::None;
        arg = 0;
        text.clear();
        key = 0;
        modCount = 0;
    }
};

struct RunContext {
    uint32_t defaultDelayMs = 0;
    bool aborted = false;
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

// Nomi dei tasti come in DuckyScript/Flipper. 0 = non riconosciuto.
uint8_t keyCode(const std::string& upperToken) {
    if (upperToken == "ENTER" || upperToken == "RETURN") {
        return hid::kEnter;
    }
    if (upperToken == "TAB") {
        return hid::kTab;
    }
    if (upperToken == "ESC" || upperToken == "ESCAPE") {
        return hid::kEsc;
    }
    if (upperToken == "SPACE") {
        return ' ';
    }
    if (upperToken == "BACKSPACE") {
        return hid::kBackspace;
    }
    if (upperToken == "DELETE" || upperToken == "DEL") {
        return hid::kDelete;
    }
    if (upperToken == "CAPSLOCK" || upperToken == "CAPS_LOCK") {
        return hid::kCapsLock;
    }
    if (upperToken == "INSERT") {
        return hid::kInsert;
    }
    if (upperToken == "HOME") {
        return hid::kHome;
    }
    if (upperToken == "END") {
        return hid::kEnd;
    }
    if (upperToken == "PAGEUP" || upperToken == "PAGE_UP" || upperToken == "PGUP") {
        return hid::kPageUp;
    }
    if (upperToken == "PAGEDOWN" || upperToken == "PAGE_DOWN" || upperToken == "PGDN") {
        return hid::kPageDown;
    }
    if (upperToken == "UP" || upperToken == "ARROW_UP") {
        return hid::kUp;
    }
    if (upperToken == "DOWN" || upperToken == "ARROW_DOWN") {
        return hid::kDown;
    }
    if (upperToken == "LEFT" || upperToken == "ARROW_LEFT") {
        return hid::kLeft;
    }
    if (upperToken == "RIGHT" || upperToken == "ARROW_RIGHT") {
        return hid::kRight;
    }
    if (upperToken == "F1") {
        return hid::kF1;
    }
    if (upperToken == "F2") {
        return hid::kF2;
    }
    if (upperToken == "F3") {
        return hid::kF3;
    }
    if (upperToken == "F4") {
        return hid::kF4;
    }
    if (upperToken == "F5") {
        return hid::kF5;
    }
    if (upperToken == "F6") {
        return hid::kF6;
    }
    if (upperToken == "F7") {
        return hid::kF7;
    }
    if (upperToken == "F8") {
        return hid::kF8;
    }
    if (upperToken == "F9") {
        return hid::kF9;
    }
    if (upperToken == "F10") {
        return hid::kF10;
    }
    if (upperToken == "F11") {
        return hid::kF11;
    }
    if (upperToken == "F12") {
        return hid::kF12;
    }
    return 0;
}

// Modificatori sinistri: bastano per CTRL/ALT/SHIFT/GUI.
uint8_t modifierCode(const std::string& upperToken) {
    if (upperToken == "CTRL" || upperToken == "CONTROL") {
        return hid::kLeftCtrl;
    }
    if (upperToken == "ALT") {
        return hid::kLeftAlt;
    }
    if (upperToken == "SHIFT") {
        return hid::kLeftShift;
    }
    if (upperToken == "GUI" || upperToken == "WINDOWS" || upperToken == "WIN") {
        return hid::kLeftGui;
    }
    return 0;
}

bool pollAbort(RunContext& ctx) {
    if (ctx.aborted) {
        return true;
    }
    const navigation::EventData event = navigation::poll();
    if (event.type == navigation::Event::Back) {
        ctx.aborted = true;
    }
    return ctx.aborted;
}

// Attesa interrompibile: un poll ogni 10 ms, cosi' Back e' pronto subito.
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

// Battuta di un carattere. Ritorna false se il byte non e' rappresentabile
// con la mappa HID US (caratteri non ASCII).
bool typeChar(unsigned char c) {
    if (c == '\n') {
        hid::tap(hid::kEnter, kKeyHoldMs);
        return true;
    }
    if (c == '\t') {
        hid::tap(hid::kTab, kKeyHoldMs);
        return true;
    }
    if (c == '\b') {
        hid::tap(hid::kBackspace, kKeyHoldMs);
        return true;
    }
    if (c >= 0x20 && c < 0x7F) {
        hid::tap(c, kKeyHoldMs);
        return true;
    }
    return false;
}

bool typeText(const std::string& text, RunContext& ctx) {
    size_t skipped = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (pollAbort(ctx)) {
            return false;
        }
        unsigned char c = static_cast<unsigned char>(text[i]);
        // Sequenze pratiche: \n = INVIO, \t = TAB, \\ = barra rovesciata.
        if (c == '\\' && i + 1 < text.size()) {
            const char next = text[i + 1];
            if (next == 'n' || next == 'N') {
                c = '\n';
                ++i;
            } else if (next == 't' || next == 'T') {
                c = '\t';
                ++i;
            } else if (next == '\\') {
                c = '\\';
                ++i;
            }
        }
        if (!typeChar(c)) {
            ++skipped;
        }
        if (ctx.defaultDelayMs > 0 && !waitPollable(ctx.defaultDelayMs, ctx)) {
            return false;
        }
    }
    if (skipped > 0) {
        logger::warn("BadUSB: %u caratteri non ASCII ignorati",
                     static_cast<unsigned>(skipped));
    }
    return !ctx.aborted;
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
            return typeText(action.text, ctx);
        case ActionType::Key:
            if (pollAbort(ctx)) {
                return false;
            }
            hid::tap(action.key, kKeyHoldMs);
            if (ctx.defaultDelayMs > 0 && !waitPollable(ctx.defaultDelayMs, ctx)) {
                return false;
            }
            return true;
        case ActionType::Chord: {
            if (pollAbort(ctx)) {
                return false;
            }
            for (uint8_t i = 0; i < action.modCount; ++i) {
                hid::press(action.mods[i]);
            }
            if (action.key != 0) {
                hid::press(action.key);
                delay(kKeyHoldMs);
                hid::release(action.key);
            } else {
                delay(kKeyHoldMs);
            }
            for (int i = static_cast<int>(action.modCount) - 1; i >= 0; --i) {
                hid::release(action.mods[i]);
            }
            hid::releaseAll();  // con i soli modificatori il report va forzato
            if (ctx.defaultDelayMs > 0 && !waitPollable(ctx.defaultDelayMs, ctx)) {
                return false;
            }
            return true;
        }
        case ActionType::Repeat:
            // Gestito dal ciclo principale, che conosce l'ultimo comando.
            return !ctx.aborted;
    }
    return true;
}

// Interpreta una riga gia' ripulita. false = riga sconosciuta (da loggare).
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

    const uint8_t firstModifier = modifierCode(firstUp);
    if (firstModifier != 0) {
        out.type = ActionType::Chord;
        out.mods[out.modCount++] = firstModifier;
        for (;;) {
            const std::string token = nextToken(line, pos);
            if (token.empty()) {
                logger::warn("BadUSB: comando ignorato: solo modificatori");
                return false;
            }
            const uint8_t modifier = modifierCode(upper(token));
            if (modifier != 0) {
                if (out.modCount < sizeof(out.mods)) {
                    out.mods[out.modCount++] = modifier;
                }
                continue;
            }
            if (!restIsSpaces(line, pos)) {
                return false;  // piu' di un tasto dopo i modificatori
            }
            uint8_t key = 0;
            if (token.size() == 1) {
                const unsigned char c = static_cast<unsigned char>(token[0]);
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

std::string shorten(const std::string& text, size_t maxChars) {
    if (text.size() <= maxChars || maxChars < 2) {
        return text;
    }
    return text.substr(0, maxChars - 1) + "~";
}

void drawRunScreen(const std::string& name, uint32_t line, uint32_t total,
                   const std::string& preview, bool capped, bool host) {
    display::clear(display::kBackground);
    display::header(name, "BadUSB");
    display::textCentered(30, shorten(name, 30), display::kAccent, 2);

    const std::string counter = "Riga " + std::to_string(line) + " / " + std::to_string(total);
    display::text(8, 50, counter, display::kText, 0);

    const float fraction =
        total > 0 ? static_cast<float>(line) / static_cast<float>(total) : 0.0f;
    display::progressBar(8, 62, display::width() - 16, 6, fraction);

    const size_t maxChars = static_cast<size_t>(display::width() - 16) / 6;
    display::text(8, 74, shorten(preview, maxChars), display::kTextDim, 0);

    int y = 90;
    if (capped) {
        display::text(8, y, "Limite di righe raggiunto", display::kDanger, 0);
        y += 11;
    }
    if (!host) {
        display::text(8, y, "Host USB non rilevato", display::kWarning, 0);
    }
    display::footer("ESC = annulla");
    display::push();
}

}  // namespace

bool runScript(const std::string& path, const std::string& name, bool preferSd) {
    File file = storage::openRead(path, preferSd);
    if (!file) {
        ui::message("BadUSB", {"!Impossibile aprire lo script.", path}, "Menu principale");
        return false;
    }

    // Primo passaggio: solo conteggio delle righe (stesso limite dell'esecuzione).
    uint32_t total = 0;
    bool capped = false;
    Line line;
    while (total < kMaxLines && readLine(file, line)) {
        ++total;
        // Il conteggio puo' scorrere migliaia di righe: tieni viva la UI e
        // lascia annullare con backtick.
        if ((total & 0x3F) == 0) {
            navigation::update();
            if (navigation::poll().type == navigation::Event::Back) {
                file.close();
                return false;
            }
        }
    }
    if (total >= kMaxLines) {
        capped = readLine(file, line);
    }
    file.close();

    if (total == 0) {
        ui::message("BadUSB", {"Script vuoto.", path}, "Menu principale");
        return false;
    }

    file = storage::openRead(path, preferSd);
    if (!file) {
        ui::message("BadUSB", {"!Script non piu' leggibile.", path}, "Menu principale");
        return false;
    }

    if (!hid::begin()) {
        file.close();
        ui::message("BadUSB", {"!Stack USB non disponibile.", "Usa l'ambiente cardputer-adv-hid."},
                    "Menu principale");
        return false;
    }

    powertimer::Hold hold;  // l'esecuzione non deve far scattare lo spegnimento
    navigation::waitRelease();

    // Attende che l'host enumeri la tastiera: i primi tasti andrebbero persi.
    const uint32_t connectStart = millis();
    while (!hid::connected() && millis() - connectStart < 3000) {
        navigation::update();
        delay(20);
    }

    RunContext ctx;
    Action action;
    Action last;
    bool hasLast = false;
    uint32_t lineNo = 0;
    uint32_t lastDraw = 0;

    for (;;) {
        if (lineNo >= total || !readLine(file, line)) {
            break;
        }
        ++lineNo;

        const std::string text = trim(line.data, line.length);
        const uint32_t now = millis();
        if (lastDraw == 0 || now - lastDraw >= kDrawIntervalMs) {
            drawRunScreen(name, lineNo, total, text, capped, hid::connected());
            lastDraw = now;
        }

        if (line.truncated) {
            logger::warn("BadUSB: riga %u oltre %u byte, ignorata",
                         static_cast<unsigned>(lineNo),
                         static_cast<unsigned>(kMaxLineBytes));
            continue;
        }

        if (pollAbort(ctx)) {
            break;
        }

        if (!parseLine(text, action)) {
            // Mai il testo: la seriale della build HID e' la porta del target.
            logger::warn("BadUSB: riga %u non riconosciuta",
                         static_cast<unsigned>(lineNo));
            continue;
        }

        if (action.type == ActionType::Repeat) {
            if (action.arg == 0) {
                continue;
            }
            if (!hasLast) {
                logger::warn("BadUSB: REPEAT senza comando precedente (riga %u)",
                             static_cast<unsigned>(lineNo));
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

    if (ctx.aborted) {
        logger::info("BadUSB: script annullato (riga %u)", static_cast<unsigned>(lineNo));
        return false;
    }

    if (lineNo < total) {
        ui::message("BadUSB", {"!Lettura interrotta.", "Il file e' cambiato durante l'esecuzione."},
                    "Menu principale");
        return false;
    }

    std::vector<std::string> lines = {"Script completato.", name};
    if (capped) {
        lines.push_back("Limite di 5000 righe raggiunto.");
    }
    ui::message("Esecuzione", lines, "BadUSB");
    return true;
}

}  // namespace badusb

#endif  // ENABLE_MODULE_BADUSB
