#pragma once

#include <cstdint>

// Spegnimento automatico per inattivita'.
//
// Il conto alla rovescia riparte a ogni pressione di tasto. Quando scade viene
// mostrato un preavviso di 10 secondi (annullabile con un tasto qualsiasi) e
// poi il dispositivo si spegne. Il tempo e' config::sleepTimeoutSec(), dove 0
// significa "disattivato".
namespace powertimer {

// Rilegge la configurazione e azzera il conteggio.
void begin();

// Segnala attivita' dell'utente: azzera il conto alla rovescia.
void activity();

// Da chiamare nei loop che leggono la tastiera (Navigation::poll lo fa gia'):
// quando il timeout scade abbassa la luminosita' dello schermo (nessuna
// schermata modale) e dopo 10 s spegne. Qualsiasi tasto annulla.
void tick();

// Sospende il timer per le schermate di solo monitoraggio, dove l'utente
// osserva senza premere tasti (radar, voli). RAII: si riattiva all'uscita.
class Hold {
public:
    Hold();
    ~Hold();
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;
};

// Spegne subito, senza preavviso.
void powerOff();

// Secondi di inattivita' configurati (0 = disattivato).
uint32_t timeoutSeconds();

}  // namespace powertimer
