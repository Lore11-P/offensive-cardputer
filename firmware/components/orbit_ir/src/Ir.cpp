#include "orbit_ir/Ir.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <esp_random.h>
#include <esp_system.h>
#include <nvs.h>
#include <sys/stat.h>

#include "Display.h"
#include "IrProtocol.h"
#include "IrRmt.h"
#include "MenuManager.h"
#include "Navigation.h"
#include "Ui.h"
#include "orbit_compat.h"
#include "orbit_mascot/Mascot.h"
#include "orbit_store/Store.h"

namespace orbit_ir {
namespace {

constexpr const char* kNvsNamespace = "orbit-ir";
constexpr const char* kNvsKeyLast = "last";
constexpr const char* kNvsKeyFavorites = "fav";
constexpr size_t kMaxSearchChars = 24;
constexpr size_t kMaxSearchResults = 40;
constexpr size_t kPerCategoryResults = 8;
constexpr size_t kPerBrandResults = 3;
constexpr uint32_t kJammerDurationMs = 30000;
// Sotto questa soglia il parsing (file 16 KB + raw 16 KB + menu) rischia di
// fallire: meglio un messaggio che un abort del firmware.
constexpr uint32_t kMinParseHeap = 56 * 1024;

bool g_sdMounted = false;
bool g_lfsMounted = false;
std::string g_lastPath;
std::vector<std::string> g_favorites;

std::string lowerAscii(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

std::string asciiOnly(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        out.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?');
    }
    return out;
}

bool isIrFile(const std::string& name) {
    if (name.size() <= 3) {
        return false;
    }
    const size_t pos = name.size() - 3;
    return name[pos] == '.' &&
           (name[pos + 1] == 'i' || name[pos + 1] == 'I') &&
           (name[pos + 2] == 'r' || name[pos + 2] == 'R');
}

std::string stripIr(std::string name) {
    if (isIrFile(name)) {
        name.resize(name.size() - 3);
    }
    return name;
}

bool isDir(const std::string& path) {
    struct stat st = {};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool isFile(const std::string& path) {
    struct stat st = {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::vector<std::string> listDirs(const std::string& path) {
    std::vector<std::string> out;
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
        return out;
    }
    for (dirent* entry = readdir(dir); entry != nullptr;
         entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        bool directory = entry->d_type == DT_DIR;
        if (entry->d_type == DT_UNKNOWN) {
            directory = isDir(path + "/" + name);
        }
        if (directory) {
            out.push_back(name);
        }
    }
    closedir(dir);
    std::sort(out.begin(), out.end(),
              [](const std::string& a, const std::string& b) {
                  return lowerAscii(a) < lowerAscii(b);
              });
    return out;
}

bool hasIrFile(const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
        return false;
    }
    bool found = false;
    for (dirent* entry = readdir(dir); entry != nullptr;
         entry = readdir(dir)) {
        if (isIrFile(entry->d_name)) {
            found = true;
            break;
        }
    }
    closedir(dir);
    return found;
}

std::vector<std::string> mountedRoots() {
    std::vector<std::string> roots;
    if (g_sdMounted) {
        roots.emplace_back("/sd");
    }
    if (g_lfsMounted) {
        roots.emplace_back("/littlefs");
    }
    return roots;
}

// Le enumerazioni su SD possono durare piu' del timeout del task watchdog:
// cede la CPU ogni tanto senza cambiare il risultato.
void cooperativeYield(size_t& counter) {
    if ((++counter % 8) == 0) {
        delay(1);
    }
}

void mountRoots() {
    g_sdMounted = orbit_store::mountSd();
    g_lfsMounted = orbit_store::mountLittleFs();
}

struct Category {
    std::string dir;
    std::string label;
    int rank = 100;
};

struct Brand {
    std::string name;
};

struct Remote {
    std::string path;
    std::string name;
};

struct CategoryDef {
    const char* dir;
    const char* label;
    int rank;
};

constexpr CategoryDef kCategoryDefs[] = {
    {"TV", "TV", 0},
    {"TVs", "TV", 0},
    {"Clima", "Clima", 1},
    {"ACs", "Clima", 1},
    {"Air_Conditioners", "Clima", 1},
    {"Audio", "Audio", 2},
    {"DVD", "DVD", 3},
    {"DVDs", "DVD", 3},
    {"DVD_Players", "DVD", 3},
    {"Blu-Ray", "DVD", 3},
    {"Proiettore", "Proiettore", 4},
    {"Proiettori", "Proiettore", 4},
    {"Projectors", "Proiettore", 4},
    {"Console", "Console", 5},
    {"Consoles", "Console", 5},
    {"Universale", "Universale", 6},
    {"Universal", "Universale", 6},
    {"Universal_TV_Remotes", "Universale", 6},
    {"Altro", "Altro", 7},
    {"Other", "Altro", 7},
    {"Misc", "Altro", 7},
};

bool knownCategory(const std::string& dir, std::string& label, int& rank) {
    const std::string id = lowerAscii(dir);
    for (const CategoryDef& def : kCategoryDefs) {
        if (id == lowerAscii(def.dir)) {
            label = def.label;
            rank = def.rank;
            return true;
        }
    }
    return false;
}

bool isKnownCategoryName(const std::string& dir) {
    std::string label;
    int rank = 0;
    return knownCategory(dir, label, rank);
}

std::vector<Category> collectCategories() {
    std::vector<Category> out;
    std::vector<std::string> seen;
    bool flatBrandFound = false;
    size_t work = 0;

    for (const std::string& root : mountedRoots()) {
        const std::string ir = root + "/ir";
        for (const std::string& name : listDirs(ir)) {
            cooperativeYield(work);
            const std::string base = ir + "/" + name;
            std::string label;
            int rank = 0;
            const bool known = knownCategory(name, label, rank);
            const std::vector<std::string> subdirs = listDirs(base);
            const bool files = hasIrFile(base);
            if (known || !subdirs.empty()) {
                if (!files && subdirs.empty()) {
                    continue;
                }
                const std::string key = lowerAscii(name);
                if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
                    continue;
                }
                seen.push_back(key);
                out.push_back({name, known ? label : asciiOnly(name),
                               known ? rank : 100});
            } else if (files && !isKnownCategoryName(name)) {
                flatBrandFound = true;
            }
        }
    }

    if (flatBrandFound) {
        const bool hasTv = std::any_of(
            out.begin(), out.end(),
            [](const Category& category) { return category.label == "TV"; });
        if (!hasTv) {
            out.push_back({"TV", "TV", 0});
        }
    }

    std::sort(out.begin(), out.end(),
              [](const Category& a, const Category& b) {
                  if (a.rank != b.rank) {
                      return a.rank < b.rank;
                  }
                  return lowerAscii(a.label) < lowerAscii(b.label);
              });
    return out;
}

std::vector<Brand> collectBrands(const Category& category) {
    std::vector<Brand> out;
    std::vector<std::string> seen;
    size_t work = 0;
    const bool tv = category.label == "TV";

    const auto add = [&](const std::string& name) {
        const std::string key = lowerAscii(name);
        if (std::find(seen.begin(), seen.end(), key) == seen.end()) {
            seen.push_back(key);
            out.push_back({name});
        }
    };

    for (const std::string& root : mountedRoots()) {
        const std::string ir = root + "/ir";
        const std::string base = ir + "/" + category.dir;
        for (const std::string& name : listDirs(base)) {
            cooperativeYield(work);
            if (hasIrFile(base + "/" + name)) {
                add(name);
            }
        }
        if (tv) {
            for (const std::string& name : listDirs(ir)) {
                cooperativeYield(work);
                if (isKnownCategoryName(name)) {
                    continue;
                }
                if (hasIrFile(ir + "/" + name)) {
                    add(name);
                }
            }
        }
    }

    std::sort(out.begin(), out.end(),
              [](const Brand& a, const Brand& b) {
                  return lowerAscii(a.name) < lowerAscii(b.name);
              });
    return out;
}

std::vector<Remote> listRemotes(const Category& category,
                                const Brand& brand) {
    std::vector<Remote> out;
    std::vector<std::string> seen;
    const auto collect = [&](const std::string& base) {
        DIR* dir = opendir(base.c_str());
        if (dir == nullptr) {
            return;
        }
        for (dirent* entry = readdir(dir); entry != nullptr;
             entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (!isIrFile(name)) {
                continue;
            }
            const std::string key = lowerAscii(name);
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
                continue;
            }
            seen.push_back(key);
            out.push_back({base + "/" + name, stripIr(name)});
        }
        closedir(dir);
    };

    for (const std::string& root : mountedRoots()) {
        const std::string ir = root + "/ir";
        collect(ir + "/" + category.dir + "/" + brand.name);
        // Il vecchio albero piatto /ir/<marca> resta visibile dentro TV.
        if (category.label == "TV") {
            collect(ir + "/" + brand.name);
        }
    }

    std::sort(out.begin(), out.end(), [](const Remote& a, const Remote& b) {
        return lowerAscii(a.name) < lowerAscii(b.name);
    });
    return out;
}

std::string favoriteLabel(const std::string& path) {
    const size_t last = path.find_last_of('/');
    if (last == std::string::npos) {
        return stripIr(path);
    }
    const std::string name = stripIr(path.substr(last + 1));
    const size_t previous =
        last == 0 ? std::string::npos : path.find_last_of('/', last - 1);
    if (previous == std::string::npos) {
        return name;
    }
    return path.substr(previous + 1, last - previous - 1) + "/" + name;
}

bool nvsRead(const char* key, std::string& out) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t length = 0;
    esp_err_t err = nvs_get_str(handle, key, nullptr, &length);
    bool ok = false;
    if (err == ESP_OK && length > 0) {
        std::vector<char> buffer(length);
        err = nvs_get_str(handle, key, buffer.data(), &length);
        if (err == ESP_OK && buffer[0] != '\0') {
            out.assign(buffer.data());
            ok = true;
        }
    }
    nvs_close(handle);
    return ok;
}

bool nvsWrite(const char* key, const std::string& value) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_str(handle, key, value.c_str());
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

void loadPreferences() {
    g_lastPath.clear();
    g_favorites.clear();
    nvsRead(kNvsKeyLast, g_lastPath);

    std::string packed;
    if (!nvsRead(kNvsKeyFavorites, packed)) {
        return;
    }
    size_t start = 0;
    while (start <= packed.size() && g_favorites.size() < kMaxFavorites) {
        const size_t end = packed.find('\n', start);
        std::string path = packed.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!path.empty() &&
            std::find(g_favorites.begin(), g_favorites.end(), path) ==
                g_favorites.end()) {
            g_favorites.push_back(std::move(path));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
}

void saveFavorites() {
    std::string packed;
    for (size_t i = 0; i < g_favorites.size(); ++i) {
        if (i != 0) {
            packed.push_back('\n');
        }
        packed += g_favorites[i];
    }
    nvsWrite(kNvsKeyFavorites, packed);
}

void saveLast(const std::string& path) {
    g_lastPath = path;
    nvsWrite(kNvsKeyLast, path);
}

bool isFavorite(const std::string& path) {
    return std::find(g_favorites.begin(), g_favorites.end(), path) !=
           g_favorites.end();
}

enum class FavoriteResult { Added, Removed, Full };

FavoriteResult toggleFavorite(const std::string& path) {
    auto it = std::find(g_favorites.begin(), g_favorites.end(), path);
    if (it != g_favorites.end()) {
        g_favorites.erase(it);
        saveFavorites();
        return FavoriteResult::Removed;
    }
    if (g_favorites.size() >= kMaxFavorites) {
        return FavoriteResult::Full;
    }
    g_favorites.push_back(path);
    saveFavorites();
    return FavoriteResult::Added;
}

void drawBusy(const std::string& title, const std::string& line) {
    display::clear(display::C(display::T_bg));
    display::header(title, "Infrarossi");
    display::textCentered(60, line, display::C(display::T_text), 2);
    display::footer("attendi");
    display::push();
}

void sendAndReport(const IrCommand& command, MenuManager& menu) {
    if (!command.supported) {
        ui::message(asciiOnly(command.name),
                    {"!Protocollo non supportato.",
                     "Formati: NEC/NECext, Samsung32,",
                     "Sony, RC5/RC5X, RC6, LG, JVC,",
                     "Kaseikyo, RCA oppure raw."},
                    "Infrarossi");
        return;
    }
    if (!sendCommand(command)) {
        ui::message("Errore IR",
                    {"!Invio non riuscito.",
                     "Frame non valido o RMT non",
                     "disponibile su GPIO44."},
                    "Infrarossi");
        return;
    }
    std::string name = asciiOnly(command.name);
    if (name.size() > 18) {
        name.resize(16);
        name += "..";
    }
    menu.setHint("Inviato: " + name + "  x3");
}

int findQuickCommand(const std::vector<IrCommand>& commands, char key) {
    int best = -1;
    int bestScore = -1;
    for (int i = 0; i < static_cast<int>(commands.size()); ++i) {
        const int score = quickKeyScore(commands[i].name, key);
        if (score > bestScore) {
            bestScore = score;
            best = i;
        }
    }
    return bestScore > 0 ? best : -1;
}

void openRemote(const std::string& path, const std::string& title) {
    if (esp_get_free_heap_size() < kMinParseHeap) {
        ui::message(asciiOnly(title),
                    {"!Memoria insufficiente.",
                     "Chiudi altre app e riprova."},
                    "Telecomandi");
        return;
    }
    const ParsedFile parsed = parseIrFile(path);
    if (parsed.tooLarge) {
        ui::message(asciiOnly(title),
                    {"!File troppo grande (>16 KB).",
                     "Riduci il numero di comandi."},
                    "Telecomandi");
        return;
    }
    if (parsed.readError) {
        ui::message(asciiOnly(title),
                    {"!Impossibile leggere il file.",
                     "Controlla la microSD e riprova."},
                    "Telecomandi");
        return;
    }
    if (parsed.commands.empty()) {
        ui::message(asciiOnly(title),
                    {"!Nessun comando riconosciuto.",
                     "Il file .ir potrebbe essere",
                     "non valido o vuoto."},
                    "Telecomandi");
        return;
    }

    saveLast(path);

    MenuManager menu(asciiOnly(title), "Telecomandi");
    const int count =
        std::min(static_cast<int>(parsed.commands.size()),
                 static_cast<int>(kMaxCommands));
    for (int i = 0; i < count; ++i) {
        const IrCommand& command = parsed.commands[i];
        std::string label = asciiOnly(command.name);
        if (!command.supported) {
            label += (command.protocol == Protocol::Raw && command.truncated)
                         ? " [raw >512]"
                         : " [n/d]";
        }
        menu.addItem(label, "ir",
                     [&parsed, &menu, i] {
                         sendAndReport(parsed.commands[i], menu);
                     },
                     command.supported);
    }
    menu.setHint("P/M/+-/C/X=invio F=fav ESC=esci");
    menu.setTextHandler([&parsed, &menu, path](char ch) {
        if (ch == 'f' || ch == 'F') {
            switch (toggleFavorite(path)) {
                case FavoriteResult::Added:
                    menu.setHint("Aggiunto ai preferiti");
                    break;
                case FavoriteResult::Removed:
                    menu.setHint("Rimosso dai preferiti");
                    break;
                case FavoriteResult::Full:
                    menu.setHint("Preferiti pieni (max 8)");
                    break;
            }
            return true;
        }
        if (!isQuickKey(ch)) {
            return false;
        }
        const int index = findQuickCommand(parsed.commands, ch);
        if (index < 0) {
            menu.setHint("Nessun comando per questo tasto");
            return true;
        }
        sendAndReport(parsed.commands[index], menu);
        return true;
    });
    menu.run(true);
}

void openBrand(const Category& category, const Brand& brand) {
    const std::vector<Remote> remotes = listRemotes(category, brand);
    if (remotes.empty()) {
        ui::message(asciiOnly(brand.name),
                    {"Nessun file .ir per questa marca."}, "Telecomandi");
        return;
    }

    MenuManager menu(asciiOnly(brand.name), "Telecomandi");
    for (const Remote& remote : remotes) {
        std::string label = asciiOnly(remote.name);
        if (isFavorite(remote.path)) {
            label = "* " + label;
        }
        menu.addItem(label, "ir",
                     [path = remote.path, name = remote.name] {
                         openRemote(path, name);
                     });
    }
    menu.setHint("ENTER=apri  ESC=indietro");
    menu.run(true);
}

void openCategory(const Category& category) {
    const std::vector<Brand> brands = collectBrands(category);
    if (brands.empty()) {
        ui::message(asciiOnly(category.label),
                    {"Nessun telecomando in questa categoria."},
                    "Telecomandi");
        return;
    }

    MenuManager menu(asciiOnly(category.label), "Telecomandi");
    for (const Brand& brand : brands) {
        menu.addItem(asciiOnly(brand.name), "ir",
                     [category, brand] { openBrand(category, brand); });
    }
    menu.setHint("ENTER=marca  ESC=indietro");
    menu.run(true);
}

void runFavorites() {
    if (g_favorites.empty()) {
        ui::message("Preferiti",
                    {"Nessun preferito salvato.",
                     "Apri un telecomando e premi F",
                     "per aggiungerlo qui."},
                    "Infrarossi");
        return;
    }

    std::vector<Remote> alive;
    for (const std::string& path : g_favorites) {
        if (isFile(path)) {
            alive.push_back({path, favoriteLabel(path)});
        }
    }
    if (alive.empty()) {
        ui::message("Preferiti",
                    {"!Preferiti non disponibili.",
                     "Reinserisci la microSD: i path",
                     "sono stati conservati."},
                    "Infrarossi");
        return;
    }

    MenuManager menu("Preferiti", "Infrarossi");
    for (const Remote& remote : alive) {
        menu.addItem(asciiOnly(remote.name), "fav",
                     [path = remote.path, name = stripIr(remote.path)] {
                         openRemote(path, name);
                     });
    }
    menu.setHint("ENTER=apri  ESC=indietro");
    menu.run(true);
}

bool editSearch(char* out, size_t capacity) {
    char buffer[kMaxSearchChars + 1] = {};
    size_t length = 0;

    navigation::waitRelease();
    for (;;) {
        navigation::update();
        const navigation::EventData event = navigation::poll();
        if (event.type == navigation::Event::Select) {
            std::snprintf(out, capacity, "%s", buffer);
            navigation::waitRelease();
            return true;
        }
        if (event.type == navigation::Event::Back) {
            if (navigation::backspacePressed()) {
                if (length > 0) {
                    buffer[--length] = '\0';
                }
            } else {
                navigation::waitRelease();
                return false;
            }
        } else if (event.type == navigation::Event::Text &&
                   length < kMaxSearchChars) {
            const char c = event.ch;
            if (c >= 0x20 && c < 0x7F) {
                buffer[length++] = c;
                buffer[length] = '\0';
            }
        }

        display::clear(display::C(display::T_bg));
        display::header("Cerca", "Infrarossi");
        display::text(8, 34, "Marca o modello:", display::C(display::T_dim), 0);
        display::text(8, 50, std::string("> ") + buffer + "_",
                      display::C(display::T_text), 2);
        display::footer("ENTER=cerca  ESC=annulla  BKSP=cancella");
        display::push();
        delay(20);
    }
}

void runSearch() {
    char query[kMaxSearchChars + 1] = {};
    if (!editSearch(query, sizeof(query))) {
        return;
    }
    const std::string needle = lowerAscii(query);
    if (needle.empty()) {
        return;
    }

    const std::vector<Category> categories = collectCategories();
    if (categories.empty()) {
        ui::message("Cerca",
                    {"!Nessun database .ir in /ir.",
                     "Copia i file su microSD in",
                     "/sd/ir/<categoria>/<marca>/."},
                    "Infrarossi");
        return;
    }

    drawBusy("Cerca", "Ricerca in corso...");
    std::vector<std::vector<MenuItem>> buckets(categories.size());
    std::vector<std::string> seen;
    size_t work = 0;
    for (size_t c = 0; c < categories.size(); ++c) {
        for (const Brand& brand : collectBrands(categories[c])) {
            cooperativeYield(work);
            if (buckets[c].size() >= kPerCategoryResults) {
                break;
            }
            const bool brandHit =
                lowerAscii(brand.name).find(needle) != std::string::npos;
            size_t brandCount = 0;
            for (const Remote& remote :
                 listRemotes(categories[c], brand)) {
                if (brandCount >= kPerBrandResults ||
                    buckets[c].size() >= kPerCategoryResults) {
                    break;
                }
                if (!brandHit &&
                    lowerAscii(remote.name).find(needle) ==
                        std::string::npos) {
                    continue;
                }
                const std::string key = lowerAscii(remote.path);
                if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
                    continue;
                }
                seen.push_back(key);
                const std::string title =
                    asciiOnly(categories[c].label + " / " + brand.name +
                              " / " + remote.name);
                buckets[c].emplace_back(
                    title, "ir",
                    [path = remote.path, name = remote.name] {
                        openRemote(path, name);
                    });
                ++brandCount;
            }
        }
    }

    std::vector<MenuItem> results;
    for (size_t round = 0;
         round < kPerCategoryResults && results.size() < kMaxSearchResults;
         ++round) {
        for (size_t c = 0;
             c < buckets.size() && results.size() < kMaxSearchResults; ++c) {
            if (round < buckets[c].size()) {
                results.push_back(std::move(buckets[c][round]));
            }
        }
    }

    if (results.empty()) {
        ui::message("Cerca",
                    {"!Nessun telecomando trovato.",
                     "Prova con il nome della marca",
                     "o con parte del modello."},
                    "Infrarossi");
        return;
    }

    MenuManager menu("Risultati", "Cerca");
    for (MenuItem& item : results) {
        menu.addItem(std::move(item));
    }
    menu.setHint("ENTER=apri  ESC=indietro");
    menu.run(true);
}

void runRemotes() {
    const std::vector<Category> categories = collectCategories();
    if (categories.empty()) {
        ui::message("Telecomandi",
                    {"!Nessun database .ir in /ir.",
                     "Copia Flipper-IRDB su microSD",
                     "in /sd/ir/<categoria>/<marca>/",
                     "oppure su LittleFS in",
                     "/littlefs/ir/<categoria>/<marca>/."},
                    "Infrarossi");
        return;
    }

    MenuManager menu("Telecomandi", "Infrarossi");
    for (const Category& category : categories) {
        menu.addItem(asciiOnly(category.label), "ir",
                     [category] { openCategory(category); });
    }
    menu.setHint("ENTER=categoria  ESC=indietro");
    menu.run(true);
}

void drawJammer(uint32_t packets, uint32_t elapsedMs) {
    const uint32_t seconds =
        std::min<uint32_t>(kJammerDurationMs / 1000, (elapsedMs + 999) / 1000);
    display::clear(display::C(display::T_bg));
    display::header("Jammer IR", "Infrarossi");
    display::textCentered(40, "TRASMISSIONE ATTIVA", display::C(display::T_err),
                          2);
    display::textCentered(62,
                          "Pacchetti: " + std::to_string(packets),
                          display::C(display::T_text), 2);
    display::textCentered(78,
                          "Tempo: " + std::to_string(seconds) + "/30 s",
                          display::C(display::T_dim), 0);
    const float fraction = std::min(
        1.0f, static_cast<float>(elapsedMs) / kJammerDurationMs);
    display::progressBar(20, 94, display::width() - 40, 8, fraction,
                         display::C(display::T_warn));
    display::footer("ESC = interrompi");
    display::push();
}

void runJammer() {
    if (!ui::legalWarning("Jammer IR",
                          {"Disturba i ricevitori infrarossi vicini.",
                           "Solo su dispositivi tuoi e in ambiente isolato."},
                          "Infrarossi")) {
        return;
    }

    navigation::waitRelease();
    const uint32_t start = millis();
    uint32_t packets = 0;
    uint32_t lastDraw = 0;
    std::vector<int32_t> frame;
    frame.reserve(80);

    for (;;) {
        const uint32_t now = millis();
        const uint32_t elapsed = now - start;
        if (elapsed >= kJammerDurationMs) {
            break;
        }
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }

        appendJamFrame(esp_random(), frame);
        if (!sendDurations(frame, 0)) {
            ui::message("Jammer IR",
                        {"!RMT non disponibile.", "GPIO44 occupato."},
                        "Infrarossi");
            return;
        }
        ++packets;

        if (now - lastDraw >= 150) {
            drawJammer(packets, elapsed);
            lastDraw = now;
        }
    }
    if (packets > 0) {
        mascot::award(mascot::Event::IrJammer);
    }
}

}  // namespace

void runMenu() {
    mountRoots();
    loadPreferences();

    MenuManager menu("Infrarossi", "Home");
    if (!g_lastPath.empty() && isFile(g_lastPath)) {
        const std::string label = favoriteLabel(g_lastPath);
        menu.addItem("Riprendi " + asciiOnly(label), "ir",
                     [path = g_lastPath, name = stripIr(g_lastPath)] {
                         openRemote(path, name);
                     });
    }
    menu.addItem("Telecomandi", "ir", runRemotes);
    menu.addItem("Cerca", "scan", runSearch);
    menu.addItem("Preferiti", "fav", runFavorites);
    menu.addItem("Jammer IR", "jammer", runJammer);
    menu.setHint("ESC=indietro  ;.=nav  ENTER=apri");
    menu.run(true);

    releaseTx();
}

}  // namespace orbit_ir
