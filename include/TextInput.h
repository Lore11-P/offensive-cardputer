#pragma once

#include <string>
#include <vector>

// Editor di testo a schermo per la tastiera della Cardputer.
//
// In questa modalita' i tasti sono letterali: anche ';' '.' ',' '/' vengono
// inseriti come caratteri, perche' SSID e password possono contenerli. Le
// frecce non sono disponibili mentre si scrive: si usa ENTER per confermare
// (o passare al campo successivo) e backtick per cancellare un carattere.
// Con il campo vuoto, backtick annulla e torna indietro.
namespace textinput {

struct Field {
    const char* label = "";
    std::string value;
    size_t maxLen = 32;
    bool secret = false;   // mostra '*' invece dei caratteri
    bool numeric = false;  // accetta solo 0-9, '.' e '-'
};

// Ritorna false se l'utente annulla (backtick su campo vuoto).
bool edit(const std::string& title, const std::string& parent,
          std::vector<Field>& fields);

// Conversione rigorosa: whole string must be numeric, '.' as decimal separator.
bool parseFloat(const std::string& text, float& out);
bool parseInt(const std::string& text, long& out);

}  // namespace textinput
