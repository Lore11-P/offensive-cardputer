#include "SdMenu.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <SD.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "Config.h"
#include "Display.h"
#include "MenuManager.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "Storage.h"
#include "Ui.h"

#if ENABLE_MODULE_SD

namespace sd_module {
namespace {

constexpr size_t kMaxViewBytes = 12288;
// Stesso limite della versione precedente: nessuna parte di file in meno.
constexpr size_t kMaxLines = 1024;

std::string joinPath(const std::string& base, const std::string& name) {
    if (base.empty() || base == "/") {
        return "/" + name;
    }
    if (base.back() == '/') {
        return base + name;
    }
    return base + "/" + name;
}

// Spezza una riga in segmenti da maxChars senza costruire copie intermedie
// del file: la memoria usata e' solo quella delle righe visibili.
void appendWrapped(std::vector<std::string>& out, const char* data, size_t len,
                   size_t maxChars) {
    if (len == 0) {
        out.emplace_back();
        return;
    }
    for (size_t pos = 0; pos < len && out.size() < kMaxLines; pos += maxChars) {
        out.emplace_back(data + pos, std::min(maxChars, len - pos));
    }
}

void showFile(const std::string& path) {
    navigation::waitRelease();
    std::string text;
    // readText() e' false anche quando il file supera kMaxViewBytes: in quel
    // caso mostra la parte letta invece di rifiutare il file.
    if (!storage::readText(path, text, kMaxViewBytes, true) && text.empty()) {
        ui::message("Apri file", {"!Impossibile leggere il file.", path}, "SD > Sfoglia");
        return;
    }

    std::vector<std::string> lines;
    const size_t maxChars = static_cast<size_t>(std::max(16, display::width() - 8) / 6);
    for (size_t start = 0; start < text.size() && lines.size() < kMaxLines;) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        size_t len = end - start;
        if (len > 0 && text[end - 1] == '\r') {
            --len;
        }
        appendWrapped(lines, text.c_str() + start, len, maxChars);
        start = end + 1;
    }
    if (lines.empty()) {
        lines.push_back("");
    }

    int top = 0;
    const int rows = std::max(1, (display::height() - 22 - 12 - 2) / 9);
    for (;;) {
        display::clear(display::kPaper);
        display::header("File", path);
        const int maxTop = std::max(0, static_cast<int>(lines.size()) - rows);
        top = std::max(0, std::min(top, maxTop));
        for (int i = 0; i < rows; ++i) {
            const int idx = top + i;
            if (idx >= static_cast<int>(lines.size())) {
                break;
            }
            display::text(4, 25 + i * 9, lines[idx], display::kInk, 0);
        }
        char status[32];
        std::snprintf(status, sizeof(status), "%d/%d  ESC=torna",
                      std::min(top + rows, static_cast<int>(lines.size())),
                      static_cast<int>(lines.size()));
        display::footer(status);
        display::push();

        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            break;
        }
        if (ev.type == navigation::Event::Up) {
            top = std::max(0, top - 1);
        } else if (ev.type == navigation::Event::Down) {
            top = std::min(maxTop, top + 1);
        } else if (ev.type == navigation::Event::Left) {
            top = std::max(0, top - rows);
        } else if (ev.type == navigation::Event::Right) {
            top = std::min(maxTop, top + rows);
        }
        delay(20);
    }
    navigation::waitRelease();
}

void browse(const std::string& path) {
    if (!storage::hasSD()) {
        ui::message("Sfoglia SD", {"!microSD non montata.", "Inserisci una card e riavvia."},
                    "SD");
        return;
    }
    // Ogni livello annidato tiene vivi un MenuManager e le sue lambda sul
    // loopTask (16 KB): un albero profondo lo farebbe esaurire.
    static int depth = 0;
    if (depth >= 8) {
        ui::message("Sfoglia SD", {"!Cartelle troppo annidate."}, "SD");
        return;
    }

    std::vector<storage::Entry> entries = storage::listEntries(path, true);
    constexpr size_t kMaxEntries = 200;
    const bool truncated = entries.size() > kMaxEntries;
    if (truncated) {
        entries.resize(kMaxEntries);
    }
    MenuManager menu("Sfoglia", path == "/" ? "SD" : path);
    ++depth;
    for (const auto& entry : entries) {
        const std::string child = joinPath(path, entry.name);
        if (entry.isDir) {
            menu.addItem(entry.name, "folder", [child] { browse(child); });
        } else {
            menu.addItem(entry.name, "file", [child] { showFile(child); });
        }
    }
    menu.setGrid(3);
    menu.setHint(truncated ? "solo 200 voci  ESC=indietro" : "ESC=indietro  ;.,/=nav  ENTER=apri");
    menu.run(true);
    --depth;
}

void showInfo() {
    std::vector<std::string> lines;
    if (storage::hasSD()) {
        const uint64_t total = SD.totalBytes();
        const uint64_t used = SD.usedBytes();
        const uint64_t freeBytes = total > used ? total - used : 0;
        lines.push_back(">microSD montata");
        lines.push_back("Totale: " + std::to_string(total / (1024 * 1024)) + " MB");
        lines.push_back("Usato: " + std::to_string(used / (1024 * 1024)) + " MB");
        lines.push_back("Libero: " + std::to_string(freeBytes / (1024 * 1024)) + " MB");
    } else {
        lines.push_back("!microSD non montata");
        lines.push_back("Inserisci una card e riavvia.");
    }

    lines.push_back("");
    if (storage::hasLittleFS()) {
        const uint64_t total = LittleFS.totalBytes();
        const uint64_t used = LittleFS.usedBytes();
        lines.push_back(">LittleFS interno");
        lines.push_back("Usato: " + std::to_string(used / 1024) + " KB / " +
                        std::to_string(total / 1024) + " KB");
    }
    ui::message("Info SD", lines, "SD");
}

void takeScreenshot() {
    if (!storage::hasSD()) {
        ui::message("Screenshot", {"!microSD non montata.", "Salvataggio non disponibile."},
                    "SD");
        return;
    }
    storage::mkdir("/screenshots", true);
    // millis() riparte da zero a ogni avvio: senza un suffisso casuale uno
    // screenshot nuovo poteva sovrascrivere quello di una sessione precedente.
    char path[64];
    std::snprintf(path, sizeof(path), "/screenshots/shot_%08lX.bmp",
                  static_cast<unsigned long>(esp_random()));
    const bool ok = display::saveScreenshot(path);
    ui::message("Screenshot", {ok ? ">Immagine salvata:" : "!Salvataggio fallito",
                               ok ? std::string(path) : "Controlla la microSD."},
                "SD");
}

void backupConfig() {
    if (!storage::hasSD()) {
        ui::message("Backup", {"!microSD non montata.", "Backup non disponibile."}, "SD");
        return;
    }
    storage::mkdir("/backup", true);
    const bool ok = storage::writeText("/backup/config.json", config::toJson(), true);

    std::string scores;
    if (storage::readText("/scores.txt", scores, 4096, true)) {
        storage::writeText("/backup/scores.txt", scores, true);
    }
    ui::message("Backup", {ok ? ">Configurazione salvata:" : "!Salvataggio fallito",
                           ok ? "/backup/config.json" : "Controlla la microSD."},
                "SD");
}

void showScores() {
    std::string text;
    if (!storage::readText("/scores.txt", text, 4096, true) || text.empty()) {
        ui::message("Punteggi", {"Nessun punteggio salvato.", "Gioca e ottieni un record!"},
                    "SD");
        return;
    }
    std::vector<std::string> lines;
    for (size_t start = 0; start < text.size();) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        size_t len = end - start;
        if (len > 0 && text[end - 1] == '\r') {
            --len;
        }
        if (len > 0) {
            lines.emplace_back(text.c_str() + start, len);
        }
        start = end + 1;
    }
    // Il file non e' piu' ordinato alfabeticamente (una volta lo era per via
    // della std::map): si ordina qui, cosi' la schermata resta identica.
    for (size_t i = 1; i < lines.size(); ++i) {
        std::string key = std::move(lines[i]);
        size_t j = i;
        while (j > 0 && key < lines[j - 1]) {
            lines[j] = std::move(lines[j - 1]);
            --j;
        }
        lines[j] = std::move(key);
    }
    if (lines.size() > 8) {
        lines.resize(8);
    }
    ui::message("Punteggi", lines, "SD");
}

}  // namespace

void runMenu() {
    MenuManager menu("SD", "Menu principale");
    menu.addItem("Sfoglia", "folder", [] { browse("/"); });
    menu.addItem("Info SD", "sd", showInfo);
    menu.addItem("Screenshot", "camera", takeScreenshot);
    menu.addItem("Backup", "backup", backupConfig);
    menu.addItem("Punteggi", "trophy", showScores);
    menu.setGrid(3);
    menu.setHint("ESC=indietro  ;.,/=nav  ENTER=apri");
    menu.run(true);
}

}  // namespace sd_module

#endif  // ENABLE_MODULE_SD
