#include "Ui.h"

#include <Arduino.h>
#include <algorithm>

#include "Display.h"
#include "Navigation.h"

namespace ui {

void drawScreen(const std::string& title, const std::string& parent,
                const std::vector<std::string>& lines, const std::string& hint,
                uint16_t accent) {
    display::clear(display::kPaper);
    display::header(title, parent);

    int y = 27;
    for (const auto& line : lines) {
        const uint16_t color = line.rfind("!", 0) == 0
                                   ? display::kNeoRed
                                   : (line.rfind(">", 0) == 0 ? display::kNeoGreen
                                                              : display::kInk);
        std::string s = line;
        if (!s.empty() && (s[0] == '!' || s[0] == '>')) {
            s.erase(0, 1);
        }
        display::text(5, y, s, color, 0);
        y += 11;
        if (y > display::height() - 20) {
            break;
        }
    }

    if (accent) {
        display::g().fillRect(3, 23, display::width() - 6,
                              display::height() - 37, display::kInk);
        display::g().fillRect(1, 21, display::width() - 6,
                              display::height() - 37, accent);
        display::g().drawRect(1, 21, display::width() - 6,
                              display::height() - 37, display::kInk);
    }
    display::footer(hint);
    display::push();
}

void message(const std::string& title, const std::vector<std::string>& lines,
             const std::string& parent, const std::string& hint) {
    navigation::waitRelease();
    bool done = false;
    while (!done) {
        drawScreen(title, parent, lines, hint, 0);
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Select || ev.type == navigation::Event::Back ||
            ev.type == navigation::Event::Text) {
            done = true;
        }
        delay(25);
    }
    navigation::waitRelease();
}

bool confirm(const std::string& title, const std::vector<std::string>& lines,
             const std::string& parent) {
    navigation::waitRelease();
    for (;;) {
        drawScreen(title, parent, lines, "ENTER = conferma    ` = annulla", display::kAccent);
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Select) {
            navigation::waitRelease();
            return true;
        }
        if (ev.type == navigation::Event::Back) {
            navigation::waitRelease();
            return false;
        }
        delay(25);
    }
}

bool legalWarning(const std::string& activity, const std::vector<std::string>& extra,
                  const std::string& parent) {
    std::vector<std::string> lines;
    lines.emplace_back("!ATTENZIONE: TRASMISSIONE RADIO");
    lines.emplace_back("Attivita': " + activity);
    lines.emplace_back("Usa solo su reti/dispositivi propri");
    lines.emplace_back("o con autorizzazione scritta.");
    lines.emplace_back("Ambiente RF isolato. Nessun uso");
    lines.emplace_back("su reti o persone terze.");
    for (const auto& e : extra) {
        lines.push_back(e);
    }
    lines.emplace_back(">Leggi il disclaimer nel README.");
    // Se ci sono troppe righe, compatta le ultime.
    if (lines.size() > 8) {
        lines.resize(8);
        lines.back() = ">Vedi README per il disclaimer.";
    }
    return confirm("Avviso legale", lines, parent);
}

void waitAnyKey(const std::string& title, const std::string& parent) {
    navigation::waitRelease();
    const std::vector<std::string> lines = {"Premi un tasto per continuare."};
    for (;;) {
        drawScreen(title, parent, lines, "qualsiasi tasto = continua", 0);
        const navigation::EventData ev = navigation::poll();
        if (ev.type != navigation::Event::None) {
            break;
        }
        delay(25);
    }
    navigation::waitRelease();
}

}  // namespace ui
