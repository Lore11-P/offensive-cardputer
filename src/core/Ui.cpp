#include "Ui.h"

#include <Arduino.h>
#include <algorithm>

#include "Display.h"
#include "Navigation.h"
#include "Orbit.h"

namespace ui {

void drawScreen(const std::string& title, const std::string& parent,
                const std::vector<std::string>& lines, const std::string& hint,
                uint16_t accent) {
    display::clear(display::C(display::T_bg));
    display::header(title, parent);

    auto& d = display::g();
    const int panelY = 24;
    const int panelH = display::height() - panelY - 15;

    // Pannello strumentazione: cornice, tacche e striscia d'allarme.
    d.fillRect(3, panelY, display::width() - 6, panelH, display::C(display::T_bg2));
    d.drawRect(3, panelY, display::width() - 6, panelH, display::C(display::T_line));
    d.fillRect(3, panelY, display::width() - 6, 1, accent ? accent : display::C(display::T_sec));

    // Tacche di scala sul bordo destro.
    for (int y = panelY + 4; y < panelY + panelH - 2; y += 5) {
        d.drawFastHLine(display::width() - 8, y, 3, display::C(display::T_line2));
    }
    // Angoli.
    d.drawFastHLine(3, panelY, 6, accent ? accent : display::C(display::T_acc));
    d.drawFastVLine(3, panelY, 6, accent ? accent : display::C(display::T_acc));

    // Riga di telemetria in fondo al pannello (riservata prima del testo).
    const int teleY = panelY + panelH - 8;
    const int sepY = teleY - 3;

    int y = panelY + 6;
    for (const auto& line : lines) {
        if (y + 8 > sepY) {
            break;
        }
        uint16_t color = display::C(display::T_text);
        std::string s = line;
        if (!s.empty() && s[0] == '!') {
            color = display::C(display::T_err);
            s.erase(0, 1);
            // Barra d'allarme a sinistra.
            d.fillRect(5, y + 1, 2, 6, display::C(display::T_err));
        } else if (!s.empty() && s[0] == '>') {
            color = display::C(display::T_acc);
            s.erase(0, 1);
            d.fillRect(5, y + 1, 2, 6, display::C(display::T_acc));
        }
        display::text(11, y, s, color, 0);
        y += 10;
    }

    d.drawFastHLine(6, sepY, display::width() - 14, display::C(display::T_line2));
    orbit::signalBars(8, teleY, 3, display::C(display::T_ok), display::C(display::T_line2));
    display::text(22, teleY, "LINK OK", display::C(display::T_dim), 0);
    display::text(display::width() - 30, teleY, "SYS", display::C(display::T_dim), 0);

    display::footer(hint);
    display::push();
}

void message(const std::string& title, const std::vector<std::string>& lines,
             const std::string& parent, const std::string& hint) {
    navigation::waitRelease();
    uint32_t lastDraw = 0;
    bool done = false;
    while (!done) {
        const uint32_t now = millis();
        if (lastDraw == 0 || now - lastDraw >= 500) {
            drawScreen(title, parent, lines, hint, 0);
            lastDraw = now;
        }
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
    uint32_t lastDraw = 0;
    for (;;) {
        const uint32_t now = millis();
        if (lastDraw == 0 || now - lastDraw >= 500) {
            drawScreen(title, parent, lines, "ENTER = conferma    ESC = annulla",
                       display::C(display::T_acc));
            lastDraw = now;
        }
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
    if (lines.size() > 8) {
        lines.resize(8);
        lines.back() = ">Vedi README per il disclaimer.";
    }
    return confirm("Avviso legale", lines, parent);
}

void waitAnyKey(const std::string& title, const std::string& parent) {
    navigation::waitRelease();
    const std::vector<std::string> lines = {"Premi un tasto per continuare."};
    uint32_t lastDraw = 0;
    for (;;) {
        const uint32_t now = millis();
        if (lastDraw == 0 || now - lastDraw >= 500) {
            drawScreen(title, parent, lines, "qualsiasi tasto = continua", 0);
            lastDraw = now;
        }
        const navigation::EventData ev = navigation::poll();
        if (ev.type != navigation::Event::None) {
            break;
        }
        delay(25);
    }
    navigation::waitRelease();
}

}  // namespace ui
