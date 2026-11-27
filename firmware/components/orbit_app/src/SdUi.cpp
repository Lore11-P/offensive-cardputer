#include "SdUi.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <esp_random.h>
#include <esp_vfs_fat.h>
#include <sys/stat.h>
#include <unistd.h>

#include "Display.h"
#include "MenuManager.h"
#include "Navigation.h"
#include "Ui.h"
#include "orbit_compat.h"
#include "orbit_settings/Settings.h"
#include "orbit_store/Store.h"

namespace orbit_app {
namespace {

constexpr size_t kMaxViewBytes = 12288;
constexpr size_t kMaxLines = 512;
constexpr size_t kMaxEntries = 200;

struct Entry {
    std::string name;
    bool dir = false;
};

std::string joinPath(const std::string& base, const std::string& name) {
    if (base.empty() || base == "/") {
        return "/" + name;
    }
    if (base.back() == '/') {
        return base + name;
    }
    return base + "/" + name;
}

std::string humanSize(uint64_t bytes) {
    char buf[32];
    if (bytes >= 1024ULL * 1024ULL) {
        std::snprintf(buf, sizeof(buf), "%llu MB",
                      static_cast<unsigned long long>(bytes / (1024 * 1024)));
    } else if (bytes >= 1024) {
        std::snprintf(buf, sizeof(buf), "%llu KB",
                      static_cast<unsigned long long>(bytes / 1024));
    } else {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

std::vector<Entry> listEntries(const std::string& path, bool* truncated) {
    std::vector<Entry> out;
    *truncated = false;
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
        return out;
    }
    for (dirent* item = readdir(dir); item != nullptr; item = readdir(dir)) {
        const std::string name = item->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        if (out.size() >= kMaxEntries) {
            *truncated = true;
            break;
        }
        Entry entry;
        entry.name = name;
        entry.dir = item->d_type == DT_DIR;
        if (item->d_type == DT_UNKNOWN) {
            struct stat st = {};
            if (stat(joinPath(path, name).c_str(), &st) == 0) {
                entry.dir = S_ISDIR(st.st_mode);
            }
        }
        out.push_back(entry);
    }
    closedir(dir);
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        if (a.dir != b.dir) {
            return a.dir;
        }
        return a.name < b.name;
    });
    return out;
}

void showFile(const std::string& path) {
    navigation::waitRelease();
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        ui::message("File", {"!Impossibile aprire:", path}, "SD");
        return;
    }
    std::string text;
    text.resize(kMaxViewBytes);
    const size_t got = std::fread(&text[0], 1, text.size(), file);
    std::fclose(file);
    text.resize(got);

    std::vector<std::string> lines;
    const size_t maxChars =
        static_cast<size_t>(std::max(16, display::width() - 8) / 6);
    for (size_t start = 0; start < text.size() && lines.size() < kMaxLines;) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        size_t len = end - start;
        if (len > 0 && text[end - 1] == '\r') {
            --len;
        }
        const char* data = text.c_str() + start;
        if (len == 0) {
            lines.emplace_back();
        }
        for (size_t pos = 0; pos < len && lines.size() < kMaxLines; pos += maxChars) {
            lines.emplace_back(data + pos, std::min(maxChars, len - pos));
        }
        start = end + 1;
    }
    if (lines.empty()) {
        lines.emplace_back("<vuoto>");
    }

    int top = 0;
    const int rows = std::max(1, (display::height() - 36) / 9);
    for (;;) {
        display::clear(display::C(display::T_bg));
        display::header("File", path);
        const int maxTop = std::max(0, static_cast<int>(lines.size()) - rows);
        top = std::max(0, std::min(top, maxTop));
        for (int i = 0; i < rows; ++i) {
            const int idx = top + i;
            if (idx >= static_cast<int>(lines.size())) {
                break;
            }
            display::text(4, 25 + i * 9, lines[idx], display::C(display::T_text), 0);
        }
        char status[48];
        std::snprintf(status, sizeof(status), "%d/%d  ESC",
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

void browse(const std::string& path, int depth) {
    if (depth >= 7) {
        ui::message("Sfoglia SD", {"!Cartelle troppo annidate."}, "SD");
        return;
    }
    bool truncated = false;
    const std::vector<Entry> entries = listEntries(path, &truncated);
    if (entries.empty()) {
        ui::message("Sfoglia SD", {"Cartella vuota o illeggibile.", path}, "SD");
        return;
    }
    MenuManager menu("Sfoglia", path);
    for (const Entry& entry : entries) {
        const std::string child = joinPath(path, entry.name);
        if (entry.dir) {
            menu.addItem(entry.name, "folder", [child, depth] { browse(child, depth + 1); });
        } else {
            menu.addItem(entry.name, "file", [child] { showFile(child); });
        }
    }
    menu.setHint(truncated ? "solo 200 voci  ESC=indietro" : "ESC=indietro  ENTER=apri");
    menu.run(true);
}

void showInfo() {
    uint64_t total = 0;
    uint64_t freeBytes = 0;
    std::vector<std::string> lines;
    if (esp_vfs_fat_info("/sd", &total, &freeBytes) == ESP_OK) {
        const uint64_t used = total > freeBytes ? total - freeBytes : 0;
        lines.push_back(">microSD montata");
        lines.push_back("Totale: " + humanSize(total));
        lines.push_back("Usato: " + humanSize(used));
        lines.push_back("Libero: " + humanSize(freeBytes));
    } else {
        lines.push_back("!microSD non montata");
    }
    lines.push_back("Heap libero: " + std::to_string(esp_get_free_heap_size() / 1024) + " KB");
    ui::message("SD", lines, "SD");
}

void screenshot() {
    if (!orbit_store::mountSd()) {
        ui::message("Screenshot", {"!microSD non montata."}, "SD");
        return;
    }
    mkdir("/sd/screenshots", 0777);
    char path[80];
    std::snprintf(path, sizeof(path), "/sd/screenshots/shot_%08lX.bmp",
                  static_cast<unsigned long>(esp_random()));
    const bool ok = display::saveScreenshot(path);
    ui::message("Screenshot",
                {ok ? ">Immagine salvata:" : "!Salvataggio fallito", path}, "SD");
}

void backupSettings() {
    if (!orbit_store::mountSd()) {
        ui::message("Backup", {"!microSD non montata."}, "SD");
        return;
    }
    mkdir("/sd/orbit-backup", 0777);
    FILE* file = std::fopen("/sd/orbit-backup/settings.txt", "wb");
    if (file == nullptr) {
        ui::message("Backup", {"!Scrittura non riuscita."}, "SD");
        return;
    }
    std::fprintf(file, "OrbitOS 3.0\nbright=%u\ntheme=%u\nrot=%u\nnick=%s\n",
                 static_cast<unsigned>(orbit_settings::brightness()),
                 static_cast<unsigned>(orbit_settings::theme()),
                 static_cast<unsigned>(orbit_settings::rotation()),
                 orbit_settings::nickname().c_str());
    std::fclose(file);
    ui::message("Backup", {">Impostazioni salvate:", "/sd/orbit-backup/settings.txt"}, "SD");
}

}  // namespace

void runSdMenu() {
    if (!orbit_store::mountSd()) {
        ui::message("SD", {"!microSD non montata.", "Inserisci la card e riprova."}, "Home");
        return;
    }
    MenuManager menu("SD", "Home");
    menu.addItem("Sfoglia", "folder", [] { browse("/sd", 0); });
    menu.addItem("Info spazio", "chip", [] { showInfo(); });
    menu.addItem("Screenshot", "camera", [] { screenshot(); });
    menu.addItem("Backup impostazioni", "store", [] { backupSettings(); });
    menu.setHint("ENTER=apri  `=indietro");
    menu.run(true);
}

}  // namespace orbit_app
