#pragma once

#include <M5Cardputer.h>
#include <string>
#include <vector>

// Piccolo layer grafico sopra M5GFX:
//  - sprite 16bpp come double buffer (nessun flicker)
//  - header/breadcrumb e footer standard
//  - icone BMP 32x32 caricate da LittleFS (fallback disegnato a runtime)
namespace display {

// Palette RGB565 del tema.
constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kBackground = 0x0861;  // blu notte
constexpr uint16_t kPanel = 0x10E4;       // pannello
constexpr uint16_t kHeader = 0x18E3;      // barra titolo
constexpr uint16_t kAccent = 0x07E0;      // verde terminale
constexpr uint16_t kAccentDark = 0x0340;
constexpr uint16_t kText = 0xE7E7;
constexpr uint16_t kTextDim = 0x8C71;
constexpr uint16_t kWarning = 0xFD20;     // arancio
constexpr uint16_t kDanger = 0xF800;      // rosso
constexpr uint16_t kInfo = 0x05BF;        // azzurro

// Tema neubrutalista per menu e schermate di servizio.
constexpr uint16_t kPaper = 0xFF9A;       // crema
constexpr uint16_t kPaperLight = 0xFFFF;  // bianco
constexpr uint16_t kInk = 0x0000;         // nero
constexpr uint16_t kMuted = 0x5AEB;       // grigio scuro
constexpr uint16_t kNeoYellow = 0xFFE0;
constexpr uint16_t kNeoCyan = 0x07FF;
constexpr uint16_t kNeoPink = 0xF81F;
constexpr uint16_t kNeoGreen = 0x07E0;
constexpr uint16_t kNeoOrange = 0xFD20;
constexpr uint16_t kNeoRed = 0xF800;

// Inizializza display e framebuffer. Ritorna false se lo sprite non e'
// allocabile (in tal caso si disegna direttamente sul display).
bool begin();

// Ricrea il framebuffer dopo un cambio di rotazione.
void applyRotation(uint8_t index);

uint8_t rotation();

// Superficie di disegno attiva (sprite se disponibile, altrimenti display).
lgfx::LGFXBase& g();

int width();
int height();

// True se il framebuffer (sprite) e' attivo. Se e' false la UI disegna
// direttamente sul display e puo' presentare flicker: succede solo quando
// l'heap non riesce ad allocare i ~64 KB del buffer.
bool framebufferActive();
// Byte occupati dal framebuffer (0 se non allocato).
uint32_t framebufferBytes();

void clear(uint16_t color = kBackground);
void push();

// Testo con font built-in: 0 = Font0 (6x8), 2 = Font2 (8x14), 4 = Font4.
void text(int x, int y, const std::string& s, uint16_t color, uint8_t font = 2);
void textCentered(int y, const std::string& s, uint16_t color, uint8_t font = 2);
void wrapText(int x, int y, int maxWidth, const std::vector<std::string>& lines, uint16_t color,
              uint8_t font = 2, int lineHeight = 12);

// Barra superiore con titolo, breadcrumb (es. "WiFi > Evil Portal") e
// indicatore batteria con percentuale e stato di carica.
void header(const std::string& title, const std::string& parent = "");

// Rilevamento della ricarica. Sui modelli con PMIC usa isCharging(), sugli
// altri (Cardputer/ADV, lettura ADC) stima la ricarica dalla tensione della
// batteria: un gradino verso l'alto indica l'inserimento del caricatore, un
// calo netto lo stacco. Il risultato e' euristico ma sufficiente per l'icona.
bool chargingLikely();
void resetChargingDetector();
void footer(const std::string& text);

// Carica /icons/<icon>.bmp (32x32 BMP 24/32 bit) e lo disegna scalato.
// Se il file non esiste, disegna un'icona vettoriale di fallback.
void drawIcon(const std::string& icon, int x, int y, int size);

// Schermata di avvio.
void splash();

// Feedback radio/IR.
void led(bool on, int x, int y, uint16_t color = kDanger);

void progressBar(int x, int y, int w, int h, float fraction, uint16_t color = kAccent);

// Salva il framebuffer corrente come BMP 24 bit (tipicamente sulla SD).
bool saveScreenshot(const std::string& path);

}  // namespace display
