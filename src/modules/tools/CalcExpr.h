#pragma once

#include <cmath>
#include <string>

// Valutatore di espressioni per la calcolatrice.
// Grammatica: expr := term (('+'|'-') term)*
//             term := factor (('*'|'/'|'%') factor)*
//             factor := ('+'|'-') factor | numero | '(' expr ')'
// '%' e' il resto (fmod), non la percentuale.
namespace calc {

namespace detail {

struct Parser {
    const char* p = nullptr;
    bool ok = true;
    int depth = 0;

    void skipSpace() {
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
    }

    double number() {
        skipSpace();
        const char* start = p;
        double value = 0.0;
        double frac = 0.1;
        bool any = false;
        while (*p >= '0' && *p <= '9') {
            any = true;
            value = value * 10.0 + (*p - '0');
            ++p;
        }
        if (*p == '.') {
            ++p;
            while (*p >= '0' && *p <= '9') {
                any = true;
                value += (*p - '0') * frac;
                frac *= 0.1;
                ++p;
            }
        }
        if (!any) {
            ok = false;
            p = start;
            return 0.0;
        }
        return value;
    }

    double factor() {
        skipSpace();
        if (depth > 24) {
            ok = false;
            return 0.0;
        }
        if (*p == '+') {
            ++p;
            return factor();
        }
        if (*p == '-') {
            ++p;
            return -factor();
        }
        if (*p == '(') {
            ++p;
            ++depth;
            const double value = expression();
            --depth;
            skipSpace();
            if (*p != ')') {
                ok = false;
                return 0.0;
            }
            ++p;
            return value;
        }
        return number();
    }

    double term() {
        double value = factor();
        for (;;) {
            skipSpace();
            if (*p == '*') {
                ++p;
                value *= factor();
            } else if (*p == '/') {
                ++p;
                const double divisor = factor();
                if (divisor == 0.0) {
                    ok = false;
                    return 0.0;
                }
                value /= divisor;
            } else if (*p == '%') {
                ++p;
                const double divisor = factor();
                if (divisor == 0.0) {
                    ok = false;
                    return 0.0;
                }
                value = std::fmod(value, divisor);
            } else {
                return value;
            }
            if (!ok) {
                return 0.0;
            }
        }
    }

    double expression() {
        double value = term();
        for (;;) {
            skipSpace();
            if (*p == '+') {
                ++p;
                value += term();
            } else if (*p == '-') {
                ++p;
                value -= term();
            } else {
                return value;
            }
            if (!ok) {
                return 0.0;
            }
        }
    }
};

}  // namespace detail

// true se l'intera stringa e' un'espressione valida e finita.
inline bool evaluate(const std::string& text, double& out) {
    if (text.empty()) {
        return false;
    }
    detail::Parser parser;
    parser.p = text.c_str();
    double value = parser.expression();
    parser.skipSpace();
    if (!parser.ok || *parser.p != '\0' || !std::isfinite(value)) {
        return false;
    }
    // Evita "-0" e rumore numerico sulle sottrazioni.
    if (std::fabs(value) < 1e-12) {
        value = 0.0;
    }
    out = value;
    return true;
}

}  // namespace calc
