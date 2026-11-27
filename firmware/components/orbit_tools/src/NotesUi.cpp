#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "Display.h"
#include "MenuManager.h"
#include "ToolsInternal.h"
#include "Ui.h"

namespace orbit_tools {
namespace {

constexpr int kNoteCount = 8;
constexpr size_t kNoteMax = 64;

std::string noteKey(int index) {
    char key[12];
    std::snprintf(key, sizeof(key), "note%d", index);
    return key;
}

bool loadNote(int index, std::string& out) {
    out.clear();
    if (!nvsReadString(noteKey(index).c_str(), out)) {
        return false;
    }
    if (out.size() > kNoteMax) {
        out.resize(kNoteMax);
    }
    return !out.empty();
}

std::string noteLabel(int index, const std::string& text) {
    char prefix[8];
    std::snprintf(prefix, sizeof(prefix), "%d. ", index + 1);
    return std::string(prefix) + asciiSnippet(text, 26);
}

void editNote(int index, std::function<void()> refresh) {
    std::string value;
    if (!loadNote(index, value)) {
        refresh();
        return;
    }
    if (!textInput("Nota " + std::to_string(index + 1), "Note", "Testo nota (max 64)", value,
                   kNoteMax)) {
        return;
    }
    if (value.empty()) {
        if (ui::confirm("Nota " + std::to_string(index + 1),
                        {"Svuotare ed eliminare la nota?"}, "Note")) {
            if (!nvsEraseKey(noteKey(index).c_str())) {
                ui::message("Note", {"!Eliminazione non riuscita.", "NVS non disponibile."},
                            "Note");
            }
        }
        refresh();
        return;
    }
    if (!nvsWriteString(noteKey(index).c_str(), value)) {
        ui::message("Note", {"!Salvataggio non riuscito.", "NVS non disponibile."}, "Note");
        return;
    }
    refresh();
}

void deleteNote(int index, std::function<void()> refresh) {
    if (!ui::confirm("Nota " + std::to_string(index + 1),
                     {"Eliminare definitivamente la nota?"}, "Note")) {
        return;
    }
    if (!nvsEraseKey(noteKey(index).c_str())) {
        ui::message("Note", {"!Eliminazione non riuscita.", "NVS non disponibile."}, "Note");
        return;
    }
    refresh();
}

void createNote(int slot, std::function<void()> refresh) {
    std::string value;
    if (!textInput("Nuova nota", "Note", "Testo nota (max 64)", value, kNoteMax) ||
        value.empty()) {
        return;
    }
    if (!nvsWriteString(noteKey(slot).c_str(), value)) {
        ui::message("Note", {"!Salvataggio non riuscito.", "NVS non disponibile."}, "Note");
        return;
    }
    refresh();
}

void noteActions(int index, std::function<void()> refresh) {
    MenuManager menu("Nota " + std::to_string(index + 1), "Note");
    menu.addItem("Modifica", "file", [index, refresh] { editNote(index, refresh); });
    menu.addItem("Elimina", "file", [index, refresh] { deleteNote(index, refresh); });
    menu.setHint("ENTER=apri  `=indietro");
    menu.run(true);
}

}  // namespace

void notesMenu() {
    MenuManager menu("Note", "Strumenti");
    std::function<void()> refresh;
    refresh = [&menu, &refresh]() {
        std::vector<MenuItem> items;
        int firstFree = -1;
        int count = 0;
        for (int i = 0; i < kNoteCount; ++i) {
            std::string text;
            if (loadNote(i, text)) {
                ++count;
                items.emplace_back(noteLabel(i, text), "file", [&refresh, i] {
                    noteActions(i, refresh);
                });
            } else if (firstFree < 0) {
                firstFree = i;
            }
        }
        if (count == 0) {
            items.emplace_back("Nessuna nota", "file", nullptr, false);
        }
        if (firstFree >= 0) {
            items.emplace_back("Nuova nota", "file", [&refresh, firstFree] {
                createNote(firstFree, refresh);
            });
        } else {
            items.emplace_back("Limite di 8 note raggiunto", "file", nullptr, false);
        }
        menu.setItems(std::move(items));
    };
    refresh();
    menu.setHint("ENTER=apri  `=indietro");
    menu.run(true);
}

}  // namespace orbit_tools
