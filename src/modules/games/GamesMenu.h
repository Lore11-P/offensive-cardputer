#pragma once

namespace games {
void runMenu();

// I giochi forzano il display in landscape e ripristinano la rotazione scelta
// dall'utente all'uscita.
void enterGameDisplay();
void exitGameDisplay();
}
