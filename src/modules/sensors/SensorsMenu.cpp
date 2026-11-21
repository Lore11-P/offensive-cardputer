#include "SensorsMenu.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <Preferences.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "Config.h"
#include "Display.h"
#include "MenuManager.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "PowerTimer.h"
#include "Ui.h"

#if ENABLE_MODULE_SENSORS

namespace sensors {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kRadToDeg = 57.2957795f;
constexpr uint32_t kFrameMs = 33;       // ~30 Hz per livella/goniometro/pedometro
constexpr uint32_t kTorchFrameMs = 40;  // 25 Hz bastano per il lampeggio SOS
constexpr uint32_t kSaveMs = 60000;     // NVS: mai a ogni passo
constexpr uint32_t kSosUnitMs = 120;

// --- ingressi/uscite comuni -------------------------------------------------

void drawPanel() {
    auto& d = display::g();
    const int y = 24;
    const int h = display::height() - y - 15;
    d.fillRect(3, y, display::width() - 6, h, display::C(display::T_bg2));
    d.drawRect(3, y, display::width() - 6, h, display::C(display::T_line));
    d.fillRect(3, y, display::width() - 6, 1, display::C(display::T_sec));
}

std::string fmtSigned(float v) {
    char b[12];
    std::snprintf(b, sizeof(b), "%+5.1f", static_cast<double>(v));
    return std::string(b);
}

std::string fmtAngle(float v) {
    char b[12];
    std::snprintf(b, sizeof(b), "%4.1f", static_cast<double>(v));
    return std::string(b);
}

void degreeMark(int x, int y, uint16_t color) {
    display::g().drawCircle(x + 4, y + 3, 2, color);
}

bool imuEnsure() {
    if (!M5.Imu.isEnabled()) {
        M5.Imu.begin();
    }
    return M5.Imu.isEnabled();
}

bool imuRead(float& ax, float& ay, float& az) {
    M5.Imu.update();
    if (!M5.Imu.getAccel(&ax, &ay, &az)) {
        return false;
    }
    // Z positiva verso l'alto: la livella funziona anche se il sensore e'
    // montato ruotato o il dispositivo e' appoggiato a faccia in giu'.
    if (az < 0.0f) {
        ax = -ax;
        ay = -ay;
        az = -az;
    }
    return true;
}

// L'IMU non viene riconfigurato: all'uscita non resta alcun offset o lock.
void imuNeutral() {
    M5.Imu.clearOffsetData();
}

// Forza il formato orizzontale (240x135) e ripristina la rotazione scelta.
uint8_t g_savedRotation = 1;
bool g_rotationOverridden = false;

void enterLandscape() {
    g_savedRotation = config::rotation();
    g_rotationOverridden = true;
    const uint8_t candidates[5] = {g_savedRotation, 0, 1, 2, 3};
    for (uint8_t i = 0; i < 5; ++i) {
        display::applyRotation(candidates[i]);
        if (display::width() > display::height()) {
            return;
        }
    }
}

void exitLandscape() {
    if (g_rotationOverridden) {
        display::applyRotation(g_savedRotation);
        g_rotationOverridden = false;
    }
}

// --- Livella ----------------------------------------------------------------

void drawLevelFrame(float roll, float pitch, bool zeroSet, bool flashZero) {
    display::clear(display::C(display::T_bg));
    display::header("Livella", "Sensori");
    drawPanel();

    auto& d = display::g();
    const int cx = 62;
    const int cy = 72;
    const int r = 40;

    d.drawCircle(cx, cy, r, display::C(display::T_line));
    d.drawCircle(cx, cy, r / 2, display::C(display::T_line2));
    d.drawFastHLine(cx - r, cy, r * 2, display::C(display::T_line2));
    d.drawFastVLine(cx, cy - r, r * 2, display::C(display::T_line2));

    for (int a = 0; a < 360; a += 45) {
        const float rad = static_cast<float>(a) * kPi / 180.0f;
        const int x1 = cx + static_cast<int>(cosf(rad) * (r - 4));
        const int y1 = cy + static_cast<int>(sinf(rad) * (r - 4));
        const int x2 = cx + static_cast<int>(cosf(rad) * r);
        const int y2 = cy + static_cast<int>(sinf(rad) * r);
        d.drawLine(x1, y1, x2, y2, display::C(display::T_line2));
    }

    // 25 gradi a fondo scala; oltre, la bolla resta sul bordo.
    float nx = -roll / 25.0f;
    float ny = pitch / 25.0f;
    const float len = sqrtf(nx * nx + ny * ny);
    if (len > 1.0f) {
        nx /= len;
        ny /= len;
    }
    const int bx = cx + static_cast<int>(nx * (r - 7));
    const int by = cy + static_cast<int>(ny * (r - 7));
    const bool centered = std::fabs(roll) < 1.0f && std::fabs(pitch) < 1.0f;
    d.fillCircle(bx, by, 5, centered ? display::C(display::T_ok) : display::C(display::T_acc));
    d.drawCircle(bx, by, 6, display::C(display::T_hi));
    d.drawPixel(cx, cy, display::C(display::T_text));

    d.drawFastVLine(108, 28, 88, display::C(display::T_line2));

    display::text(114, 30, "X", display::C(display::T_dim), 0);
    display::text(126, 26, fmtSigned(roll), display::C(display::T_text), 4);
    display::text(114, 56, "Y", display::C(display::T_dim), 0);
    display::text(126, 52, fmtSigned(pitch), display::C(display::T_text), 4);

    display::text(114, 80, "RIF", display::C(display::T_dim), 0);
    display::text(136, 80, zeroSet ? "ZERO" : "ASSOLUTO",
                  zeroSet ? display::C(display::T_ok) : display::C(display::T_dim), 0);

    if (flashZero) {
        display::text(114, 98, zeroSet ? "ZERO OK" : "ZERO RESET",
                      display::C(display::T_acc), 0);
    }

    display::footer("ENTER=zero  R=reset  ESC=esci");
    display::push();
}

void runLevel() {
    if (!imuEnsure()) {
        ui::message("Livella", {"!IMU non disponibile.", "Il BMI270 non risponde."}, "Sensori");
        return;
    }
    powertimer::Hold hold;
    navigation::waitRelease();

    float zeroX = 0.0f;
    float zeroY = 0.0f;
    float smX = 0.0f;
    float smY = 0.0f;
    bool have = false;
    bool calibrated = false;
    uint32_t flashUntil = 0;
    uint32_t nextFrame = 0;

    for (;;) {
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            break;
        }
        const uint32_t now = millis();
        if (now < nextFrame) {
            delay(2);
            continue;
        }
        nextFrame = now + kFrameMs;

        float ax = 0.0f;
        float ay = 0.0f;
        float az = 1.0f;
        if (!imuRead(ax, ay, az)) {
            continue;
        }
        const float rawX = atan2f(ax, az) * kRadToDeg;
        const float rawY = atan2f(ay, az) * kRadToDeg;
        if (!have) {
            smX = rawX;
            smY = rawY;
            have = true;
        } else {
            smX += 0.20f * (rawX - smX);
            smY += 0.20f * (rawY - smY);
        }

        if (ev.type == navigation::Event::Select) {
            zeroX = smX;
            zeroY = smY;
            calibrated = true;
            flashUntil = now + 1200;
        } else if (ev.type == navigation::Event::Text && (ev.ch == 'r' || ev.ch == 'R')) {
            zeroX = 0.0f;
            zeroY = 0.0f;
            calibrated = false;
            flashUntil = now + 1200;
        }

        drawLevelFrame(smX - zeroX, smY - zeroY, calibrated,
                       now - flashUntil < 1200);
    }

    imuNeutral();
}

// --- Goniometro -------------------------------------------------------------

void runGoniometer() {
    if (!imuEnsure()) {
        ui::message("Goniometro", {"!IMU non disponibile.", "Il BMI270 non risponde."}, "Sensori");
        return;
    }
    powertimer::Hold hold;
    navigation::waitRelease();

    float smoothed = 0.0f;
    bool have = false;
    bool locked = false;
    float lockedAngle = 0.0f;
    uint32_t nextFrame = 0;

    for (;;) {
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            break;
        }
        const uint32_t now = millis();
        if (now < nextFrame) {
            delay(2);
            continue;
        }
        nextFrame = now + kFrameMs;

        float ax = 0.0f;
        float ay = 0.0f;
        float az = 1.0f;
        if (!imuRead(ax, ay, az)) {
            continue;
        }
        const float raw =
            atan2f(sqrtf(ax * ax + ay * ay), std::fabs(az)) * kRadToDeg;
        if (!have) {
            smoothed = raw;
            have = true;
        } else {
            smoothed += 0.25f * (raw - smoothed);
        }

        if (ev.type == navigation::Event::Select) {
            locked = !locked;
            if (locked) {
                lockedAngle = smoothed;
            }
        } else if (ev.type == navigation::Event::Text && (ev.ch == 'r' || ev.ch == 'R')) {
            locked = false;
            smoothed = raw;
            have = true;
        }

        const float shown = locked ? lockedAngle : smoothed;

        display::clear(display::C(display::T_bg));
        display::header("Goniometro", "Sensori");
        drawPanel();

        auto& d = display::g();
        const int cx = 64;
        const int cy = 100;
        const int r = 42;

        for (int a = 0; a <= 90; ++a) {
            const float rad = static_cast<float>(360 - a) * kPi / 180.0f;
            d.drawPixel(cx + static_cast<int>(cosf(rad) * r),
                        cy + static_cast<int>(sinf(rad) * r), display::C(display::T_line));
        }
        for (int a = 0; a <= 90; a += 15) {
            const float ga = static_cast<float>(360 - a) * kPi / 180.0f;
            const int x1 = cx + static_cast<int>(cosf(ga) * (r - 4));
            const int y1 = cy + static_cast<int>(sinf(ga) * (r - 4));
            const int x2 = cx + static_cast<int>(cosf(ga) * r);
            const int y2 = cy + static_cast<int>(sinf(ga) * r);
            d.drawLine(x1, y1, x2, y2, display::C(display::T_line2));
        }
        d.drawFastHLine(cx - r, cy, r, display::C(display::T_line2));
        const float needle = static_cast<float>(360.0f - shown) * kPi / 180.0f;
        const int ex = cx + static_cast<int>(cosf(needle) * (r - 2));
        const int ey = cy + static_cast<int>(sinf(needle) * (r - 2));
        d.drawLine(cx, cy, ex, ey, display::C(display::T_acc));
        d.fillCircle(ex, ey, 3, display::C(display::T_acc));
        d.fillCircle(cx, cy, 2, display::C(display::T_text));

        display::text(7, 72, "0", display::C(display::T_dim), 0);
        display::text(55, 40, "90", display::C(display::T_dim), 0);

        d.drawFastVLine(116, 28, 88, display::C(display::T_line2));
        display::text(124, 30, "ANGOLO", display::C(display::T_dim), 0);
        const std::string value = fmtAngle(shown);
        display::text(124, 42, value, display::C(display::T_text), 4);
        degreeMark(124 + display::textWidth(value, 4), 42, display::C(display::T_text));
        display::text(124, 64, "DAL PIANO", display::C(display::T_dim), 0);

        d.fillRect(124, 80, 54, 13,
                   locked ? display::C(display::T_warn) : display::C(display::T_okD));
        display::text(130, 83, locked ? "LOCK" : "LIVE",
                      locked ? display::C(display::T_ink) : display::C(display::T_okL), 0);

        display::footer("ENTER=lock  R=reset  ESC=esci");
        display::push();
    }

    imuNeutral();
}

// --- Pedometro --------------------------------------------------------------

uint32_t epochDayNow() {
    const time_t now = time(nullptr);
    // Orologio non sincronizzato (nessun modulo fa NTP oggi).
    if (now < 1700000000) {
        return 0;
    }
    return static_cast<uint32_t>(now / 86400);
}

void runPedometer() {
    if (!imuEnsure()) {
        ui::message("Pedometro", {"!IMU non disponibile.", "Il BMI270 non risponde."}, "Sensori");
        return;
    }
    powertimer::Hold hold;
    navigation::waitRelease();

    Preferences prefs;
    const bool nvs = prefs.begin("orbit_sensors", false);
    uint32_t steps = nvs ? prefs.getUInt("steps", 0) : 0;
    uint32_t day = nvs ? prefs.getUInt("day", 0) : 0;
    uint32_t baseUp = nvs ? prefs.getUInt("up", 0) : 0;
    uint32_t sessionStart = millis();

    const uint32_t today = epochDayNow();
    if (today != 0) {
        if (day != 0 && day != today) {
            steps = 0;
            baseUp = 0;
        }
        day = today;
    } else if (baseUp >= 86400) {
        steps = 0;
        baseUp = 0;
    }

    uint8_t buckets[20] = {0};  // 20 x 3 s = ultimo minuto
    uint32_t bucketStart = millis();
    float gravity = 1.0f;
    bool armed = true;
    uint32_t lastStepMs = millis() - 1000;
    uint32_t lastKeyMs = millis() - 1000;
    uint32_t lastSave = millis();
    uint32_t lastDayCheck = millis();
    uint32_t flashUntil = 0;
    uint32_t messageUntil = 0;
    uint32_t nextFrame = 0;

    auto save = [&]() {
        if (!nvs) {
            return;
        }
        const uint32_t up = baseUp + (millis() - sessionStart) / 1000;
        prefs.putUInt("steps", steps);
        prefs.putUInt("day", day);
        prefs.putUInt("up", day != 0 ? 0 : up);
        lastSave = millis();
    };

    for (;;) {
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            break;
        }
        uint32_t now = millis();

        // La tastiera e' solidale all'IMU: ogni tasto (anche una freccia)
        // sospende il conteggio per 350 ms.
        if (ev.type != navigation::Event::None || M5Cardputer.Keyboard.isPressed() != 0) {
            lastKeyMs = now;
        }

        if (ev.type == navigation::Event::Text && (ev.ch == 'r' || ev.ch == 'R')) {
            if (ui::confirm("Pedometro",
                            {"Azzerare il conteggio", "dei passi di oggi?",
                             ">L'operazione non si annulla."},
                            "Sensori")) {
                steps = 0;
                baseUp = 0;
                sessionStart = millis();
                bucketStart = millis();
                std::memset(buckets, 0, sizeof(buckets));
                day = epochDayNow();
                save();
                messageUntil = millis() + 1500;
            }
            nextFrame = 0;  // ridisegna subito dopo la finestra modale
            now = millis();
        }

        if (now < nextFrame) {
            delay(2);
            continue;
        }
        nextFrame = now + kFrameMs;

        float ax = 0.0f;
        float ay = 0.0f;
        float az = 1.0f;
        if (!imuRead(ax, ay, az)) {
            continue;
        }

        // Rotazione della finestra del grafico (non tocca la NVS).
        if (now - bucketStart >= 3000) {
            const uint32_t n = (now - bucketStart) / 3000;
            if (n >= 20) {
                std::memset(buckets, 0, sizeof(buckets));
            } else {
                std::memmove(buckets, buckets + n, 20 - n);
                std::memset(buckets + 20 - n, 0, n);
            }
            bucketStart += n * 3000;
        }

        const float mag = sqrtf(ax * ax + ay * ay + az * az);
        gravity += 0.12f * (mag - gravity);
        const float dev = mag - gravity;

        if (now - lastKeyMs > 350) {
            if (armed) {
                if (dev > 0.13f && now - lastStepMs >= 280) {
                    ++steps;
                    if (buckets[19] < 255) {
                        ++buckets[19];
                    }
                    lastStepMs = now;
                    flashUntil = now + 250;
                    armed = false;
                }
            } else if (dev < 0.03f) {
                armed = true;
            }
        } else if (dev < 0.03f) {
            armed = true;  // dopo la digitazione si riparte da fermo
        }

        // Rollover giornaliero: data di sistema se disponibile, altrimenti
        // 24 h di uptime accumulato; in mancanza di entrambi vale il reset R.
        if (now - lastDayCheck >= 30000) {
            lastDayCheck = now;
            const uint32_t currentDay = epochDayNow();
            if (currentDay != 0) {
                if (day != 0 && day != currentDay) {
                    steps = 0;
                    baseUp = 0;
                    sessionStart = now;
                    std::memset(buckets, 0, sizeof(buckets));
                    save();
                    messageUntil = now + 1500;
                }
                day = currentDay;
            } else if (day == 0) {
                const uint32_t up = baseUp + (now - sessionStart) / 1000;
                if (up >= 86400) {
                    steps = 0;
                    baseUp = 0;
                    sessionStart = now;
                    std::memset(buckets, 0, sizeof(buckets));
                    save();
                    messageUntil = now + 1500;
                }
            }
        }

        // Scrittura NVS periodica, mai a ogni passo.
        if (nvs && now - lastSave >= kSaveMs) {
            save();
        }

        uint32_t perMinute = 0;
        for (uint8_t b : buckets) {
            perMinute += b;
        }

        display::clear(display::C(display::T_bg));
        display::header("Pedometro", "Sensori");
        drawPanel();

        auto& d = display::g();
        display::text(10, 28, "PASSI", display::C(display::T_dim), 0);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(steps));
        display::text(10, 38, buf, display::C(display::T_text), 4);
        display::text(10, 60, "OGGI", display::C(display::T_dim), 0);

        if (now - flashUntil < 250) {
            display::text(10, 78, "PASSO", display::C(display::T_acc), 2);
        } else if (now - messageUntil < 1500) {
            display::text(10, 78, "AZZERATO", display::C(display::T_warn), 2);
        } else if (!nvs) {
            display::text(10, 78, "!NVS ERR", display::C(display::T_err), 0);
        }

        display::text(120, 28, "PASSI/MIN", display::C(display::T_dim), 0);
        std::snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(perMinute));
        display::text(120, 38, buf, display::C(display::T_text), 4);

        display::text(106, 70, "ULTIMO MINUTO", display::C(display::T_dim), 0);
        const int gx = 106;
        const int gy = 112;
        d.drawFastHLine(gx, gy, 122, display::C(display::T_line));
        for (int i = 0; i < 20; ++i) {
            int bh = static_cast<int>(buckets[i]) * 5;
            if (bh > 25) {
                bh = 25;
            }
            if (bh > 0) {
                d.fillRect(gx + i * 6, gy - bh, 4, bh,
                           i == 19 ? display::C(display::T_acc) : display::C(display::T_accD));
            }
        }

        display::footer("R=azzera  ESC=esci");
        display::push();
    }

    save();
    prefs.end();
    imuNeutral();
}

// --- Torcia -----------------------------------------------------------------

struct TorchColor {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    const char* name;
    uint16_t ui;
};

const TorchColor kTorchColors[] = {
    {255, 255, 255, "BIANCO", 0xFFFF},
    {255, 0, 0, "ROSSO", 0xF800},
    {0, 255, 0, "VERDE", 0x07E0},
};
constexpr uint8_t kTorchColorCount = 3;

void ledOffHard() {
    M5.Led.setBrightness(0);
    M5.Led.setAllColor(static_cast<uint8_t>(0), static_cast<uint8_t>(0),
                       static_cast<uint8_t>(0));
    M5.Led.display();
}

bool sosLit(uint32_t elapsedMs) {
    static const uint8_t kDur[18] = {1, 1, 1, 1, 1, 3, 3, 1, 3,
                                     1, 3, 3, 1, 1, 1, 1, 1, 7};
    static const bool kOn[18] = {true,  false, true,  false, true, false,
                                 true,  false, true,  false, true, false,
                                 true,  false, true,  false, true, false};
    uint32_t phase = (elapsedMs / kSosUnitMs) % 33;
    uint8_t acc = 0;
    for (uint8_t i = 0; i < 18; ++i) {
        acc += kDur[i];
        if (phase < acc) {
            return kOn[i];
        }
    }
    return false;
}

void runTorch() {
    navigation::waitRelease();
    // Allinea il timeout globale al nostro conteggio: cosi' il LED viene
    // spento prima che powertimer::powerOff() blocchi il loop.
    powertimer::activity();
    ledOffHard();

    uint8_t colorIndex = 0;
    uint8_t brightness = 60;
    bool sos = false;
    bool standby = false;
    uint32_t lastActivity = millis();
    uint32_t sosStart = 0;
    uint32_t nextFrame = 0;
    int lastColor = -1;
    int lastBrightness = -1;
    int lastLit = -1;

    const uint32_t timeoutMs = powertimer::timeoutSeconds() * 1000UL;
    // Il LED si spegne 1 s prima della sospensione: powerOff() blocca il loop
    // dentro navigation::poll(), quindi non si puo' aspettare l'ultimo istante.
    const uint32_t cutLeadMs = timeoutMs > 3000 ? 1000 : 0;
    const uint8_t baseBrightness = M5Cardputer.Display.getBrightness();

    auto applyLed = [&](bool lit) {
        const bool on = lit && !standby && brightness > 0;
        if (!on) {
            if (lastLit != 0) {
                M5.Led.setBrightness(0);
                M5.Led.setAllColor(static_cast<uint8_t>(0), static_cast<uint8_t>(0),
                                   static_cast<uint8_t>(0));
                lastBrightness = 0;
                lastColor = -2;
                lastLit = 0;
            }
            return;
        }
        if (lastBrightness != brightness) {
            M5.Led.setBrightness(static_cast<uint8_t>(brightness * 255 / 100));
            lastBrightness = brightness;
        }
        if (lastColor != colorIndex || lastLit != 1) {
            const TorchColor& c = kTorchColors[colorIndex];
            M5.Led.setAllColor(c.r, c.g, c.b);
            lastColor = colorIndex;
        }
        lastLit = 1;
    };

    for (;;) {
        const navigation::EventData ev = navigation::poll();
        uint32_t now = millis();

        if (ev.type == navigation::Event::Back) {
            break;
        }

        if (ev.type != navigation::Event::None) {
            lastActivity = now;
            if (standby) {
                standby = false;  // il tasto successivo riaccende la torcia
                powertimer::activity();
            }
        }
        if (M5Cardputer.Keyboard.isPressed() != 0) {
            lastActivity = now;
        }

        // Spegne il LED prima della sospensione: powertimer::tick() gira dentro
        // navigation::poll() e puo' bloccare a lungo in powerOff().
        if (!standby && timeoutMs != 0 && now - lastActivity + cutLeadMs >= timeoutMs) {
            standby = true;
            applyLed(false);
        }
        if (!standby && M5Cardputer.Display.getBrightness() == 0) {
            standby = true;
            applyLed(false);
        }
        // Seconda rete: se la UI e' gia' attenuata dal powertimer, il LED va
        // spento subito anche se il timeout software non e' scattato.
        if (!standby && baseBrightness > 16 &&
            M5Cardputer.Display.getBrightness() < baseBrightness) {
            standby = true;
            applyLed(false);
        }

        const char c = ev.type == navigation::Event::Text ? ev.ch : 0;
        if (ev.type == navigation::Event::Left || c == ',') {
            colorIndex = (colorIndex + kTorchColorCount - 1) % kTorchColorCount;
        } else if (ev.type == navigation::Event::Right || c == '/') {
            colorIndex = (colorIndex + 1) % kTorchColorCount;
        } else if (ev.type == navigation::Event::Up || c == ';') {
            brightness = static_cast<uint8_t>(std::min<int>(100, brightness + 10));
        } else if (ev.type == navigation::Event::Down || c == '.') {
            brightness = static_cast<uint8_t>(std::max<int>(0, brightness - 10));
        } else if (ev.type == navigation::Event::Select) {
            sos = !sos;
            sosStart = now;
        }

        now = millis();
        if (now < nextFrame) {
            delay(2);
            continue;
        }
        nextFrame = now + kTorchFrameMs;

        const bool lit = !sos || sosLit(now - sosStart);
        applyLed(lit);

        display::clear(display::C(display::T_bg));
        display::header("Torcia", "Sensori");
        drawPanel();

        auto& d = display::g();
        const int lx = 52;
        const int ly = 72;
        const int lr = 33;
        const uint16_t uiColor = kTorchColors[colorIndex].ui;
        const bool visible = !standby && brightness > 0 && lit;
        d.drawCircle(lx, ly, lr + 2, display::C(display::T_line));
        d.fillCircle(lx, ly, lr, visible ? uiColor : display::C(display::T_bg3));
        if (visible) {
            for (int a = 0; a < 360; a += 45) {
                const float rad = static_cast<float>(a) * kPi / 180.0f;
                const int x1 = lx + static_cast<int>(cosf(rad) * (lr + 5));
                const int y1 = ly + static_cast<int>(sinf(rad) * (lr + 5));
                const int x2 = lx + static_cast<int>(cosf(rad) * (lr + 10));
                const int y2 = ly + static_cast<int>(sinf(rad) * (lr + 10));
                d.drawLine(x1, y1, x2, y2, display::C(display::T_line2));
            }
        }

        d.drawFastVLine(96, 28, 88, display::C(display::T_line2));
        display::text(102, 28, "COLORE", display::C(display::T_dim), 0);
        display::text(102, 37, kTorchColors[colorIndex].name, display::C(display::T_text), 4);

        display::text(102, 58, "LUMINOSITA", display::C(display::T_dim), 0);
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%u%%", static_cast<unsigned>(brightness));
        display::text(102, 66, buf, display::C(display::T_text), 4);
        display::progressBar(102, 88, 126, 8, static_cast<float>(brightness) / 100.0f,
                            uiColor);

        display::text(102, 100, sos ? "SOS: ON" : "SOS: OFF",
                      sos ? display::C(display::T_warn) : display::C(display::T_dim), 2);
        if (standby) {
            display::text(102, 110, "STANDBY  PREMI UN TASTO",
                          display::C(display::T_warn), 0);
        }

        display::footer("ENTER=SOS  </>=colore  ;/.=lume");
        display::push();
    }

    ledOffHard();
    imuNeutral();
}

}  // namespace

void runMenu() {
    enterLandscape();

    MenuManager menu("Sensori", "Menu principale");
    menu.addItem("Livella", "planet", [] { runLevel(); });
    menu.addItem("Goniometro", "rotate", [] { runGoniometer(); });
    menu.addItem("Pedometro", "chip", [] { runPedometer(); });
    menu.addItem("Torcia", "bright", [] { runTorch(); });
    menu.setHint("ESC=indietro  ENTER=apri");
    menu.run(true);

    // Difesa in profondita': nessun percorso lascia il LED acceso.
    ledOffHard();
    exitLandscape();
}

}  // namespace sensors

#endif  // ENABLE_MODULE_SENSORS
