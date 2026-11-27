#pragma once

namespace games {
namespace ui {

// Attende INVIO (true) o ESC (false) sulla schermata di game over.
// Nessun effetto grafico: i pannelli restano quelli disegnati da ogni gioco.
bool waitRetry();

}  // namespace ui
}  // namespace games
