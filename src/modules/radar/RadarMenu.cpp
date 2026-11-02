// ============================================================================
//  Radar - modulo di ricostruzione funzionale del "Wi-Fi & BLE Radar ADV"
//  per M5Stack Cardputer / Cardputer ADV (ESP32-S3), senza codice originale.
//
//  Il progetto di riferimento (Zeloksa, closed source) espone tre blocchi
//  osservabili dalla documentazione pubblica:
//    - Single Target Radar con HUD "pinpoint" stile contatore Geiger
//      (barra a 16 segmenti con Peak Hold, indicatori WARMER/COLDER/HOTSPOT);
//    - Spectrum/Channel Analyzer con vista a barre e a arco su un asse di
//      frequenza comune, con duty cycle ridotto per la coesistenza TDM
//      tra radio Wi-Fi e radio BLE (unico radio 2.4 GHz);
//    - Radar a schermo (PPI) con blip posizionati per intensita' di segnale.
//
//  Qui vengono ricostruiti i comportamenti osservabili con API pubbliche:
//  NimBLE per il BLE, sniffer promiscuo 802.11 per il Wi-Fi e, in via
//  sperimentale, il motore CSI dell'ESP32-S3 (vedi ENABLE_RADAR_CSI).
//
//  Il modulo e' passivo: non trasmette frame 802.11 ne' advertising BLE.
// ============================================================================

#include "RadarMenu.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "Config.h"
#include "Compass.h"
#include "Display.h"
#include "Logger.h"
#include "MenuManager.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "PowerTimer.h"
#include "Ui.h"

#if ENABLE_MODULE_RADAR

namespace radar {
namespace {

// ----------------------------------------------------------------------------
//  Costanti di misura
// ----------------------------------------------------------------------------

// Estremi della scala RSSI usata per gauge e barre: -95 dBm = rumore di fondo,
// -35 dBm = trasmettitore praticamente a contatto con l'antenna.
constexpr int kRssiFloor = -95;
constexpr int kRssiCeil = -35;

// Modello log-distanza a due parametri: RSSI(d) = RSSI(1m) - 10*n*log10(d).
// -59 dBm a 1 metro e n = 2.2 sono valori tipici per BLE 2.4 GHz indoors.
constexpr float kRssiAtOneMeter = -59.0f;
constexpr float kPathLossExponent = 2.2f;
constexpr float kMaxRangeMeters = 30.0f;

constexpr uint8_t kWifiChannelCount = 13;
constexpr uint32_t kWifiDwellMs = 140;
constexpr uint32_t kBleBandSliceMs = 300;
constexpr uint8_t kBleBandSlices = 3;

constexpr int kMaxDevices = 24;
constexpr int kFinderSegments = 16;
constexpr uint32_t kFinderSliceMs = 220;
constexpr uint32_t kRadarSliceMs = 150;
constexpr uint32_t kRadarBlipTtlMs = 12000;
constexpr uint32_t kFinderLinkTimeoutMs = 6000;

constexpr uint32_t kCsiCalibrationMs = 8000;

// Limiti di frequenza del grafico "banda" (canali 1..13 + canale BLE 39).
constexpr float kBandFreqMin = 2400.0f;
constexpr float kBandFreqMax = 2484.0f;

// Canali advertising BLE: 37 = 2402 MHz, 38 = 2426 MHz, 39 = 2480 MHz.
constexpr float kBleAdvFreqMhz[3] = {2402.0f, 2426.0f, 2480.0f};

// ----------------------------------------------------------------------------
//  Utility
// ----------------------------------------------------------------------------

std::string shortened(const std::string& text, size_t maxLen) {
    if (text.size() <= maxLen || maxLen < 2) {
        return text;
    }
    return text.substr(0, maxLen - 1) + "~";
}

std::string formatFloat(float value, int decimals) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, static_cast<double>(value));
    return std::string(buffer);
}

int clampInt(int value, int low, int high) {
    return value < low ? low : (value > high ? high : value);
}

// 0% = rumore di fondo, 100% = segnale a contatto.
float rssiToFraction(int rssi) {
    const float span = static_cast<float>(kRssiCeil - kRssiFloor);
    return static_cast<float>(clampInt(rssi - kRssiFloor, 0, static_cast<int>(span))) / span;
}

float rssiToMeters(int rssi) {
    if (rssi <= kRssiFloor) {
        return kMaxRangeMeters * 3.0f;
    }
    const float ratio = (kRssiAtOneMeter - static_cast<float>(rssi)) / (10.0f * kPathLossExponent);
    const float meters = std::pow(10.0f, ratio);
    return std::min(meters, kMaxRangeMeters * 3.0f);
}

// ----------------------------------------------------------------------------
//  Stato BLE condiviso con il task host NimBLE.
//
//  Il callback di advertising gira nel task del controller: la lista dei
//  dispositivi e' protetta da una critical section, i contatori della vista
//  "banda" sono semplici incrementi volatili (valori statistici).
// ----------------------------------------------------------------------------

struct BleDevice {
    char address[18] = {0};
    char name[20] = {0};
    int8_t addressType = 0;
    int rssi = kRssiFloor;
    uint32_t lastSeenMs = 0;
    bool used = false;
};

portMUX_TYPE g_deviceMux = portMUX_INITIALIZER_UNLOCKED;
BleDevice g_devices[kMaxDevices];
int g_deviceCount = 0;

volatile bool g_trackDevices = false;
volatile bool g_countAdverts = false;
volatile uint32_t g_advertCount = 0;

// Finder: bersaglio selezionato. Si usa NimBLEAddress (confronto nativo):
// toString() stampa i byte in ordine inverso rispetto all'array interno, quindi
// confrontare i byte della stringa con getNative() non combaciava mai.
NimBLEAddress g_targetAddress;
volatile bool g_hasTarget = false;
volatile int g_targetRssi = kRssiFloor;
volatile uint32_t g_targetHits = 0;
volatile uint32_t g_targetLastMs = 0;

void resetDevices() {
    portENTER_CRITICAL(&g_deviceMux);
    g_deviceCount = 0;
    for (int i = 0; i < kMaxDevices; ++i) {
        g_devices[i].used = false;
    }
    portEXIT_CRITICAL(&g_deviceMux);
}

int snapshotDevices(BleDevice* out, int maxOut) {
    portENTER_CRITICAL(&g_deviceMux);
    const int count = std::min(g_deviceCount, maxOut);
    for (int i = 0; i < count; ++i) {
        out[i] = g_devices[i];
    }
    portEXIT_CRITICAL(&g_deviceMux);
    return count;
}

void pruneDevices(uint32_t maxAgeMs) {
    const uint32_t now = millis();
    portENTER_CRITICAL(&g_deviceMux);
    int write = 0;
    for (int read = 0; read < g_deviceCount; ++read) {
        const bool alive = (now - g_devices[read].lastSeenMs) <= maxAgeMs;
        if (!alive) {
            continue;
        }
        if (write != read) {
            g_devices[write] = g_devices[read];
        }
        ++write;
    }
    g_deviceCount = write;
    portEXIT_CRITICAL(&g_deviceMux);
}

void rememberDevice(const NimBLEAddress& address, const char* name, int rssi) {
    // toString() alloca: viene chiamata prima della critical section.
    const std::string text = address.toString();
    const uint32_t now = millis();

    portENTER_CRITICAL(&g_deviceMux);
    for (int i = 0; i < g_deviceCount; ++i) {
        if (std::strcmp(g_devices[i].address, text.c_str()) == 0) {
            if (rssi > g_devices[i].rssi) {
                g_devices[i].rssi = rssi;
            }
            g_devices[i].lastSeenMs = now;
            portEXIT_CRITICAL(&g_deviceMux);
            return;
        }
    }
    if (g_deviceCount < kMaxDevices) {
        BleDevice& slot = g_devices[g_deviceCount];
        std::strncpy(slot.address, text.c_str(), sizeof(slot.address) - 1);
        slot.address[sizeof(slot.address) - 1] = '\0';
        std::strncpy(slot.name, name, sizeof(slot.name) - 1);
        slot.name[sizeof(slot.name) - 1] = '\0';
        slot.addressType = static_cast<int8_t>(address.getType());
        slot.rssi = rssi;
        slot.lastSeenMs = now;
        slot.used = true;
        ++g_deviceCount;
    }
    portEXIT_CRITICAL(&g_deviceMux);
}

class RadarAdvertiseCallbacks : public NimBLEAdvertisedDeviceCallbacks {
public:
    void onResult(NimBLEAdvertisedDevice* device) override {
        if (device == nullptr) {
            return;
        }
        if (g_countAdverts) {
            ++g_advertCount;
        }

        const NimBLEAddress& address = device->getAddress();
        const int rssi = device->getRSSI();

        if (g_hasTarget && device->getAddress() == g_targetAddress) {
            if (rssi > g_targetRssi) {
                g_targetRssi = rssi;
            }
            g_targetLastMs = millis();
            ++g_targetHits;
        }

        if (g_trackDevices) {
            const char* name = device->haveName() ? device->getName().c_str() : "(senza nome)";
            rememberDevice(address, name, rssi);
        }
    }
};

void ensureBle() {
    if (NimBLEDevice::getInitialized()) {
        return;
    }
    // NimBLEDevice::init() non restituisce un esito: si verifica dopo.
    NimBLEDevice::init("CardputerFW");
    if (NimBLEDevice::getInitialized()) {
        logger::info("NimBLE: inizializzata, heap=%u", static_cast<unsigned>(ESP.getFreeHeap()));
    } else {
        logger::error("NimBLE: init fallita");
    }
}

// Finestra di scansione NON bloccante.
//
// La variante bloccante di NimBLEScan::start() resta in ulTaskNotifyTake su
// un evento di fine scansione che, se il controller risponde BLE_HS_EALREADY
// o non completa, non arriva mai: la UI si pianta e non risponde piu' ai tasti.
// Qui si usa la variante con callback (ritorna subito), si attendono i tasti e
// si chiude la finestra con un timeout di sicurezza.
navigation::EventData scanWindow(NimBLEScan* scan, uint32_t durationMs,
                                 const std::function<void()>& draw = nullptr) {
    navigation::EventData relevant;
    if (scan == nullptr) {
        return relevant;
    }
    scan->start(durationMs, nullptr, false);
    const uint32_t deadline = millis() + durationMs + 1500;
    uint32_t lastDraw = 0;
    while (millis() < deadline) {
        navigation::update();
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            relevant = ev;
            break;
        }
        if (ev.type == navigation::Event::Select || ev.type == navigation::Event::Left ||
            ev.type == navigation::Event::Right) {
            relevant = ev;
        }
        // Disegno durante la finestra: e' questo che rende fluida l'animazione
        // (prima si ridisegnava solo tra una finestra e l'altra, ~4 fps).
        if (draw && (millis() - lastDraw) >= 40) {
            lastDraw = millis();
            draw();
        }
        if (!scan->isScanning()) {
            break;
        }
        delay(10);
    }
    if (scan->isScanning()) {
        scan->stop();
    }
    return relevant;
}

void configureScan(NimBLEScan* scan, RadarAdvertiseCallbacks* callbacks, bool active) {
    scan->setAdvertisedDeviceCallbacks(callbacks, true);  // true = ricevi i duplicati
    scan->setActiveScan(active);
    scan->setInterval(100);
    scan->setWindow(100);  // duty 100%: serve per la misura di densita'
}

// ----------------------------------------------------------------------------
//  Disegno: primitive riusate da tutte le schermate del modulo.
// ----------------------------------------------------------------------------

// Arco graduato da 135 a 405 gradi (apertura verso il basso).
void drawArcGauge(int cx, int cy, int radius, float fraction, uint16_t color) {
    constexpr int kSteps = 36;
    constexpr float kStart = 2.356194f;  // 135 gradi
    constexpr float kSpan = 4.712389f;   // 270 gradi
    fraction = std::max(0.0f, std::min(1.0f, fraction));
    for (int i = 0; i <= kSteps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSteps);
        const float angle = kStart + kSpan * t;
        const int x = cx + static_cast<int>(std::cos(angle) * static_cast<float>(radius));
        const int y = cy + static_cast<int>(std::sin(angle) * static_cast<float>(radius));
        const bool filled = t <= fraction;
        display::g().fillCircle(x, y, filled ? 2 : 1, filled ? color : display::kPanel);
    }
}

// Barra a segmenti con indicatore di picco (Peak Hold), come nell'HUD originale.
void drawSegmentBar(int x, int y, int w, int h, int segments, float fraction, float peak,
                    uint16_t color) {
    const int gap = 2;
    const int segmentWidth = std::max(2, (w - (segments - 1) * gap) / segments);
    const int lit = clampInt(static_cast<int>(fraction * segments + 0.5f), 0, segments);
    for (int i = 0; i < segments; ++i) {
        const int sx = x + i * (segmentWidth + gap);
        const uint16_t fill = (i < lit) ? color : display::kPanel;
        display::g().fillRect(sx, y, segmentWidth, h, fill);
    }
    const int peakIndex = clampInt(static_cast<int>(peak * segments), 0, segments - 1);
    display::g().fillRect(x + peakIndex * (segmentWidth + gap), y - 3, segmentWidth, 2,
                          display::kWhite);
}

// Testo centrato su un asse verticale arbitrario (usato dentro i gauge).
void textCenteredAt(int centerX, int y, const std::string& text, uint16_t color,
                    uint8_t font = 0) {
    auto& d = display::g();
    const lgfx::IFont* typeface = &fonts::Font2;
    if (font == 0) {
        typeface = &fonts::Font0;
    } else if (font == 4) {
        typeface = &fonts::Font4;
    }
    d.setFont(typeface);
    const int width = d.textWidth(text.c_str());
    display::text(centerX - width / 2, y, text, color, font);
}

std::string levelLabel(int rssi) {    if (rssi <= kRssiFloor) {
        return "nessun segnale";
    }
    if (rssi > -48) {
        return "HOTSPOT";
    }
    if (rssi > -62) {
        return "vicino";
    }
    if (rssi > -78) {
        return "medio";
    }
    return "lontano";
}

uint16_t levelColor(int rssi) {
    if (rssi > -48) {
        return display::kDanger;
    }
    if (rssi > -62) {
        return display::kWarning;
    }
    if (rssi > -78) {
        return display::kAccent;
    }
    return display::kInfo;
}

// Elenco scorrevole in stile scuro, come i selettori del modulo WiFi.
int chooseList(const std::string& title, const std::string& parent,
               const std::vector<std::string>& labels, const std::string& hint) {
    if (labels.empty()) {
        return -1;
    }
    constexpr int kHeaderH = 22;
    constexpr int kFooterH = 12;
    constexpr int kRowH = 18;
    const int visible = std::max(1, (display::height() - kHeaderH - kFooterH) / kRowH);
    const int total = static_cast<int>(labels.size());
    int selected = 0;
    int top = 0;

    navigation::waitRelease();
    for (;;) {
        navigation::update();
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            navigation::waitRelease();
            return -1;
        }
        if (ev.type == navigation::Event::Select) {
            navigation::waitRelease();
            return selected;
        }
        if (ev.type == navigation::Event::Up) {
            selected = (selected + total - 1) % total;
        } else if (ev.type == navigation::Event::Down) {
            selected = (selected + 1) % total;
        }
        if (selected < top) {
            top = selected;
        } else if (selected >= top + visible) {
            top = selected - visible + 1;
        }

        display::clear();
        display::header(title, parent);
        for (int row = 0; row < visible; ++row) {
            const int index = top + row;
            if (index >= total) {
                break;
            }
            const int y = kHeaderH + row * kRowH;
            const bool isSelected = (index == selected);
            if (isSelected) {
                display::g().fillRoundRect(2, y + 1, display::width() - 6, kRowH - 2, 3,
                                           display::kAccentDark);
                display::g().fillRect(2, y + 4, 2, kRowH - 8, display::kAccent);
            }
            display::text(10, y + 3, shortened(labels[index], 36),
                          isSelected ? display::kWhite : display::kText, 2);
        }
        const int trackX = display::width() - 4;
        const int trackY = kHeaderH;
        const int trackH = display::height() - kHeaderH - kFooterH;
        display::g().fillRect(trackX, trackY, 3, trackH, display::kPanel);
        if (total > visible) {
            const int thumbH = std::max(10, trackH * visible / total);
            const int thumbY = trackY + (trackH - thumbH) * top / std::max(1, total - visible);
            display::g().fillRect(trackX, thumbY, 3, thumbH, display::kAccent);
        }
        display::footer(hint);
        display::push();
        delay(16);
    }
}

// ----------------------------------------------------------------------------
//  1) Finder BLE "hot/cold" (Single Target Radar)
// ----------------------------------------------------------------------------

struct DeviceRow {
    std::string label;
    std::string name;
    std::string address;
    int rssi = kRssiFloor;
};

// --- Direzione stimata con la bussola ---------------------------------------
//
// L'RSSI da solo non da' la direzione. Il trucco (lo stesso del progetto
// originale) e' ruotare su se stessi tenendo il device al petto: il segnale
// cresce quando il corpo non scherma piu' il bersaglio. Si accumula il massimo
// di RSSI per settore di heading e si indica il settore vincente: la freccia
// mostra di quanto ruotare per riallinearsi.
constexpr int kDirBins = 24;  // 15 gradi per settore

int16_t g_dirRssi[kDirBins];
int g_dirPeakBin = -1;
float g_heading = 0.0f;
bool g_compassOk = false;

void resetDirection() {
    for (int i = 0; i < kDirBins; ++i) {
        g_dirRssi[i] = static_cast<int16_t>(kRssiFloor);
    }
    g_dirPeakBin = -1;
}

void sampleDirection(float headingDeg, int rssi) {
    if (rssi <= kRssiFloor) {
        return;
    }
    int bin = static_cast<int>(headingDeg / (360.0f / static_cast<float>(kDirBins)));
    bin %= kDirBins;
    if (bin < 0) {
        bin += kDirBins;
    }
    if (rssi > g_dirRssi[bin]) {
        g_dirRssi[bin] = static_cast<int16_t>(rssi);
    }
    int best = -1;
    int bestRssi = kRssiFloor;
    for (int i = 0; i < kDirBins; ++i) {
        if (g_dirRssi[i] > bestRssi) {
            bestRssi = g_dirRssi[i];
            best = i;
        }
    }
    g_dirPeakBin = best;
}

int directionCoverage() {
    int used = 0;
    for (int i = 0; i < kDirBins; ++i) {
        if (g_dirRssi[i] > kRssiFloor) {
            ++used;
        }
    }
    return used * 100 / kDirBins;
}

float peakHeading() {
    if (g_dirPeakBin < 0) {
        return -1.0f;
    }
    return (static_cast<float>(g_dirPeakBin) + 0.5f) *
           (360.0f / static_cast<float>(kDirBins));
}

// Intervallo del tic del finder in funzione di sensibilita' e RSSI.
// Livello 1 = curva ripida (il tic accelera solo da molto vicino),
// livello 5 = molto reattivo (accelera gia' a media distanza).
// Usato sia dal finder sia dall'anteprima nella schermata di impostazione.
int beepIntervalMs(int level, int rssi) {
    const float slope = 8.0f - static_cast<float>(clampInt(level, 1, 5) - 1) * 1.5f;
    const float raw = 20.0f + static_cast<float>(kRssiCeil - rssi) * slope;
    return clampInt(static_cast<int>(raw + 0.5f), 20, 900);
}

void drawFinderFrame(const std::string& name, const std::string& address, int rssi, int trend,
                     float peakFraction, bool linkLost, bool sound, uint32_t hits) {
    display::clear();
    display::header("Find BLE", "Radar");

    const uint16_t color = linkLost ? display::kTextDim : levelColor(rssi);
    display::text(6, 25, shortened(name, 16), display::kText, 2);
    display::text(6, 41, address, display::kTextDim, 0);

    const std::string rssiText = (rssi <= kRssiFloor) ? "--" : std::to_string(rssi);
    display::text(6, 52, rssiText, color, 4);
    display::text(6, 84, "dBm", display::kTextDim, 0);

    const float meters = rssiToMeters(rssi);
    std::string distance = "~ " + formatFloat(meters, 1) + " m";
    if (meters >= kMaxRangeMeters) {
        distance = "> " + formatFloat(kMaxRangeMeters, 0) + " m";
    }
    display::text(44, 84, distance, linkLost ? display::kTextDim : display::kText, 2);

    std::string trendText = "-- stabile --";
    uint16_t trendColor = display::kTextDim;
    if (linkLost) {
        trendText = "SEGNALE PERSO";
        trendColor = display::kDanger;
    } else if (rssi > -48) {
        trendText = "!! HOTSPOT !!";
        trendColor = display::kDanger;
    } else if (trend > 2) {
        trendText = "WARMER >>";
        trendColor = display::kAccent;
    } else if (trend < -2) {
        trendText = "<< COLDER";
        trendColor = display::kInfo;
    }
    display::text(6, 100, trendText, trendColor, 2);

    // --- Radar direzionale con bussola -------------------------------------
    const int cx = 182;
    const int cy = 66;
    const int radius = 40;
    display::g().drawCircle(cx, cy, radius, display::kPanel);
    display::g().drawCircle(cx, cy, radius - 12, display::kPanel);

    // Tacche del compasso: ruotano con l'heading letto dal giroscopio.
    for (int deg = 0; deg < 360; deg += 45) {
        const float angle =
            (static_cast<float>(deg) - g_heading) * 0.0174533f - 1.5707963f;
        const int x1 = cx + static_cast<int>(std::cos(angle) * (radius - 5));
        const int y1 = cy + static_cast<int>(std::sin(angle) * (radius - 5));
        const int x2 = cx + static_cast<int>(std::cos(angle) * radius);
        const int y2 = cy + static_cast<int>(std::sin(angle) * radius);
        display::g().drawLine(x1, y1, x2, y2, display::kTextDim);
    }
    // Muso del device: fisso in alto, e' il riferimento 0 gradi.
    display::g().fillTriangle(cx, cy - radius - 7, cx - 4, cy - radius + 1, cx + 4,
                              cy - radius + 1, display::kNeoYellow);

    if (!g_compassOk) {
        textCenteredAt(cx, cy - 12, "IMU", display::kDanger, 0);
        textCenteredAt(cx, cy - 2, "assente", display::kTextDim, 0);
    } else if (g_dirPeakBin < 0) {
        textCenteredAt(cx, cy - 12, "ruota su", display::kTextDim, 0);
        textCenteredAt(cx, cy - 2, "te stesso", display::kTextDim, 0);
    } else {
        float delta = peakHeading() - g_heading;
        while (delta > 180.0f) {
            delta -= 360.0f;
        }
        while (delta < -180.0f) {
            delta += 360.0f;
        }
        const bool aligned = std::fabs(delta) < 20.0f;
        const uint16_t needleColor = aligned ? display::kAccent : display::kNeoOrange;
        const float angle = delta * 0.0174533f - 1.5707963f;
        const int nx = cx + static_cast<int>(std::cos(angle) * (radius - 6));
        const int ny = cy + static_cast<int>(std::sin(angle) * (radius - 6));
        display::g().drawLine(cx, cy, nx, ny, needleColor);
        display::g().fillCircle(nx, ny, 3, needleColor);
        display::g().fillCircle(cx, cy, 2, display::kWhite);
        textCenteredAt(cx, cy + 16, aligned ? "LOCKED" : "segui freccia",
                       aligned ? display::kAccent : display::kTextDim, 0);
        textCenteredAt(cx, cy + 26, std::to_string(directionCoverage()) + "% giro",
                       display::kTextDim, 0);
    }

    const float fraction = linkLost ? 0.0f : rssiToFraction(rssi);
    drawSegmentBar(6, 118, 128, 4, kFinderSegments, fraction, peakFraction, color);
    display::text(140, 112, sound ? "audio ON" : "audio OFF", display::kTextDim, 0);
    display::text(196, 112, "hit " + std::to_string(hits), display::kTextDim, 0);
    display::footer("ENTER=muto  `=stop  S" +
                    std::to_string(config::radarSensitivity()));
    display::push();
}

void runBleFinder() {
    powertimer::Hold hold;  // schermata di monitoraggio: niente spegnimento
    ensureBle();
    RadarAdvertiseCallbacks callbacks;
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan == nullptr) {
        ui::message("Find BLE", {"!NimBLE non disponibile."}, "Radar");
        return;
    }

    // Fase 1: elenco dei dispositivi presenti.
    resetDevices();
    g_hasTarget = false;
    g_countAdverts = false;
    g_trackDevices = true;
    configureScan(scan, &callbacks, true);

    navigation::waitRelease();
    uint32_t started = millis();
    uint32_t lastDraw = 0;
    while (millis() - started < 6000) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            g_trackDevices = false;
            scan->stop();
            return;
        }
        // Finestre da 800 ms: piu' tempo per le SCAN_RESPONSE (nome del device).
        if (scanWindow(scan, 800).type == navigation::Event::Back) {
            g_trackDevices = false;
            scan->stop();
            return;
        }
        if (millis() - lastDraw > 150) {
            lastDraw = millis();
            const int elapsed = static_cast<int>(millis() - started);
            display::clear();
            display::header("Find BLE", "Radar");
            display::textCentered(38, "Ricerca dispositivi BLE", display::kAccent, 2);
            display::progressBar(20, 62, 200, 8, static_cast<float>(elapsed) / 5000.0f,
                                 display::kInfo);
            BleDevice devices[kMaxDevices];
            const int found = snapshotDevices(devices, kMaxDevices);
            display::textCentered(80, "Trovati: " + std::to_string(found), display::kText, 2);
            display::footer("` = annulla");
            display::push();
        }
    }
    g_trackDevices = false;
    scan->stop();

    BleDevice devices[kMaxDevices];
    const int found = snapshotDevices(devices, kMaxDevices);
    if (found == 0) {
        ui::message("Find BLE", {"!Nessun dispositivo rilevato.", "Avvicinati a un beacon o tag."},
                    "Radar");
        return;
    }

    std::vector<DeviceRow> rows;
    rows.reserve(static_cast<size_t>(found));
    for (int i = 0; i < found; ++i) {
        if (!devices[i].used) {
            continue;
        }
        DeviceRow row;
        row.name = devices[i].name;
        row.address = devices[i].address;
        row.rssi = devices[i].rssi;
        row.label = shortened(devices[i].name, 18) + "  " + std::to_string(row.rssi) + "dBm";
        rows.push_back(row);
    }
    std::sort(rows.begin(), rows.end(),
              [](const DeviceRow& a, const DeviceRow& b) { return a.rssi > b.rssi; });

    std::vector<std::string> labels;
    labels.reserve(rows.size());
    for (const auto& row : rows) {
        labels.push_back(row.label);
    }
    const int choice = chooseList("Target BLE", "Radar", labels, "ENTER=traccia  `=annulla");
    if (choice < 0) {
        return;
    }

    // Fase 2: inseguimento del bersaglio con HUD hot/cold.
    const DeviceRow target = rows[static_cast<size_t>(choice)];
    g_targetAddress = NimBLEAddress(target.address);
    g_hasTarget = true;
    g_targetRssi = kRssiFloor;
    g_targetHits = 0;
    g_targetLastMs = 0;
    g_trackDevices = false;
    configureScan(scan, &callbacks, false);  // passivo: non genera traffico extra

    int ema = kRssiFloor;
    int previousEma = kRssiFloor;
    int trend = 0;
    float peak = 0.0f;
    bool sound = true;
    uint32_t nextTrend = millis() + 900;
    uint32_t nextBeep = 0;
    uint32_t lastHeapLog = millis();

    navigation::waitRelease();

    // Bussola: serve per sapere di quanto si e' ruotato mentre si cerca il
    // massimo di segnale. Se l'IMU non c'e', il finder resta usabile come
    // hot/cold ma senza freccia.
    g_compassOk = compass::begin();
    resetDirection();
    g_heading = 0.0f;
    if (!g_compassOk) {
        ui::message("Find BLE",
                    {"!Bussola non disponibile.", "Senza IMU resta solo la stima", "di distanza (hot/cold)."},
                    "Radar");
    }

    for (;;) {
        navigation::update();
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            break;
        }
        if (ev.type == navigation::Event::Select) {
            sound = !sound;
            navigation::waitRelease();
        }

        const int sample = g_targetRssi;
        g_targetRssi = kRssiFloor;

        // Disegno a ~25 fps mentre la scansione procede: e' questo che rende
        // fluida l'animazione (bussola, freccia e sweep).
        const auto drawFrame = [&]() {
            if (g_compassOk) {
                compass::update();
                g_heading = compass::heading();
            }
            const uint32_t t = millis();
            const bool lost = (g_targetLastMs == 0) ||
                              (t - g_targetLastMs > kFinderLinkTimeoutMs);
            drawFinderFrame(shortened(target.name, 16), target.address,
                            (ema <= kRssiFloor) ? kRssiFloor : ema, trend, peak, lost, sound,
                            g_targetHits);
        };

        // Finestra non bloccante: il tasto back resta reattivo e non si puo'
        // restare appesi in attesa dell'evento di fine scansione.
        drawFrame();
        const navigation::EventData windowEvent =
            scanWindow(scan, kFinderSliceMs, drawFrame);
        if (windowEvent.type == navigation::Event::Back) {
            break;
        }
        if (windowEvent.type == navigation::Event::Select) {
            sound = !sound;
        }

        const bool heard = sample > kRssiFloor;
        if (heard) {
            if (ema <= kRssiFloor) {
                ema = sample;
            } else {
                ema = (ema * 3 + sample) / 4;  // media mobile esponenziale
            }
            peak = std::max(peak, rssiToFraction(sample));
            // Ogni campione viene associato all'heading in cui e' arrivato:
            // il settore con il segnale piu' forte e' la direzione stimata.
            if (g_compassOk) {
                compass::update();
                sampleDirection(compass::heading(), sample);
            }
        } else {
            peak *= 0.985f;  // decadimento lento del picco
        }

        const uint32_t now = millis();
        if (now >= nextTrend) {
            trend = (ema > kRssiFloor) ? (ema - previousEma) : 0;
            if (previousEma <= kRssiFloor) {
                trend = 0;
            }
            previousEma = ema;
            nextTrend = now + 900;
        }

        // Tic del contatore Geiger: piu' ci si avvicina (RSSI alto), piu' il
        // tic e' fitto e piu' il tono sale. Quando la freccia e' allineata il
        // tic passa a un tono acuto, per confermare la direzione.
        if (sound && heard && now >= nextBeep) {
            const float fraction = rssiToFraction(ema);
            // Velocita' del tic pilotata dalla sensibilita' scelta in
            // Radar > Sensibilita (20 ms a contatto, fino a 900 ms lontano).
            const uint32_t interval =
                static_cast<uint32_t>(beepIntervalMs(config::radarSensitivity(), ema));
            float delta = peakHeading() - g_heading;
            while (delta > 180.0f) {
                delta -= 360.0f;
            }
            while (delta < -180.0f) {
                delta += 360.0f;
            }
            const bool aligned = (g_dirPeakBin >= 0) && (std::fabs(delta) < 20.0f);
            const float tone = aligned ? 2600.0f : (700.0f + fraction * 1600.0f);
            M5Cardputer.Speaker.tone(tone, 7);
            nextBeep = now + (aligned ? std::max<uint32_t>(12, interval / 2) : interval);
        }

        if (now - lastHeapLog > 30000) {
            lastHeapLog = now;
            logger::info("Radar finder: heap=%u B", static_cast<unsigned>(ESP.getFreeHeap()));
        }
        // Il frame successivo lo disegna la prossima finestra di scansione.
    }

    g_hasTarget = false;
    scan->stop();
    navigation::waitRelease();
    // Lo stack BLE resta inizializzato: deinit/init ripetuti rendevano fragile
    // la reinizializzazione (scansioni successive senza risultati).
}

// ----------------------------------------------------------------------------
//  2) Radar PPI: tutti i dispositivi come blip posizionati sullo schermo
// ----------------------------------------------------------------------------

uint32_t hashAddress(const char* text) {
    uint32_t hash = 2166136261u;  // FNV-1a: angolo stabile per lo stesso MAC
    for (const char* p = text; *p != '\0'; ++p) {
        hash ^= static_cast<uint8_t>(*p);
        hash *= 16777619u;
    }
    return hash;
}

void drawPpi(const BleDevice* devices, int count, uint32_t sweepMs) {
    display::clear();
    display::header("Radar BLE", "Radar");

    const int cx = 62;
    const int cy = 74;
    const int radius = 46;

    display::g().drawCircle(cx, cy, radius, display::kPanel);
    display::g().drawCircle(cx, cy, radius * 2 / 3, display::kPanel);
    display::g().drawCircle(cx, cy, radius / 3, display::kPanel);
    display::g().drawFastHLine(cx - radius, cy, radius * 2, display::kPanel);
    display::g().drawFastVLine(cx, cy - radius, radius * 2, display::kPanel);

    // Sweep con scia di 4 segmenti sempre piu' tenui.
    const float sweepAngle = static_cast<float>(sweepMs % 3000) / 3000.0f * 6.283185f;
    for (int trail = 0; trail < 4; ++trail) {
        const float angle = sweepAngle - static_cast<float>(trail) * 0.12f;
        const uint16_t color = (trail == 0) ? display::kAccent : display::kAccentDark;
        display::g().drawLine(cx, cy, cx + static_cast<int>(std::cos(angle) * radius),
                              cy + static_cast<int>(std::sin(angle) * radius), color);
    }

    const uint32_t now = millis();
    int active = 0;
    for (int i = 0; i < count; ++i) {
        if (!devices[i].used) {
            continue;
        }
        const uint32_t age = now - devices[i].lastSeenMs;
        if (age > kRadarBlipTtlMs) {
            continue;
        }
        ++active;
        const float fraction = rssiToFraction(devices[i].rssi);
        // I blip sono ancorati al mondo: ruotando il corpo ruotano sul display
        // (heading relativo della bussola), come su un radar vero.
        const float angle =
            (static_cast<float>(hashAddress(devices[i].address) % 360) - g_heading) * 0.0174533f;
        const int distance = static_cast<int>((1.0f - fraction) * radius) + 6;
        const int x = cx + static_cast<int>(std::cos(angle) * distance);
        const int y = cy + static_cast<int>(std::sin(angle) * distance);
        const bool fresh = age < 3000;
        display::g().fillCircle(x, y, fresh ? 2 : 1,
                                fresh ? levelColor(devices[i].rssi) : display::kTextDim);
        if (fresh) {
            display::g().drawCircle(x, y, 3, levelColor(devices[i].rssi));
        }
    }
    display::g().fillCircle(cx, cy, 2, display::kNeoYellow);

    display::text(112, 24, "attivi " + std::to_string(active), display::kAccent, 2);
    int y = 44;
    int shown = 0;
    for (int i = 0; i < count && shown < 4; ++i) {
        if (!devices[i].used || (now - devices[i].lastSeenMs) > kRadarBlipTtlMs) {
            continue;
        }
        const std::string name = shortened(devices[i].name, 16);
        display::text(112, y, name, display::kText, 0);
        display::text(112, y + 9, std::to_string(devices[i].rssi) + "dBm  " +
                                        formatFloat(rssiToMeters(devices[i].rssi), 1) + "m",
                      levelColor(devices[i].rssi), 0);
        y += 20;
        ++shown;
    }
    if (shown == 0) {
        display::text(112, 60, "nessun blip", display::kTextDim, 0);
    }
    display::footer("` = stop");
    display::push();
}

void runBleRadar() {
    powertimer::Hold hold;  // schermata di monitoraggio: niente spegnimento
    ensureBle();
    RadarAdvertiseCallbacks callbacks;
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan == nullptr) {
        ui::message("Radar BLE", {"!NimBLE non disponibile."}, "Radar");
        return;
    }

    resetDevices();
    g_hasTarget = false;
    g_countAdverts = false;
    g_trackDevices = true;
    configureScan(scan, &callbacks, false);
    // Bussola attiva anche qui: i blip ruotano con il corpo.
    g_compassOk = compass::begin();

    uint32_t nextPrune = millis() + 5000;
    navigation::waitRelease();
    for (;;) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }
        // Disegno durante la finestra: sweep fluido a ~25 fps. Il disegno viene
        // fatto anche fuori dalla finestra, cosi' se la scansione non parte lo
        // schermo continua comunque ad animarsi invece di restare fermo.
        const auto drawFrame = [&]() {
            if (g_compassOk) {
                compass::update();
                g_heading = compass::heading();
            }
            BleDevice devices[kMaxDevices];
            const int count = snapshotDevices(devices, kMaxDevices);
            drawPpi(devices, count, millis());
        };
        drawFrame();
        if (scanWindow(scan, kRadarSliceMs, drawFrame).type == navigation::Event::Back) {
            break;
        }
        if (millis() >= nextPrune) {
            nextPrune = millis() + 5000;
            pruneDevices(kRadarBlipTtlMs);
        }
    }

    g_trackDevices = false;
    scan->stop();
    navigation::waitRelease();
    // Nessun deinit: vedi la nota in runBleFinder.
}

// ----------------------------------------------------------------------------
//  3) Vista "banda" 2.4 GHz: canali Wi-Fi 1..13 e canali advertising BLE 37/38/39
//
//  Il Wi-Fi viene misurato davvero, canale per canale, con lo sniffer
//  promiscuo (frame e byte ricevuti nella finestra di dwell). Il BLE viene
//  misurato come advertising rate aggregato: il controller salta internamente
//  tra i canali 37/38/39 e l'API NimBLE non espone il canale di ricezione.
//  Poiche' un advertiser ripete lo stesso PDU su tutti e tre i canali, il rate
//  per canale e' stimato pari al rate aggregato osservato (valore marcato ~).
// ----------------------------------------------------------------------------

volatile uint32_t g_wifiWindowFrames = 0;
volatile uint32_t g_wifiWindowBytes = 0;
volatile bool g_sniffing = false;

void wifiSnifferCallback(void* buffer, wifi_promiscuous_pkt_type_t type) {
    if (!g_sniffing || buffer == nullptr) {
        return;
    }
    (void)type;
    const wifi_promiscuous_pkt_t* packet = static_cast<const wifi_promiscuous_pkt_t*>(buffer);
    ++g_wifiWindowFrames;
    g_wifiWindowBytes += packet->rx_ctrl.sig_len;
}

struct BandReading {
    float wifiPps[kWifiChannelCount] = {0.0f};
    float wifiPeak[kWifiChannelCount] = {0.0f};
    float bleChannelPps[3] = {0.0f};
    float bleChannelPeak[3] = {0.0f};
    float totalWifiPps = 0.0f;
    uint32_t wifiMs = 0;
    uint32_t bleMs = 0;
    uint32_t advTotal = 0;
};

bool beginSniffer() {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(120);

    wifi_promiscuous_filter_t filter = {};
    filter.filter_mask =
        WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_CTRL | WIFI_PROMIS_FILTER_MASK_DATA;
    if (esp_wifi_set_promiscuous_filter(&filter) != ESP_OK) {
        return false;
    }
    if (esp_wifi_set_promiscuous_rx_cb(&wifiSnifferCallback) != ESP_OK) {
        return false;
    }
    return true;
}

// Il ricevitore promiscuo viene acceso solo durante la finestra Wi-Fi: la radio
// e' una sola, quindi la finestra BLE (TDM) deve trovare il Wi-Fi spento.
bool setSnifferActive(bool active) {
    if (esp_wifi_set_promiscuous(active) != ESP_OK) {
        return false;
    }
    g_sniffing = active;
    return true;
}

void endSniffer() {
    g_sniffing = false;
    esp_wifi_set_promiscuous(false);
    WiFi.mode(WIFI_OFF);
}

void drawBandMeasure(int channelDone, int channelTotal) {
    display::clear();
    display::header("Banda 2.4 GHz", "Radar");
    display::textCentered(44, "Misura canale " + std::to_string(channelDone) + "/" +
                                  std::to_string(channelTotal),
                          display::kAccent, 2);
    display::progressBar(20, 66, 200, 8,
                         static_cast<float>(channelDone) / static_cast<float>(channelTotal),
                         display::kInfo);
    display::textCentered(84, "Sniffer 802.11 passivo", display::kTextDim, 0);
    display::footer("` = annulla");
    display::push();
}

// Ritorna false se l'utente interrompe.
bool measureWifiChannels(BandReading& reading) {
    reading.wifiMs = 0;
    for (uint8_t channel = 1; channel <= kWifiChannelCount; ++channel) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            return false;
        }
        if (esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
            reading.wifiPps[channel - 1] = 0.0f;
            continue;
        }
        delay(30);  // assestamento del sintetizzatore
        g_wifiWindowFrames = 0;
        g_wifiWindowBytes = 0;
        const uint32_t windowStart = millis();
        uint32_t lastDraw = windowStart;
        while (millis() - windowStart < kWifiDwellMs) {
            navigation::update();
            if (navigation::poll().type == navigation::Event::Back) {
                return false;
            }
            if (millis() - lastDraw > 60) {
                lastDraw = millis();
                drawBandMeasure(channel, kWifiChannelCount);
            }
            delay(10);
        }
        const uint32_t elapsed = std::max<uint32_t>(1, millis() - windowStart);
        const float frames = static_cast<float>(g_wifiWindowFrames);
        reading.wifiPps[channel - 1] = frames * 1000.0f / static_cast<float>(elapsed);
        reading.wifiMs += elapsed;
    }
    return true;
}

// Accende il ricevitore promiscuo solo per la durata della scansione Wi-Fi.
bool measureWifiBand(BandReading& reading) {
    if (!setSnifferActive(true)) {
        return false;
    }
    const bool completed = measureWifiChannels(reading);
    setSnifferActive(false);
    return completed;
}

bool measureBleBand(BandReading& reading) {
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan == nullptr) {
        return false;
    }
    RadarAdvertiseCallbacks callbacks;
    g_trackDevices = false;
    g_hasTarget = false;
    g_countAdverts = true;
    configureScan(scan, &callbacks, false);

    g_advertCount = 0;
    reading.bleMs = 0;
    const uint32_t start = millis();
    for (uint8_t slice = 0; slice < kBleBandSlices; ++slice) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            g_countAdverts = false;
            scan->stop();
            return false;
        }
        if (scanWindow(scan, kBleBandSliceMs).type == navigation::Event::Back) {
            g_countAdverts = false;
            scan->stop();
            return false;
        }
    }
    const uint32_t elapsed = std::max<uint32_t>(1, millis() - start);
    reading.advTotal = g_advertCount;
    reading.bleMs = elapsed;
    const float advPerSecond =
        static_cast<float>(g_advertCount) * 1000.0f / static_cast<float>(elapsed);
    g_countAdverts = false;
    scan->stop();

    // Stima per canale: vedi commento di sezione (ogni evento pubblicitario
    // viene ripetuto dall'advertiser su tutti e tre i canali, quindi il rate
    // osservato coincide, in prima approssimazione, con il rate per canale).
    for (int i = 0; i < 3; ++i) {
        reading.bleChannelPps[i] = advPerSecond;
    }
    return true;
}

int bandFrequencyToX(float mhz, int left, int right) {
    const float t = (mhz - kBandFreqMin) / (kBandFreqMax - kBandFreqMin);
    return left + static_cast<int>(t * static_cast<float>(right - left));
}

void drawBandChart(const BandReading& reading, int activeChannel, bool blePhase,
                   bool coexisting) {
    display::clear();
    display::header("Banda 2.4 GHz", "Radar");

    constexpr int kLeft = 6;
    constexpr int kRight = 234;
    constexpr int kBase = 80;
    constexpr int kMaxHeight = 42;

    float scale = 0.5f;
    for (int i = 0; i < kWifiChannelCount; ++i) {
        scale = std::max(scale, reading.wifiPps[i]);
    }
    for (int i = 0; i < 3; ++i) {
        scale = std::max(scale, reading.bleChannelPps[i]);
    }

    display::g().drawFastHLine(kLeft, kBase, kRight - kLeft, display::kPanel);
    for (uint8_t channel = 1; channel <= kWifiChannelCount; ++channel) {
        const int x = bandFrequencyToX(2407.0f + 5.0f * channel, kLeft, kRight);
        const float value = reading.wifiPps[channel - 1];
        const float peak = reading.wifiPeak[channel - 1];
        const int height = clampInt(static_cast<int>(value / scale * kMaxHeight), 0, kMaxHeight);
        const int peakHeight = clampInt(static_cast<int>(peak / scale * kMaxHeight), 0, kMaxHeight);
        const uint16_t color = height > kMaxHeight * 2 / 3
                                   ? display::kDanger
                                   : (height > kMaxHeight / 3 ? display::kWarning
                                                              : display::kAccent);
        display::g().fillRect(x - 3, kBase - height, 6, height, color);
        display::g().drawFastHLine(x - 3, kBase - peakHeight - 1, 6, display::kWhite);
        // Canale in misura in questo istante: cornice e tacca di scansione,
        // e' quello che da' la sensazione di "radar" che scorre.
        if (!blePhase && channel == activeChannel) {
            display::g().drawRect(x - 4, kBase - kMaxHeight - 2, 8, kMaxHeight + 3,
                                  display::kNeoYellow);
            display::g().fillRect(x - 1, kBase + 1, 3, 3, display::kNeoYellow);
        }
    }
    // Canali advertising BLE sulla stessa scala di frequenza. Il numero di
    // canale e' scritto in alto, sopra le barre, per non finire sopra le
    // etichette dei canali Wi-Fi.
    for (int i = 0; i < 3; ++i) {
        const int x = bandFrequencyToX(kBleAdvFreqMhz[i], kLeft, kRight);
        const float value = reading.bleChannelPps[i];
        const float peak = reading.bleChannelPeak[i];
        const int height = clampInt(static_cast<int>(value / scale * kMaxHeight), 0, kMaxHeight);
        const int peakHeight = clampInt(static_cast<int>(peak / scale * kMaxHeight), 0, kMaxHeight);
        display::g().fillRect(x - 1, kBase - height, 3, height, display::kNeoCyan);
        display::g().drawFastVLine(x, kBase, 4, display::kNeoCyan);
        display::g().drawFastHLine(x - 2, kBase - peakHeight - 1, 5, display::kWhite);
        if (blePhase) {
            display::g().drawRect(x - 3, kBase - kMaxHeight - 2, 6, kMaxHeight + 3,
                                  display::kNeoCyan);
        }
        display::text(x - 4, 24, std::to_string(37 + i), display::kNeoCyan, 0);
    }

    display::text(6, kBase + 4, "WiFi ch 1", display::kTextDim, 0);
    display::text(96, kBase + 4, "6", display::kTextDim, 0);
    display::text(186, kBase + 4, "11", display::kTextDim, 0);
    display::text(212, kBase + 4, "13", display::kTextDim, 0);

    const std::string phase =
        blePhase ? std::string("fase BLE")
                 : ("WiFi ch " + std::to_string(activeChannel) + "/13");
    display::text(6, 94, phase, blePhase ? display::kNeoCyan : display::kNeoYellow, 0);
    display::text(86, 94, "BLE 37/38/39 stimati", display::kTextDim, 0);
    display::text(206, 94, "peak", display::kWhite, 0);

    const uint32_t totalMs = std::max<uint32_t>(1, reading.wifiMs + reading.bleMs);
    const int wifiShare = static_cast<int>(reading.wifiMs * 100 / totalMs);
    const int bleShare = 100 - wifiShare;
    const std::string duty = std::string("TDM  WiFi ") + std::to_string(wifiShare) + "%  BLE " +
                             std::to_string(bleShare) + "%";
    display::text(6, 104, duty, coexisting ? display::kWarning : display::kText, 0);

    const uint32_t advPerSecond = reading.advTotal * 1000 / std::max<uint32_t>(1, reading.bleMs);
    display::text(6, 114, "wifi " + std::to_string(static_cast<int>(reading.totalWifiPps)) +
                              " fr/s",
                  display::kAccent, 0);
    display::text(112, 114, "adv " + std::to_string(advPerSecond) + "/s",
                  display::kNeoCyan, 0);
    display::text(178, 114, coexisting ? "radio unica" : "sequenziale", display::kTextDim, 0);
    display::footer("` = stop");
    display::push();
}

void runBand() {
    powertimer::Hold hold;  // schermata di monitoraggio: niente spegnimento
    if (!beginSniffer()) {
        endSniffer();
        ui::message("Banda 2.4 GHz", {"!Sniffer Wi-Fi non disponibile."}, "Radar");
        return;
    }
    ensureBle();
    NimBLEScan* scan = NimBLEDevice::getScan();
    RadarAdvertiseCallbacks callbacks;
    if (scan != nullptr) {
        g_trackDevices = false;
        g_hasTarget = false;
        configureScan(scan, &callbacks, false);
    }

    // Macchina a stati non bloccante: un canale WiFi per finestra, poi una
    // finestra BLE, mentre il grafico viene ridisegnato a ogni giro di loop.
    // Prima ogni ciclo misurava 13 canali + BLE e poi aspettava: per questo
    // sembrava "scansionare di continuo" a scatti.
    BandReading reading;
    int channel = 1;
    bool blePhase = false;
    uint32_t phaseStart = millis();

    setSnifferActive(true);
    esp_wifi_set_channel(static_cast<uint8_t>(channel), WIFI_SECOND_CHAN_NONE);
    g_wifiWindowFrames = 0;
    reading.wifiMs = 0;
    reading.bleMs = 0;

    navigation::waitRelease();
    for (;;) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }
        const uint32_t now = millis();

        if (!blePhase) {
            if (now - phaseStart >= kWifiDwellMs) {
                const uint32_t elapsed = std::max<uint32_t>(1, now - phaseStart);
                reading.wifiPps[channel - 1] = static_cast<float>(g_wifiWindowFrames) * 1000.0f /
                                               static_cast<float>(elapsed);
                reading.wifiMs += elapsed;
                ++channel;
                if (channel > kWifiChannelCount) {
                    // Giro WiFi completo: finestra BLE (la radio e' una sola).
                    setSnifferActive(false);
                    blePhase = true;
                    g_advertCount = 0;
                    g_countAdverts = true;
                    if (scan != nullptr) {
                        scan->start(kBleBandSliceMs, nullptr, false);
                    }
                    phaseStart = now;
                } else {
                    esp_wifi_set_channel(static_cast<uint8_t>(channel), WIFI_SECOND_CHAN_NONE);
                    g_wifiWindowFrames = 0;
                    phaseStart = now;
                }
            }
        } else if (now - phaseStart >= kBleBandSliceMs) {
            if (scan != nullptr) {
                scan->stop();
            }
            g_countAdverts = false;
            const uint32_t elapsed = std::max<uint32_t>(1, now - phaseStart);
            reading.advTotal = g_advertCount;
            reading.bleMs += elapsed;
            const float advPerSecond =
                static_cast<float>(g_advertCount) * 1000.0f / static_cast<float>(elapsed);
            for (int i = 0; i < 3; ++i) {
                reading.bleChannelPps[i] = advPerSecond;
            }
            for (int i = 0; i < kWifiChannelCount; ++i) {
                reading.wifiPeak[i] = std::max(reading.wifiPps[i], reading.wifiPeak[i] * 0.92f);
            }
            for (int i = 0; i < 3; ++i) {
                reading.bleChannelPeak[i] =
                    std::max(reading.bleChannelPps[i], reading.bleChannelPeak[i] * 0.92f);
            }
            reading.totalWifiPps = 0.0f;
            for (int i = 0; i < kWifiChannelCount; ++i) {
                reading.totalWifiPps += reading.wifiPps[i];
            }
            // Nuovo giro.
            blePhase = false;
            channel = 1;
            reading.wifiMs = 0;
            reading.bleMs = 0;
            setSnifferActive(true);
            esp_wifi_set_channel(static_cast<uint8_t>(channel), WIFI_SECOND_CHAN_NONE);
            g_wifiWindowFrames = 0;
            phaseStart = millis();
        }

        drawBandChart(reading, blePhase ? -1 : channel, blePhase, true);
        delay(25);
    }

    endSniffer();
    if (scan != nullptr) {
        scan->stop();
    }
    navigation::waitRelease();
}

// ----------------------------------------------------------------------------
//  4) Presenza via CSI (Channel State Information)
//
//  API verificate sul core in uso (arduino-esp32 2.0.16 / IDF 4.4.5):
//    esp_wifi_set_csi_rx_cb(), esp_wifi_set_csi_config(), esp_wifi_set_csi()
//  sono dichiarate per ESP32-S3 e CONFIG_ESP32_WIFI_CSI_ENABLED=1 nella
//  sdkconfig precompilata. Il limite pratico e' il traffico: senza un AP che
//  generi frame verso la stazione, arrivano solo i beacon degli AP vicini
//  (tipicamente 10..30 frame/s) e la sensibilita' e' molto inferiore a quella
//  del progetto originale, che inietta traffico a 290..320 pkt/s.
// ----------------------------------------------------------------------------

#if ENABLE_RADAR_CSI

volatile uint32_t g_csiFrames = 0;
volatile int32_t g_csiEnergy = 0;
volatile int8_t g_csiRssi = 0;
volatile uint32_t g_csiLastMs = 0;

void csiCallback(void* context, wifi_csi_info_t* info) {
    (void)context;
    if (info == nullptr || info->buf == nullptr || info->len < 8) {
        return;
    }
    const int pairs = static_cast<int>(info->len) / 2;
    const int stride = (pairs > 16) ? pairs / 16 : 1;
    int32_t sum = 0;
    for (int i = 0; i < pairs; i += stride) {
        const int re = static_cast<int>(info->buf[2 * i]);
        const int im = static_cast<int>(info->buf[2 * i + 1]);
        sum += (re < 0 ? -re : re) + (im < 0 ? -im : im);
    }
    g_csiEnergy = sum;
    g_csiRssi = info->rx_ctrl.rssi;
    g_csiLastMs = millis();
    ++g_csiFrames;
}

bool beginCsi(uint8_t channel) {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(120);

    wifi_promiscuous_filter_t filter = {};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

    wifi_csi_config_t config = {};
    config.lltf_en = true;
    config.htltf_en = false;
    config.stbc_htltf2_en = false;
    config.ltf_merge_en = true;
    config.channel_filter_en = false;
    config.manu_scale = false;
    config.shift = 0;

    if (esp_wifi_set_csi_config(&config) != ESP_OK) {
        return false;
    }
    if (esp_wifi_set_csi_rx_cb(&csiCallback, nullptr) != ESP_OK) {
        return false;
    }
    if (esp_wifi_set_csi(true) != ESP_OK) {
        return false;
    }
    g_csiFrames = 0;
    g_csiLastMs = 0;
    return true;
}

void endCsi() {
    esp_wifi_set_csi(false);
    esp_wifi_set_csi_rx_cb(nullptr, nullptr);
    esp_wifi_set_promiscuous(false);
    WiFi.mode(WIFI_OFF);
}

void drawCsi(int channel, bool calibrating, int progressPercent, float energy, float baseline,
             float threshold, float motion, uint32_t frames, int rssi) {
    display::clear();
    display::header("Presenza CSI", "Radar");

    const int cx = 62;
    const int cy = 72;
    const float normalized = std::max(0.0f, std::min(1.0f, motion));
    const int radius = 12 + static_cast<int>(normalized * 34.0f);

    display::g().drawCircle(cx, cy, 46, display::kPanel);
    display::g().drawCircle(cx, cy, 30, display::kPanel);
    display::g().drawCircle(cx, cy, 14, display::kPanel);
    const uint16_t waveColor = normalized > 0.6f
                                   ? display::kDanger
                                   : (normalized > 0.3f ? display::kWarning
                                                        : display::kAccent);
    display::g().drawCircle(cx, cy, radius, waveColor);
    if (normalized > 0.25f) {
        display::g().drawCircle(cx, cy, radius - 8, waveColor);
        display::g().drawCircle(cx, cy, radius - 16, waveColor);
    }
    display::g().fillCircle(cx, cy, 3, display::kNeoYellow);

    if (calibrating) {
        display::text(112, 30, "calibrazione", display::kWarning, 2);
        display::text(112, 50, std::to_string(progressPercent) + "%", display::kText, 4);
        display::text(112, 82, "ambiente vuoto", display::kTextDim, 0);
        display::text(112, 94, "ch " + std::to_string(channel), display::kTextDim, 0);
        display::footer("` = annulla");
        display::push();
        return;
    }

    const char* state = normalized > 0.6f ? "MOVIMENTO" : (normalized > 0.3f ? "variazione"
                                                                             : "quiete");
    display::text(112, 26, state, waveColor, 2);
    display::text(112, 46, "energia " + std::to_string(static_cast<int>(normalized * 100.0f)) + "%",
                  display::kText, 0);
    display::text(112, 58, "soglia " + formatFloat(threshold, 1), display::kTextDim, 0);
    display::text(112, 70, "base " + formatFloat(baseline, 1), display::kTextDim, 0);
    display::text(112, 82, "energia " + formatFloat(energy, 1), display::kTextDim, 0);
    display::text(112, 94, "rssi " + std::to_string(rssi) + " dBm", display::kTextDim, 0);
    display::text(112, 106, "frame " + std::to_string(frames) + "  ch " + std::to_string(channel),
                  display::kTextDim, 0);
    display::footer("ENTER=ricalibra  ,/=canale  `=stop");
    display::push();
}

void runCsi() {
    powertimer::Hold hold;  // schermata di monitoraggio: niente spegnimento
    uint8_t channel = 6;
    bool calibrating = true;
    uint32_t calibrationStart = millis();
    float baselineSum = 0.0f;
    float baselineSqSum = 0.0f;
    int baselineCount = 0;
    float baseline = 0.0f;
    float deviation = 1.0f;
    float ema = 0.0f;
    float motion = 0.0f;

    if (!beginCsi(channel)) {
        endCsi();
        ui::message("Presenza CSI",
                    {"!CSI non attivabile su questo core.", "Verifica ENABLE_RADAR_CSI e il supporto",
                     "del driver Wi-Fi per la CSI."},
                    "Radar");
        return;
    }

    navigation::waitRelease();
    uint32_t nextSample = 0;
    for (;;) {
        navigation::update();
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            break;
        }
        if (ev.type == navigation::Event::Select) {
            calibrating = true;
            calibrationStart = millis();
            baselineSum = 0.0f;
            baselineSqSum = 0.0f;
            baselineCount = 0;
            motion = 0.0f;
            navigation::waitRelease();
        }
        if (ev.type == navigation::Event::Left || ev.type == navigation::Event::Right) {
            const int delta = (ev.type == navigation::Event::Right) ? 1 : -1;
            channel = static_cast<uint8_t>(clampInt(static_cast<int>(channel) + delta, 1, 13));
            endCsi();
            beginCsi(channel);
            calibrating = true;
            calibrationStart = millis();
            baselineSum = 0.0f;
            baselineSqSum = 0.0f;
            baselineCount = 0;
        }

        const uint32_t now = millis();
        if (now >= nextSample) {
            nextSample = now + 40;  // 25 Hz
            const float energy = static_cast<float>(g_csiEnergy);
            if (energy > 0.0f) {
                ema = (ema <= 0.0f) ? energy : (ema * 0.85f + energy * 0.15f);
            }
            if (calibrating) {
                if (ema > 0.0f && (now - calibrationStart) > 1500) {
                    baselineSum += ema;
                    baselineSqSum += ema * ema;
                    ++baselineCount;
                }
                if (now - calibrationStart >= kCsiCalibrationMs) {
                    calibrating = false;
                    if (baselineCount > 4) {
                        baseline = baselineSum / static_cast<float>(baselineCount);
                        const float variance =
                            std::max(0.0f, baselineSqSum / static_cast<float>(baselineCount) -
                                               baseline * baseline);
                        deviation = std::sqrt(variance) + 0.5f;
                    } else {
                        baseline = ema;
                        deviation = 1.0f;
                    }
                    logger::info("CSI: base=%.1f dev=%.2f", static_cast<double>(baseline),
                                 static_cast<double>(deviation));
                }
            } else if (baseline > 0.0f) {
                const float delta = ema - baseline;
                // Spike rejector: un salto oltre 6 deviazioni e' tipicamente un
                // aggiustamento di AGC del ricevitore, non movimento reale.
                if (std::fabs(delta) < deviation * 6.0f) {
                    motion = motion * 0.9f + std::min(1.0f, std::fabs(delta) / (deviation * 6.0f)) * 0.1f;
                } else {
                    motion *= 0.9f;
                }
            }
        }

        const int progress = calibrating
                                 ? clampInt(static_cast<int>((now - calibrationStart) * 100 /
                                                             kCsiCalibrationMs),
                                            0, 100)
                                 : 100;
        drawCsi(channel, calibrating, progress, ema, baseline, deviation * 6.0f, motion, g_csiFrames,
                static_cast<int>(g_csiRssi));
        delay(10);
    }

    endCsi();
    navigation::waitRelease();
}

#endif  // ENABLE_RADAR_CSI

// ----------------------------------------------------------------------------
//  Info radio: utile in campo per capire cosa e' compilato e quanta RAM resta.
// ----------------------------------------------------------------------------

// Impostazione della sensibilita': mostra l'anteprima della velocita' del tic
// a tre livelli di segnale, cosi' si sceglie l'effetto e non un numero astratto.
void runSensitivity() {
    navigation::waitRelease();
    for (;;) {
        const int level = config::radarSensitivity();
        display::clear();
        display::header("Sensibilita radar", "Radar");
        display::textCentered(26, "livello " + std::to_string(level) + "/5",
                              display::kAccent, 2);
        display::textCentered(50, "tic ogni " +
                                      std::to_string(beepIntervalMs(level, -70)) +
                                      " ms  (-70 dBm)",
                              display::kText, 0);
        display::textCentered(62, "tic ogni " +
                                      std::to_string(beepIntervalMs(level, -55)) +
                                      " ms  (-55 dBm)",
                              display::kText, 0);
        display::textCentered(74, "tic ogni " +
                                      std::to_string(beepIntervalMs(level, -40)) +
                                      " ms  (-40 dBm)",
                              display::kText, 0);
        display::textCentered(94, "1 = solo da vicino   5 = molto reattivo",
                              display::kTextDim, 0);
        display::textCentered(108, ", / = cambia", display::kText, 0);
        display::footer("ENTER/` = salva e torna");
        display::push();

        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back || ev.type == navigation::Event::Select) {
            break;
        }
        if (ev.type == navigation::Event::Left || ev.type == navigation::Event::Right) {
            int value = level + ((ev.type == navigation::Event::Right) ? 1 : -1);
            if (value < 1) {
                value = 5;
            } else if (value > 5) {
                value = 1;
            }
            config::setRadarSensitivity(static_cast<uint8_t>(value));
        }
        delay(25);
    }
    navigation::waitRelease();
}

void showRadioInfo() {
    std::vector<std::string> lines;
    lines.push_back(">Radar: passivo (nessuna TX)");
    lines.push_back("Wi-Fi: canali 1..13, BLE: 37/38/39");
    lines.push_back("Radio unica 2.4 GHz: misura in TDM");
#if ENABLE_RADAR_CSI
    lines.push_back(">CSI: compilata (sperimentale)");
#else
    lines.push_back("!CSI: non compilata");
#endif
    lines.push_back("Heap libero: " + std::to_string(ESP.getFreeHeap() / 1024) + " KB");
    lines.push_back(std::string("NimBLE: ") +
                    (NimBLEDevice::getInitialized() ? "attiva" : "spenta"));
    ui::message("Info radio", lines, "Radar");
}

}  // namespace

void runMenu() {
    MenuManager menu("Radar", "Menu principale");
    menu.addItem("Find BLE", "ble", [] { runBleFinder(); });
    menu.addItem("Radar BLE", "scan", [] { runBleRadar(); });
    menu.addItem("Banda 2.4 GHz", "wifi", [] { runBand(); });
#if ENABLE_RADAR_CSI
    menu.addItem("Presenza CSI", "radar", [] { runCsi(); });
#endif
    menu.addSeparator("Strumenti");
    menu.addItem("Sensibilita", "settings", [] { runSensitivity(); });
    menu.addItem("Info radio", "scan", [] { showRadioInfo(); });
    menu.setHint("`=indietro  ;.=nav  ENTER=apri");
    menu.run(true);
}

}  // namespace radar

#endif  // ENABLE_MODULE_RADAR
