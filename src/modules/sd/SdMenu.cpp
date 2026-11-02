#include "SdMenu.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <SD.h>

#include <algorithm>
#include <cstdio>
#include <string>
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

std::string joinPath(const std::string& base, const std::string& name) {
    if (base.empty() || base == "/") {
        return "/" + name;
    }
    if (base.back() == '/') {
        return base + name;
    }
    return base + "/" + name;
}

std::string parentPath(const std::string& path) {
    if (path.empty() || path == "/") {
        return "/";
    }
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
        return "/";
    }
    return path.substr(0, slash);
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string line;
    for (char c : text) {
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            lines.push_back(line);
            line.clear();
        } else {
            line.push_back(c);
        }
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    return lines;
}

std::vector<std::string> wrapLine(const std::string& line, size_t maxChars) {
    std::vector<std::string> out;
    if (line.empty()) {
        out.push_back("");
        return out;
    }
    for (size_t pos = 0; pos < line.size(); pos += maxChars) {
        out.push_back(line.substr(pos, maxChars));
    }
    return out;
}

bool looksText(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return true;
    }
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(c | 0x20); });
    return ext == "txt" || ext == "ir" || ext == "json" || ext == "cfg" ||
           ext == "log" || ext == "ini" || ext == "csv" || ext == "md";
}

void showFile(const std::string& path) {
    navigation::waitRelease();
    std::string text;
    if (!storage::readText(path, text, kMaxViewBytes, true)) {
        ui::message("Apri file", {"!Impossibile leggere il file.", path}, "SD > Sfoglia");
        return;
    }

    std::vector<std::string> lines;
    const size_t maxChars = static_cast<size_t>(std::max(16, display::width() - 8) / 6);
    for (const auto& raw : splitLines(text)) {
        for (auto& part : wrapLine(raw, maxChars)) {
            lines.push_back(std::move(part));
        }
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
        std::snprintf(status, sizeof(status), "%d/%d  `=torna",
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

    const std::vector<storage::Entry> entries = storage::listEntries(path, true);
    MenuManager menu("Sfoglia", path == "/" ? "SD" : path);
    for (const auto& entry : entries) {
        const std::string child = joinPath(path, entry.name);
        if (entry.isDir) {
            menu.addItem(entry.name, "folder", [child] { browse(child); });
        } else {
            menu.addItem(entry.name, "file", [child] { showFile(child); });
        }
    }
    menu.setGrid(3);
    menu.setHint("`=indietro  ;.,/=nav  ENTER=apri");
    menu.run(true);
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
    char path[64];
    std::snprintf(path, sizeof(path), "/screenshots/shot_%lu.bmp",
                  static_cast<unsigned long>(millis()));
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
    for (const auto& line : splitLines(text)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
        if (lines.size() >= 8) {
            break;
        }
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
    menu.setHint("`=indietro  ;.,/=nav  ENTER=apri");
    menu.run(true);
}

}  // namespace sd_module

#endif  // ENABLE_MODULE_SD
