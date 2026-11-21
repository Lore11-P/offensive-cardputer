#include "ToolsMenu.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <esp_sntp.h>
#include <mbedtls/md.h>
#include <sys/time.h>
#include <time.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "CalcExpr.h"
#include "Display.h"
#include "MenuManager.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "PowerTimer.h"
#include "Storage.h"
#include "TextInput.h"
#include "Ui.h"
#include "WifiSetup.h"
#include "qrcode.h"

#if ENABLE_MODULE_TOOLS

namespace tools {
namespace {

constexpr const char* kToolsDir = "/tools";
constexpr const char* kNotesPath = "/tools/notes.txt";
constexpr const char* kCalendarPath = "/tools/calendar.txt";
constexpr const char* kTotpPath = "/tools/totp.txt";
constexpr const char* kQrBmpPath = "/tools/qr.bmp";
constexpr const char* kTimezone = "CET-1CEST,M3.5.0,M10.5.0/3";

// 2023-11-14: qualunque epoch piu' piccolo significa orologio non impostato.
constexpr time_t kEpochValid = 1700000000;
constexpr size_t kNotesMax = 1024;
constexpr size_t kCalendarMax = 8192;
constexpr size_t kTotpMax = 4096;
constexpr size_t kCalcMax = 64;
constexpr size_t kQrTextMax = 100;
constexpr int kNoteWrap = 32;
constexpr int kNoteLines = 7;

const char* const kMonths[] = {"GENNAIO", "FEBBRAIO", "MARZO",    "APRILE",
                               "MAGGIO",  "GIUGNO",   "LUGLIO",   "AGOSTO",
                               "SETTEMBRE", "OTTOBRE", "NOVEMBRE", "DICEMBRE"};
const char* const kMonthsShort[] = {"gen", "feb", "mar", "apr", "mag", "giu",
                                    "lug", "ago", "set", "ott", "nov", "dic"};
// Lunedi = 0.
const char* const kDaysShort[] = {"lun", "mar", "mer", "gio", "ven", "sab", "dom"};

void ensureTimezone() {
    static bool done = false;
    if (!done) {
        setenv("TZ", kTimezone, 1);
        tzset();
        done = true;
    }
}

bool timeValid(time_t t) {
    return t >= kEpochValid;
}

bool ensureToolsDir() {
    if (!storage::hasSD()) {
        return false;
    }
    if (!storage::exists(kToolsDir, true)) {
        storage::mkdir(kToolsDir, true);
    }
    return storage::exists(kToolsDir, true);
}

std::string trimCopy(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' ||
                           text[begin] == '\r' || text[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                           text[end - 1] == '\r' || text[end - 1] == '\n')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string shorten(const std::string& text, size_t maxChars) {
    if (text.size() <= maxChars || maxChars < 2) {
        return text;
    }
    return text.substr(0, maxChars - 1) + "~";
}

bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

bool isLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int daysInMonth(int year, int month) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 0;
    }
    if (month == 2 && isLeapYear(year)) {
        return 29;
    }
    return kDays[month - 1];
}

// 0 = lunedi. Zeller semplificato, valido per gli anni del calendario.
int weekdayMonday(int year, int month, int day) {
    static const int kMonthOffset[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 3) {
        --year;
    }
    const int w = (year + year / 4 - year / 100 + year / 400 +
                   kMonthOffset[month - 1] + day) %
                  7;
    return (w + 6) % 7;
}

int makeDateValue(int year, int month, int day) {
    return year * 10000 + month * 100 + day;
}

void splitDateValue(int value, int& year, int& month, int& day) {
    year = value / 10000;
    month = (value / 100) % 100;
    day = value % 100;
}

bool parseIsoDate(const std::string& text, int& year, int& month, int& day) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        return false;
    }
    for (size_t i = 0; i < text.size(); ++i) {
        if (i == 4 || i == 7) {
            continue;
        }
        if (!isDigit(text[i])) {
            return false;
        }
    }
    year = (text[0] - '0') * 1000 + (text[1] - '0') * 100 + (text[2] - '0') * 10 +
           (text[3] - '0');
    month = (text[5] - '0') * 10 + (text[6] - '0');
    day = (text[8] - '0') * 10 + (text[9] - '0');
    return year >= 2000 && year <= 2099 && month >= 1 && month <= 12 && day >= 1 &&
           day <= daysInMonth(year, month);
}

bool parseHourMinute(const std::string& text, int& hour, int& minute) {
    if (text.size() != 4) {
        return false;
    }
    for (char c : text) {
        if (!isDigit(c)) {
            return false;
        }
    }
    hour = (text[0] - '0') * 10 + (text[1] - '0');
    minute = (text[2] - '0') * 10 + (text[3] - '0');
    return hour >= 0 && hour < 24 && minute >= 0 && minute < 60;
}

std::string dateIso(int value) {
    char buffer[12];
    int year = 0;
    int month = 0;
    int day = 0;
    splitDateValue(value, year, month, day);
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", year, month, day);
    return buffer;
}

std::string datePretty(int value) {
    int year = 0;
    int month = 0;
    int day = 0;
    splitDateValue(value, year, month, day);
    if (year < 2000 || month < 1 || month > 12 || day < 1) {
        return "--";
    }
    return std::string(kDaysShort[weekdayMonday(year, month, day)]) + " " +
           std::to_string(day) + " " + kMonthsShort[month - 1] + " " +
           std::to_string(year);
}

// Aggiunge una riga a un file di testo preservando il contenuto esistente.
bool appendLine(const char* path, const std::string& line, size_t maxBytes) {
    if (!ensureToolsDir()) {
        return false;
    }
    if (storage::fileSize(path, true) > maxBytes) {
        return false;
    }
    std::string data;
    storage::readText(path, data, maxBytes, true);
    if (!data.empty() && data.back() != '\n') {
        data.push_back('\n');
    }
    data += line;
    data.push_back('\n');
    if (data.size() > maxBytes) {
        return false;
    }
    return storage::writeText(path, data, true);
}

// ---------------------------------------------------------------------------
// Orologio
// ---------------------------------------------------------------------------

bool syncNtp(const std::string& parent) {
    if (!wifisetup::ensureConnected(true, parent)) {
        return false;
    }
    configTzTime(kTimezone, "pool.ntp.org", "time.cloudflare.com");
    // Forza un nuovo ciclo: senza reset una sincronizzazione precedente
    // farebbe uscire subito dalla schermata.
    sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);

    navigation::waitRelease();
    const uint32_t started = millis();
    uint32_t lastDraw = 0;
    bool ok = false;
    for (;;) {
        const uint32_t now = millis();
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED &&
            timeValid(time(nullptr))) {
            ok = true;
            break;
        }
        if (now - started >= 10000) {
            break;
        }
        if (lastDraw == 0 || now - lastDraw >= 400) {
            lastDraw = now;
            display::clear();
            display::header("Orologio", parent);
            display::textCentered(44, "Sincronizzazione NTP...", display::kText, 2);
            display::progressBar(30, 64, 180, 6,
                                 static_cast<float>(now - started) / 10000.0f,
                                 display::kAccent);
            display::textCentered(
                78, std::to_string((now - started) / 1000) + " s", display::kTextDim, 0);
            display::footer("ESC=annulla");
            display::push();
        }
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            break;
        }
        delay(20);
    }
    wifisetup::disconnect();
    sntp_stop();
    return ok;
}

void drawClockScreen() {
    display::clear();
    display::header("Orologio", "Utility");

    const time_t now = time(nullptr);
    const bool valid = timeValid(now);
    struct tm local = {};
    localtime_r(&now, &local);

    char big[16];
    if (valid) {
        std::snprintf(big, sizeof(big), "%02d:%02d:%02d", local.tm_hour, local.tm_min,
                      local.tm_sec);
    } else {
        std::strcpy(big, "--:--:--");
    }
    display::textCentered(30, big, valid ? display::kAccent : display::kTextDim, 4);

    const std::string date = valid ? datePretty(makeDateValue(
                                         local.tm_year + 1900, local.tm_mon + 1,
                                         local.tm_mday))
                                   : std::string("ora non impostata");
    display::textCentered(54, date, valid ? display::kText : display::kTextDim, 2);
    display::textCentered(70, valid ? "ora locale (CET/CEST)" : "usa NTP o M per impostare",
                          display::kTextDim, 0);
    display::textCentered(88, "ENTER = sincronizza NTP", display::kTextDim, 0);
    display::textCentered(100, "M = imposta data e ora", display::kTextDim, 0);
    display::footer("ENTER=NTP M=manuale ESC=esci");
    display::push();
}

void manualTimeScreen() {
    ensureTimezone();
    std::vector<textinput::Field> fields(2);
    fields[0].label = "Data AAAA-MM-GG";
    fields[0].maxLen = 10;
    fields[0].numeric = true;
    fields[1].label = "Ora HHMM";
    fields[1].maxLen = 4;
    fields[1].numeric = true;
    if (!textinput::edit("Data e ora", "Orologio", fields)) {
        return;
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    if (!parseIsoDate(fields[0].value, year, month, day) ||
        !parseHourMinute(fields[1].value, hour, minute)) {
        ui::message("Data e ora", {"!Formato non valido.", "Data AAAA-MM-GG, ora HHMM."},
                    "Orologio");
        return;
    }

    struct tm local = {};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_sec = 0;
    local.tm_isdst = -1;
    const time_t stamp = mktime(&local);
    if (stamp == static_cast<time_t>(-1) || local.tm_year != year - 1900 ||
        local.tm_mon != month - 1 || local.tm_mday != day) {
        ui::message("Data e ora", {"!Data non valida."}, "Orologio");
        return;
    }

    struct timeval tv = {};
    tv.tv_sec = stamp;
    tv.tv_usec = 0;
    if (settimeofday(&tv, nullptr) != 0) {
        ui::message("Data e ora", {"!Impostazione non riuscita."}, "Orologio");
        return;
    }

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d %02d/%02d/%04d", hour, minute, day,
                  month, year);
    ui::message("Data e ora", {std::string("Impostato: ") + buffer}, "Orologio");
}

void clockScreen() {
    ensureTimezone();
    powertimer::Hold hold;
    navigation::waitRelease();
    uint32_t lastDraw = 0;
    for (;;) {
        const uint32_t now = millis();
        if (lastDraw == 0 || now - lastDraw >= 250) {
            lastDraw = now;
            drawClockScreen();
        }
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            break;
        }
        if (ev.type == navigation::Event::Select) {
            syncNtp("Orologio");
            lastDraw = 0;
        } else if (ev.type == navigation::Event::Text && (ev.ch == 'm' || ev.ch == 'M')) {
            manualTimeScreen();
            lastDraw = 0;
        }
        delay(5);
    }
}

// ---------------------------------------------------------------------------
// Calendario
// ---------------------------------------------------------------------------

struct CalEvent {
    int date = 0;
    std::string text;
};

std::vector<CalEvent> loadCalendar() {
    std::vector<CalEvent> events;
    std::string data;
    if (!storage::readText(kCalendarPath, data, kCalendarMax, true)) {
        return events;
    }
    size_t pos = 0;
    while (pos < data.size() && events.size() < 120) {
        size_t end = data.find('\n', pos);
        if (end == std::string::npos) {
            end = data.size();
        }
        std::string line = data.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.size() < 12 || line[10] != '|') {
            continue;
        }
        int year = 0;
        int month = 0;
        int day = 0;
        if (!parseIsoDate(line.substr(0, 10), year, month, day)) {
            continue;
        }
        const std::string text = trimCopy(line.substr(11));
        if (text.empty()) {
            continue;
        }
        CalEvent event;
        event.date = makeDateValue(year, month, day);
        event.text = text;
        events.push_back(std::move(event));
    }
    return events;
}

void drawCalendarMonth(int year, int month, int selected,
                       const std::vector<CalEvent>& events, bool todayValid,
                       int todayValue) {
    display::clear();
    display::header("Calendario", "Utility");

    char title[24];
    std::snprintf(title, sizeof(title), "%s %d", kMonths[month - 1], year);
    display::textCentered(24, title, display::kAccent, 2);

    const char* const kWeekdays[] = {"LU", "MA", "ME", "GI", "VE", "SA", "DO"};
    constexpr int kCellW = 32;
    constexpr int kCellH = 12;
    constexpr int kGridX = 8;
    constexpr int kGridY = 48;
    for (int i = 0; i < 7; ++i) {
        display::text(kGridX + i * kCellW + (kCellW - 11) / 2, 37, kWeekdays[i],
                      display::kTextDim, 0);
    }

    bool hasEvent[32] = {};
    for (const auto& event : events) {
        int ey = 0;
        int em = 0;
        int ed = 0;
        splitDateValue(event.date, ey, em, ed);
        if (ey == year && em == month && ed >= 1 && ed <= 31) {
            hasEvent[ed] = true;
        }
    }

    const int first = weekdayMonday(year, month, 1);
    const int count = daysInMonth(year, month);
    for (int day = 1; day <= count; ++day) {
        const int index = first + day - 1;
        const int x = kGridX + (index % 7) * kCellW;
        const int y = kGridY + (index / 7) * kCellH;
        const int value = makeDateValue(year, month, day);
        if (day == selected) {
            display::g().fillRoundRect(x + 1, y, kCellW - 2, 11, 2,
                                       display::C(display::T_bg3));
        }
        if (todayValid && value == todayValue) {
            display::g().drawRect(x + 1, y, kCellW - 2, 11, display::kAccent);
        }
        char label[4];
        std::snprintf(label, sizeof(label), "%02d", day);
        const uint16_t color = day == selected
                                   ? display::kAccent
                                   : (hasEvent[day] ? display::kText : display::kTextDim);
        display::text(x + (kCellW - 11) / 2, y + 2, label, color, 0);
        if (hasEvent[day]) {
            display::g().fillRect(x + kCellW - 6, y + 8, 2, 2, display::kAccent);
        }
    }
    display::footer("ESC=esci </>=mese ;.=giorno ENTER=apri");
    display::push();
}

void addCalendarEvent(int dateValue) {
    std::vector<textinput::Field> fields(1);
    fields[0].label = "Evento";
    fields[0].maxLen = 70;
    if (!textinput::edit("Nuovo evento", "Calendario", fields)) {
        return;
    }
    const std::string text = trimCopy(fields[0].value);
    if (text.empty()) {
        return;
    }
    if (!appendLine(kCalendarPath, dateIso(dateValue) + "|" + text, kCalendarMax)) {
        ui::message("Calendario",
                    {"!Salvataggio non riuscito.", "Controlla la microSD."},
                    "Calendario");
    }
}

void calendarDay(int year, int month, int day, std::vector<CalEvent>& events) {
    const int value = makeDateValue(year, month, day);
    navigation::waitRelease();
    bool dirty = true;
    for (;;) {
        if (dirty) {
            dirty = false;
            char title[16];
            std::snprintf(title, sizeof(title), "%02d/%02d/%04d", day, month, year);
            display::clear();
            display::header(title, "Calendario");
            display::text(8, 26, datePretty(value), display::kText, 2);

            int shown = 0;
            for (const auto& event : events) {
                if (event.date != value) {
                    continue;
                }
                if (shown >= 7) {
                    break;
                }
                display::text(8, 42 + shown * 11, "- " + shorten(event.text, 36),
                              display::kText, 0);
                ++shown;
            }
            if (shown == 0) {
                display::text(8, 44, "Nessun evento.", display::kTextDim, 0);
            }
            display::footer("A=aggiungi ESC=indietro");
            display::push();
        }

        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            break;
        }
        if (ev.type == navigation::Event::Text && (ev.ch == 'a' || ev.ch == 'A')) {
            addCalendarEvent(value);
            events = loadCalendar();
            dirty = true;
        }
        delay(5);
    }
}

void calendarScreen() {
    ensureTimezone();
    std::vector<CalEvent> events = loadCalendar();

    const time_t now = time(nullptr);
    const bool valid = timeValid(now);
    struct tm local = {};
    localtime_r(&now, &local);
    int year = valid ? local.tm_year + 1900 : 2026;
    int month = valid ? local.tm_mon + 1 : 1;
    int selected = valid ? local.tm_mday : 1;
    const int todayValue =
        valid ? makeDateValue(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday) : 0;

    navigation::waitRelease();
    bool dirty = true;
    for (;;) {
        if (dirty) {
            dirty = false;
            drawCalendarMonth(year, month, selected, events, valid, todayValue);
        }
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            break;
        } else if (ev.type == navigation::Event::Left) {
            if (--month < 1) {
                month = 12;
                --year;
            }
            if (year < 2000) {
                year = 2000;
                month = 1;
            }
            selected = std::min(selected, daysInMonth(year, month));
            dirty = true;
        } else if (ev.type == navigation::Event::Right) {
            if (++month > 12) {
                month = 1;
                ++year;
            }
            if (year > 2099) {
                year = 2099;
                month = 12;
            }
            selected = std::min(selected, daysInMonth(year, month));
            dirty = true;
        } else if (ev.type == navigation::Event::Up) {
            if (selected > 1) {
                --selected;
                dirty = true;
            }
        } else if (ev.type == navigation::Event::Down) {
            if (selected < daysInMonth(year, month)) {
                ++selected;
                dirty = true;
            }
        } else if (ev.type == navigation::Event::Select) {
            calendarDay(year, month, selected, events);
            dirty = true;
        }
        delay(5);
    }
}

// ---------------------------------------------------------------------------
// Note
// ---------------------------------------------------------------------------

// Una riga visiva alla volta: niente vettore di stringhe ricostruito a ogni
// frame (il framebuffer viene ridisegnato spesso).
template <typename Callback>
void forEachNoteLine(const std::string& note, Callback callback) {
    std::string line;
    for (char c : note) {
        if (c == '\n') {
            callback(line);
            line.clear();
            continue;
        }
        line.push_back(c);
        if (static_cast<int>(line.size()) >= kNoteWrap) {
            callback(line);
            line.clear();
        }
    }
    callback(line);
}

void drawNotes(const std::string& note) {
    display::clear();
    display::header("Note", "Utility");

    int total = 0;
    forEachNoteLine(note, [&](const std::string&) { ++total; });
    const int skip = total > kNoteLines ? total - kNoteLines : 0;

    int index = 0;
    int y = 26;
    forEachNoteLine(note, [&](const std::string& line) {
        ++index;
        if (index <= skip) {
            return;
        }
        std::string shown = line;
        const bool last = index == total;
        if (last) {
            shown += '_';
        }
        display::text(8, y, shown.empty() ? std::string("_") : shown,
                      last ? display::kAccent : display::kText, 2);
        y += 12;
    });

    char info[24];
    std::snprintf(info, sizeof(info), "%u/%u", static_cast<unsigned>(note.size()),
                  static_cast<unsigned>(kNotesMax));
    display::text(8, 116, info, display::kTextDim, 0);
    display::footer("ESC=salva DEL=cancella ENTER=a capo");
    display::push();
}

bool saveNotes(const std::string& note) {
    return ensureToolsDir() && storage::writeText(kNotesPath, note, true);
}

void notesScreen() {
    if (!ensureToolsDir()) {
        ui::message("Note", {"!microSD assente.", "Inserisci una SD per salvare."},
                    "Utility");
        return;
    }

    std::string note;
    if (storage::fileSize(kNotesPath, true) > kNotesMax) {
        ui::message("Note", {"!File troppo grande.", "Il limite e' 1024 byte."}, "Utility");
        return;
    }
    storage::readText(kNotesPath, note, kNotesMax, true);
    if (note.size() > kNotesMax) {
        note.resize(kNotesMax);
    }
    note.erase(std::remove(note.begin(), note.end(), '\r'), note.end());

    navigation::waitRelease();
    M5Cardputer.update();
    std::vector<char> lastWord = M5Cardputer.Keyboard.keysState().word;
    bool lastEnter = M5Cardputer.Keyboard.keysState().enter;
    bool delHeld = false;
    uint32_t nextDel = 0;
    uint32_t lastDraw = 0;
    uint32_t lastSave = millis();
    bool unsaved = false;

    for (;;) {
        powertimer::tick();
        const uint32_t now = millis();
        if (lastDraw == 0 || now - lastDraw >= 80) {
            lastDraw = now;
            drawNotes(note);
        }

        M5Cardputer.update();
        auto& state = M5Cardputer.Keyboard.keysState();
        const std::vector<char>& word = state.word;

        for (char c : word) {
            if (c < 0x20 || c >= 0x7F || std::find(lastWord.begin(), lastWord.end(), c) !=
                                              lastWord.end()) {
                continue;
            }
            powertimer::activity();
            if (c == '`') {
                navigation::waitRelease();
                if (!saveNotes(note)) {
                    ui::message("Note", {"!Salvataggio non riuscito.", "Controlla la microSD."},
                                "Utility");
                }
                return;
            }
            if (note.size() < kNotesMax) {
                note.push_back(c);
                unsaved = true;
            }
        }

        if (state.del) {
            if (!delHeld) {
                delHeld = true;
                nextDel = millis() + 400;
                powertimer::activity();
                if (!note.empty()) {
                    note.pop_back();
                    unsaved = true;
                }
            } else if (millis() >= nextDel) {
                nextDel = millis() + 60;
                powertimer::activity();
                if (!note.empty()) {
                    note.pop_back();
                    unsaved = true;
                }
            }
        } else {
            delHeld = false;
        }

        if (state.enter && !lastEnter && note.size() < kNotesMax) {
            note.push_back('\n');
            unsaved = true;
        }

        lastWord = word;
        lastEnter = state.enter;

        if (unsaved && millis() - lastSave >= 15000) {
            if (saveNotes(note)) {
                unsaved = false;
                lastSave = millis();
            }
        }
        delay(18);
    }
}

// ---------------------------------------------------------------------------
// Calcolatrice
// ---------------------------------------------------------------------------

bool isCalculatorChar(char c) {
    return (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '*' || c == '/' ||
           c == '%' || c == '(' || c == ')' || c == '.';
}

void formatResult(double value, char* out, size_t size) {
    std::snprintf(out, size, "%.6g", value);
}

void drawCalculator(const std::string& expression, bool hasResult, bool resultOk,
                    double result) {
    display::clear();
    display::header("Calcolatrice", "Utility");
    display::text(8, 26, "ESPRESSIONE", display::kTextDim, 0);

    std::string shown = expression;
    if (shown.size() > 32) {
        shown = "..." + shown.substr(shown.size() - 29);
    }
    display::text(8, 36, shown.empty() ? std::string("_") : shown, display::kText, 2);
    display::g().drawFastHLine(8, 52, display::width() - 16,
                               display::C(display::T_line));
    display::text(8, 60, "RISULTATO", display::kTextDim, 0);

    if (hasResult) {
        if (resultOk) {
            char buffer[24];
            formatResult(result, buffer, sizeof(buffer));
            display::textCentered(74, buffer, display::kAccent, 4);
        } else {
            display::textCentered(74, "Errore", display::kDanger, 4);
        }
    } else {
        display::textCentered(80, "0", display::kTextDim, 4);
    }
    display::footer("ENTER=calcola DEL=cancella ESC=esci");
    display::push();
}

void calculatorScreen() {
    std::string expression;
    bool hasResult = false;
    bool resultOk = false;
    double result = 0.0;

    navigation::waitRelease();
    M5Cardputer.update();
    std::vector<char> lastWord = M5Cardputer.Keyboard.keysState().word;
    bool lastEnter = M5Cardputer.Keyboard.keysState().enter;
    bool delHeld = false;
    uint32_t nextDel = 0;
    uint32_t lastDraw = 0;

    for (;;) {
        powertimer::tick();
        const uint32_t now = millis();
        if (lastDraw == 0 || now - lastDraw >= 60) {
            lastDraw = now;
            drawCalculator(expression, hasResult, resultOk, result);
        }

        M5Cardputer.update();
        auto& state = M5Cardputer.Keyboard.keysState();
        const std::vector<char>& word = state.word;
        bool evaluate = false;

        for (char c : word) {
            if (c < 0x20 || c >= 0x7F || std::find(lastWord.begin(), lastWord.end(), c) !=
                                              lastWord.end()) {
                continue;
            }
            powertimer::activity();
            if (c == '`') {
                navigation::waitRelease();
                return;
            }
            if (c == '=') {
                evaluate = true;
            } else if (isCalculatorChar(c) && expression.size() < kCalcMax) {
                expression.push_back(c);
                hasResult = false;
            }
        }

        if (state.del) {
            if (!delHeld) {
                delHeld = true;
                nextDel = millis() + 400;
                powertimer::activity();
                if (!expression.empty()) {
                    expression.pop_back();
                    hasResult = false;
                }
            } else if (millis() >= nextDel) {
                nextDel = millis() + 60;
                powertimer::activity();
                if (!expression.empty()) {
                    expression.pop_back();
                    hasResult = false;
                }
            }
        } else {
            delHeld = false;
        }

        if (state.enter && !lastEnter) {
            evaluate = true;
        }
        if (evaluate) {
            hasResult = true;
            resultOk = calc::evaluate(expression, result);
        }

        lastWord = word;
        lastEnter = state.enter;
        delay(18);
    }
}

// ---------------------------------------------------------------------------
// QR
// ---------------------------------------------------------------------------

void drawQr(QRCode& qr) {
    display::clear();
    display::header("QR code", "Utility");

    const int modules = qr.size;
    const int size = modules + 8;  // quiet zone di 4 moduli per lato
    int scale = 100 / size;
    scale = std::max(2, std::min(3, scale));
    const int total = size * scale;
    const int ox = (display::width() - total) / 2;
    const int oy = 23 + (100 - total) / 2;

    auto& g = display::g();
    g.fillRect(ox, oy, total, total, 0xFFFF);
    for (int y = 0; y < modules; ++y) {
        for (int x = 0; x < modules; ++x) {
            if (qrcode_getModule(&qr, x, y)) {
                g.fillRect(ox + (x + 4) * scale, oy + (y + 4) * scale, scale, scale,
                           0x0000);
            }
        }
    }
    display::footer("S=salva BMP ESC=esci");
    display::push();
}

void qrScreen() {
    std::vector<textinput::Field> fields(1);
    fields[0].label = "Testo";
    fields[0].maxLen = kQrTextMax;
    if (!textinput::edit("QR code", "Utility", fields) || fields[0].value.empty()) {
        return;
    }

    // La libreria ricmoo/QRCode non rifiuta un testo troppo lungo (scrive
    // oltre il buffer della versione): la capacita' va scelta qui. Valori in
    // byte con ECC_LOW per le versioni 1..6.
    constexpr size_t kByteCapacity[] = {17, 32, 53, 78, 106, 134};
    int version = 0;
    for (int v = 1; v <= 6; ++v) {
        if (fields[0].value.size() <= kByteCapacity[v - 1]) {
            version = v;
            break;
        }
    }
    if (version == 0) {
        ui::message("QR code", {"!Testo troppo lungo.", "Riduci il contenuto."}, "Utility");
        return;
    }

    // qrcode_getBufferSize(6) = 176 byte: formula della libreria, costante qui
    // perche' il buffer e' statico (niente PSRAM).
    static uint8_t modules[((16 * 6 + 128) * 6 + 64) / 8];
    QRCode qr;
    if (qrcode_initText(&qr, modules, static_cast<uint8_t>(version), ECC_LOW,
                        fields[0].value.c_str()) != 0) {
        ui::message("QR code", {"!Generazione non riuscita."}, "Utility");
        return;
    }

    navigation::waitRelease();
    bool dirty = true;
    for (;;) {
        if (dirty) {
            dirty = false;
            drawQr(qr);
        }
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            break;
        }
        if (ev.type == navigation::Event::Text && (ev.ch == 's' || ev.ch == 'S')) {
            const bool ok = ensureToolsDir() && display::saveScreenshot(kQrBmpPath);
            ui::message("QR code",
                        ok ? std::vector<std::string>{"BMP salvato:", kQrBmpPath}
                           : std::vector<std::string>{"!Salvataggio non riuscito.",
                                                      "Controlla la microSD."},
                        "Utility");
            dirty = true;
        }
        delay(5);
    }
}

// ---------------------------------------------------------------------------
// TOTP
// ---------------------------------------------------------------------------

struct TotpSecret {
    std::string name;
    std::string source;
    uint8_t key[64] = {};
    size_t keyLen = 0;
};

bool base32Decode(const std::string& text, uint8_t* out, size_t capacity,
                  size_t& outLen) {
    uint32_t buffer = 0;
    int bits = 0;
    size_t length = 0;
    for (char c : text) {
        int value = 0;
        if (c >= 'A' && c <= 'Z') {
            value = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            value = c - 'a';
        } else if (c >= '2' && c <= '7') {
            value = c - '2' + 26;
        } else if (c == '=' || c == ' ' || c == '\t' || c == '-') {
            continue;
        } else {
            return false;
        }
        buffer = (buffer << 5) | static_cast<uint32_t>(value);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            if (length >= capacity) {
                return false;
            }
            out[length++] = static_cast<uint8_t>((buffer >> bits) & 0xFF);
        }
    }
    outLen = length;
    return length > 0;
}

bool hotpCode(const TotpSecret& secret, uint64_t counter, uint32_t& code) {
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (info == nullptr) {
        return false;
    }
    uint8_t message[8];
    for (int i = 7; i >= 0; --i) {
        message[i] = static_cast<uint8_t>(counter & 0xFF);
        counter >>= 8;
    }
    uint8_t digest[20];
    if (mbedtls_md_hmac(info, secret.key, secret.keyLen, message, sizeof(message),
                        digest) != 0) {
        return false;
    }
    const int offset = digest[19] & 0x0F;
    const uint32_t binary =
        (static_cast<uint32_t>(digest[offset] & 0x7F) << 24) |
        (static_cast<uint32_t>(digest[offset + 1]) << 16) |
        (static_cast<uint32_t>(digest[offset + 2]) << 8) |
        static_cast<uint32_t>(digest[offset + 3]);
    code = binary % 1000000u;
    return true;
}

std::vector<TotpSecret> loadTotp() {
    std::vector<TotpSecret> secrets;
    std::string data;
    if (!storage::readText(kTotpPath, data, kTotpMax, true)) {
        return secrets;
    }
    size_t pos = 0;
    while (pos < data.size() && secrets.size() < 16) {
        size_t end = data.find('\n', pos);
        if (end == std::string::npos) {
            end = data.size();
        }
        std::string line = data.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t separator = line.find('|');
        if (separator == std::string::npos) {
            continue;
        }
        TotpSecret secret;
        secret.name = trimCopy(line.substr(0, separator));
        secret.source = trimCopy(line.substr(separator + 1));
        if (secret.name.empty() || secret.source.empty()) {
            continue;
        }
        if (!base32Decode(secret.source, secret.key, sizeof(secret.key),
                          secret.keyLen)) {
            continue;
        }
        secrets.push_back(std::move(secret));
    }
    return secrets;
}

void drawTotp(const std::vector<TotpSecret>& secrets, int selected) {
    display::clear();
    display::header("TOTP", "Utility");

    if (secrets.empty()) {
        display::textCentered(44, "Nessun segreto.", display::kTextDim, 2);
        display::textCentered(60, "A = aggiungi", display::kText, 0);
        display::textCentered(74, "Formato: Nome|SECRET", display::kTextDim, 0);
        display::textCentered(86, "(segreto Base32)", display::kTextDim, 0);
        display::footer("A=aggiungi ESC=esci");
        display::push();
        return;
    }

    const TotpSecret& secret = secrets[selected];
    display::textCentered(25, shorten(secret.name, 30), display::kAccent, 2);
    char position[16];
    std::snprintf(position, sizeof(position), "%d/%d", selected + 1,
                  static_cast<int>(secrets.size()));
    display::textCentered(37, position, display::kTextDim, 0);

    const time_t now = time(nullptr);
    if (timeValid(now)) {
        uint32_t code = 0;
        if (hotpCode(secret, static_cast<uint64_t>(now) / 30, code)) {
            char digits[16];
            std::snprintf(digits, sizeof(digits), "%03u %03u", code / 1000,
                          code % 1000);
            display::textCentered(50, digits, display::kAccent, 4);
        } else {
            display::textCentered(50, "ERRORE", display::kDanger, 4);
        }
        const int left = 30 - static_cast<int>(now % 30);
        display::textCentered(72, std::to_string(left) + " s", display::kText, 0);
        display::progressBar(40, 88, 160, 6,
                             static_cast<float>(left) / 30.0f, display::kAccent);
    } else {
        display::textCentered(52, "-- -- --", display::kTextDim, 4);
        display::textCentered(74, "Ora non impostata.", display::kDanger, 0);
        display::textCentered(86, "Sincronizza da Orologio.", display::kTextDim, 0);
    }
    display::footer("A=nuovo ;.=cambia ESC=esci");
    display::push();
}

void addTotpSecret() {
    std::vector<textinput::Field> fields(2);
    fields[0].label = "Nome";
    fields[0].maxLen = 20;
    fields[1].label = "Segreto Base32";
    fields[1].maxLen = 64;
    fields[1].secret = true;
    if (!textinput::edit("Nuovo TOTP", "TOTP", fields)) {
        return;
    }

    std::string name = trimCopy(fields[0].value);
    const std::string source = trimCopy(fields[1].value);
    name.erase(std::remove(name.begin(), name.end(), '|'), name.end());
    name.erase(std::remove(name.begin(), name.end(), '\n'), name.end());
    name.erase(std::remove(name.begin(), name.end(), '\r'), name.end());
    if (name.empty() || source.empty()) {
        ui::message("TOTP", {"!Nome o segreto mancante."}, "TOTP");
        return;
    }

    uint8_t key[64];
    size_t keyLen = 0;
    if (!base32Decode(source, key, sizeof(key), keyLen)) {
        ui::message("TOTP", {"!Segreto Base32 non valido."}, "TOTP");
        return;
    }
    if (!ensureToolsDir()) {
        ui::message("TOTP", {"!microSD assente.", "I segreti stanno in /tools/totp.txt."},
                    "Utility");
        return;
    }
    if (!ui::confirm("TOTP",
                     {"Salvare il segreto in chiaro", "su /tools/totp.txt?",
                      "La microSD non e' cifrata.", "Chi la legge puo' generare i codici."},
                     "TOTP")) {
        return;
    }
    if (!appendLine(kTotpPath, name + "|" + source, kTotpMax)) {
        ui::message("TOTP", {"!Salvataggio non riuscito.", "Controlla la microSD."},
                    "TOTP");
    }
}

void totpScreen() {
    if (!ensureToolsDir()) {
        ui::message("TOTP", {"!microSD assente.", "I segreti stanno in /tools/totp.txt."},
                    "Utility");
        return;
    }
    std::vector<TotpSecret> secrets = loadTotp();
    int selected = 0;
    navigation::waitRelease();
    uint32_t lastDraw = 0;

    for (;;) {
        const uint32_t now = millis();
        if (lastDraw == 0 || now - lastDraw >= 250) {
            lastDraw = now;
            if (!secrets.empty() && selected >= static_cast<int>(secrets.size())) {
                selected = static_cast<int>(secrets.size()) - 1;
            }
            drawTotp(secrets, selected);
        }

        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            break;
        } else if (ev.type == navigation::Event::Up && !secrets.empty()) {
            selected = (selected + static_cast<int>(secrets.size()) - 1) %
                       static_cast<int>(secrets.size());
            lastDraw = 0;
        } else if (ev.type == navigation::Event::Down && !secrets.empty()) {
            selected = (selected + 1) % static_cast<int>(secrets.size());
            lastDraw = 0;
        } else if (ev.type == navigation::Event::Text &&
                   (ev.ch == 'a' || ev.ch == 'A')) {
            addTotpSecret();
            secrets = loadTotp();
            lastDraw = 0;
        }
        delay(5);
    }
}

}  // namespace

void runMenu() {
    ensureTimezone();
    MenuManager menu("Utility", "Menu principale");
    menu.addItem("Orologio", "globe", clockScreen);
    menu.addItem("Data e ora", "settings", manualTimeScreen);
    menu.addItem("Calendario", "info", calendarScreen);
    menu.addItem("Note", "file", notesScreen);
    menu.addItem("Calcolatrice", "chip", calculatorScreen);
    menu.addItem("QR code", "scan", qrScreen);
    menu.addItem("TOTP", "key", totpScreen);
    menu.setHint("ENTER=apri  ESC=indietro");
    menu.run(true);
}

}  // namespace tools

#endif  // ENABLE_MODULE_TOOLS
