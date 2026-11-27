#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>

#include "Display.h"
#include "MenuManager.h"
#include "ToolsInternal.h"
#include "Ui.h"
#include "orbit_store/Store.h"

namespace orbit_tools {
namespace {

constexpr size_t kMaxEntries = 64;
constexpr size_t kMaxPath = 160;
constexpr int kMaxDepth = 10;

struct Entry {
    std::string name;
    bool dir = false;
    uint64_t size = 0;
};

std::string joinPath(const std::string& dir, const std::string& name) {
    return dir + "/" + name;
}

bool asciiLess(const std::string& a, const std::string& b) {
    const size_t count = std::min(a.size(), b.size());
    for (size_t i = 0; i < count; ++i) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        const int la = std::tolower(ca);
        const int lb = std::tolower(cb);
        if (la != lb) {
            return la < lb;
        }
    }
    return a.size() < b.size();
}

bool sdReady() {
    DIR* probe = opendir("/sd");
    if (probe != nullptr) {
        closedir(probe);
        return true;
    }
    return orbit_store::mountSd();
}

bool loadEntries(const std::string& path, std::vector<Entry>& entries, bool& truncated) {
    entries.clear();
    truncated = false;
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
        return false;
    }
    for (struct dirent* item = readdir(dir); item != nullptr; item = readdir(dir)) {
        const std::string name = item->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        if (entries.size() >= kMaxEntries) {
            truncated = true;
            break;
        }
        Entry entry;
        entry.name = name;
        const std::string full = joinPath(path, name);
        struct stat st = {};
        if (stat(full.c_str(), &st) == 0) {
            entry.dir = S_ISDIR(st.st_mode);
            entry.size = static_cast<uint64_t>(st.st_size);
        }
        entries.push_back(std::move(entry));
    }
    closedir(dir);
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.dir != b.dir) {
            return a.dir;
        }
        return asciiLess(a.name, b.name);
    });
    return true;
}

void formatSize(uint64_t size, char* out, size_t len) {
    if (size >= 1024ULL * 1024ULL) {
        std::snprintf(out, len, "%.2f MB", static_cast<double>(size) / (1024.0 * 1024.0));
    } else if (size >= 1024ULL) {
        std::snprintf(out, len, "%.1f KB", static_cast<double>(size) / 1024.0);
    } else {
        std::snprintf(out, len, "%llu B", static_cast<unsigned long long>(size));
    }
}

std::string dirTitle(const std::string& path) {
    if (path == "/sd") {
        return "microSD";
    }
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= path.size()) {
        return "microSD";
    }
    return path.substr(slash + 1);
}

void showFile(const std::string& title, const std::string& name, uint64_t size,
              const std::string& full) {
    char sizeText[32];
    formatSize(size, sizeText, sizeof(sizeText));
    ui::message("File",
                {asciiSnippet(name, 36), std::string("Dimensione: ") + sizeText,
                 asciiSnippet(full, 36)},
                title);
}

void sdBrowserAt(const std::string& path, int depth);

void sdBrowserAt(const std::string& path, int depth) {
    if (depth > kMaxDepth) {
        ui::message("microSD", {"!Limite di profondita'.", "Torna indietro e apri una cartella."},
                    "Strumenti");
        return;
    }
    if (path.size() > kMaxPath) {
        ui::message("microSD", {"!Percorso troppo lungo.", "Apri una cartella piu' vicina."},
                    "Strumenti");
        return;
    }
    if (!sdReady()) {
        ui::message("microSD", {"!microSD non montata.", "Inserisci la card."}, "Strumenti");
        return;
    }

    std::vector<Entry> entries;
    bool truncated = false;
    if (!loadEntries(path, entries, truncated)) {
        ui::message("microSD", {"!Cartella non leggibile.", asciiSnippet(path, 36)},
                    "Strumenti");
        return;
    }

    const std::string title = dirTitle(path);
    navigation::waitRelease();
    MenuManager menu(title, "Strumenti");
    for (const Entry& entry : entries) {
        const std::string full = joinPath(path, entry.name);
        if (full.size() > kMaxPath) {
            menu.addItem(entry.name, entry.dir ? "folder" : "file", nullptr, false);
            continue;
        }
        if (entry.dir) {
            menu.addItem(entry.name + "/", "folder", [full, depth] {
                sdBrowserAt(full, depth + 1);
            });
        } else {
            menu.addItem(entry.name, "file", [full, entry, title] {
                showFile(title, entry.name, entry.size, full);
            });
        }
    }
    if (entries.empty()) {
        menu.addItem("Cartella vuota", "folder", nullptr, false);
    }
    if (truncated) {
        menu.addItem("Altri file non mostrati", "file", nullptr, false);
    }
    menu.setHint("ENTER=apri  `=indietro");
    menu.run(true);
}

}  // namespace

void sdBrowser(const std::string& path) {
    sdBrowserAt(path, 0);
}

}  // namespace orbit_tools
