#pragma once

namespace navigation {

// Eventi logici prodotti dalla tastiera della Cardputer.
enum class Event {
    None,
    Up,      // ';'
    Down,    // '.'
    Left,    // ','
    Right,   // '/'
    Select,  // Invio
    Back,    // '`' (tasto esc fisico) o backspace con FN non necessario
    Text     // altro carattere stampabile
};

struct EventData {
    Event type = Event::None;
    char ch = 0;
};

// Inizializza lo stato di rilevamento (da chiamare dopo M5Cardputer.begin).
void begin();

// Aggiorna lo stato hardware della tastiera (M5Cardputer.update()).
void update();

// Restituisce un nuovo evento (fronta i tasti premuti, non le ripetizioni).
EventData poll();

// Stato "tenuto premuto" utile per i giochi e per l'auto-repeat dei menu.
bool pressed(char c);
bool enterPressed();
bool backPressed();
// True se un tasto qualsiasi (o Invio/ESC) e' premuto.
bool anyPressed();
// Distinguono Backspace da ESC (`) per gli input di testo.
bool backspacePressed();
bool backtickPressed();

// Attende il rilascio di tutti i tasti (evita di consumare il tasto Invio
// usato per aprire una schermata successiva).
void waitRelease();

}  // namespace navigation
