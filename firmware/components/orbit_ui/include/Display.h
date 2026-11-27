#pragma once

#include "orbit_compat.h"
#include <stdint.h>
#include <string>
#include <vector>

#include "OrbitPalette.h"

// Layer grafico "Orbit": sala di controllo in miniatura.
//  - sprite 16bpp come double buffer (nessun flicker)
//  - sette palette orbitali selezionabili a runtime
//  - font pixel 5x7 disegnato dal codice (nessun font di sistema)
//  - header/footer telemetrici, icone procedurali, titoli animati
namespace display {

// Token della palette Orbit (stessi nomi del launcher di riferimento).
enum Tok : uint8_t {
    T_bg,
    T_bg2,
    T_bg3,
    T_line,
    T_line2,
    T_dim,
    T_text,
    T_hi,
    T_acc,
    T_accD,
    T_accL,
    T_sec,
    T_secD,
    T_onAcc,
    T_ok,
    T_warn,
    T_err,
    T_sky0,
    T_sky1,
    T_sky2,
    T_pl0,
    T_pl1,
    T_pl2,
    T_pl3,
    T_pl4,
    T_atm,
    T_city,
    T_star,
    T_accM,
    T_secM,
    T_lineM,
    T_textM,
    T_black,
    T_gold,
    T_goldD,
    T_goldL,
    T_flame0,
    T_flame1,
    T_flame2,
    T_flame3,
    T_ink,
    T_secL,
    T_errD,
    T_errL,
    T_okL,
    T_okD,
    T_COUNT
};

extern uint16_t pal[orbit_palette::kTokenCount];
inline uint16_t C(Tok t) { return pal[t]; }

// Nomi storici della palette, ora rimappati sul tema attivo: tutto il
// firmware esistente cambia atmosfera senza essere toccato.
extern uint16_t kBlack;
extern uint16_t kWhite;
extern uint16_t kBackground;
extern uint16_t kPanel;
extern uint16_t kHeader;
extern uint16_t kAccent;
extern uint16_t kAccentDark;
extern uint16_t kText;
extern uint16_t kTextDim;
extern uint16_t kWarning;
extern uint16_t kDanger;
extern uint16_t kInfo;
extern uint16_t kPaper;
extern uint16_t kPaperLight;
extern uint16_t kInk;
extern uint16_t kMuted;
extern uint16_t kNeoYellow;
extern uint16_t kNeoCyan;
extern uint16_t kNeoPink;
extern uint16_t kNeoGreen;
extern uint16_t kNeoOrange;
extern uint16_t kNeoRed;

constexpr uint8_t kThemeCount = orbit_palette::kThemeCount;
void setTheme(uint8_t id);
uint8_t themeId();
const char* themeName(uint8_t id);
const char* themeNote(uint8_t id);
// Colore di un token in una palette specifica (anteprime, selettore temi).
uint16_t themeColor(uint8_t id, Tok token);

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

// True se il framebuffer (sprite) e' attivo.
bool framebufferActive();
// Byte occupati dal framebuffer (0 se non allocato).
uint32_t framebufferBytes();
// Profondita' in bit (16 o 8) e true se e' il ripiego su heap normale.
uint8_t framebufferBpp();
bool framebufferFallback();
// Libera il framebuffer full-screen per lasciare heap ai giochi; il disegno
// passa diretto al display finche' non viene ricreato.
bool releaseFramebuffer();
bool restoreFramebuffer();

void clear(uint16_t color = kBackground);
void push();
// Pubblica solo una porzione del framebuffer (evita push a schermo intero
// quando cambia solo l'header o il footer).
void push(int x, int y, int w, int h);

// Testo con font pixel disegnato dal codice:
//   0 = micro 5x7 (advance 6), 2 = body (advance 7), 4 = headline (scala 2x)
void text(int x, int y, const std::string& s, uint16_t color, uint8_t font = 2);
void text(int x, int y, const char* s, uint16_t color, uint8_t font = 2);
void textCentered(int y, const std::string& s, uint16_t color, uint8_t font = 2);
void textCentered(int y, const char* s, uint16_t color, uint8_t font = 2);
void wrapText(int x, int y, int maxWidth, const std::vector<std::string>& lines, uint16_t color,
              uint8_t font = 2, int lineHeight = 12);
int textWidth(const std::string& s, uint8_t font = 2);
int textWidth(const char* s, uint8_t font = 2);
int textHeight(uint8_t font = 2);

// Barra superiore telemetrica: titolo, breadcrumb, segnale e batteria.
void header(const std::string& title, const std::string& parent = "");

// Rilevamento della ricarica: VBUS se il PMIC lo espone, altrimenti
// euristico sui modelli senza PMIC.
bool chargingLikely();
void resetChargingDetector();

// Barra inferiore: keycap + telemetria di stato. `trailing` (es. "P2/3")
// viene allineato a destra al posto del LED di stato.
void footer(const std::string& hint, const std::string& trailing = "");
// Aggiorna solo il LED di stato lampeggiante del footer (nessun ridisegno
// del resto della schermata). No-op se l'ultimo footer aveva un `trailing`.
void footerTick();

// Stato batteria in forma compatta: bit 8 = in carica, bit 0..7 = percentuale
// (0xFF se sconosciuta). Usato per ridisegnare l'header solo quando cambia.
uint16_t batterySignature();

// Icona procedurale in stile Orbit (nessun asset preimpostato). Per gli id
// non ancora disegnati ripiega sul BMP in /icons e poi su un glifo neutro.
void drawIcon(const std::string& icon, int x, int y, int size);
// Variante esplicita: solo disegno procedurale. false se l'id e' sconosciuto.
bool drawOrbitIcon(const std::string& icon, int x, int y, int size, uint16_t tint = 0);

// Schermata di avvio: titolo orbitale + rampa, countdown e decollo.
void splash();

// Feedback radio/IR.
void led(bool on, int x, int y, uint16_t color = kDanger);

void progressBar(int x, int y, int w, int h, float fraction, uint16_t color = kAccent);
// Anello di progresso telemetrico (0..1) con tacche e valore centrale.
void progressRing(int cx, int cy, int radius, float fraction, uint16_t color = kAccent);
// Traiettoria di ascesa: curva puntinata con razzo in punta e percentuale.
void trajectory(int x, int y, int w, int h, float fraction, const std::string& label = "");

// Salva il framebuffer corrente come BMP 24 bit (tipicamente sulla SD).
bool saveScreenshot(const std::string& path);

}  // namespace display
