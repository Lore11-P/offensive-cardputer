#pragma once

// Hub del menu principale: raggruppano i moduli per area senza cambiare
// l'entry point o il comportamento dei moduli stessi.
namespace hub {

void networkMenu();
void bluetoothMenu();
void analysisMenu();
void mediaMenu();
void toolsMenu();
void systemMenu();

// Giochi resta una voce diretta: il modulo ha gia' il proprio menu.
void gamesEntry();

}  // namespace hub
