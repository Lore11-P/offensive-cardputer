#include "PowerTimer.h"

#include <Arduino.h>
#include <M5Cardputer.h>

#include <algorithm>
#include <cstdio>

#include "Config.h"
#include "Display.h"
#include "Logger.h"

namespace powertimer {
namespace {

// Durata dell'attenuazione prima dello spegnimento.
constexpr uint32_t kWarnMs = 10000;
// Luminosita' durante il preavviso: bassa ma leggibile.
constexpr uint8_t kDimBrightness = 12;
// Per quanto resta accesa la schermata di sospensione se non si e' in carica.
constexpr uint32_t kSuspendScreenMs = 8000;

uint32_t s_lastActivity = 0;
int s_holdCount = 0;
bool s_dimmed = false;
uint32_t s_dimDeadline = 0;
uint8_t s_savedBrightness = 128;

uint32_t timeoutMs() {
    return static_cast<uint32_t>(timeoutSeconds()) * 1000UL;
}

void restoreBrightness() {
    M5Cardputer.Display.setBrightness(s_savedBrightness);
}

void centerDirect(lgfx::LGFXBase& d, int y, const char* text, uint16_t color, uint8_t font) {
    if (font == 2) {
        d.setFont(&fonts::Font2);
    } else {
        d.setFont(&fonts::Font0);
    }
    d.setTextSize(1.0f);
    d.setTextDatum(0);
    d.setTextColor(color);
    d.drawString(text, (d.width() - d.textWidth(text)) / 2, y);
}

// Disegna direttamente sull'LCD: il framebuffer dei giochi resta intatto e
// alla ripresa basta ripubblicarlo, senza perdere la schermata precedente.
void drawSuspendScreen(bool charging, uint32_t seconds) {
    auto& d = M5Cardputer.Display;
    const int w = d.width();
    const int h = d.height();
    d.startWrite();
    d.fillScreen(display::kBlack);

    centerDirect(d, 14, "SOSPENSIONE", display::kTextDim, 0);

    const int level = static_cast<int>(M5Cardputer.Power.getBatteryLevel());
    const int pct = level < 0 ? -1 : std::min(level, 100);

    constexpr int kBodyW = 84;
    constexpr int kBodyH = 34;
    const int bx = (w - kBodyW) / 2;
    const int by = h / 2 - kBodyH / 2 - 6;
    const uint16_t color =
        charging ? display::kNeoCyan
                 : (pct >= 0 && pct <= 15 ? display::kNeoRed : display::kNeoGreen);

    d.drawRect(bx, by, kBodyW, kBodyH, display::kPaper);
    d.fillRect(bx + kBodyW, by + kBodyH / 3, 3, kBodyH / 3, display::kPaper);
    if (pct > 0) {
        d.fillRect(bx + 3, by + 3, (kBodyW - 6) * pct / 100, kBodyH - 6, color);
    }
    if (charging && seconds % 2 == 0) {
        const int cx = bx + kBodyW / 2;
        d.fillTriangle(cx + 7, by + 4, cx - 6, by + kBodyH / 2 + 2, cx + 1, by + kBodyH / 2 + 2,
                       display::kNeoYellow);
        d.fillTriangle(cx - 7, by + kBodyH - 4, cx + 6, by + kBodyH / 2 - 2, cx - 1,
                       by + kBodyH / 2 - 2, display::kNeoYellow);
    }

    char buf[16];
    if (pct < 0) {
        std::snprintf(buf, sizeof(buf), "--%%");
    } else {
        std::snprintf(buf, sizeof(buf), "%d%%", pct);
    }
    centerDirect(d, by + kBodyH + 8, buf, display::kText, 2);

    if (charging) {
        centerDirect(d, h - 34, "IN RICARICA", display::kNeoCyan, 2);
    } else {
        centerDirect(d, h - 34, "Premi un tasto per riaccendere", display::kTextDim, 0);
    }
    d.endWrite();
}

}  // namespace

void begin() {
    s_lastActivity = millis();
    s_holdCount = 0;
    s_dimmed = false;
    s_dimDeadline = 0;
    logger::info("Spegnimento auto: %u s (0 = off)",
                 static_cast<unsigned>(timeoutSeconds()));
}

uint32_t timeoutSeconds() {
    return config::sleepTimeoutSec();
}

void activity() {
    s_lastActivity = millis();
    if (s_dimmed) {
        // Il primo tasto annulla l'attenuazione e riparte il conteggio.
        s_dimmed = false;
        s_dimDeadline = 0;
        restoreBrightness();
        logger::info("Spegnimento annullato");
    }
}

void tick() {
    if (s_holdCount > 0) {
        return;
    }
    const uint32_t limit = timeoutMs();
    if (limit == 0) {
        return;
    }

    // Fase 2: schermo attenuato, si attende lo spegnimento. Nessuna schermata
    // modale: la UI continua a essere disegnata, solo molto piu' scura.
    if (s_dimmed) {
        if (millis() >= s_dimDeadline) {
            restoreBrightness();
            s_dimmed = false;
            powerOff();
        }
        return;
    }

    // Fase 1: e' scaduto il timeout di inattivita'.
    if (millis() - s_lastActivity < limit) {
        return;
    }
    s_savedBrightness = M5Cardputer.Display.getBrightness();
    if (s_savedBrightness < kDimBrightness) {
        s_savedBrightness = kDimBrightness;  // evita di "schiarire" al ripristino
    }
    M5Cardputer.Display.setBrightness(kDimBrightness);
    s_dimmed = true;
    s_dimDeadline = millis() + kWarnMs;
    logger::info("Spegnimento in %u s: schermo attenuato",
                 static_cast<unsigned>(kWarnMs / 1000));
}

void powerOff() {
    // Sospensione software: il deep sleep della Cardputer ADV non ha una
    // sorgente di risveglio configurata e il dispositivo non si riaccendeva.
    // Qui si spengono retroilluminazione e CPU resta a 80 MHz, con risveglio
    // immediato al primo tasto (tastiera o G0).
    logger::info("Sospensione");
    M5Cardputer.Speaker.stop();

    // Attende il rilascio dei tasti per non risvegliarsi subito.
    uint32_t start = millis();
    while (M5Cardputer.Keyboard.isPressed() != 0 && millis() - start < 2000) {
        M5Cardputer.update();
        delay(20);
    }

    const uint8_t previousBrightness = M5Cardputer.Display.getBrightness();
    const uint32_t previousCpuMhz = getCpuFrequencyMhz();
    display::resetChargingDetector();
    M5Cardputer.Display.setBrightness(kDimBrightness);
    drawSuspendScreen(false, 0);

    setCpuFrequencyMhz(80);
    const uint32_t suspendStart = millis();
    uint32_t lastRefresh = 0;
    int16_t suspendRefMv = -1;
    bool charging = false;
    bool screenOn = true;

    for (;;) {
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isPressed() != 0 || M5Cardputer.BtnA.isPressed()) {
            break;
        }
        const uint32_t now = millis();
        if (now - lastRefresh >= 1000) {
            lastRefresh = now;
            // Riferimento di tensione preso a schermo gia' attenuato: serve a
            // riconoscere anche una ricarica iniziata prima della sospensione.
            const int16_t mv = M5Cardputer.Power.getBatteryVoltage();
            if (suspendRefMv < 0 && now - suspendStart >= 5000 && mv > 0) {
                suspendRefMv = mv;
            }
            charging = display::chargingLikely() ||
                       (suspendRefMv > 0 && mv > suspendRefMv + 25);
            if (charging) {
                if (!screenOn) {
                    screenOn = true;
                    M5Cardputer.Display.setBrightness(kDimBrightness);
                }
            } else if (screenOn && now - suspendStart > kSuspendScreenMs) {
                screenOn = false;
                M5Cardputer.Display.setBrightness(0);
            }
            if (screenOn) {
                drawSuspendScreen(charging, now / 1000);
            }
        }
        delay(20);
    }

    // Attende il rilascio del tasto che ha svegliato il dispositivo.
    start = millis();
    while ((M5Cardputer.Keyboard.isPressed() != 0 || M5Cardputer.BtnA.isPressed()) &&
           millis() - start < 2000) {
        M5Cardputer.update();
        delay(20);
    }

    setCpuFrequencyMhz(previousCpuMhz);
    M5Cardputer.Display.setBrightness(previousBrightness);
    // Ripubblica l'ultimo framebuffer del gioco/menu: la schermata di
    // sospensione era disegnata direttamente sull'LCD.
    display::push();
    logger::info("Ripresa");
    begin();
}

Hold::Hold() {
    ++s_holdCount;
}

Hold::~Hold() {
    if (s_holdCount > 0) {
        --s_holdCount;
    }
    s_lastActivity = millis();  // alla ripresa il conteggio riparte da zero
}

}  // namespace powertimer
