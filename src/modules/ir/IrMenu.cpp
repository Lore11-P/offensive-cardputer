#include "IrMenu.h"

#include <Arduino.h>
#include <IRsend.h>
#include <Preferences.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "Config.h"
#include "Display.h"
#include "Logger.h"
#include "MenuManager.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "PowerTimer.h"
#include "Storage.h"
#include "TextInput.h"
#include "Ui.h"

#if ENABLE_MODULE_IR

namespace ir {
namespace {

enum class Protocol {
    Unknown,
    Nec,
    Samsung,
    Lg,
    Sony,
    Rc5,
    Rc6,
    Jvc,
    Kaseikyo,
    Rca
};

struct IrCommand {
    std::string name;
    std::string protocolName;
    Protocol protocol = Protocol::Unknown;
    uint16_t bits = 32;
    uint32_t address = 0;
    uint32_t command = 0;
    uint32_t frequency = 38000;
    bool raw = false;
    bool rawOverflow = false;
    bool supported = false;
    std::vector<uint16_t> rawData;
};

struct TvCode {
    const char* brand;
    Protocol protocol;
    uint32_t address;
    uint32_t command;
    uint16_t bits;
};

const TvCode kTvCodes[] = {
    {"LG", Protocol::Nec, 0x04, 0x08, 32},
    {"Samsung", Protocol::Samsung, 0x07, 0x02, 32},
    {"Sony", Protocol::Sony, 0x01, 0x15, 12},
    {"Philips", Protocol::Rc5, 0x00, 0x0C, 13},
    {"Toshiba", Protocol::Nec, 0x40, 0x12, 32},
    {"Hitachi", Protocol::Nec, 0x50, 0x17, 32},
    {"Sharp", Protocol::Nec, 0x02, 0x16, 32},
    {"JVC", Protocol::Jvc, 0x00, 0x17, 16},
};

IRsend g_sender(IR_TX_PIN);
bool g_senderReady = false;

struct Brand;

void runScanner(const Brand& brand);

std::string trim(const std::string& value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string upper(const std::string& value) {
    std::string out = value;
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string lower(const std::string& value) {
    std::string out = value;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool startsWith(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::string baseName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (name.size() > 3 && upper(name).substr(name.size() - 3) == ".IR") {
        name = name.substr(0, name.size() - 3);
    }
    return name;
}

// Database su microSD: /ir/<categoria>/<marca>/<file>.ir. LittleFS (e le
// vecchie SD) usano /ir/<marca>: quell'albero resta visibile dentro TV.
struct CategoryDef {
    const char* dir;
    const char* label;
};

constexpr CategoryDef kCategories[] = {
    {"TV", "TV"},          {"Clima", "Clima"},
    {"Audio", "Audio"},    {"DVD", "DVD/Blu-ray"},
    {"Proiettori", "Proiettore"}, {"Console", "Console"},
    {"Universal", "Universale"},  {"Altro", "Altro"},
};
constexpr size_t kCategoryCount = sizeof(kCategories) / sizeof(kCategories[0]);
constexpr int kCategoryTv = 0;

int categoryIndex(const std::string& name) {
    const std::string id = lower(name);
    for (size_t i = 0; i < kCategoryCount; ++i) {
        if (id == lower(kCategories[i].dir)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string categoryPath(int category) {
    return std::string("/ir/") + kCategories[category].dir;
}

struct Brand {
    int category = kCategoryTv;
    std::string name;
};

std::string brandFromPath(const std::string& path) {
    const size_t first = path.find('/', 1);
    if (first == std::string::npos) {
        return "";
    }
    const size_t second = path.find('/', first + 1);
    if (second == std::string::npos) {
        return "";
    }
    const std::string head = path.substr(first + 1, second - first - 1);
    if (categoryIndex(head) < 0) {
        return head;
    }
    const size_t third = path.find('/', second + 1);
    if (third == std::string::npos) {
        return "";
    }
    return path.substr(second + 1, third - second - 1);
}

// Cartelle di marca della categoria. Per TV si aggiunge il vecchio albero
// piatto: se la marca esiste gia' in /ir/TV i file dei due alberi vengono
// fusi da listRemotes() senza duplicati.
std::vector<Brand> categoryBrands(int category) {
    std::vector<Brand> out;
    for (const auto& name : storage::list(categoryPath(category), true)) {
        out.push_back({category, name});
    }
    if (category == kCategoryTv) {
        for (const auto& name : storage::list("/ir", true)) {
            if (categoryIndex(name) >= 0) {
                continue;
            }
            const bool known =
                std::any_of(out.begin(), out.end(), [&](const Brand& b) {
                    return lower(b.name) == lower(name);
                });
            if (!known) {
                out.push_back({kCategoryTv, name});
            }
        }
    }
    std::sort(out.begin(), out.end(),
              [](const Brand& a, const Brand& b) { return a.name < b.name; });
    return out;
}

std::vector<std::string> brandBases(const Brand& brand) {
    std::vector<std::string> bases;
    const std::string dir = categoryPath(brand.category) + "/" + brand.name;
    if (storage::exists(dir)) {
        bases.push_back(dir);
    }
    if (brand.category == kCategoryTv) {
        const std::string legacy = "/ir/" + brand.name;
        if (storage::exists(legacy)) {
            bases.push_back(legacy);
        }
    }
    return bases;
}

struct Remote {
    std::string path;
    std::string name;
};

bool isIrFile(const std::string& name) {
    const std::string id = upper(name);
    return id.size() > 3 && id.substr(id.size() - 3) == ".IR";
}

std::vector<Remote> listRemotes(const Brand& brand) {
    std::vector<Remote> out;
    std::vector<std::string> seen;
    for (const auto& base : brandBases(brand)) {
        for (const auto& file : storage::list(base, false)) {
            if (!isIrFile(file)) {
                continue;
            }
            const std::string key = lower(file);
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
                continue;
            }
            const std::string path = base + "/" + file;
            if (!storage::exists(path)) {
                continue;
            }
            seen.push_back(key);
            out.push_back({path, baseName(file)});
        }
    }
    return out;
}

bool brandHasFiles(const Brand& brand) {
    return !listRemotes(brand).empty();
}

// Una categoria senza file non deve comparire nel menu.
bool categoryHasFiles(int category) {
    for (const auto& brand : categoryBrands(category)) {
        if (brandHasFiles(brand)) {
            return true;
        }
    }
    return false;
}

std::string favoriteLabel(const std::string& path) {
    const std::string brand = brandFromPath(path);
    const std::string model = baseName(path);
    return brand.empty() ? model : brand + " / " + model;
}

constexpr const char* kFavoriteNamespace = "ir-fav";
constexpr const char* kFavoriteKey = "list";
constexpr size_t kMaxFavorites = 20;

// Il backend fa parte del preferito: se la microSD non e' inserita il path
// SD non deve essere scartato come "file inesistente".
enum class FavoriteBackend : uint8_t { Unknown, LittleFs, Sd };

struct Favorite {
    std::string path;
    FavoriteBackend backend = FavoriteBackend::Unknown;
};

std::vector<Favorite> loadFavorites() {
    std::vector<Favorite> out;
    Preferences preferences;
    if (!preferences.begin(kFavoriteNamespace, true)) {
        return out;
    }
    const String blob = preferences.getString(kFavoriteKey, "");
    preferences.end();

    std::istringstream stream(blob.c_str());
    std::string line;
    while (std::getline(stream, line) && out.size() < kMaxFavorites) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        Favorite favorite;
        if (startsWith(line, "sd|")) {
            favorite.backend = FavoriteBackend::Sd;
            favorite.path = line.substr(3);
        } else if (startsWith(line, "lfs|")) {
            favorite.backend = FavoriteBackend::LittleFs;
            favorite.path = line.substr(4);
        } else {
            favorite.path = line;  // formato precedente, backend sconosciuto
        }
        if (!favorite.path.empty()) {
            out.push_back(std::move(favorite));
        }
    }
    return out;
}

void saveFavorites(const std::vector<Favorite>& favorites) {
    std::string blob;
    for (const auto& favorite : favorites) {
        if (favorite.backend == FavoriteBackend::Sd) {
            blob += "sd|";
        } else if (favorite.backend == FavoriteBackend::LittleFs) {
            blob += "lfs|";
        }
        blob += favorite.path;
        blob += '\n';
    }
    Preferences preferences;
    if (preferences.begin(kFavoriteNamespace, false)) {
        preferences.putString(kFavoriteKey, blob.c_str());
        preferences.end();
    }
}

bool isFavorite(const std::string& path) {
    const auto favorites = loadFavorites();
    return std::any_of(favorites.begin(), favorites.end(),
                       [&](const Favorite& favorite) {
                           return favorite.path == path;
                       });
}

enum class FavoriteResult { Added, Removed, Full };

// true = il preferito va tenuto. Un backend non montato non e' una prova che
// il file non esista: si potra' verificare quando la SD torna disponibile.
bool favoriteAlive(const Favorite& favorite) {
    switch (favorite.backend) {
        case FavoriteBackend::Sd:
            return !storage::hasSD() || storage::exists(favorite.path, true);
        case FavoriteBackend::LittleFs:
            return !storage::hasLittleFS() ||
                   storage::exists(favorite.path, false);
        default:
            return !(storage::hasLittleFS() && storage::hasSD()) ||
                   storage::exists(favorite.path, false);
    }
}

FavoriteResult toggleFavorite(const std::string& path) {
    auto favorites = loadFavorites();
    const auto it = std::find_if(favorites.begin(), favorites.end(),
                                 [&](const Favorite& favorite) {
                                     return favorite.path == path;
                                 });
    if (it != favorites.end()) {
        favorites.erase(it);
        saveFavorites(favorites);
        return FavoriteResult::Removed;
    }
    favorites.erase(std::remove_if(favorites.begin(), favorites.end(),
                                   [](const Favorite& favorite) {
                                       return !favoriteAlive(favorite);
                                   }),
                    favorites.end());
    if (favorites.size() >= kMaxFavorites) {
        return FavoriteResult::Full;
    }
    Favorite favorite;
    favorite.path = path;
    favorite.backend = storage::exists(path, true)
                           ? FavoriteBackend::Sd
                           : (storage::exists(path, false)
                                  ? FavoriteBackend::LittleFs
                                  : FavoriteBackend::Unknown);
    favorites.push_back(std::move(favorite));
    saveFavorites(favorites);
    return FavoriteResult::Added;
}

std::string favoriteHint(const std::string& path) {
    // I tasti rapidi sono ispirati a Ultimate-Remote, che permette di inviare
    // i comandi principali senza scorrere l'elenco.
    return isFavorite(path) ? "P M +/- C X   F=togli   `=esci"
                            : "P M +/- C X   F=aggiungi   `=esci";
}

// Ultimo telecomando aperto: riprenderlo e' un tasto in meno rispetto a
// navigare marca -> modello ogni volta.
constexpr const char* kLastKey = "last";

std::string loadLastRemote() {
    Preferences preferences;
    if (!preferences.begin(kFavoriteNamespace, true)) {
        return std::string();
    }
    const String value = preferences.getString(kLastKey, "");
    preferences.end();
    return value.c_str();
}

void saveLastRemote(const std::string& path) {
    Preferences preferences;
    if (preferences.begin(kFavoriteNamespace, false)) {
        preferences.putString(kLastKey, path.c_str());
        preferences.end();
    }
}

// --- Tasti rapidi del telecomando ------------------------------------------
// Nome "compattato": minuscolo, senza spazi e underscore, cosi' che "Vol_up",
// "VOL +" e "Volume+" finiscano tutti sulla stessa chiave di ricerca.
std::string compactName(const std::string& value) {
    std::string out;
    for (char c : lower(value)) {
        if (c == ' ' || c == '_') {
            continue;
        }
        out.push_back(c);
    }
    return out;
}

const std::vector<const char*>* shortcutNames(char key) {
    static const std::vector<const char*> kPower = {"power", "accensione", "onoff",
                                                    "standby", "toggle"};
    static const std::vector<const char*> kMute = {"mute", "mudo", "mut", "silenzio"};
    static const std::vector<const char*> kVolUp = {"vol+", "volup", "volume+",
                                                    "volumeup", "volsu", "alza"};
    static const std::vector<const char*> kVolDn = {"vol-", "voldown", "voldn",
                                                    "volumedown", "volgiu", "abbassa"};
    static const std::vector<const char*> kChUp = {"ch+", "chup", "chnext",
                                                   "canale+", "canaleup", "prog+"};
    static const std::vector<const char*> kChDn = {"ch-", "chdown", "chprev",
                                                   "canale-", "canaledown", "prog-"};
    switch (key) {
        case 'p':
            return &kPower;
        case 'm':
            return &kMute;
        case '=':
        case '+':
            return &kVolUp;
        case '-':
            return &kVolDn;
        case 'c':
            return &kChUp;
        case 'x':
            return &kChDn;
        default:
            return nullptr;
    }
}

// Prima uguaglianza esatta, poi contenimento: cosi' "Vol+" vince su "Volume giu".
int findCommandByName(const std::vector<IrCommand>& commands,
                      const std::vector<const char*>& names) {
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < static_cast<int>(commands.size()); ++i) {
            if (!commands[i].supported) {
                continue;
            }
            const std::string key = compactName(commands[i].name);
            for (const char* name : names) {
                const std::string target(name);
                const bool hit = (pass == 0) ? (key == target)
                                             : (key.find(target) != std::string::npos);
                if (hit) {
                    return i;
                }
            }
        }
    }
    return -1;
}

// Parser senza stream: niente seconda copia del file in RAM.
uint32_t parseHexBytes(const char* value) {
    uint32_t result = 0;
    uint8_t shift = 0;
    bool first = true;
    const char* p = value;
    while (*p && shift < 32) {
        while (*p == ' ' || *p == '\t' || *p == ',') {
            ++p;
        }
        if (!*p) {
            break;
        }
        const char* begin = p;
        while (*p && *p != ' ' && *p != '\t' && *p != ',') {
            ++p;
        }
        char token[20];
        size_t len = static_cast<size_t>(p - begin);
        if (len >= sizeof(token)) {
            len = sizeof(token) - 1;
        }
        std::memcpy(token, begin, len);
        token[len] = '\0';
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(token, &end, 16);
        if (end == token) {
            continue;
        }
        const bool hexPrefix =
            len > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X');
        const size_t digits = len - (hexPrefix ? 2 : 0);
        if (first && digits > 2) {
            return static_cast<uint32_t>(parsed);
        }
        result |= static_cast<uint32_t>(parsed & 0xFF) << shift;
        shift += 8;
        first = false;
    }
    return result;
}

uint8_t reverseBits8(uint8_t value) {
    value = static_cast<uint8_t>(((value & 0xF0) >> 4) | ((value & 0x0F) << 4));
    value = static_cast<uint8_t>(((value & 0xCC) >> 2) | ((value & 0x33) << 2));
    value = static_cast<uint8_t>(((value & 0xAA) >> 1) | ((value & 0x55) << 1));
    return value;
}

uint64_t encodeKaseikyo(uint32_t address, uint32_t command) {
    const uint16_t vendor = static_cast<uint16_t>((address >> 8) & 0xFFFF);
    const uint8_t id = static_cast<uint8_t>((address >> 24) & 0x03);
    const uint8_t genre1 = static_cast<uint8_t>((address >> 4) & 0x0F);
    const uint8_t genre2 = static_cast<uint8_t>(address & 0x0F);
    const uint16_t code = static_cast<uint16_t>(command & 0x03FF);

    uint8_t bytes[6];
    bytes[0] = static_cast<uint8_t>(vendor & 0xFF);
    bytes[1] = static_cast<uint8_t>(vendor >> 8);
    uint8_t vendorParity = static_cast<uint8_t>(bytes[0] ^ bytes[1]);
    vendorParity =
        static_cast<uint8_t>((vendorParity & 0x0F) ^ (vendorParity >> 4));
    bytes[2] = static_cast<uint8_t>((genre1 << 4) | (vendorParity & 0x0F));
    bytes[3] = static_cast<uint8_t>(((code & 0x0F) << 4) | (genre2 & 0x0F));
    bytes[4] = static_cast<uint8_t>((id << 6) | ((code >> 4) & 0x3F));
    bytes[5] = static_cast<uint8_t>(bytes[2] ^ bytes[3] ^ bytes[4]);

    uint64_t data = 0;
    for (uint8_t byte : bytes) {
        data = (data << 8) | reverseBits8(byte);
    }
    return data;
}

// Limiti del parser: un file .ir ostile o corrotto non deve esaurire la SRAM.
// Il tetto richiesto e' 128 comandi, ma openFile() ne mostra solo 100:
// fermarsi prima evita di allocare comandi che verrebbero scartati.
constexpr size_t kMaxParsedCommands = 128;
constexpr size_t kMaxMenuCommands = 100;
constexpr size_t kParsedCommandsLimit =
    kMaxMenuCommands < kMaxParsedCommands ? kMaxMenuCommands : kMaxParsedCommands;
// Un comando raw oltre 512 valori non viene inviato: un frame troncato
// sembrerebbe valido ma il dispositivo non risponderebbe.
constexpr size_t kMaxRawValues = 512;
// Tetto complessivo per file (~32 KB di uint16): copre i telecomandi con
// molti tasti raw senza saturare l'heap. Oltre il budget i comandi sono [n/d].
constexpr size_t kMaxRawBudget = 16384;
// Riga massima letta: 512 valori da 5 cifre + spazi stanno in ~3 KB.
constexpr size_t kMaxIrLine = 3584;
// Un .ir oltre 64 KB viene rifiutato con un messaggio esplicito.
constexpr size_t kMaxIrFileBytes = 65536;
// Il parser puo' arrivare a ~36 KB (budget raw + riga) e poi serve il menu:
// sotto 64 KB liberi si rischia l'allocazione fallita.
constexpr uint32_t kMinIrHeap = 64 * 1024;

void trimInPlace(std::string& value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    if (begin > 0) {
        value.erase(0, begin);
    }
    if (end - begin < value.size()) {
        value.resize(end - begin);
    }
}

// true se il comando supera il limite: il chiamante lo marca [n/d].
bool parseRawValues(const char* value, std::vector<uint16_t>& out, size_t limit) {
    out.reserve(std::min(limit, out.size() + 128));
    const char* p = value;
    while (*p) {
        while (*p && !std::isdigit(static_cast<unsigned char>(*p)) && *p != '-') {
            ++p;
        }
        if (!*p) {
            break;
        }
        char* end = nullptr;
        const long parsed = std::strtol(p, &end, 10);
        if (end == p) {
            ++p;
            continue;
        }
        p = end;
        // Un valore fuori range va saturato, non scartato: toglierlo
        // sbilancerebbe la parita' mark/space e corromperebbe tutta la cornice.
        if (parsed <= 0) {
            continue;
        }
        if (out.size() >= limit) {
            return true;
        }
        out.push_back(static_cast<uint16_t>(std::min<long>(parsed, 65535)));
    }
    return false;
}

Protocol protocolFromName(const std::string& name, uint16_t& bits) {
    const std::string id = upper(name);
    // Solo varianti che sappiamo generare con la lunghezza corretta: inviare
    // NEC42 o Samsung36 come frame a 32 bit produrrebbe un falso successo.
    if (id == "NEC" || id == "NECEXT") {
        bits = 32;
        return Protocol::Nec;
    }
    if (id == "SAMSUNG" || id == "SAMSUNG32") {
        bits = 32;
        return Protocol::Samsung;
    }
    if (id == "LG" || id == "LG2") {
        bits = 28;
        return Protocol::Lg;
    }
    if (id == "SONY" || id == "SIRC") {
        bits = 12;
        return Protocol::Sony;
    }
    if (id == "SONY15" || id == "SIRC15") {
        bits = 15;
        return Protocol::Sony;
    }
    if (id == "SONY20" || id == "SIRC20") {
        bits = 20;
        return Protocol::Sony;
    }
    if (id == "RC5") {
        bits = 13;
        return Protocol::Rc5;
    }
    if (id == "RC5X") {
        // RC5X usa un bit di comando in piu' (14 bit totali).
        bits = 14;
        return Protocol::Rc5;
    }
    if (id == "RC6") {
        bits = 20;
        return Protocol::Rc6;
    }
    if (id == "JVC") {
        bits = 16;
        return Protocol::Jvc;
    }
    if (id == "KASEIKYO" || id == "PANASONIC") {
        bits = 48;
        return Protocol::Kaseikyo;
    }
    if (id == "RCA") {
        bits = 24;
        return Protocol::Rca;
    }
    return Protocol::Unknown;
}

void finalizeCommand(IrCommand& command) {
    if (command.name.empty()) {
        return;
    }
    if (command.raw) {
        command.supported = !command.rawData.empty() && !command.rawOverflow;
        if (command.frequency < 20000 || command.frequency > 60000) {
            command.frequency = 38000;
        }
    } else if (!command.protocolName.empty()) {
        command.protocol = protocolFromName(command.protocolName, command.bits);
        command.supported = command.protocol != Protocol::Unknown;
    }
}

// Legge il File a blocchi e restituisce una riga per volta: il file completo
// non viene mai copiato in RAM.
class IrLineReader {
public:
    explicit IrLineReader(File& file) : file_(file) {}

    bool next(std::string& out, bool& truncated) {
        out.clear();
        truncated = false;
        for (;;) {
            if (pos_ >= len_) {
                const size_t got = file_.read(
                    reinterpret_cast<uint8_t*>(chunk_), sizeof(chunk_));
                if (got == 0) {
                    return !out.empty();
                }
                len_ = got;
                pos_ = 0;
            }
            const char c = static_cast<char>(chunk_[pos_++]);
            if (c == '\n') {
                return true;
            }
            if (c == '\r') {
                continue;
            }
            if (out.size() < kMaxIrLine) {
                out.push_back(c);
            } else {
                truncated = true;
            }
        }
    }

private:
    File& file_;
    char chunk_[256];
    size_t len_ = 0;
    size_t pos_ = 0;
};

std::vector<IrCommand> parseIrFile(File& file) {
    std::vector<IrCommand> commands;
    commands.reserve(kParsedCommandsLimit);
    IrLineReader reader(file);
    std::string line;
    line.reserve(128);
    IrCommand current;
    bool active = false;
    size_t rawBudget = kMaxRawBudget;

    auto flush = [&] {
        if (active && !current.name.empty() &&
            commands.size() < kParsedCommandsLimit) {
            finalizeCommand(current);
            commands.push_back(std::move(current));
        }
        current = IrCommand();
        active = false;
    };

    auto addRaw = [&](const char* data, bool truncated) {
        current.raw = true;
        const size_t before = current.rawData.size();
        const size_t perCommand =
            before < kMaxRawValues ? kMaxRawValues - before : 0;
        const size_t room = std::min(perCommand, rawBudget);
        if (truncated) {
            current.rawOverflow = true;
        }
        if (parseRawValues(data, current.rawData, room)) {
            current.rawOverflow = true;
        }
        const size_t added = current.rawData.size() - before;
        rawBudget = added <= rawBudget ? rawBudget - added : 0;
    };

    while (commands.size() < kParsedCommandsLimit) {
        bool truncated = false;
        if (!reader.next(line, truncated)) {
            break;
        }
        trimInPlace(line);
        if (line.empty()) {
            continue;
        }
        if (line[0] == '#') {
            flush();
            continue;
        }
        if (startsWith(line, "name:")) {
            flush();
            current.name = trim(line.substr(5));
            active = true;
            continue;
        }
        if (!active) {
            continue;
        }
        if (startsWith(line, "type:")) {
            current.raw = upper(trim(line.substr(5))) == "RAW";
        } else if (startsWith(line, "protocol:")) {
            current.protocolName = trim(line.substr(9));
            current.protocol =
                protocolFromName(current.protocolName, current.bits);
        } else if (startsWith(line, "address:")) {
            current.address = parseHexBytes(line.c_str() + 8);
        } else if (startsWith(line, "command:")) {
            current.command = parseHexBytes(line.c_str() + 8);
        } else if (startsWith(line, "frequency:")) {
            current.frequency = static_cast<uint32_t>(
                std::strtoul(line.c_str() + 10, nullptr, 10));
        } else if (startsWith(line, "data:")) {
            addRaw(line.c_str() + 5, truncated);
        } else if (current.raw &&
                   std::isdigit(static_cast<unsigned char>(line[0]))) {
            addRaw(line.c_str(), truncated);
        }
    }
    flush();
    return commands;
}

bool loadCommands(const std::string& path, std::vector<IrCommand>& out,
                  std::string& error) {
    out.clear();
    File file = storage::openRead(path);
    if (!file) {
        error = "Impossibile leggere il file.";
        return false;
    }
    if (static_cast<size_t>(file.size()) > kMaxIrFileBytes) {
        file.close();
        error = "File troppo grande (>64 KB).";
        return false;
    }
    const uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < kMinIrHeap) {
        file.close();
        error = "Memoria insufficiente (" + std::to_string(freeHeap / 1024) +
                " KB liberi, servono 64 KB).";
        return false;
    }
    out = parseIrFile(file);
    file.close();
    if (out.empty()) {
        error = "Nessun comando riconosciuto.";
        return false;
    }
    return true;
}

void ensureSender() {
    if (!g_senderReady) {
        g_sender.begin();
        g_senderReady = true;
    }
}

uint16_t repeatCount() {
    return std::min<uint16_t>(config::irRepeat(), 4);
}

bool sendProtocol(Protocol protocol, uint32_t address, uint32_t command,
                  uint16_t bits) {
    ensureSender();
    const uint16_t repeat = repeatCount();
    switch (protocol) {
        case Protocol::Nec: {
            const uint32_t data =
                g_sender.encodeNEC(static_cast<uint16_t>(address & 0xFFFF),
                                   static_cast<uint16_t>(command & 0xFFFF));
            g_sender.sendNEC(data, 32, repeat);
            return true;
        }
        case Protocol::Samsung: {
            const uint32_t data = g_sender.encodeSAMSUNG(
                static_cast<uint8_t>(address & 0xFF),
                static_cast<uint8_t>(command & 0xFF));
            g_sender.sendSAMSUNG(data, 32, repeat);
            return true;
        }
        case Protocol::Lg: {
            const uint32_t data =
                g_sender.encodeLG(static_cast<uint16_t>(address & 0xFFFF),
                                  static_cast<uint16_t>(command & 0xFFFF));
            g_sender.sendLG(data, 28, repeat);
            return true;
        }
        case Protocol::Sony: {
            // SIRC20: 5 bit device + 8 bit extended, entrambi nell'address
            // Flipper (device nei bit bassi).
            uint16_t device = static_cast<uint16_t>(address & 0xFFFF);
            uint16_t extended = 0;
            if (bits == 20) {
                extended = static_cast<uint16_t>((device >> 5) & 0xFF);
                device &= 0x1F;
            }
            const uint32_t data = g_sender.encodeSony(
                bits, static_cast<uint16_t>(command & 0x7F), device, extended);
            g_sender.sendSony(data, bits, std::max<uint16_t>(repeat, 2));
            return true;
        }
        case Protocol::Rc5: {
            if (bits >= 14) {
                const uint16_t data = g_sender.encodeRC5X(
                    static_cast<uint8_t>(address & 0x7F),
                    static_cast<uint8_t>(command & 0x7F));
                g_sender.sendRC5(data, 14, repeat);
            } else {
                const uint16_t data = g_sender.encodeRC5(
                    static_cast<uint8_t>(address & 0xFF),
                    static_cast<uint8_t>(command & 0xFF));
                g_sender.sendRC5(data, 13, repeat);
            }
            return true;
        }
        case Protocol::Rc6: {
            const uint64_t data = g_sender.encodeRC6(address & 0xFFFF,
                                                     command & 0xFF);
            g_sender.sendRC6(data, 20, repeat);
            return true;
        }
        case Protocol::Jvc: {
            const uint16_t data = g_sender.encodeJVC(
                static_cast<uint8_t>(address & 0xFF),
                static_cast<uint8_t>(command & 0xFF));
            g_sender.sendJVC(data, 16, repeat);
            return true;
        }
        case Protocol::Kaseikyo: {
            const uint64_t data = encodeKaseikyo(address, command);
            g_sender.sendPanasonic64(data, 48, repeat);
            return true;
        }
        case Protocol::Rca: {
            const uint32_t data =
                (address & 0x0F) | ((command & 0xFF) << 4) |
                (((~address) & 0x0F) << 12) |
                (((~command) & 0xFF) << 16);
            std::vector<uint16_t> raw;
            raw.reserve(50);
            raw.push_back(4000);
            raw.push_back(4000);
            for (uint8_t bit = 0; bit < 24; ++bit) {
                raw.push_back(500);
                raw.push_back(((data >> bit) & 1) ? 2000 : 1000);
            }
            for (uint16_t r = 0; r <= repeat; ++r) {
                g_sender.sendRaw(raw.data(), static_cast<uint16_t>(raw.size()),
                                 38000);
                if (r < repeat) {
                    delay(8);
                }
            }
            return true;
        }
        case Protocol::Unknown:
        default:
            return false;
    }
}

bool sendCommand(const IrCommand& command) {
    if (command.raw) {
        if (command.rawData.empty()) {
            return false;
        }
        ensureSender();
        g_sender.sendRaw(command.rawData.data(),
                         static_cast<uint16_t>(command.rawData.size()),
                         static_cast<uint16_t>(command.frequency));
        return true;
    }
    return sendProtocol(command.protocol, command.address, command.command,
                        command.bits);
}

void openFile(const std::string& path, const std::string& title) {
    std::vector<IrCommand> commands;
    std::string error;
    if (!loadCommands(path, commands, error)) {
        if (error.rfind("Memoria", 0) == 0) {
            ui::message(title, {error, "Chiudi altre app e riprova."},
                        "Telecomandi");
        } else if (startsWith(error, "File troppo grande")) {
            ui::message(title,
                        {error, "Riduci i comandi o usa un file piu' piccolo."},
                        "Telecomandi");
        } else if (startsWith(error, "Nessun comando")) {
            ui::message(title,
                        {error, "Il file .ir potrebbe essere non valido."},
                        "Telecomandi");
        } else {
            ui::message(title, {"Impossibile leggere il file:", path},
                        "Telecomandi");
        }
        return;
    }

    saveLastRemote(path);

    MenuManager menu(title, "Telecomandi");
    const int count = std::min<int>(static_cast<int>(commands.size()), 100);
    for (int i = 0; i < count; ++i) {
        std::string label = commands[i].name;
        if (!commands[i].supported) {
            label += (commands[i].raw && commands[i].rawOverflow)
                         ? " [raw >512]"
                         : " [n/d]";
        }
        // Riferimento, non copia: un comando raw puo' valere 512 uint16.
        menu.addItem(label, "ir",
                     [&commands, i] {
                         const IrCommand& command = commands[i];
                         if (!command.supported) {
                             ui::message(command.name,
                                         {"Protocollo non supportato.",
                                          "Converti il codice in NEC/Samsung/LG/Sony/RC5/RC6/JVC/Kaseikyo/RCA/raw."},
                                         "Infrarossi");
                             return;
                         }
                         if (!sendCommand(command)) {
                             ui::message("Errore IR", {"Invio non riuscito."},
                                         "Infrarossi");
                         }
                     },
                     commands[i].supported);
    }
    menu.setHint(favoriteHint(path));
    menu.setTextHandler([path, &menu, &commands](char ch) {
        if (ch == 'f' || ch == 'F') {
            switch (toggleFavorite(path)) {
                case FavoriteResult::Added:
                    menu.setHint("Aggiunto ai preferiti   ESC=esci");
                    break;
                case FavoriteResult::Removed:
                    menu.setHint("Rimosso dai preferiti   ESC=esci");
                    break;
                case FavoriteResult::Full:
                    menu.setHint("Preferiti pieni (max 20)");
                    break;
            }
            return true;
        }

        // Tasti rapidi: spostano la selezione sul comando corrispondente e lo
        // inviano subito (ENTER ripete senza cercarlo nell'elenco).
        const char key = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const std::vector<const char*>* names = shortcutNames(key);
        if (names == nullptr) {
            return false;
        }
        const int index = findCommandByName(commands, *names);
        if (index < 0) {
            menu.setHint("Nessun comando per questo tasto");
            return true;
        }
        menu.selectIndex(index);
        std::string sent = commands[index].name;
        if (sent.size() > 14) {
            sent = sent.substr(0, 13) + "~";
        }
        menu.setHint(sendCommand(commands[index])
                         ? ("Inviato " + sent + "   ENTER=ripeti")
                         : "Invio non riuscito");
        return true;
    });
    menu.run(true);
}

void openBrand(const Brand& brand) {
    const std::vector<Remote> remotes = listRemotes(brand);
    if (remotes.empty()) {
        ui::message(brand.name, {"Nessun file .ir per questa marca."},
                    "Telecomandi");
        return;
    }
    MenuManager menu(brand.name, "Telecomandi");
    menu.addItem("Scansiona brand", "scan", [brand] { runScanner(brand); });
    menu.addSeparator();
    for (const auto& remote : remotes) {
        std::string title = remote.name;
        if (isFavorite(remote.path)) {
            title = "* " + title;
        }
        menu.addItem(title, "ir", [path = remote.path, title] {
            openFile(path, title);
        });
    }
    menu.setHint("ENTER=apri  ESC=indietro");
    menu.run(true);
}

bool isPowerName(const std::string& name) {
    const std::string id = lower(name);
    return id.find("power") != std::string::npos ||
           id.find("accensione") != std::string::npos ||
           id.find("on/off") != std::string::npos ||
           id.find("standby") != std::string::npos ||
           id.find("toggle") != std::string::npos || id == "on" || id == "off";
}

int findPowerCommand(const std::vector<IrCommand>& commands) {
    for (int i = 0; i < static_cast<int>(commands.size()); ++i) {
        if (commands[i].supported && isPowerName(commands[i].name)) {
            return i;
        }
    }
    return -1;
}

void runScanner(const Brand& brand) {
    const std::vector<Remote> remotes = listRemotes(brand);
    if (remotes.empty()) {
        ui::message(brand.name, {"Nessun telecomando da scansionare."},
                    "Scanner IR");
        return;
    }
    if (!ui::confirm("Scanner IR",
                     {"Invia il tasto Power dei telecomandi",
                      "uno alla volta, finche' il dispositivo",
                      "non reagisce. Usa solo su dispositivi tuoi.",
                      "F = salva il telecomando nei preferiti."},
                     brand.name)) {
        return;
    }

    ensureSender();
    // Lo scanner manda un comando ogni pochi secondi: senza Hold il timeout di
    // inattivita' spegnerebbe il dispositivo a meta' scansione.
    powertimer::Hold hold;
    navigation::waitRelease();

    int index = 0;
    bool loadPending = true;
    bool sent = false;
    bool favorite = false;
    int powerIndex = -1;
    std::string model;
    std::string feedback;
    std::vector<IrCommand> commands;
    uint32_t nextAt = millis() + 400;

    for (;;) {
        const std::string path = remotes[index].path;
        if (loadPending) {
            model = remotes[index].name;
            favorite = isFavorite(path);
            feedback.clear();
            commands.clear();
            powerIndex = -1;
            std::string error;
            if (loadCommands(path, commands, error)) {
                powerIndex = findPowerCommand(commands);
            }
            loadPending = false;
            sent = false;
            nextAt = millis() + 500;
        }

        if (powerIndex >= 0 && !sent) {
            if (sendCommand(commands[powerIndex])) {
                sent = true;
                nextAt = millis() + 1400;
            } else {
                feedback = "Invio non riuscito";
                sent = true;
                nextAt = millis() + 900;
            }
        }

        display::clear(display::kPaper);
        display::header("Scanner IR", brand.name);
        display::text(6, 26,
                      "File " + std::to_string(index + 1) + "/" +
                          std::to_string(remotes.size()),
                      display::kMuted, 0);
        std::string shownModel = model;
        while (shownModel.size() > 4 &&
               display::textWidth(shownModel + "..", 2) > display::width() - 12) {
            shownModel.pop_back();
        }
        display::text(6, 38, shownModel, display::kInk, 2);
        if (powerIndex >= 0) {
            display::text(6, 58,
                          "Power > " + commands[powerIndex].name,
                          display::kNeoGreen, 0);
        } else {
            display::text(6, 58, "Nessun comando Power", display::kNeoRed, 0);
        }
        if (!feedback.empty()) {
            display::text(6, 74, feedback, display::kNeoPink, 0);
        } else if (favorite) {
            display::text(6, 74, "* PREFERITO", display::kNeoPink, 0);
        } else {
            display::text(6, 74, "F = salva nei preferiti", display::kMuted, 0);
        }
        display::progressBar(6, 96, display::width() - 12, 8,
                             static_cast<float>(index + 1) / remotes.size(),
                             display::kNeoCyan);
        display::footer("INVIO=ripeti  F=salva  </>=cambia");
        display::push();

        navigation::update();
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            return;
        }
        if (ev.type == navigation::Event::Select) {
            sent = false;
            nextAt = millis() + 1400;
        } else if (ev.type == navigation::Event::Left) {
            index = (index - 1 + static_cast<int>(remotes.size())) %
                    static_cast<int>(remotes.size());
            loadPending = true;
        } else if (ev.type == navigation::Event::Right) {
            index = (index + 1) % static_cast<int>(remotes.size());
            loadPending = true;
        } else if (ev.type == navigation::Event::Text &&
                   (ev.ch == 'f' || ev.ch == 'F')) {
            switch (toggleFavorite(path)) {
                case FavoriteResult::Added:
                    favorite = true;
                    feedback = "Salvato nei preferiti";
                    break;
                case FavoriteResult::Removed:
                    favorite = false;
                    feedback = "Rimosso dai preferiti";
                    break;
                case FavoriteResult::Full:
                    feedback = "Preferiti pieni (max 20)";
                    break;
            }
            nextAt = std::max<uint32_t>(nextAt, millis() + 1200);
        }

        if (millis() >= nextAt) {
            index = (index + 1) % static_cast<int>(remotes.size());
            loadPending = true;
        }
        delay(12);
    }
}

void runScannerCategory(int category) {
    MenuManager menu(kCategories[category].label, "Scanner IR");
    int shown = 0;
    for (const auto& brand : categoryBrands(category)) {
        const size_t count = listRemotes(brand).size();
        if (count == 0) {
            continue;
        }
        menu.addItem(brand.name + " (" + std::to_string(count) + ")", "scan",
                     [brand] { runScanner(brand); });
        ++shown;
    }
    if (shown == 0) {
        ui::message(kCategories[category].label,
                    {"Nessun telecomando in questa categoria."}, "Scanner IR");
        return;
    }
    menu.setGrid(3);
    menu.setHint("ENTER=scansiona  ;.=riga/pagina  ESC=indietro");
    menu.run(true);
}

void runScannerBrands() {
    MenuManager menu("Scanner IR", "Infrarossi");
    int shown = 0;
    for (size_t i = 0; i < kCategoryCount; ++i) {
        if (!categoryHasFiles(static_cast<int>(i))) {
            continue;
        }
        menu.addItem(kCategories[i].label, "scan",
                     [i] { runScannerCategory(static_cast<int>(i)); });
        ++shown;
    }
    if (shown == 0) {
        ui::message("Scanner IR", {"Nessun database in /ir."}, "Infrarossi");
        return;
    }
    menu.setGrid(2);
    menu.setHint("ENTER=categoria  ESC=indietro");
    menu.run(true);
}

void runFavorites() {
    const auto stored = loadFavorites();
    std::vector<Favorite> favorites;
    for (const auto& favorite : stored) {
        const bool mounted =
            favorite.backend == FavoriteBackend::Sd ? storage::hasSD()
            : favorite.backend == FavoriteBackend::LittleFs
                ? storage::hasLittleFS()
                : true;
        if (mounted && storage::exists(favorite.path)) {
            favorites.push_back(favorite);
        }
    }
    if (favorites.empty()) {
        if (stored.empty()) {
            ui::message("Preferiti", {"Nessun preferito salvato.",
                                      "Apri un telecomando e premi F",
                                      "per aggiungerlo qui."},
                        "Infrarossi");
        } else {
            ui::message("Preferiti",
                        {"Preferiti non disponibili.",
                         "Reinserisci la microSD: i path",
                         "SD sono stati conservati."},
                        "Infrarossi");
        }
        return;
    }
    MenuManager menu("Preferiti", "Infrarossi");
    for (const auto& favorite : favorites) {
        const std::string label = favoriteLabel(favorite.path);
        menu.addItem(label, "fav", [path = favorite.path, label] {
            openFile(path, label);
        });
    }
    menu.setHint("ENTER=apri  ESC=indietro");
    menu.run(true);
}

void runCategory(int category) {
    MenuManager menu(kCategories[category].label, "Telecomandi");
    int shown = 0;
    for (const auto& brand : categoryBrands(category)) {
        const size_t count = listRemotes(brand).size();
        if (count == 0) {
            continue;
        }
        menu.addItem(brand.name + " (" + std::to_string(count) + ")", "ir",
                     [brand] { openBrand(brand); });
        ++shown;
    }
    if (shown == 0) {
        ui::message(kCategories[category].label,
                    {"Nessun telecomando in questa categoria."}, "Telecomandi");
        return;
    }
    menu.setGrid(3);
    menu.setHint("ENTER=apri  ;.=riga/pagina  ESC=indietro");
    menu.run(true);
}

void runRemotes() {
    MenuManager menu("Telecomandi", "Infrarossi");
    int shown = 0;
    for (size_t i = 0; i < kCategoryCount; ++i) {
        if (!categoryHasFiles(static_cast<int>(i))) {
            continue;
        }
        menu.addItem(kCategories[i].label, "ir",
                     [i] { runCategory(static_cast<int>(i)); });
        ++shown;
    }
    if (shown == 0) {
        ui::message("Telecomandi", {"Nessun database in /ir.",
                                    "Carica i file .ir su LittleFS o microSD.",
                                    "Formato: Flipper Zero .ir"},
                    "Infrarossi");
        return;
    }
    menu.setGrid(2);
    menu.setHint("ENTER=categoria  ESC=indietro");
    menu.run(true);
}

// Ricerca su tutto il database .ir, ispirata alla ricerca per nome di
// Ultimate-Remote: evita di navigare marca -> modello per trovare un codice.
void runSearch() {
    std::vector<textinput::Field> fields = {
        {"Cerca marca o modello", "", 24, false, false},
    };
    if (!textinput::edit("Cerca telecomando", "Infrarossi", fields)) {
        return;
    }
    const std::string needle = lower(fields[0].value);
    if (needle.empty()) {
        return;
    }

    // Quota per categoria e per marca, poi merge round-robin: "samsung" non
    // deve esaurire il cap nella sola categoria TV.
    constexpr size_t kMaxResults = 40;
    constexpr size_t kPerCategory = 10;
    constexpr size_t kPerBrand = 3;
    const std::vector<Favorite> favorites = loadFavorites();
    std::vector<MenuItem> buckets[kCategoryCount];
    std::vector<std::string> seenPaths;
    bool hasDatabase = false;

    for (size_t c = 0; c < kCategoryCount; ++c) {
        const std::vector<Brand> brands = categoryBrands(static_cast<int>(c));
        if (!brands.empty()) {
            hasDatabase = true;
        }
        for (const auto& brand : brands) {
            if (buckets[c].size() >= kPerCategory) {
                break;
            }
            const bool brandHit =
                lower(brand.name).find(needle) != std::string::npos;
            size_t brandCount = 0;
            for (const auto& remote : listRemotes(brand)) {
                if (brandCount >= kPerBrand ||
                    buckets[c].size() >= kPerCategory) {
                    break;
                }
                if (!brandHit &&
                    lower(remote.name).find(needle) == std::string::npos) {
                    continue;
                }
                const std::string key = lower(remote.path);
                if (std::find(seenPaths.begin(), seenPaths.end(), key) !=
                    seenPaths.end()) {
                    continue;
                }
                std::string title = std::string(kCategories[c].label) +
                                    " / " + brand.name + " / " + remote.name;
                const bool favorite =
                    std::any_of(favorites.begin(), favorites.end(),
                                [&](const Favorite& item) {
                                    return item.path == remote.path;
                                });
                if (favorite) {
                    title = "* " + title;
                }
                buckets[c].emplace_back(
                    title, "ir",
                    [path = remote.path, name = remote.name] {
                        openFile(path, name);
                    });
                seenPaths.push_back(key);
                ++brandCount;
            }
        }
    }

    std::vector<MenuItem> matches;
    for (size_t round = 0;
         round < kPerCategory && matches.size() < kMaxResults; ++round) {
        for (size_t c = 0; c < kCategoryCount && matches.size() < kMaxResults;
             ++c) {
            if (round < buckets[c].size()) {
                matches.push_back(std::move(buckets[c][round]));
            }
        }
    }

    if (!hasDatabase) {
        ui::message("Cerca", {"!Nessun database in /ir."}, "Infrarossi");
        return;
    }

    if (matches.empty()) {
        ui::message("Cerca",
                    {"!Nessun telecomando trovato.", "Prova con il nome della marca", "o con parte del modello."},
                    "Infrarossi");
        return;
    }

    MenuManager menu("Risultati", "Cerca");
    for (auto& item : matches) {
        menu.addItem(std::move(item));
    }
    menu.setHint("ENTER=apri  ESC=indietro");
    menu.run(true);
}

void drawTvBg(const char* brand, int index, int total, int cycle) {
    display::clear();
    display::header("TV-B-Gone", "Infrarossi");
    display::text(10, 34, "Invio codice:", display::kTextDim, 0);
    display::text(10, 48, brand, display::kAccent, 2);
    display::text(10, 72, "Ciclo " + std::to_string(cycle), display::kTextDim, 0);
    display::progressBar(10, 88, 140, 8,
                         static_cast<float>(index + 1) / total);
    display::g().fillCircle(200, 60, 14, display::kDanger);
    display::g().fillCircle(200, 60, 8, display::kWarning);
    display::footer("ESC = interrompi");
    display::push();
}

void runTvBg() {
    // Sessione di monitoraggio: il timer di spegnimento non deve interromperla.
    powertimer::Hold hold;
    if (!ui::confirm("TV-B-Gone",
                     {"Spegne televisori nelle vicinanze.",
                      "Usalo solo su dispositivi tuoi o con permesso."},
                     "Infrarossi")) {
        return;
    }

    ensureSender();
    const int total = static_cast<int>(sizeof(kTvCodes) / sizeof(kTvCodes[0]));
    int cycle = 0;
    bool running = true;
    navigation::waitRelease();

    while (running) {
        for (int i = 0; i < total && running; ++i) {
            navigation::update();
            if (navigation::poll().type == navigation::Event::Back) {
                running = false;
                break;
            }
            drawTvBg(kTvCodes[i].brand, i, total, cycle + 1);
            sendProtocol(kTvCodes[i].protocol, kTvCodes[i].address,
                         kTvCodes[i].command, kTvCodes[i].bits);
            const uint32_t waitMs = std::max<uint16_t>(config::irTvbgDelayMs(), 40);
            const uint32_t until = millis() + waitMs;
            while (millis() < until) {
                navigation::update();
                if (navigation::poll().type == navigation::Event::Back) {
                    running = false;
                    break;
                }
                delay(5);
            }
        }
        ++cycle;
    }
}

void runJammer() {
#if ENABLE_OFFENSIVE
    powertimer::Hold hold;
    if (!ui::legalWarning("Jammer IR",
                          {"Disturba i ricevitori infrarossi vicini.",
                           "Attivita' consentita solo in ambiente isolato",
                           "e con autorizzazione."},
                          "Infrarossi")) {
        return;
    }
    ensureSender();
    uint32_t packets = 0;
    uint32_t lastDraw = 0;
    navigation::waitRelease();
    while (true) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }
        for (int i = 0; i < 6; ++i) {
            g_sender.sendNEC(static_cast<uint64_t>(esp_random()) +
                                 (static_cast<uint64_t>(esp_random()) << 32),
                             32, 0);
            ++packets;
            delay(6);
        }
        const uint32_t now = millis();
        if (now - lastDraw > 100) {
            display::clear();
            display::header("Jammer IR", "Infrarossi");
            display::textCentered(48, "TRASMISSIONE ATTIVA", display::kDanger, 2);
            display::textCentered(70, "Pacchetti: " + std::to_string(packets),
                                  display::kText, 2);
            display::progressBar(40, 92, 160, 8,
                                 static_cast<float>(now % 2000) / 2000.0f,
                                 display::kWarning);
            display::footer("ESC = interrompi");
            display::push();
            lastDraw = now;
        }
    }
#else
    ui::message("Jammer IR",
                {"Funzione non compilata.", "Compila con ENABLE_OFFENSIVE=1."},
                "Infrarossi");
#endif
}

}  // namespace

void runDatabaseInfo() {
    display::clear(display::kPaper);
    display::header("Database IR", "Infrarossi");
    display::text(6, 50, "Conteggio file...", display::kInk, 2);
    display::footer("attendi");
    display::push();

    size_t counts[kCategoryCount] = {};
    size_t total = 0;
    for (size_t c = 0; c < kCategoryCount; ++c) {
        for (const auto& brand : categoryBrands(static_cast<int>(c))) {
            counts[c] += listRemotes(brand).size();
        }
        total += counts[c];
    }
    const auto row = [&](const char* left, size_t leftCount,
                         const char* right, size_t rightCount) {
        return std::string(left) + " " + std::to_string(leftCount) + "   " +
               right + " " + std::to_string(rightCount);
    };
    std::vector<std::string> lines;
    lines.push_back("Totale: " + std::to_string(total) + " file");
    lines.push_back(row("TV", counts[0], "Clima", counts[1]));
    lines.push_back(row("Audio", counts[2], "DVD", counts[3]));
    lines.push_back(row("Proiett.", counts[4], "Console", counts[5]));
    lines.push_back(row("Universale", counts[6], "Altro", counts[7]));
    if (!storage::hasSD()) {
        lines.push_back("microSD assente (solo LittleFS)");
    }
    ui::message("Database IR", lines, "Infrarossi");
}

void runMenu() {
    MenuManager menu("Infrarossi", "Menu principale");
    menu.setCarousel();
    // Ripresa rapida: ultimo telecomando aperto, se il file esiste ancora.
    const std::string last = loadLastRemote();
    if (!last.empty() && storage::exists(last)) {
        const std::string label = favoriteLabel(last);
        menu.addItem("Riprendi " + label, "ir",
                     [last, label] { openFile(last, label); });
    }
    menu.addItem("Preferiti", "fav", [] { runFavorites(); });
    menu.addItem("Telecomandi", "ir", [] { runRemotes(); });
    menu.addItem("Cerca", "scan", [] { runSearch(); });
    menu.addItem("Database IR", "ir", [] { runDatabaseInfo(); });
    menu.addItem("Scanner", "scan", [] { runScannerBrands(); });
    menu.addItem("TV-B-Gone", "tv", [] { runTvBg(); });
#if ENABLE_OFFENSIVE
    menu.addItem("Jammer IR", "jammer", [] { runJammer(); });
#endif
#if ENABLE_IR_LEARN
    menu.addItem("Impara codice", "learn", [] {
        ui::message("Impara codice",
                    {"Richiede un ricevitore IR esterno.",
                     "Collega il RX e implementa la lettura."},
                    "Infrarossi");
    });
#else
    menu.addItem("Impara codice (RX esterno)", "learn",
                 [] {
                     ui::message("Impara codice",
                                 {"Non compilato (ENABLE_IR_LEARN=0).",
                                  "La Cardputer ADV ha solo il trasmettitore IR."},
                                 "Infrarossi");
                 },
                 false);
#endif
    menu.setHint("ESC=indietro  </>=scorri  ENTER=apri");
    menu.run(true);
}

}  // namespace ir

#endif  // ENABLE_MODULE_IR
