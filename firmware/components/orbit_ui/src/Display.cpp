#include "Display.h"

#include "orbit_compat.h"
#include <esp_heap_caps.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Logger.h"
#include "Navigation.h"
#include "Orbit.h"
#include "OrbitPalette.h"
#include "PixelFont.h"

namespace display {

// ---------------------------------------------------------------------------
//  Palette Orbit: sette atmosfere, un solo set di token.
// ---------------------------------------------------------------------------
uint16_t pal[orbit_palette::kTokenCount];

uint16_t kBlack = 0x0000;
uint16_t kWhite = 0xFFFF;
uint16_t kBackground = 0x0843;
uint16_t kPanel = 0x18E8;
uint16_t kHeader = 0x1085;
uint16_t kAccent = 0xFB4F;
uint16_t kAccentDark = 0x998A;
uint16_t kText = 0xDF1F;
uint16_t kTextDim = 0x73D6;
uint16_t kWarning = 0xFE4A;
uint16_t kDanger = 0xFA4B;
uint16_t kInfo = 0x8D1F;
uint16_t kPaper = 0x1085;
uint16_t kPaperLight = 0x18E8;
uint16_t kInk = 0xDF1F;
uint16_t kMuted = 0x73D6;
uint16_t kNeoYellow = 0xE5A9;
uint16_t kNeoCyan = 0x8D1F;
uint16_t kNeoPink = 0xFDB7;
uint16_t kNeoGreen = 0xFE4A;
uint16_t kNeoOrange = 0xFF12;
uint16_t kNeoRed = 0xFA4B;

namespace {

uint8_t s_theme = 0;

void applyPalette(uint8_t id) {
    if (id >= kThemeCount) {
        id = 0;
    }
    const uint16_t* row = orbit_palette::kThemes[id];
    std::memcpy(pal, row, sizeof(pal));
    kBlack = pal[T_black];
    kWhite = pal[T_hi];
    kBackground = pal[T_bg];
    kPanel = pal[T_bg3];
    kHeader = pal[T_bg2];
    kAccent = pal[T_acc];
    kAccentDark = pal[T_accD];
    kText = pal[T_text];
    kTextDim = pal[T_dim];
    kWarning = pal[T_warn];
    kDanger = pal[T_err];
    kInfo = pal[T_sec];
    // Superfici: "carta" = pannello, inchiostro = testo. Con PAPER il tema si
    // inverte da solo e tutte le schermate restano leggibili.
    kPaper = pal[T_bg2];
    kPaperLight = pal[T_bg3];
    kInk = pal[T_text];
    kMuted = pal[T_dim];
    kNeoYellow = pal[T_gold];
    kNeoCyan = pal[T_sec];
    kNeoPink = pal[T_accL];
    kNeoGreen = pal[T_ok];
    kNeoOrange = pal[T_goldL];
    kNeoRed = pal[T_err];
}

struct PaletteBoot {
    PaletteBoot() { applyPalette(0); }
};
PaletteBoot s_paletteBoot;

}  // namespace

void setTheme(uint8_t id) {
    if (id >= kThemeCount) {
        id = 0;
    }
    s_theme = id;
    applyPalette(id);
}

uint8_t themeId() {
    return s_theme;
}

const char* themeName(uint8_t id) {
    return orbit_palette::kNames[id >= kThemeCount ? 0 : id];
}

const char* themeNote(uint8_t id) {
    return orbit_palette::kNotes[id >= kThemeCount ? 0 : id];
}

uint16_t themeColor(uint8_t id, Tok token) {
    if (id >= kThemeCount) {
        id = 0;
    }
    if (token >= T_COUNT) {
        return 0;
    }
    return orbit_palette::kThemes[id][token];
}

namespace {

M5Canvas s_sprite(&M5Cardputer.Display);
bool s_spriteOk = false;
uint8_t s_rotation = 1;

// Buffer di riserva (non DMA) usato se l'allocazione DMA dello sprite
// fallisce: senza framebuffer la UI ripiegherebbe sul disegno diretto e
// flickerebbe. Con questa riserva il double buffer resta disponibile.
void* s_fallbackBuffer = nullptr;
bool s_fallbackMode = false;
uint8_t s_spriteBpp = 2;
uint32_t s_spriteBytes = 0;

void releaseFallbackBuffer() {
    if (s_fallbackBuffer != nullptr) {
        heap_caps_free(s_fallbackBuffer);
        s_fallbackBuffer = nullptr;
    }
    s_fallbackMode = false;
}

// Ripiego su heap normale: niente DMA (push piu' lento), ma resta un vero
// double buffer. 8 bpp = RGB332, meta' memoria.
bool ensureHeapSprite(int w, int h, uint8_t bpp) {
    const size_t bytes = static_cast<size_t>(w) * h * (bpp / 8);
    void* buffer = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    if (buffer == nullptr) {
        return false;
    }
    s_sprite.setColorDepth(bpp);
    s_sprite.setBuffer(buffer, w, h, bpp);
    if (s_sprite.getBuffer() == nullptr) {
        heap_caps_free(buffer);
        return false;
    }
    s_fallbackBuffer = buffer;
    s_fallbackMode = true;
    s_spriteBpp = bpp / 8;
    s_spriteBytes = static_cast<uint32_t>(bytes);
    // Senza questo il ripiego resta "invisibile": push() non disegna e
    // releaseFramebuffer() non libera il buffer (32 KB persi a ogni gioco).
    s_spriteOk = true;
    return true;
}

// Crea (o riusa) un framebuffer full-screen per la dimensione data.
// Prima prova la memoria DMA (pushSprite piu' veloce); se non basta ripiega
// su heap normale. Dopo un gioco l'heap resta frammentato (un'allocazione di
// poche decine di byte divide il blocco del framebuffer): 64,8 KB contigui
// mancano, 32,4 KB a 8 bpp ci stanno. Meglio RGB332 che disegno diretto.
bool ensureSprite(int w, int h) {
    if (s_spriteOk && s_sprite.width() == w && s_sprite.height() == h) {
        return true;
    }

    s_spriteOk = false;
    s_spriteBytes = 0;
    s_spriteBpp = 2;
    s_sprite.deleteSprite();
    releaseFallbackBuffer();
    s_sprite.setColorDepth(16);

    if (s_sprite.createSprite(w, h) != nullptr) {
        s_spriteOk = true;
        s_spriteBytes = static_cast<uint32_t>(w) * h * 2;
#ifdef ORBIT_DIAG_LOG
        logger::info("FB create %dx%d alloc=%u free=%u blk=%u", w, h,
                     static_cast<unsigned>(heap_caps_get_allocated_size(s_sprite.getBuffer())),
                     static_cast<unsigned>(esp_get_free_heap_size()),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
#endif
        return true;
    }

    if (ensureHeapSprite(w, h, 16) || ensureHeapSprite(w, h, 8)) {
        logger::warn("Framebuffer %d bpp su heap normale (heap %u KB)", s_spriteBpp * 8,
                     static_cast<unsigned>(esp_get_free_heap_size() / 1024));
        return true;
    }

    logger::warn("Framebuffer non allocato: disegno diretto (heap %u KB, DMA %u KB)",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA) / 1024));
    return false;
}

}  // namespace

bool begin() {
    if (!orbit_hal::displayBegin()) {
        logger::error("pannello non inizializzato");
    }
    // Parte direttamente in landscape (rotazione 1, il default): creare prima
    // lo sprite in portrait e poi ricrearlo raddoppiava le allocazioni grosse
    // all'avvio, con rischio di fallire per frammentazione dell'heap.
    s_rotation = 1;
    M5Cardputer.Display.setRotation(s_rotation);
    M5Cardputer.Display.setBrightness(128);
    M5Cardputer.Display.fillScreen(kBlack);
    const bool ok = ensureSprite(M5Cardputer.Display.width(), M5Cardputer.Display.height());
    logger::info("Framebuffer %ux%u %s %ubpp (%u KB, heap libero %u KB)",
                 static_cast<unsigned>(M5Cardputer.Display.width()),
                 static_cast<unsigned>(M5Cardputer.Display.height()),
                 !ok ? "diretto" : (s_fallbackMode ? "sprite RAM" : "sprite DMA"),
                 static_cast<unsigned>(s_spriteBpp * 8),
                 static_cast<unsigned>(s_spriteBytes / 1024),
                 static_cast<unsigned>(esp_get_free_heap_size() / 1024));
    if (ok) {
        g().fillScreen(kBackground);
        push();
    }
    return ok;
}

void applyRotation(uint8_t index) {
    const uint8_t previous = s_rotation;
    s_rotation = index & 0x03;
    M5Cardputer.Display.setRotation(s_rotation);

    if (!ensureSprite(M5Cardputer.Display.width(), M5Cardputer.Display.height())) {
        // Il nuovo framebuffer non e' allocabile: torna alla rotazione
        // precedente invece di restare senza double buffer (flicker).
        s_rotation = previous;
        M5Cardputer.Display.setRotation(s_rotation);
        ensureSprite(M5Cardputer.Display.width(), M5Cardputer.Display.height());
        logger::warn("Rotazione annullata: framebuffer non allocabile");
    }

    clear();
    push();
}

bool framebufferActive() {
    return s_spriteOk;
}

uint32_t framebufferBytes() {
    return s_spriteBytes;
}

uint8_t framebufferBpp() {
    return static_cast<uint8_t>(s_spriteBpp * 8);
}

bool framebufferFallback() {
    return s_fallbackMode;
}

bool releaseFramebuffer() {
    if (!s_spriteOk) {
        // Nessun framebuffer attivo, ma un buffer di ripiego orfano va liberato.
        releaseFallbackBuffer();
        return false;
    }
#ifdef ORBIT_DIAG_LOG
    logger::info("FB release pre free=%u blk=%u",
                 static_cast<unsigned>(esp_get_free_heap_size()),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
#endif
    s_sprite.deleteSprite();
    releaseFallbackBuffer();
    s_spriteOk = false;
    s_spriteBytes = 0;
#ifdef ORBIT_DIAG_LOG
    logger::info("FB release post free=%u blk=%u",
                 static_cast<unsigned>(esp_get_free_heap_size()),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
#endif
    return true;
}

bool restoreFramebuffer() {
    return ensureSprite(M5Cardputer.Display.width(), M5Cardputer.Display.height());
}

uint8_t rotation() {
    return s_rotation;
}

lgfx::LGFXBase& g() {
    if (s_spriteOk) {
        return static_cast<lgfx::LGFXBase&>(s_sprite);
    }
    return static_cast<lgfx::LGFXBase&>(M5Cardputer.Display);
}

int width() {
    return g().width();
}

int height() {
    return g().height();
}

void clear(uint16_t color) {
    g().fillScreen(color);
}

void push() {
    if (s_spriteOk) {
        s_sprite.pushSprite(0, 0);
    }
}

void push(int x, int y, int w, int h) {
    if (!s_spriteOk) {
        return;
    }
    const int sw = s_sprite.width();
    const int sh = s_sprite.height();
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > sw) {
        w = sw - x;
    }
    if (y + h > sh) {
        h = sh - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    const uint8_t* base = static_cast<const uint8_t*>(s_sprite.getBuffer());
    if (base == nullptr) {
        return;
    }
    // Ogni riga e' contigua: le strisce a piena larghezza si pushano senza
    // copia, le altre passano da un buffer statico (nessuna allocazione).
    static uint8_t s_row[240 * 2];
    const int stride = s_sprite.width() * s_spriteBpp;
    const auto srcDepth = s_sprite.getColorDepth();
    auto& d = M5Cardputer.Display;
    const auto dstDepth = d.getColorDepth();
    d.startWrite();
    if (w == sw) {
        for (int r = 0; r < h; ++r) {
            lgfx::pixelcopy_t pc(base + (y + r) * stride, dstDepth, srcDepth);
            d.pushImage(0, y + r, w, 1, &pc);
        }
    } else if (w <= static_cast<int>(sizeof(s_row) / s_spriteBpp)) {
        for (int r = 0; r < h; ++r) {
            std::memcpy(s_row, base + (y + r) * stride + x * s_spriteBpp,
                        static_cast<size_t>(w) * s_spriteBpp);
            lgfx::pixelcopy_t pc(s_row, dstDepth, srcDepth);
            d.pushImage(x, y + r, w, 1, &pc);
        }
    }
    d.endWrite();
}

namespace {

// Font pixel 5x7 disegnato dal codice: nessun font della libreria grafica.
//  - font 0: micro, advance 6, altezza 8 (label, hint)
//  - font 2: body, advance 7, altezza 9 (testo corrente)
//  - font 4: headline, scala 2x, advance 12, altezza 16
int advanceFor(uint8_t font) {
    if (font == 4) {
        return 12;
    }
    return font == 0 ? 6 : 7;
}

int glyphHeight(uint8_t font) {
    if (font == 4) {
        return 16;
    }
    return font == 0 ? 8 : 9;
}

void drawGlyph(int x, int y, char ch, uint16_t color, uint8_t scale) {
    uint8_t c = static_cast<uint8_t>(ch);
    if (c < pixelfont::kFirst || c > pixelfont::kLast) {
        c = '?';
    }
    const uint8_t* glyph = pixelfont::kGlyphs[c - pixelfont::kFirst];
    auto& d = g();
    for (int col = 0; col < 5; ++col) {
        const uint8_t bits = glyph[col];
        if (bits == 0) {
            continue;
        }
        // Colonna in run verticali: meno chiamate di drawPixel.
        int row = 0;
        while (row < 8) {
            if (!(bits & (1u << row))) {
                ++row;
                continue;
            }
            int end = row;
            while (end < 8 && (bits & (1u << end))) {
                ++end;
            }
            if (scale == 1) {
                d.drawFastVLine(x + col, y + row, end - row, color);
            } else {
                d.fillRect(x + col * scale, y + row * scale, scale, (end - row) * scale, color);
            }
            row = end;
        }
    }
}

int widthForLen(size_t len, uint8_t font) {
    if (len == 0) {
        return 0;
    }
    const int adv = advanceFor(font);
    return static_cast<int>(len) * adv - (adv - 5 * (font == 4 ? 2 : 1));
}

// Disegna senza costruire std::string: i letterali e i buffer dei chiamanti
// non producono allocazioni per frame.
void drawSpan(int x, int y, const char* s, size_t len, uint16_t color, uint8_t font) {
    const uint8_t scale = (font == 4) ? 2 : 1;
    const int adv = advanceFor(font);
    int cx = x;
    for (size_t i = 0; i < len; ++i) {
        const char ch = s[i];
        if (ch != ' ') {
            drawGlyph(cx, y, ch, color, scale);
        }
        cx += adv;
    }
}

}  // namespace

int textWidth(const std::string& s, uint8_t font) {
    return widthForLen(s.size(), font);
}

int textWidth(const char* s, uint8_t font) {
    return s == nullptr ? 0 : widthForLen(std::strlen(s), font);
}

int textHeight(uint8_t font) {
    return glyphHeight(font);
}

void text(int x, int y, const std::string& s, uint16_t color, uint8_t font) {
    drawSpan(x, y, s.data(), s.size(), color, font);
}

void text(int x, int y, const char* s, uint16_t color, uint8_t font) {
    if (s != nullptr) {
        drawSpan(x, y, s, std::strlen(s), color, font);
    }
}

void textCentered(int y, const std::string& s, uint16_t color, uint8_t font) {
    text((width() - textWidth(s, font)) / 2, y, s, color, font);
}

void textCentered(int y, const char* s, uint16_t color, uint8_t font) {
    text((width() - textWidth(s, font)) / 2, y, s, color, font);
}

void wrapText(int x, int y, int maxWidth, const std::vector<std::string>& lines, uint16_t color,
              uint8_t font, int lineHeight) {
    (void)maxWidth;  // Le righe sono gia' mandate a capo dal chiamante.
    for (const auto& line : lines) {
        text(x, y, line, color, font);
        y += lineHeight;
    }
}

namespace {

// Stato del rilevamento euristico della ricarica per i modelli senza PMIC.
int16_t s_battBaseline = -1;
uint32_t s_battSampleAt = 0;
uint8_t s_battRise = 0;
bool s_battCharging = false;

// Cache delle letture di tensione/livello/carica: sono letture lente e non
// cambiano abbastanza da giustificarne una a ogni frame.
constexpr uint32_t kBattCacheMs = 2000;
uint32_t s_battReadAt = 0;
bool s_battReadValid = false;
bool s_battExplicit = false;
int s_battLevel = -1;
uint32_t s_battLevelAt = 0;
bool s_battLevelValid = false;

int batteryLevelCached() {
    const uint32_t now = millis();
    if (!s_battLevelValid || now - s_battLevelAt >= kBattCacheMs) {
        s_battLevel = static_cast<int>(M5Cardputer.Power.getBatteryLevel());
        s_battLevelAt = now;
        s_battLevelValid = true;
    }
    return s_battLevel;
}

}  // namespace

void resetChargingDetector() {
    s_battBaseline = -1;
    s_battSampleAt = 0;
    s_battRise = 0;
    s_battCharging = false;
    s_battReadAt = 0;
    s_battReadValid = false;
    s_battExplicit = false;
    s_battLevelAt = 0;
    s_battLevelValid = false;
}

bool chargingLikely() {
    // Controlla il tempo PRIMA di toccare il PMIC/ADC.
    const uint32_t now = millis();
    if (s_battReadValid && now - s_battReadAt < kBattCacheMs) {
        return s_battExplicit || s_battCharging;
    }
    s_battReadAt = now;
    s_battReadValid = true;

    // Solo lo stato esplicito "in carica" e' affidabile: su Cardputer/ADV
    // isCharging() ritorna charge_unknown, che come bool sarebbe sempre vero.
    s_battExplicit = false;
    if (s_battExplicit) {
        return true;
    }
    // Se il PMIC espone VBUS (M5PM1/AXP), quello e' il dato affidabile:
    // l'euristica sotto non vede una ricarica gia' attiva al boot. -1 =
    // modello senza monitor VBUS (Cardputer/ADV: ADC batteria).
    const int16_t vbus = M5Cardputer.Power.getVBUSVoltage();
    if (vbus >= 0) {
        s_battCharging = vbus > 4000;
        return s_battCharging;
    }
    const int16_t mv = M5Cardputer.Power.getBatteryVoltage();
    if (mv <= 0) {
        return false;
    }
    if (s_battBaseline < 0) {
        s_battBaseline = mv;
        s_battSampleAt = now;
        return false;
    }
    if (now - s_battSampleAt < kBattCacheMs) {
        return s_battCharging;
    }
    s_battSampleAt = now;

    const int16_t delta = static_cast<int16_t>(mv - s_battBaseline);
    if (s_battCharging) {
        // Il caricatore e' stato staccato: la tensione crolla sotto la linea
        // di base registrata prima della ricarica.
        if (delta < -15) {
            s_battCharging = false;
            s_battRise = 0;
            s_battBaseline = mv;
        }
        return s_battCharging;
    }

    // Gradino persistente sopra la tensione a riposo: probabile caricatore.
    if (delta > 15) {
        if (s_battRise < 3) {
            ++s_battRise;
        }
    } else {
        s_battRise = 0;
    }
    // La linea di base insegue lentamente la tensione a riposo.
    s_battBaseline = static_cast<int16_t>((s_battBaseline * 7 + mv) / 8);
    if (s_battRise >= 3) {
        s_battCharging = true;
    }
    return s_battCharging;
}

uint16_t batterySignature() {
    const int level = batteryLevelCached();
    const int pct = level < 0 ? -1 : std::min(level, 100);
    return static_cast<uint16_t>((chargingLikely() ? 0x100 : 0) |
                                 static_cast<uint16_t>(pct & 0xFF));
}

void drawBatteryWidget(int right, int y) {
    auto& d = g();
    const int level = batteryLevelCached();
    const bool charging = chargingLikely();
    const int percent = level < 0 ? -1 : std::min(level, 100);

    uint16_t color = C(T_dim);
    char label[8];
    if (percent < 0) {
        std::snprintf(label, sizeof(label), "--%%");
    } else {
        std::snprintf(label, sizeof(label), "%d%%", percent);
        color = percent <= 15 ? C(T_err) : (percent <= 35 ? C(T_warn) : C(T_ok));
    }

    constexpr int kBodyW = 18;
    constexpr int kBodyH = 8;
    const int bodyX = right - (kBodyW + 2);
    const int bodyY = y;
    text(bodyX - 4 - textWidth(label, 0), y - 1, label, color, 0);

    // Fuel gauge telemetrico: cornice + segmenti.
    d.drawRect(bodyX, bodyY, kBodyW, kBodyH, C(T_line));
    d.fillRect(bodyX + kBodyW, bodyY + 2, 2, kBodyH - 4, C(T_line));
    if (percent > 0) {
        const int inner = kBodyW - 4;
        const int fillW = inner * percent / 100;
        for (int i = 0; i < fillW; i += 3) {
            const int seg = std::min(2, fillW - i);
            d.fillRect(bodyX + 2 + i, bodyY + 2, seg, kBodyH - 4, color);
        }
    }
    if (charging) {
        const int cx = bodyX + kBodyW / 2;
        d.drawLine(cx + 1, bodyY + 1, cx - 2, bodyY + 3, C(T_acc));
        d.drawLine(cx - 2, bodyY + 3, cx + 2, bodyY + 3, C(T_acc));
        d.drawLine(cx + 2, bodyY + 3, cx - 1, bodyY + 6, C(T_acc));
    }
}

void header(const std::string& title, const std::string& parent) {
    auto& d = g();
    constexpr int kBar = 22;
    d.fillRect(0, 0, width(), kBar, C(T_bg2));
    d.drawFastHLine(0, 0, width(), C(T_line));
    d.drawFastHLine(0, kBar - 1, width(), C(T_line));
    // Tacche telemetriche sul bordo superiore.
    for (int x = 26; x < width() - 70; x += 14) {
        d.drawPixel(x, 0, C(T_line2));
    }

    // Emblema orbitale: anello + satellite.
    d.drawCircle(8, 11, 4, C(T_acc));
    d.drawPixel(8, 11, C(T_text));
    d.drawPixel(12, 8, C(T_sec));

    constexpr int kTitleX = 17;
    constexpr int kReserve = 44;  // solo la batteria: il segnale sta nel footer
    const int maxTitleW = width() - kTitleX - kReserve;
    const bool hasParent = !parent.empty();
    const int parentLen = static_cast<int>(parent.size());
    const int totalLen = parentLen + (hasParent ? 3 : 0) + static_cast<int>(title.size());

    uint8_t font = 2;
    if (widthForLen(totalLen, 2) > maxTitleW) {
        font = 0;
    }
    const bool split = hasParent && widthForLen(totalLen, font) <= maxTitleW;
    const int textY = font == 0 ? 7 : 6;
    if (split) {
        // Breadcrumb attenuato, titolo corrente in evidenza. Disegnare i pezzi
        // evita la stringa temporanea "parent > title" a ogni ridisegno.
        const int crumbLen = parentLen + 3;
        text(kTitleX, textY, parent, C(T_dim), font);
        text(kTitleX + widthForLen(parentLen, font), textY, " > ", C(T_dim), font);
        text(kTitleX + widthForLen(crumbLen, font), textY, title, C(T_text), font);
    } else if (widthForLen(totalLen, font) > maxTitleW) {
        constexpr size_t kCap = 96;
        if (static_cast<size_t>(totalLen) + 3 <= kCap) {
            char buf[kCap];
            size_t n = 0;
            if (hasParent) {
                std::memcpy(buf, parent.data(), parent.size());
                n = parent.size();
                buf[n++] = ' ';
                buf[n++] = '>';
                buf[n++] = ' ';
            }
            std::memcpy(buf + n, title.data(), title.size());
            n += title.size();
            const int adv = advanceFor(font);
            const int pad = adv - 5 * (font == 4 ? 2 : 1);
            int keep = (maxTitleW + pad) / adv - 2;
            if (keep < 0) {
                keep = 0;
            }
            if (keep > static_cast<int>(n)) {
                keep = static_cast<int>(n);
            }
            buf[keep] = '.';
            buf[keep + 1] = '.';
            buf[keep + 2] = '\0';
            text(kTitleX, textY, buf, C(T_text), font);
        } else {
            std::string full = hasParent ? (parent + " > " + title) : title;
            while (!full.empty() && textWidth(full + "..", font) > maxTitleW) {
                full.pop_back();
            }
            full += "..";
            text(kTitleX, textY, full, C(T_text), font);
        }
    } else if (hasParent) {
        text(kTitleX, textY, parent, C(T_text), font);
        text(kTitleX + widthForLen(parentLen, font), textY, " > ", C(T_text), font);
        text(kTitleX + widthForLen(parentLen + 3, font), textY, title, C(T_text), font);
    } else {
        text(kTitleX, textY, title, C(T_text), font);
    }

    // Batteria (fuel gauge) a destra: unico elemento dello stato di bordo.
    drawBatteryWidget(width() - 3, 7);
}

namespace {

bool s_footerLed = false;
bool s_ledOn = false;
bool s_ledValid = false;

// Disegna un testo troncato con ".." senza costruire stringhe temporanee.
void textClipped(int x, int y, const std::string& s, int maxWidth, uint16_t color) {
    if (textWidth(s, 0) <= maxWidth) {
        text(x, y, s, color, 0);
        return;
    }
    const int adv = advanceFor(0);
    const int pad = adv - 5;
    int keep = (maxWidth + pad) / adv - 2;
    if (keep < 0) {
        keep = 0;
    }
    if (keep > static_cast<int>(s.size())) {
        keep = static_cast<int>(s.size());
    }
    drawSpan(x, y, s.data(), keep, color, 0);
    drawSpan(x + keep * adv, y, "..", 2, color, 0);
}

}  // namespace

void footer(const std::string& hint, const std::string& trailing) {
    auto& d = g();
    const int h = height();
    d.fillRect(0, h - 12, width(), 12, C(T_bg2));
    d.drawFastHLine(0, h - 12, width(), C(T_line));
    // Segmento d'accento: la "riga di stato" del computer di bordo.
    d.fillRect(0, h - 12, 14, 1, C(T_acc));
    d.drawPixel(4, h - 7, C(T_acc));

    constexpr int kHintX = 9;
    constexpr int kGap = 4;
    if (!trailing.empty()) {
        const int trailingX = width() - 6 - textWidth(trailing, 0);
        const int hintMax = trailingX - kGap - kHintX;
        if (hintMax >= textWidth("..", 0)) {
            textClipped(kHintX, h - 10, hint, hintMax, C(T_dim));
        }
        text(trailingX, h - 10, trailing, C(T_acc), 0);
        s_footerLed = false;
        s_ledValid = false;
        return;
    }
    const int hintMax = width() - kHintX - 12;  // spazio per il LED a destra
    if (hintMax >= textWidth("..", 0)) {
        textClipped(kHintX, h - 10, hint, hintMax, C(T_dim));
    }
    // LED di stato pulsante.
    const bool blink = (millis() / 700) % 2 == 0;
    d.fillCircle(width() - 6, h - 6, 2, blink ? C(T_ok) : C(T_line2));
    s_footerLed = true;
    s_ledOn = blink;
    s_ledValid = true;
}

void footerTick() {
    if (!s_footerLed) {
        return;
    }
    const bool blink = (millis() / 700) % 2 == 0;
    if (s_ledValid && blink == s_ledOn) {
        return;
    }
    s_ledOn = blink;
    s_ledValid = true;
    g().fillCircle(width() - 6, height() - 6, 2, blink ? C(T_ok) : C(T_line2));
    push(width() - 9, height() - 9, 7, 7);
}

void drawIcon(const std::string& icon, int x, int y, int size) {
    size = std::max(8, std::min(32, size));
    // Icone procedurali Orbit: nessun asset, stessa estetica in ogni tema.
    if (drawOrbitIcon(icon, x, y, size)) {
        return;
    }
    // Fallback minimo: iniziale dell'id (o '?') in un riquadro.
    auto& d = g();
    d.drawRoundRect(x + 2, y + 2, size - 4, size - 4, 4, C(T_dim));
    const char initial = icon.empty() ? '?' : static_cast<char>(toupper(icon[0]));
    char buf[2] = {initial, 0};
    text(x + size / 2 - 3, y + size / 2 - 5, buf, C(T_dim), 2);
}

namespace {

// Attende ms millisecondi ridisegnando; true se l'utente preme un tasto.
template <typename Fn>
bool bootFrames(uint32_t ms, Fn&& draw) {
    const uint32_t start = millis();
    uint32_t frame = 0;
    for (;;) {
        // Aggiorna la tastiera: senza questo isPressed() vede lo stato vecchio
        // e lo splash non e' saltabile.
        navigation::update();
        const uint32_t now = millis();
        const uint32_t elapsed = now - start;
        if (elapsed >= ms) {
            break;
        }
        draw(elapsed, frame);
        push();
        if (M5Cardputer.Keyboard.isPressed()) {
            return true;
        }
        delay(30);
        ++frame;
    }
    return false;
}

inline float easeOut(float t) {
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return 1.0f - (1.0f - t) * (1.0f - t);
}

}  // namespace

void splash() {
    clear(C(T_sky0));

    // ── Fase 1: carta del titolo. Il pianeta sorge, il razzo entra in orbita.
    const uint32_t titleMs = 1250;
    bool skipped = bootFrames(titleMs, [&](uint32_t t, uint32_t frame) {
        const float p = static_cast<float>(t) / static_cast<float>(titleMs);
        orbit::sky(0, 0, width(), height(), 0x51ED);
        const int cy = 150 - static_cast<int>(easeOut(p) * 46.0f);
        orbit::orbitPath(width() / 2, cy, 96, 24, C(T_line2), C(T_line), static_cast<int>(frame / 3));
        orbit::planet(width() / 2, cy, 38, -0.6f, -0.5f, true);

        const float a = -1.15f + easeOut(p) * 2.6f;
        const int rx = static_cast<int>(sinf(a) * 96);
        const int ry = static_cast<int>(cosf(a) * 24);
        orbit::rocket(width() / 2 + rx, cy + ry, 13, a - 0.35f, true,
                      static_cast<uint8_t>(frame));

        if (p > 0.18f) {
            const uint16_t titleColor = C(T_hi);
            textCentered(14, "ORBIT", titleColor, 4);
            textCentered(34, "OS 3.0 IDF", C(T_acc), 0);
            textCentered(45, "flight deck online", C(T_dim), 0);
            textCentered(56, "MUSIC - BLE - WIFI", C(T_ok), 0);
        }
        // Telemetria di fondo: striscia HUD che chiude la scena.
        auto& d = g();
        d.fillRect(0, height() - 16, width(), 16, C(T_bg2));
        d.drawFastHLine(0, height() - 16, width(), C(T_line));
        d.fillRect(0, height() - 16, 14, 1, C(T_acc));
        text(6, height() - 14, "SYS " + std::string(__DATE__), C(T_dim), 0);
        orbit::signalBars(width() - 22, height() - 14, 4, C(T_ok), C(T_line2));
    });

    if (!skipped) {
        // ── Fase 2: rampa di lancio, countdown, decollo.
        const uint32_t bootMs = 1900;
        const int padY = 104;
        skipped = bootFrames(bootMs, [&](uint32_t t, uint32_t frame) {
            const float p = static_cast<float>(t) / static_cast<float>(bootMs);
            orbit::sky(0, 0, width(), height(), 0xB007);
            auto& d = g();

            // Terreno, piattaforma e torre di lancio (complesso a destra).
            const int padX = 194;
            d.fillRect(0, padY, width(), height() - padY, C(T_sky2));
            d.drawFastHLine(0, padY, width(), C(T_line));
            for (int x = 6; x < width(); x += 11) {
                d.drawPixel(x, padY + 4, C(T_line2));
            }
            d.fillRect(padX - 18, padY - 2, 38, 3, C(T_line));
            d.fillRect(212, padY - 40, 4, 38, C(T_line));
            d.fillRect(206, padY - 40, 10, 2, C(T_line));
            d.drawLine(206, padY - 32, 202, padY - 32, C(T_line2));
            d.drawLine(206, padY - 24, 199, padY - 24, C(T_line2));
            d.drawFastVLine(214, padY - 50, 10, C(T_line2));
            d.fillRect(213, padY - 53, 3, 3, C(T_err));
            d.drawPixel(214, padY - 52, C(T_hi));

            // Countdown.
            const int step = static_cast<int>(t / 380);
            const char* label = step == 0 ? "T-3" : (step == 1 ? "T-2" : (step == 2 ? "T-1" : "T+0"));
            const bool ignition = (t > 1450);
            text(10, 12, "RAMP 01", C(T_dim), 0);
            text(10, 22, label, ignition ? C(T_acc) : C(T_hi), 4);
            text(10, 44, ignition ? "MOTORI ACCESI" : "SEQUENZA DI LANCIO",
                 ignition ? C(T_flame0) : C(T_sec), 0);
            text(10, 55, ignition ? "spinta nominale" : "telemetria nominale", C(T_dim), 0);
            text(10, 66, ignition ? "GATE OPEN" : "GATE HOLD",
                 ignition ? C(T_ok) : C(T_line2), 0);

            // Razzo: fermo sulla rampa, poi decollo con accelerazione.
            int rocketY = padY - 16;
            bool flame = false;
            if (ignition) {
                flame = true;
                const float lift = easeOut((t - 1450) / 450.0f);
                rocketY -= static_cast<int>(lift * 125.0f);
            }
            if (!ignition && step >= 3) {
                flame = true;  // sfiammata di accensione
            }
            // Fumo alla base.
            const int puffs = ignition ? std::min(6, static_cast<int>((t - 1450) / 60) + 1) : 1;
            for (int i = 0; i < puffs; ++i) {
                const int px = padX - 22 + ((i * 37) % 46);
                const int py = padY - 2 - ((i * 13) % 10);
                const int pr = 5 + (i * 3) % 7;
                orbit::ditherRect(px - pr, py - pr / 2, pr * 2, pr, C(T_line2), C(T_sky2), 8);
            }
            if (rocketY > -36) {
                orbit::rocket(padX, rocketY, 24, 0.0f, flame,
                              static_cast<uint8_t>(frame + 2));
            }

            // Barra di stato del lancio.
            const int barW = width() - 20;
            d.drawRect(10, height() - 18, barW, 7, C(T_line));
            d.fillRect(12, height() - 16,
                       static_cast<int>((barW - 4) * std::min(1.0f, p * 1.05f)), 3,
                       ignition ? C(T_flame1) : C(T_sec));
            text(12, height() - 28, ignition ? "DECOLLO" : "PRE-FLIGHT CHECK", C(T_dim), 0);
        });
    }

    // Chiusura: lampo d'orbita e passaggio al menu.
    clear(C(T_bg));
    orbit::sky(0, 0, width(), height(), 0x0B17);
    textCentered(height() / 2 - 12, "ORBIT READY", C(T_hi), 4);
    textCentered(height() / 2 + 10, "flight deck online", C(T_acc), 0);
    progressBar(40, height() - 26, width() - 80, 6, 1.0f, C(T_ok));
    push();
    delay(skipped ? 120 : 260);
    navigation::waitRelease();
}

void led(bool on, int x, int y, uint16_t color) {
    auto& d = g();
    d.fillCircle(x, y, 3, on ? color : C(T_line2));
    if (on) {
        d.drawCircle(x, y, 5, orbit::blend(color, C(T_bg), 150));
        d.drawPixel(x, y, C(T_hi));
    }
}

void progressBar(int x, int y, int w, int h, float fraction, uint16_t color) {
    auto& d = g();
    fraction = std::max(0.0f, std::min(1.0f, fraction));
    d.drawRect(x, y, w, h, C(T_line));
    d.fillRect(x + 1, y + 1, w - 2, h - 2, C(T_bg));
    const int inner = w - 4;
    const int fill = static_cast<int>(inner * fraction);
    if (fill > 0) {
        d.fillRect(x + 2, y + 2, fill, h - 4, color);
    }
    // Tacche ogni 8 px: scala di telemetria.
    for (int i = 0; i < inner; i += 8) {
        if (i >= fill) {
            d.drawFastVLine(x + 2 + i, y + 2, h - 4, C(T_line2));
        }
    }
    if (fraction > 0.0f && fraction < 1.0f) {
        d.fillRect(x + 2 + fill - 1, y + 1, 1, h - 2, C(T_hi));
    }
}

void progressRing(int cx, int cy, int radius, float fraction, uint16_t color) {
    auto& d = g();
    fraction = std::max(0.0f, std::min(1.0f, fraction));
    const int total = 96;
    const int lit = static_cast<int>(total * fraction);
    for (int i = 0; i < total; ++i) {
        const float a = (static_cast<float>(i) / total) * 6.2831853f - 1.5707963f;
        const int x = cx + static_cast<int>(cosf(a) * radius);
        const int y = cy + static_cast<int>(sinf(a) * radius);
        const bool on = i < lit;
        const bool tick = (i % 8) == 0;
        const uint16_t c = on ? color : C(T_line2);
        d.drawPixel(x, y, c);
        if (tick) {
            const int x2 = cx + static_cast<int>(cosf(a) * (radius + 2));
            const int y2 = cy + static_cast<int>(sinf(a) * (radius + 2));
            d.drawPixel(x2, y2, c);
        }
        if (on) {
            const int x2 = cx + static_cast<int>(cosf(a) * (radius - 1));
            const int y2 = cy + static_cast<int>(sinf(a) * (radius - 1));
            d.drawPixel(x2, y2, orbit::blend(color, C(T_bg), 120));
        }
    }
    d.drawPixel(cx, cy, C(T_dim));
}

void trajectory(int x, int y, int w, int h, float fraction, const std::string& label) {
    auto& d = g();
    fraction = std::max(0.0f, std::min(1.0f, fraction));
    // Curva di ascesa: quarto di ellisse dal basso a sinistra all'alto a destra.
    const int steps = 48;
    const int lit = static_cast<int>(steps * fraction);
    int lastX = x;
    int lastY = y + h;
    for (int i = 0; i <= steps; ++i) {
        const float a = 3.1415927f * 0.5f * (1.0f - static_cast<float>(i) / steps);
        const int px = x + static_cast<int>((1.0f - cosf(a)) * w);
        const int py = y + h - static_cast<int>(sinf(a) * h);
        if (i % 3 == 0) {
            d.drawPixel(px, py, i <= lit ? C(T_sec) : C(T_line2));
        }
        if (i == lit) {
            lastX = px;
            lastY = py;
        }
    }
    d.drawFastHLine(x, y + h, w, C(T_line2));
    orbit::rocket(lastX, lastY, 11, 0.6f, fraction > 0.02f && fraction < 0.995f, 0);
    if (!label.empty()) {
        text(x, y + h + 3, label, C(T_dim), 0);
    }
}

bool saveScreenshot(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const int w = width();
    const int h = height();
    const int rowSize = ((w * 3 + 3) / 4) * 4;
    const uint32_t pixelBytes = static_cast<uint32_t>(rowSize) * h;
    uint8_t header[54] = {0};
    header[0] = 'B';
    header[1] = 'M';
    auto put16 = [](uint8_t* p, uint16_t v) {
        p[0] = static_cast<uint8_t>(v & 0xFF);
        p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    };
    auto put32 = [](uint8_t* p, uint32_t v) {
        p[0] = static_cast<uint8_t>(v & 0xFF);
        p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    };
    put32(header + 2, 54 + pixelBytes);
    put32(header + 10, 54);
    put32(header + 14, 40);
    put32(header + 18, static_cast<uint32_t>(w));
    put32(header + 22, static_cast<uint32_t>(h));
    put16(header + 26, 1);
    put16(header + 28, 24);
    put32(header + 34, pixelBytes);
    put32(header + 38, 2835);
    put32(header + 42, 2835);

    bool ok = std::fwrite(header, 1, sizeof(header), file) == sizeof(header);
    std::vector<uint8_t> row(static_cast<size_t>(rowSize), 0);
    for (int y = h - 1; ok && y >= 0; --y) {
        for (int x = 0; x < w; ++x) {
            const uint16_t c = g().readPixel(x, y);
            row[static_cast<size_t>(x) * 3] =
                static_cast<uint8_t>((c & 0x1F) * 255 / 31);
            row[static_cast<size_t>(x) * 3 + 1] =
                static_cast<uint8_t>(((c >> 5) & 0x3F) * 255 / 63);
            row[static_cast<size_t>(x) * 3 + 2] =
                static_cast<uint8_t>(((c >> 11) & 0x1F) * 255 / 31);
        }
        ok = std::fwrite(row.data(), 1, row.size(), file) == row.size();
    }
    std::fclose(file);
    return ok;
}

}  // namespace display
