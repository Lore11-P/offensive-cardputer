#pragma once

#include "MascotLogic.h"

// Mascotte interattiva "Zorbo" (aliena simpatica).
// I moduli offensivi chiamano award() solo dopo il legalWarning e a fine
// azione; i punti sono fissi e non dipendono dal numero di pacchetti.
namespace mascot {

// Carica punteggio/affetto da NVS. Chiamare una volta in app_main.
void begin();

// Aggiunge i punti dell'evento e salva. Sicura anche se begin() non e' stata
// chiamata: carica lo stato al primo uso.
void award(Event event);

// Schermata interattiva: ENTER = coccola, ESC = esci.
void runMenu();

}  // namespace mascot
