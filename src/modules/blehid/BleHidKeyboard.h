#pragma once

#include <cstdint>
#include <string>

#include "ModuleFlags.h"

#if ENABLE_MODULE_BLEHID

namespace blehid {

// Layout della tastiera del PC/telefono a cui viene inviato il testo.
// Il modulo traduce i caratteri (quelli stampati sulla Cardputer, US) nei
// tasti del layout scelto sull'host.
enum class Layout : uint8_t { It = 0, Us = 1 };

// Tasti speciali: stessi valori usati dal parser Ducky di BadUSB.
enum Special : uint8_t {
    kEnter = 0x80,
    kTab,
    kEsc,
    kBackspace,
    kDelete,
    kCapsLock,
    kInsert,
    kHome,
    kEnd,
    kPageUp,
    kPageDown,
    kUp,
    kDown,
    kLeft,
    kRight,
    kF1,
    kF2,
    kF3,
    kF4,
    kF5,
    kF6,
    kF7,
    kF8,
    kF9,
    kF10,
    kF11,
    kF12
};

namespace hid {

// Inizializza NimBLE e il servizio HID (0x1812) con nome ORBIT-<nick>.
// Se lo stack e' gia' attivo (Chat, BLE Spam) viene prima fermato e
// ricreato: nessun modulo resta in esecuzione in parallelo.
bool begin(const std::string& name);

// Ferma advertising e connessioni, poi deinit(true) per restituire la RAM.
void end();

bool active();
bool connected();
// True se l'host ha sottoscritto almeno un report: le notifiche arrivano.
bool subscribed();
const std::string& deviceName();

Layout layout();
void setLayout(Layout value);
// Stato LED ricevuto dall'host (bit 0 num, 1 caps, 2 scroll).
uint8_t leds();

// Conteggio dei report di tasto inviati dall'inizio della sessione.
uint32_t sentKeys();

// Traduce un code point Unicode nei tasti del layout corrente.
bool mapCodepoint(uint32_t codepoint, uint8_t& usage, uint8_t& mods);

// Invia un report HID (modificatori + un tasto) e lo rilascia.
// usage >= 0xE0 viene trattato come modificatore.
bool tapUsage(uint8_t usage, uint8_t mods = 0, uint32_t holdMs = 6);

bool tapSpecial(uint8_t code);
bool tapChar(char c);
// Carattere Unicode con le sequenze di controllo gia' risolte.
bool tapCodepoint(uint32_t codepoint);
// Tasto di uno script: ASCII oppure Special, con modificatori gia' pronti.
bool tapScriptKey(uint8_t code, uint8_t mods = 0);

// Testo con sequenze \n \t \\ e caratteri UTF-8 italiani.
// Ritorna il numero di caratteri inviati.
size_t typeText(const std::string& text);

void releaseAll();

}  // namespace hid
}  // namespace blehid

#endif  // ENABLE_MODULE_BLEHID
