#include <cstdio>
#include <string>

#include "CalcExpr.h"
#include "Display.h"
#include "Navigation.h"
#include "ToolsInternal.h"
#include "orbit_compat.h"

namespace orbit_tools {
namespace {

constexpr size_t kCalcMax = 48;

bool isCalcChar(char c) {
    return (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '*' || c == '/' ||
           c == '%' || c == '(' || c == ')' || c == '.';
}

void drawCalculator(const std::string& expression, bool hasResult, bool resultOk, bool divZero,
                    double result) {
    display::clear(display::C(display::T_bg));
    display::header("Calcolatrice", "Strumenti");
    display::text(8, 27, "ESPRESSIONE", display::C(display::T_dim), 0);

    std::string shown = expression;
    if (shown.size() > 30) {
        shown = ".." + shown.substr(shown.size() - 28);
    }
    display::text(8, 39, shown.empty() ? std::string("_") : shown,
                  display::C(display::T_text), 2);
    display::g().drawFastHLine(8, 55, display::width() - 16, display::C(display::T_line));

    display::text(8, 61, "RISULTATO", display::C(display::T_dim), 0);
    if (hasResult) {
        if (resultOk) {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.6g", result);
            display::textCentered(74, buffer, display::C(display::T_acc), 4);
        } else {
            display::textCentered(74, divZero ? "DIV/0" : "ERRORE",
                                  display::C(display::T_err), 4);
        }
    } else {
        display::textCentered(74, "0", display::C(display::T_dim), 4);
    }

    display::text(8, 106, "ENTER o = calcola  BKSP cancella",
                  display::C(display::T_dim), 0);
    display::footer("`=esci");
    display::push();
}

}  // namespace

void calculatorScreen() {
    std::string expression;
    bool hasResult = false;
    bool resultOk = false;
    bool divZero = false;
    double result = 0.0;
    bool dirty = true;
    uint16_t lastBattery = 0;
    uint32_t lastStatus = millis();

    navigation::waitRelease();
    for (;;) {
        if (dirty) {
            dirty = false;
            drawCalculator(expression, hasResult, resultOk, divZero, result);
            lastBattery = display::batterySignature();
        }
        const uint32_t now = millis();
        if (now - lastStatus >= 500) {
            lastStatus = now;
            refreshHeader("Calcolatrice", "Strumenti", lastBattery);
        }

        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            if (navigation::backtickPressed()) {
                break;
            }
            if (!expression.empty()) {
                expression.pop_back();
                hasResult = false;
                dirty = true;
            }
            continue;
        }
        if (ev.type == navigation::Event::Select) {
            hasResult = true;
            resultOk = calc::evaluate(expression, result, &divZero);
            dirty = true;
            continue;
        }

        const char c = eventChar(ev);
        if (c == '=') {
            hasResult = true;
            resultOk = calc::evaluate(expression, result, &divZero);
            dirty = true;
        } else if (isCalcChar(c) && expression.size() < kCalcMax) {
            expression.push_back(c);
            hasResult = false;
            dirty = true;
        }
        delay(15);
    }
    navigation::waitRelease();
}

}  // namespace orbit_tools
