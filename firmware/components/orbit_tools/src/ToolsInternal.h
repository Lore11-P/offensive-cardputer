#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "Navigation.h"

namespace orbit_tools {

constexpr const char* kNvsNamespace = "orbit-tools";

bool nvsReadString(const char* key, std::string& out);
bool nvsWriteString(const char* key, const std::string& value);
bool nvsEraseKey(const char* key);

// Editor locale a una riga: INVIO conferma, Backspace cancella, ` annulla.
bool textInput(const std::string& title, const std::string& parent, const std::string& prompt,
               std::string& value, size_t maxLen);

// La UI trasforma ; . , / in frecce: i tool di testo hanno bisogno del
// carattere originale. Questa funzione fa la conversione inversa.
char eventChar(const navigation::EventData& ev);

// Ridisegna solo l'header quando cambia la batteria (schermate lunghe).
void refreshHeader(const std::string& title, const std::string& parent, uint16_t& lastBattery);

// Testo ASCII stampabile, troncato, per le etichette dei menu.
std::string asciiSnippet(const std::string& text, size_t maxChars);

void clockScreen();
void notesMenu();
void calculatorScreen();
void totpMenu();
void qrScreen();
void infoScreen();
void sdBrowser(const std::string& path);

}  // namespace orbit_tools
