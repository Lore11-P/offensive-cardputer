#pragma once

#include <string>

// Runtime Wren sandbox per i giochi scaricati.
namespace store {

// Esegue un file .wren dalla microSD. Mostra un messaggio in caso di errore.
// ESC interrompe il gioco; nessun binding a filesystem, rete o moduli Wren.
bool runGame(const std::string& path);

}  // namespace store
