// ============================================================================
//  Audio - microfono MEMS/ES8311, spettro FFT, registratore WAV e voice memo
//
//  Mic e speaker condividono BCLK/WS sull'ES8311: non possono stare accesi
//  insieme. Ogni schermata apre solo il codec che le serve e l'uscita dal
//  modulo restituisce sempre lo speaker a M5Unified.
//
//  Tutti i buffer sono statici (BSS), nessuna PSRAM: niente allocazioni nel
//  loop di cattura o di disegno. Il WAV e' PCM 16-bit mono a 16 kHz.
// ============================================================================

#include "AudioMenu.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <SD.h>
#include <math.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Display.h"
#include "Logger.h"
#include "MenuManager.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "PowerTimer.h"
#include "Storage.h"
#include "TextInput.h"
#include "Ui.h"

#if ENABLE_MODULE_AUDIO

namespace audio {
namespace {

constexpr uint32_t kRate = 16000;          // Hz, WAV e FFT
constexpr const char* kRecDir = "/audio/rec";
constexpr const char* kMemoDir = "/audio/memo";
constexpr size_t kWavHeader = 44;

// Spettro: FFT 512 punti (bin 31.25 Hz) su 32 ms di cattura.
constexpr int kFftSize = 512;
constexpr int kFftSamples = kFftSize;
constexpr int kBars = 26;
// Un tono a 1 kHz deve cadere nel bin 32: controllo in compilazione.
static_assert((1000 * kFftSize) / kRate == 32, "1 kHz non cade nel bin 32");
constexpr float kDbFloor = -72.0f;         // fondo scala dei grafici

// Ping-pong: 2 x 1024 campioni = 2 x 64 ms. Usato dal registratore e dal
// playback (mai insieme): un solo blocco PCM da 4 KB in BSS.
constexpr size_t kPcmSamples = 1024;

int16_t g_fftSamples[kFftSamples];         // 1 KB
float g_fftRe[kFftSize];                   // 1 KB
float g_fftIm[kFftSize];                   // 1 KB
float g_window[kFftSize];                  // 1 KB
int16_t g_pcm[2][kPcmSamples];             // 4 KB

float g_bar[kBars];
float g_barPeak[kBars];
float g_level = 0.0f;                      // RMS 0..1, con decadimento
float g_peakHold = 0.0f;                   // picco assoluto 0..1
bool g_fftReady = false;
uint32_t g_micStartedAt = 0;               // inizio cattura, per il warm-up ES8311

// ---------------------------------------------------------------------------
//  Codec: mic e speaker mai insieme
// ---------------------------------------------------------------------------
void releaseSpeaker() {
    if (M5.Speaker.isRunning()) {
        M5.Speaker.stop();
        M5.Speaker.end();
    }
}

bool beginMic() {
    releaseSpeaker();
    const bool ok = M5.Mic.begin();
    g_micStartedAt = ok ? millis() : 0;
    return ok;
}

void endMic() {
    if (M5.Mic.isRunning()) {
        M5.Mic.end();
    }
    g_micStartedAt = 0;
}

void restoreSpeaker() {
    if (!M5.Speaker.isRunning()) {
        if (!M5.Speaker.begin()) {
            logger::warn("Audio: ripristino speaker fallito");
        }
    }
}

// ---------------------------------------------------------------------------
//  Utility testuali e filesystem
// ---------------------------------------------------------------------------
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string clip(const std::string& text, int maxWidth, uint8_t font = 2) {
    if (maxWidth <= 0 || display::textWidth(text, font) <= maxWidth) {
        return text;
    }
    std::string out = text;
    while (!out.empty() && display::textWidth(out + "~", font) > maxWidth) {
        out.pop_back();
    }
    return out + "~";
}

std::string stem(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

bool isWav(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    return dot != std::string::npos && lower(name.substr(dot + 1)) == "wav";
}

std::string joinPath(const char* dir, const std::string& name) {
    return std::string(dir) + "/" + name;
}

std::string fmtDuration(uint32_t seconds) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%lu:%02lu",
                  static_cast<unsigned long>(seconds / 60),
                  static_cast<unsigned long>(seconds % 60));
    return buf;
}

uint64_t sdFreeBytes() {
    if (!storage::hasSD()) {
        return 0;
    }
    const uint64_t total = SD.totalBytes();
    const uint64_t used = SD.usedBytes();
    return total > used ? total - used : 0;
}

std::string fmtFree() {
    const uint64_t bytes = sdFreeBytes();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f GB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

void ensureAudioDirs() {
    if (!storage::hasSD()) {
        return;
    }
    storage::mkdir("/audio");
    storage::mkdir(kRecDir);
    storage::mkdir(kMemoDir);
}

// ---------------------------------------------------------------------------
//  WAV PCM 16-bit
// ---------------------------------------------------------------------------
void putLE16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void putLE32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

uint16_t getLE16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t getLE32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void fillWavHeader(uint8_t* h, uint32_t dataBytes) {
    std::memcpy(h, "RIFF", 4);
    putLE32(h + 4, 36 + dataBytes);
    std::memcpy(h + 8, "WAVE", 4);
    std::memcpy(h + 12, "fmt ", 4);
    putLE32(h + 16, 16);
    putLE16(h + 20, 1);                 // PCM
    putLE16(h + 22, 1);                 // mono
    putLE32(h + 24, kRate);
    putLE32(h + 28, kRate * 2);         // byte/s
    putLE16(h + 32, 2);                 // block align
    putLE16(h + 34, 16);                // bit
    std::memcpy(h + 36, "data", 4);
    putLE32(h + 40, dataBytes);
}

bool writeAll(File& file, const uint8_t* data, size_t length) {
    size_t done = 0;
    while (done < length) {
        const int n = file.write(data + done, length - done);
        if (n <= 0) {
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
}

bool patchWavHeader(File& file, uint32_t dataBytes) {
    uint8_t b[4];
    if (!file.seek(4)) {
        return false;
    }
    putLE32(b, 36 + dataBytes);
    if (!writeAll(file, b, 4) || !file.seek(40)) {
        return false;
    }
    putLE32(b, dataBytes);
    if (!writeAll(file, b, 4)) {
        return false;
    }
    return file.seek(0, SeekEnd);
}

struct WavInfo {
    uint16_t channels = 1;
    uint32_t rate = 0;
    uint16_t bits = 0;
    uint32_t dataOffset = 0;
    uint32_t dataBytes = 0;
};

bool parseWav(File& file, WavInfo& info) {
    uint8_t hdr[12];
    file.seek(0);
    if (file.read(hdr, sizeof(hdr)) != static_cast<int>(sizeof(hdr)) ||
        std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
        return false;
    }
    bool haveFmt = false;
    for (int guard = 0; guard < 16; ++guard) {
        uint8_t ch[8];
        if (file.read(ch, sizeof(ch)) != static_cast<int>(sizeof(ch))) {
            return false;
        }
        const uint32_t size = getLE32(ch + 4);
        const uint32_t body = file.position();
        if (std::memcmp(ch, "fmt ", 4) == 0 && size >= 16) {
            uint8_t fmt[16];
            if (file.read(fmt, sizeof(fmt)) != static_cast<int>(sizeof(fmt))) {
                return false;
            }
            const uint16_t tag = getLE16(fmt);
            info.channels = getLE16(fmt + 2);
            info.rate = getLE32(fmt + 4);
            info.bits = getLE16(fmt + 14);
            if (tag != 1 && tag != 0xFFFE) {
                return false;
            }
            haveFmt = true;
        } else if (std::memcmp(ch, "data", 4) == 0) {
            const uint32_t end = static_cast<uint32_t>(file.size());
            if (body > end) {
                return false;
            }
            info.dataOffset = body;
            info.dataBytes = (size == 0 || size > end - body) ? end - body : size;
            return haveFmt && info.rate >= 8000 && info.rate <= 48000 &&
                   (info.channels == 1 || info.channels == 2) && info.bits == 16 &&
                   info.dataBytes > 0;
        }
        if (!file.seek(body + size + (size & 1u))) {
            return false;
        }
    }
    return false;
}

uint32_t wavSeconds(const WavInfo& info) {
    const uint32_t frame = static_cast<uint32_t>(info.channels) * 2;
    return frame ? info.dataBytes / (info.rate * frame) : 0;
}

bool readWavInfo(const std::string& path, WavInfo& info) {
    File file = storage::openRead(path, true);
    if (!file) {
        return false;
    }
    const bool ok = parseWav(file, info);
    file.close();
    return ok;
}

// ---------------------------------------------------------------------------
//  FFT radix-2 (float, 512 punti) + barre logaritmiche
// ---------------------------------------------------------------------------
void fft(float* re, float* im, int n) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float angle = -2.0f * 3.14159265f / static_cast<float>(len);
        const float wlR = cosf(angle);
        const float wlI = sinf(angle);
        const int half = len >> 1;
        for (int i = 0; i < n; i += len) {
            float wr = 1.0f;
            float wi = 0.0f;
            for (int j = 0; j < half; ++j) {
                const float ur = re[i + j];
                const float ui = im[i + j];
                const float vr = re[i + j + half] * wr - im[i + j + half] * wi;
                const float vi = re[i + j + half] * wi + im[i + j + half] * wr;
                re[i + j] = ur + vr;
                im[i + j] = ui + vi;
                re[i + j + half] = ur - vr;
                im[i + j + half] = ui - vi;
                const float nr = wr * wlR - wi * wlI;
                wi = wr * wlI + wi * wlR;
                wr = nr;
            }
        }
    }
}

int barEdges[kBars + 1];  // indici dei bin per le barre logaritmiche

void initFft() {
    if (g_fftReady) {
        return;
    }
    for (int i = 0; i < kFftSize; ++i) {
        g_window[i] = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * i /
                                        static_cast<float>(kFftSize - 1));
    }
    const float minF = 100.0f;
    const float maxF = 6000.0f;
    const float binHz = static_cast<float>(kRate) / kFftSize;
    int prev = 1;
    for (int i = 0; i <= kBars; ++i) {
        const float f = minF * powf(maxF / minF, static_cast<float>(i) / kBars);
        int bin = static_cast<int>(lroundf(f / binHz));
        bin = std::max(prev, std::min(bin, kFftSize / 2 - 1));
        barEdges[i] = bin;
        prev = bin + 1;
    }
    g_fftReady = true;
}

float toDb(float amplitude) {
    return 20.0f * log10f(amplitude + 1e-9f);
}

void analyseSpectrum() {
    initFft();
    float sumSq = 0.0f;
    float peak = 0.0f;
    for (int i = 0; i < kFftSize; ++i) {
        const float s = static_cast<float>(g_fftSamples[i]) / 32768.0f;
        sumSq += s * s;
        peak = std::max(peak, fabsf(s));
        g_fftRe[i] = s * g_window[i];
        g_fftIm[i] = 0.0f;
    }
    fft(g_fftRe, g_fftIm, kFftSize);

    for (int b = 0; b < kBars; ++b) {
        float best = 0.0f;
        for (int bin = barEdges[b]; bin < barEdges[b + 1]; ++bin) {
            const float re = g_fftRe[bin];
            const float im = g_fftIm[bin];
            best = std::max(best, re * re + im * im);
        }
        // Ampiezza di un tono a fondo scala (correzione della finestra Hann).
        const float amp = 4.0f * sqrtf(best) / static_cast<float>(kFftSize);
        const float db = toDb(amp);
        const float norm = std::min(1.0f, std::max(0.0f, (db - kDbFloor) / -kDbFloor));
        g_bar[b] = norm > g_bar[b] ? norm : g_bar[b] * 0.72f + norm * 0.28f;
        g_barPeak[b] = std::max(norm, g_barPeak[b] - 0.006f);
    }

    const float rms = sqrtf(sumSq / static_cast<float>(kFftSize));
    g_level = std::max(rms, g_level * 0.80f);
    g_peakHold = peak > g_peakHold ? peak : g_peakHold * 0.995f;
}

// ---------------------------------------------------------------------------
//  UI comune
// ---------------------------------------------------------------------------
void drawBusy(const std::string& title, const std::string& line) {
    display::clear();
    display::header(title, "Audio");
    display::drawOrbitIcon("music", display::width() / 2 - 12, 38, 24);
    display::textCentered(72, line, display::kText, 0);
    display::footer("attendi");
    display::push();
}

void drawSpectrum(bool warm) {
    display::clear();
    display::header("Spettro FFT", "Audio");

    auto& d = display::g();
    constexpr int kTop = 27;
    constexpr int kBottom = 84;
    constexpr int kBarW = 6;
    constexpr int kGap = 2;
    const int startX = (display::width() -
                        (kBars * kBarW + (kBars - 1) * kGap)) / 2;

    d.fillRect(4, kTop - 2, display::width() - 8, kBottom - kTop + 4,
               display::C(display::T_bg2));
    d.drawRect(4, kTop - 2, display::width() - 8, kBottom - kTop + 4,
               display::C(display::T_line));
    for (int g = 1; g < 3; ++g) {
        const int y = kTop + (kBottom - kTop) * g / 3;
        d.drawFastHLine(6, y, display::width() - 12, display::C(display::T_line2));
    }

    const int scale = kBottom - kTop;
    for (int b = 0; b < kBars; ++b) {
        const int x = startX + b * (kBarW + kGap);
        d.fillRect(x, kTop, kBarW, scale, display::C(display::T_bg3));
        const int h = static_cast<int>(g_bar[b] * scale);
        if (h > 0) {
            const uint16_t color = g_bar[b] > 0.86f ? display::C(display::T_err)
                                   : g_bar[b] > 0.70f ? display::C(display::T_warn)
                                                      : display::C(display::T_acc);
            d.fillRect(x, kBottom - h, kBarW, h, color);
        }
        const int py = kBottom - static_cast<int>(g_barPeak[b] * scale);
        if (g_barPeak[b] > 0.01f) {
            d.fillRect(x, std::max(kTop, py - 1), kBarW, 1, display::C(display::T_hi));
        }
    }

    display::text(startX, kBottom + 2, "100", display::C(display::T_dim), 0);
    display::text(startX + 112, kBottom + 2, "1k", display::C(display::T_dim), 0);
    display::text(display::width() - 22, kBottom + 2, "6k",
                  display::C(display::T_dim), 0);

    const float levelDb = std::max(kDbFloor, toDb(g_level));
    const float peakDb = std::max(kDbFloor, toDb(g_peakHold));
    char buf[40];
    std::snprintf(buf, sizeof(buf), "LIV %4.0f dB", static_cast<double>(levelDb));
    display::text(7, 96, buf,
                  peakDb > -1.0f ? display::C(display::T_err) : display::C(display::T_text), 0);
    std::snprintf(buf, sizeof(buf), "PK %4.0f dB", static_cast<double>(peakDb));
    display::text(display::width() - 7 - display::textWidth(buf, 0), 96, buf,
                  display::C(display::T_dim), 0);

    const float norm = std::min(1.0f, std::max(0.0f, (levelDb + 60.0f) / 60.0f));
    display::progressBar(8, 105, display::width() - 16, 5, norm,
                         peakDb > -1.0f ? display::C(display::T_err)
                                        : display::C(display::T_acc));

    display::text(8, 113, warm ? "MIC: riscaldamento ES8311"
                               : "FFT 512 punti, 16 kHz, mono",
                  warm ? display::C(display::T_warn) : display::C(display::T_dim), 0);
    display::footer("ESC=esci", "MIC ON");
    display::push();
}

void drawRecorderIdle(const std::string& lastSaved, uint32_t lastSeconds) {
    display::clear();
    display::header("Registra", "Audio");

    auto& d = display::g();
    d.fillCircle(12, 32, 3, display::C(display::T_ok));
    display::text(20, 28, "MIC PRONTO", display::C(display::T_ok), 2);
    display::text(8, 44, "SD libera: " + fmtFree(), display::C(display::T_text), 0);
    display::text(8, 54, "/audio/rec  WAV 16-bit mono 16 kHz",
                  display::C(display::T_dim), 0);

    d.fillRoundRect(4, 66, display::width() - 8, 30, 3, display::C(display::T_bg2));
    d.drawRoundRect(4, 66, display::width() - 8, 30, 3, display::C(display::T_line));
    display::textCentered(76, "ENTER = AVVIA", display::kAccent, 2);

    if (!lastSaved.empty()) {
        display::text(8, 102, "Ultimo: " + clip(lastSaved, display::width() - 100, 0) +
                                  " (" + fmtDuration(lastSeconds) + ")",
                      display::C(display::T_dim), 0);
    }
    display::footer("ENTER=registra  ESC=esci");
    display::push();
}

void drawRecorderActive(const std::string& name, uint32_t seconds, uint32_t bytes,
                        float level, bool warming) {
    display::clear();
    display::header("Registrazione", "Audio");

    auto& d = display::g();
    const bool blink = (millis() / 500) % 2 == 0;
    d.fillCircle(12, 32, 4, blink ? display::C(display::T_err) : display::C(display::T_bg3));
    display::text(22, 27, "REC", display::C(display::T_err), 2);

    const std::string elapsed = fmtDuration(seconds);
    display::text(display::width() - 8 - display::textWidth(elapsed, 4), 24,
                  elapsed, display::C(display::T_text), 4);

    display::text(8, 46, clip(name, display::width() - 16, 0), display::C(display::T_dim), 0);
    display::progressBar(8, 58, display::width() - 16, 6, std::min(1.0f, level),
                         display::C(display::T_ok));

    char buf[48];
    std::snprintf(buf, sizeof(buf), "Scritti %.2f MB", static_cast<double>(bytes) /
                                                          (1024.0 * 1024.0));
    display::text(8, 70, buf, display::C(display::T_text), 0);
    display::text(display::width() - 8 - display::textWidth("SD: " + fmtFree(), 0),
                  70, "SD: " + fmtFree(), display::C(display::T_dim), 0);
    display::text(8, 84, "WAV 16 kHz mono - 32000 B/s",
                  display::C(display::T_dim), 0);
    display::text(8, 96, warming ? "ES8311: scarto i primi campioni a zero"
                                 : "La registrazione si ferma con ENTER o ESC.",
                  warming ? display::C(display::T_warn) : display::C(display::T_dim), 0);
    display::footer("ENTER=stop  ESC=stop");
    display::push();
}

// ---------------------------------------------------------------------------
//  Spettro
// ---------------------------------------------------------------------------
void runSpectrum() {
    powertimer::Hold hold;  // osservazione continua: niente spegnimento
    drawBusy("Spettro FFT", "avvio microfono...");
    if (!beginMic()) {
        restoreSpeaker();
        ui::message("Spettro FFT",
                    {"!Microfono non disponibile.", "Il bus I2S e' occupato.",
                     "Riavvia il dispositivo e riprova."},
                    "Audio");
        return;
    }

    const uint32_t micAt = millis();
    bool pending = false;
    uint32_t lastDraw = 0;
    navigation::waitRelease();

    for (;;) {
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            break;
        }
        if (!pending) {
            if (!M5.Mic.record(g_fftSamples, kFftSamples, kRate)) {
                ui::message("Spettro FFT", {"!Cattura non avviata.", "Riprova."}, "Audio");
                break;
            }
            pending = true;
        } else if (M5.Mic.isRecording() == 0) {
            pending = false;
            analyseSpectrum();
            if (millis() - lastDraw >= 50) {
                lastDraw = millis();
                drawSpectrum(millis() - micAt < 1200);
            }
        }
        delay(1);
    }

    endMic();
    restoreSpeaker();
    logger::info("Audio: spettro chiuso");
}

// ---------------------------------------------------------------------------
//  Registratore WAV
// ---------------------------------------------------------------------------
std::string uniqueRecPath() {
    char base[48];
    m5::rtc_datetime_t dt;
    if (M5.Rtc.isEnabled() && M5.Rtc.getDateTime(&dt) && dt.date.year >= 2020) {
        std::snprintf(base, sizeof(base), "REC_%04d%02d%02d_%02d%02d%02d",
                      static_cast<int>(dt.date.year), static_cast<int>(dt.date.month),
                      static_cast<int>(dt.date.date), static_cast<int>(dt.time.hours),
                      static_cast<int>(dt.time.minutes), static_cast<int>(dt.time.seconds));
    } else {
        // Nessun RTC sulla Cardputer ADV: timestamp = tempo dall'accensione.
        std::snprintf(base, sizeof(base), "REC_%05lu",
                      static_cast<unsigned long>(millis() / 1000));
    }
    std::string path = joinPath(kRecDir, std::string(base) + ".wav");
    for (int n = 2; n < 100 && storage::exists(path, true); ++n) {
        char alt[48];
        std::snprintf(alt, sizeof(alt), "%s_%d.wav", base, n);
        path = joinPath(kRecDir, alt);
    }
    return path;
}

float bufferLevel(const int16_t* data, size_t count) {
    float sum = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        const float s = static_cast<float>(data[i]) / 32768.0f;
        sum += s * s;
    }
    return sqrtf(sum / static_cast<float>(count));
}

struct RecSession {
    File file;
    std::string path;
    uint32_t bytes = 0;
    uint32_t startedAt = 0;
    uint32_t lastPatch = 0;
    uint32_t skipUntil = 0;   // il codec ES8311 impiega ~1 s a dare campioni validi
    uint64_t startFree = 0;
    uint8_t next = 0;
    bool active = false;
    bool error = false;
    bool discarded = false;
    float level = 0.0f;
};

bool recorderStart(RecSession& r) {
    ensureAudioDirs();
    r.active = false;
    if (!storage::hasSD()) {
        return false;
    }
    r.path = uniqueRecPath();
    r.file = storage::openWrite(r.path, true);
    if (!r.file) {
        return false;
    }
    uint8_t header[kWavHeader];
    fillWavHeader(header, 0);
    if (!writeAll(r.file, header, sizeof(header))) {
        r.file.close();
        storage::removeFile(r.path, true);
        return false;
    }
    r.bytes = 0;
    r.lastPatch = 0;
    r.next = 0;
    r.error = false;
    r.discarded = false;
    r.level = 0.0f;
    r.startFree = sdFreeBytes();
    r.startedAt = millis();
    // Se la cattura e' partita da meno di 1,2 s, i primi campioni possono
    // essere gli zeri di avvio del codec: non vanno nel WAV.
    r.skipUntil = g_micStartedAt ? g_micStartedAt + 1200 : 0;
    if (!M5.Mic.record(g_pcm[0], kPcmSamples, kRate) ||
        !M5.Mic.record(g_pcm[1], kPcmSamples, kRate)) {
        r.file.close();
        storage::removeFile(r.path, true);
        M5.Mic.end();          // scarta una eventuale richiesta rimasta in coda
        beginMic();
        return false;
    }
    r.active = true;
    return true;
}

void recorderWrite(RecSession& r) {
    if (!r.active || r.error || M5.Mic.isRecording() > 1) {
        return;
    }
    const uint8_t idx = r.next;
    const size_t bytes = sizeof(g_pcm[idx]);
    const bool warming = static_cast<int32_t>(millis() - r.skipUntil) < 0;
    if (!warming) {
        if (!writeAll(r.file, reinterpret_cast<const uint8_t*>(g_pcm[idx]), bytes)) {
            r.error = true;
            return;
        }
        r.bytes += bytes;
        r.level = bufferLevel(g_pcm[idx], kPcmSamples);
    }
    if (!M5.Mic.record(g_pcm[idx], kPcmSamples, kRate)) {
        r.error = true;
        return;
    }
    r.next ^= 1;
    // Header aggiornato ogni secondo: se manca la corrente il file resta
    // riproducibile al netto dell'ultimo secondo.
    if (r.bytes - r.lastPatch >= kRate * 2) {
        patchWavHeader(r.file, r.bytes);
        r.lastPatch = r.bytes;
    }
}

void recorderFinish(RecSession& r) {
    if (!r.active) {
        return;
    }
    // Le ultime due richieste possono essere ancora in coda: si lasciano
    // completare (max 128 ms) senza accodarne altre, poi si chiude il file.
    const uint32_t start = millis();
    while (M5.Mic.isRecording() != 0 && millis() - start < 250) {
        navigation::poll();
        delay(1);
    }
    patchWavHeader(r.file, r.bytes);
    r.file.flush();
    r.file.close();
    r.active = false;
    r.discarded = r.bytes < 3200;  // meno di ~100 ms: niente file
    if (r.discarded) {
        storage::removeFile(r.path, true);
    }
}

void runRecorder() {
    ensureAudioDirs();
    if (!storage::hasSD()) {
        ui::message("Registra",
                    {"!microSD non montata.", "Inserisci una card e riavvia",
                     "oppure usa un adattatore."},
                    "Audio");
        return;
    }

    drawBusy("Registra", "avvio microfono...");
    if (!beginMic()) {
        restoreSpeaker();
        ui::message("Registra",
                    {"!Microfono non disponibile.", "Il bus I2S e' occupato.",
                     "Riavvia il dispositivo e riprova."},
                    "Audio");
        return;
    }

    RecSession rec;
    std::string lastSaved;
    uint32_t lastSeconds = 0;
    const char* notice = nullptr;
    bool noticeError = false;
    uint32_t lastDraw = 0;
    uint32_t lastActivity = 0;
    navigation::waitRelease();

    for (;;) {
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            if (rec.active) {
                recorderFinish(rec);
            }
            break;
        }
        if (ev.type == navigation::Event::Select) {
            if (!rec.active) {
                if (sdFreeBytes() < 1024 * 1024) {
                    notice = "Spazio SD insufficiente (serve 1 MB).";
                    noticeError = true;
                } else if (!recorderStart(rec)) {
                    notice = "Avvio registrazione non riuscito.";
                    noticeError = true;
                } else {
                    notice = nullptr;
                    noticeError = false;
                    lastDraw = 0;
                }
            } else {
                recorderFinish(rec);
                lastSaved = rec.discarded ? std::string() : rec.path;
                lastSeconds = rec.bytes / (kRate * 2);
                notice = rec.discarded ? "Registrazione troppo breve: scartata."
                                       : "Registrazione salvata.";
                noticeError = rec.discarded;
            }
        }

        if (rec.active) {
            recorderWrite(rec);
            // Una registrazione in corso non deve far scattare lo sleep.
            if (millis() - lastActivity >= 5000) {
                lastActivity = millis();
                powertimer::activity();
            }
            const uint32_t elapsed = millis() - rec.startedAt;
            if (rec.error) {
                recorderFinish(rec);
                lastSaved = rec.discarded ? std::string() : rec.path;
                lastSeconds = rec.bytes / (kRate * 2);
                notice = "!Errore di scrittura sulla SD.";
                noticeError = true;
            } else if (rec.startFree < static_cast<uint64_t>(rec.bytes) + 512 * 1024) {
                recorderFinish(rec);
                lastSaved = rec.discarded ? std::string() : rec.path;
                lastSeconds = rec.bytes / (kRate * 2);
                notice = "SD piena: registrazione interrotta.";
                noticeError = true;
            } else if (elapsed >= 3600000UL) {
                recorderFinish(rec);
                lastSaved = rec.discarded ? std::string() : rec.path;
                lastSeconds = rec.bytes / (kRate * 2);
                notice = "Limite di 60 minuti raggiunto.";
                noticeError = false;
            }
        }

        if (millis() - lastDraw >= 100) {
            lastDraw = millis();
            if (rec.active) {
                drawRecorderActive(rec.path, (millis() - rec.startedAt) / 1000,
                                   rec.bytes, rec.level,
                                   static_cast<int32_t>(millis() - rec.skipUntil) < 0);
            } else {
                drawRecorderIdle(lastSaved, lastSeconds);
                if (notice != nullptr) {
                    display::text(8, 114, notice,
                                  noticeError ? display::C(display::T_err)
                                              : display::C(display::T_ok), 0);
                    display::push();
                }
            }
        }
        delay(1);
    }

    if (rec.active) {
        recorderFinish(rec);
    }
    endMic();
    restoreSpeaker();
    logger::info("Audio: registratore chiuso");
}

// ---------------------------------------------------------------------------
//  Voice memo
// ---------------------------------------------------------------------------
struct Memo {
    std::string name;
    uint32_t seconds = 0;
};

std::string sanitizeName(const std::string& in) {
    std::string out;
    for (char c : in) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F || c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            continue;
        }
        out.push_back(c);
        if (out.size() >= 24) {
            break;
        }
    }
    while (!out.empty() && out.front() == ' ') {
        out.erase(out.begin());
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

bool renameMemo(const std::string& name) {
    std::vector<textinput::Field> fields(1);
    fields[0].label = "Nuovo nome";
    fields[0].value = stem(name);
    fields[0].maxLen = 24;
    if (!textinput::edit("Rinomina memo", "Voice memo", fields)) {
        return false;
    }
    const std::string clean = sanitizeName(fields[0].value);
    if (clean.empty()) {
        ui::message("Voice memo", {"!Nome non valido."}, "Voice memo");
        return false;
    }
    const std::string target = joinPath(kMemoDir, clean + ".wav");
    const std::string source = joinPath(kMemoDir, name);
    if (target == source) {
        return false;
    }
    if (storage::exists(target, true)) {
        ui::message("Voice memo", {"!Esiste gia' un memo", "con questo nome."}, "Voice memo");
        return false;
    }
    if (!SD.rename(source.c_str(), target.c_str())) {
        ui::message("Voice memo", {"!Rinomina non riuscita.", "La SD potrebbe essere",
                                   "in sola lettura."}, "Voice memo");
        return false;
    }
    logger::info("Audio: memo rinominato in %s", clean.c_str());
    return true;
}

bool deleteMemo(const std::string& name) {
    if (!ui::confirm("Elimina memo", {"Cancellare \"" + stem(name) + "\"?",
                                      "L'operazione non e' reversibile."},
                     "Voice memo")) {
        return false;
    }
    if (!storage::removeFile(joinPath(kMemoDir, name), true)) {
        ui::message("Voice memo", {"!Cancellazione non riuscita."}, "Voice memo");
        return false;
    }
    logger::info("Audio: memo eliminato");
    return true;
}

void drawPlayback(const std::string& name, const WavInfo& info, uint32_t played) {
    display::clear();
    display::header("Voice memo", "Audio");
    display::text(8, 28, clip(stem(name), display::width() - 16, 2),
                  display::kAccent, 2);
    display::text(8, 42, "in riproduzione", display::kText, 0);

    const uint32_t total = wavSeconds(info);
    const uint32_t byteRate = info.rate * info.channels * 2;
    const uint32_t current = byteRate ? played / byteRate : 0;
    const float fraction = info.dataBytes ? std::min(1.0f, static_cast<float>(played) /
                                                              info.dataBytes)
                                          : 0.0f;
    display::progressBar(8, 56, display::width() - 16, 6, fraction);

    display::text(8, 68, fmtDuration(current), display::kTextDim, 0);
    const std::string totalText = fmtDuration(total);
    display::text(display::width() - 8 - display::textWidth(totalText, 0), 68,
                  totalText, display::kTextDim, 0);

    char buf[40];
    std::snprintf(buf, sizeof(buf), "%u Hz %s 16-bit", static_cast<unsigned>(info.rate),
                  info.channels == 2 ? "stereo" : "mono");
    display::text(8, 84, buf, display::kTextDim, 0);
    display::footer("ESC=stop  volume fisso");
    display::push();
}

void playMemo(const std::string& name) {
    powertimer::Hold hold;  // ascolto passivo: niente spegnimento a metà memo
    const std::string path = joinPath(kMemoDir, name);
    File file = storage::openRead(path, true);
    WavInfo info;
    if (!file || !parseWav(file, info)) {
        file.close();
        ui::message("Voice memo",
                    {"!Formato non supportato.", "Servono WAV PCM 16-bit",
                     "mono o stereo."},
                    "Voice memo");
        return;
    }

    endMic();
    drawBusy("Voice memo", "avvio speaker...");
    if (!M5.Speaker.begin()) {
        file.close();
        restoreSpeaker();
        ui::message("Voice memo", {"!Speaker non disponibile."}, "Voice memo");
        return;
    }
    const uint8_t oldVolume = M5.Speaker.getVolume();
    M5.Speaker.setVolume(190);

    file.seek(info.dataOffset);
    uint32_t remain = info.dataBytes;
    uint32_t played = 0;
    uint8_t next = 0;
    bool userStop = false;
    uint32_t lastDraw = 0;
    const uint32_t frame = static_cast<uint32_t>(info.channels) * 2;

    while (remain > 0) {
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back ||
            ev.type == navigation::Event::Select) {
            userStop = true;
            break;
        }
        size_t want = std::min<size_t>(remain, sizeof(g_pcm[0]));
        want -= want % frame;
        if (want == 0) {
            break;
        }
        const int got = file.read(reinterpret_cast<uint8_t*>(g_pcm[next]), want);
        if (got <= 0) {
            break;
        }
        const size_t usable = static_cast<size_t>(got) - static_cast<size_t>(got) % frame;
        if (usable == 0) {
            break;
        }
        if (!M5.Speaker.playRaw(g_pcm[next], usable / 2, info.rate,
                                info.channels == 2, 1, 1, false)) {
            break;
        }
        next ^= 1;
        remain -= usable;
        played += usable;
        if (millis() - lastDraw >= 150) {
            lastDraw = millis();
            drawPlayback(name, info, played);
        }
    }

    while (!userStop && M5.Speaker.isPlaying(1) != 0) {
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back ||
            ev.type == navigation::Event::Select) {
            userStop = true;
            break;
        }
        if (millis() - lastDraw >= 150) {
            lastDraw = millis();
            drawPlayback(name, info, played);
        }
        delay(2);
    }

    if (userStop) {
        M5.Speaker.stop(1);
    }
    file.close();
    M5.Speaker.setVolume(oldVolume);
    M5.Speaker.end();
    restoreSpeaker();
    logger::info("Audio: memo %s", userStop ? "interrotto" : "riprodotto");
}

std::vector<Memo> scanMemos() {
    ensureAudioDirs();
    std::vector<Memo> memos;
    for (const auto& entry : storage::listEntries(kMemoDir, true)) {
        if (entry.isDir || !isWav(entry.name)) {
            continue;
        }
        Memo memo;
        memo.name = entry.name;
        WavInfo info;
        if (readWavInfo(joinPath(kMemoDir, entry.name), info)) {
            memo.seconds = wavSeconds(info);
        }
        memos.push_back(std::move(memo));
        if (memos.size() >= 40) {
            break;  // shortcut: oltre 40 memo la lista si ferma qui
        }
    }
    return memos;
}

void fillMemoMenu(MenuManager& menu, const std::vector<Memo>& memos, int keep) {
    std::vector<MenuItem> items;
    for (const auto& memo : memos) {
        const std::string name = memo.name;
        items.emplace_back(fmtDuration(memo.seconds) + "  " + stem(name), "music",
                           [name] { playMemo(name); });
    }
    if (items.empty()) {
        items.emplace_back("Nessun memo", "info", nullptr, false);
    }
    menu.setItems(std::move(items));
    menu.selectIndex(keep);
}

void runMemos() {
    if (!storage::hasSD()) {
        ui::message("Voice memo",
                    {"!microSD non montata.", "I memo vivono in /audio/memo."},
                    "Audio");
        return;
    }
    ensureAudioDirs();
    std::vector<Memo> memos = scanMemos();
    if (memos.empty()) {
        ui::message("Voice memo",
                    {"!Nessun memo in /audio/memo.", "Copia file WAV PCM 16-bit",
                     "oppure registra dalla SD."},
                    "Audio");
        return;
    }

    MenuManager menu("Voice memo", "Audio");
    fillMemoMenu(menu, memos, 0);
    menu.setHint("ENTER=ascolta R=rinomina X=elimina");
    menu.setTextHandler([&menu, &memos](char c) {
        const int idx = menu.selectedIndex();
        if (idx < 0 || idx >= static_cast<int>(memos.size())) {
            return false;
        }
        const std::string name = memos[idx].name;
        if (c == 'r' || c == 'R') {
            if (renameMemo(name)) {
                memos = scanMemos();
                fillMemoMenu(menu, memos, idx);
            }
            return true;
        }
        if (c == 'x' || c == 'X') {
            if (deleteMemo(name)) {
                memos = scanMemos();
                fillMemoMenu(menu, memos, idx);
            }
            return true;
        }
        return false;
    });
    menu.run(true);

    endMic();
    restoreSpeaker();
}

// ---------------------------------------------------------------------------
//  Info
// ---------------------------------------------------------------------------
void showInfo() {
    std::vector<std::string> lines = {
        "Mic: MEMS I2S + codec ES8311",
        "WAV: PCM 16-bit mono, 16 kHz",
        "Spettro: FFT 512 punti, 26 barre",
        "Rec: /audio/rec  Memo: /audio/memo",
    };
    if (storage::hasSD()) {
        lines.push_back("SD libera: " + fmtFree());
    } else {
        lines.push_back("!microSD non montata");
    }
    lines.push_back("Nessuna PSRAM: buffer statici ~11 KB");
    ui::message("Audio", lines, "Audio");
}

}  // namespace

void runMenu() {
    ensureAudioDirs();

    MenuManager menu("Audio", "ORBIT");
    menu.addItem("Spettro FFT", "music", runSpectrum);
    menu.addItem("Registra WAV", "capture", runRecorder);
    menu.addItem("Voice memo", "chat", runMemos);
    menu.addItem("Info e limiti", "info", showInfo);
    menu.setHint("ENTER=apri  ESC=indietro");
    menu.run(true);

    // Uscita dal modulo: mai mic e speaker insieme, speaker sempre ripristinato.
    // Limite noto: audioout::decoder() (Radio/Musica) crea il proprio canale su
    // I2S_NUM_0, lo stesso del mic. M5.Mic.end() toglie il driver del mic e il
    // decoder puo' restare muto finche' non viene ricreato. AudioOut non si
    // tocca qui (feature 02): da verificare il ciclo Audio > Radio > Audio.
    endMic();
    restoreSpeaker();
    logger::info("Audio: uscita, speaker ripristinato");
}

}  // namespace audio

#endif  // ENABLE_MODULE_AUDIO
