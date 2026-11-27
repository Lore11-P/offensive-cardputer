#pragma once

#include <string>

// Runtime Wren sandbox per i giochi su microSD.
namespace orbit_wren {

// Esegue un file .wren (es. "/sd/games/2048.wren") e blocca il chiamante
// finche' il gioco non esce. false in caso di errore o memoria insufficiente:
// il messaggio va mostrato dalla UI.
bool runGame(const std::string& path);

}  // namespace orbit_wren
