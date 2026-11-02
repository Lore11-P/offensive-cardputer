#include "PowerTimer.h"

#include <Arduino.h>
#include <M5Cardputer.h>

#include "Config.h"
#include "Display.h"
#include "Logger.h"

namespace powertimer {
namespace {

// Durata dell'attenuazione prima dello spegnimento.
constexpr uint32_t kWarnMs = 10000;
// Luminosita' durante il preavviso: bassa ma leggibile.
constexpr uint8_t kDimBrightness = 12;

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
    logger::info("Spegnimento");
    display::clear();
    display::textCentered(58, "Spegnimento...", display::kTextDim, 2);
    display::push();
    delay(150);

    M5Cardputer.Power.powerOff();
    // Se il PMIC non gestisce lo spegnimento la chiamata ritorna: si passa allo
    // sleep profondo senza timer di risveglio (si riaccende dal pulsante).
    logger::warn("powerOff non supportato, deep sleep");
    M5Cardputer.Power.deepSleep();
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
