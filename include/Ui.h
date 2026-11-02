#pragma once

#include <string>
#include <vector>

// Schermate di servizio condivise: messaggi, conferme e avviso legale.
namespace ui {

void message(const std::string& title, const std::vector<std::string>& lines,
             const std::string& parent = "", const std::string& hint = "ENTER/` = torna");

// Conferma esplicita: true se l'utente preme ENTER, false con ESC.
bool confirm(const std::string& title, const std::vector<std::string>& lines,
             const std::string& parent = "");

// Avviso legale obbligatorio prima di qualsiasi trasmissione RF offensiva.
bool legalWarning(const std::string& activity, const std::vector<std::string>& extra = {},
                  const std::string& parent = "");

void waitAnyKey(const std::string& title, const std::string& parent = "");

// Disegna una schermata con header e righe gia' formattate.
void drawScreen(const std::string& title, const std::string& parent,
                const std::vector<std::string>& lines, const std::string& hint,
                uint16_t accent = 0);

}  // namespace ui
