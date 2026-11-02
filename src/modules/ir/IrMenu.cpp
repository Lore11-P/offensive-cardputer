#include "IrMenu.h"

#include <Arduino.h>
#include <IRsend.h>
#include <Preferences.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "Config.h"
#include "Display.h"
#include "Logger.h"
#include "MenuManager.h"
#include "ModuleFlags.h"
#include "Navigation.h"
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

void runScanner(const std::string& brand);

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

std::string brandFromPath(const std::string& path) {
    const size_t first = path.find('/', 1);
    if (first == std::string::npos) {
        return "";
    }
    const size_t second = path.find('/', first + 1);
    if (second == std::string::npos) {
        return "";
    }
    return path.substr(first + 1, second - first - 1);
}

std::string favoriteLabel(const std::string& path) {
    const std::string brand = brandFromPath(path);
    const std::string model = baseName(path);
    return brand.empty() ? model : brand + " / " + model;
}

constexpr const char* kFavoriteNamespace = "ir-fav";
constexpr const char* kFavoriteKey = "list";
constexpr size_t kMaxFavorites = 20;

std::vector<std::string> loadFavorites() {
    std::vector<std::string> out;
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
        if (!line.empty()) {
            out.push_back(line);
        }
    }
    return out;
}

void saveFavorites(const std::vector<std::string>& favorites) {
    std::string blob;
    for (const auto& path : favorites) {
        blob += path;
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
    return std::find(favorites.begin(), favorites.end(), path) != favorites.end();
}

enum class FavoriteResult { Added, Removed, Full };

FavoriteResult toggleFavorite(const std::string& path) {
    auto favorites = loadFavorites();
    const auto it = std::find(favorites.begin(), favorites.end(), path);
    if (it != favorites.end()) {
        favorites.erase(it);
        saveFavorites(favorites);
        return FavoriteResult::Removed;
    }
    if (favorites.size() >= kMaxFavorites) {
        return FavoriteResult::Full;
    }
    favorites.push_back(path);
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

uint32_t parseHexBytes(const std::string& value) {
    std::istringstream stream(value);
    std::string token;
    uint32_t result = 0;
    uint8_t shift = 0;
    bool first = true;
    while (stream >> token && shift < 32) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(token.c_str(), &end, 16);
        if (end == token.c_str()) {
            continue;
        }
        const std::string digits =
            startsWith(token, "0x") || startsWith(token, "0X")
                ? token.substr(2)
                : token;
        if (first && digits.size() > 2) {
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

void parseRawValues(const std::string& value, std::vector<uint16_t>& out) {
    std::string cleaned = value;
    std::replace(cleaned.begin(), cleaned.end(), ',', ' ');
    std::istringstream stream(cleaned);
    std::string token;
    while (stream >> token && out.size() < 512) {
        char* end = nullptr;
        const long parsed = std::strtol(token.c_str(), &end, 10);
        if (end == token.c_str()) {
            continue;
        }
        if (parsed > 0 && parsed < 65536) {
            out.push_back(static_cast<uint16_t>(parsed));
        }
    }
}

Protocol protocolFromName(const std::string& name, uint16_t& bits) {
    const std::string id = upper(name);
    if (id == "NEC" || id == "NECEXT") {
        bits = 32;
        return Protocol::Nec;
    }
    if (id.rfind("NEC", 0) == 0) {
        bits = 32;
        return Protocol::Nec;
    }
    if (id.rfind("SAMSUNG", 0) == 0 || id == "SAMSUNG32") {
        bits = 32;
        return Protocol::Samsung;
    }
    if (id == "LG" || id == "LG2") {
        bits = 28;
        return Protocol::Lg;
    }
    if (id.rfind("SONY", 0) == 0 || id.rfind("SIRC", 0) == 0) {
        if (id.find("20") != std::string::npos) {
            bits = 20;
        } else if (id.find("15") != std::string::npos) {
            bits = 15;
        } else {
            bits = 12;
        }
        return Protocol::Sony;
    }
    if (id == "RC5" || id == "RC5X") {
        bits = 13;
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
        command.supported = !command.rawData.empty();
        if (command.frequency < 20000 || command.frequency > 60000) {
            command.frequency = 38000;
        }
    } else if (!command.protocolName.empty()) {
        command.protocol = protocolFromName(command.protocolName, command.bits);
        command.supported = command.protocol != Protocol::Unknown;
    }
}

std::vector<IrCommand> parseIrFile(const std::string& text) {
    std::vector<IrCommand> commands;
    std::istringstream stream(text);
    std::string line;
    IrCommand current;
    bool active = false;

    auto flush = [&] {
        if (active && !current.name.empty()) {
            finalizeCommand(current);
            commands.push_back(current);
        }
        current = IrCommand();
        active = false;
    };

    while (std::getline(stream, line)) {
        line = trim(line);
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
            current.address = parseHexBytes(trim(line.substr(8)));
        } else if (startsWith(line, "command:")) {
            current.command = parseHexBytes(trim(line.substr(8)));
        } else if (startsWith(line, "frequency:")) {
            current.frequency =
                static_cast<uint32_t>(std::strtoul(line.substr(10).c_str(),
                                                   nullptr, 10));
        } else if (startsWith(line, "data:")) {
            current.raw = true;
            parseRawValues(line.substr(5), current.rawData);
        } else if (current.raw && std::isdigit(static_cast<unsigned char>(line[0]))) {
            parseRawValues(line, current.rawData);
        }
    }
    flush();
    return commands;
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
            const uint32_t data = g_sender.encodeSony(
                bits, static_cast<uint16_t>(command & 0xFFFF),
                static_cast<uint16_t>(address & 0xFFFF));
            g_sender.sendSony(data, bits, std::max<uint16_t>(repeat, 2));
            return true;
        }
        case Protocol::Rc5: {
            const uint16_t data = g_sender.encodeRC5(
                static_cast<uint8_t>(address & 0xFF),
                static_cast<uint8_t>(command & 0xFF));
            g_sender.sendRC5(data, 13, repeat);
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
    std::string text;
    if (!storage::readText(path, text, 65536)) {
        ui::message(title, {"Impossibile leggere il file:", path}, "Telecomandi");
        return;
    }

    std::vector<IrCommand> commands = parseIrFile(text);
    if (commands.empty()) {
        ui::message(title, {"Nessun comando riconosciuto.", "Il file .ir potrebbe essere raw/non valido."},
                    "Telecomandi");
        return;
    }

    saveLastRemote(path);

    MenuManager menu(title, "Telecomandi");
    int index = 0;
    for (const auto& command : commands) {
        if (index++ >= 100) {
            break;
        }
        std::string label = command.name;
        if (!command.supported) {
            label += " [n/d]";
        }
        IrCommand copy = command;
        menu.addItem(label, "ir",
                     [copy] {
                         if (!copy.supported) {
                             ui::message(copy.name,
                                         {"Protocollo non supportato.",
                                          "Converti il codice in NEC/Samsung/LG/Sony/RC5/RC6/JVC/Kaseikyo/RCA/raw."},
                                         "Infrarossi");
                             return;
                         }
                         if (!sendCommand(copy)) {
                             ui::message("Errore IR", {"Invio non riuscito."},
                                         "Infrarossi");
                         }
                     },
                     command.supported);
    }
    menu.setHint(favoriteHint(path));
    menu.setTextHandler([path, &menu, &commands](char ch) {
        if (ch == 'f' || ch == 'F') {
            switch (toggleFavorite(path)) {
                case FavoriteResult::Added:
                    menu.setHint("Aggiunto ai preferiti   `=esci");
                    break;
                case FavoriteResult::Removed:
                    menu.setHint("Rimosso dai preferiti   `=esci");
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

void openBrand(const std::string& brand) {
    const std::vector<std::string> files = storage::list("/ir/" + brand, false);
    if (files.empty()) {
        ui::message(brand, {"Nessun file .ir in /ir/" + brand}, "Telecomandi");
        return;
    }
    MenuManager menu(brand, "Telecomandi");
    menu.addItem("Scansiona brand", "scan", [brand] { runScanner(brand); });
    menu.addSeparator();
    for (const auto& file : files) {
        if (upper(file).size() < 3 ||
            upper(file).substr(upper(file).size() - 3) != ".IR") {
            continue;
        }
        const std::string path = "/ir/" + brand + "/" + file;
        std::string title = file;
        if (title.size() > 3) {
            title = title.substr(0, title.size() - 3);
        }
        if (isFavorite(path)) {
            title = "* " + title;
        }
        menu.addItem(title, "ir", [path, title] { openFile(path, title); });
    }
    menu.setHint("ENTER=apri  `=indietro");
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

void runScanner(const std::string& brand) {
    const std::vector<std::string> files = storage::list("/ir/" + brand, false);
    std::vector<std::string> remotes;
    for (const auto& file : files) {
        if (upper(file).size() > 3 &&
            upper(file).substr(upper(file).size() - 3) == ".IR") {
            remotes.push_back(file);
        }
    }
    if (remotes.empty()) {
        ui::message(brand, {"Nessun telecomando da scansionare."}, "Scanner IR");
        return;
    }
    if (!ui::confirm("Scanner IR",
                     {"Invia il tasto Power dei telecomandi",
                      "uno alla volta, finche' il dispositivo",
                      "non reagisce. Usa solo su dispositivi tuoi.",
                      "F = salva il telecomando nei preferiti."},
                     brand)) {
        return;
    }

    ensureSender();
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
        const std::string path = "/ir/" + brand + "/" + remotes[index];
        if (loadPending) {
            model = baseName(path);
            favorite = isFavorite(path);
            feedback.clear();
            commands.clear();
            powerIndex = -1;
            std::string text;
            if (storage::readText(path, text, 65536)) {
                commands = parseIrFile(text);
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
        display::header("Scanner IR", brand);
        display::text(6, 26,
                      "File " + std::to_string(index + 1) + "/" +
                          std::to_string(remotes.size()),
                      display::kMuted, 0);
        std::string shownModel = model;
        auto& d = display::g();
        d.setFont(&fonts::Font2);
        while (shownModel.size() > 4 &&
               d.textWidth((shownModel + "..").c_str()) > display::width() - 12) {
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

void runScannerBrands() {
    const std::vector<std::string> brands = storage::list("/ir", true);
    if (brands.empty()) {
        ui::message("Scanner IR", {"Nessun database in /ir."}, "Infrarossi");
        return;
    }
    MenuManager menu("Scanner IR", "Infrarossi");
    for (const auto& brand : brands) {
        menu.addItem(brand, "ir", [brand] { runScanner(brand); });
    }
    menu.setGrid(3);
    menu.setHint("ENTER=scansiona  ;.=riga/pagina  `=indietro");
    menu.run(true);
}

void runFavorites() {
    const auto stored = loadFavorites();
    std::vector<std::string> favorites;
    for (const auto& path : stored) {
        if (storage::exists(path)) {
            favorites.push_back(path);
        }
    }
    if (favorites.empty()) {
        ui::message("Preferiti", {"Nessun preferito salvato.",
                                  "Apri un telecomando e premi F",
                                  "per aggiungerlo qui."},
                    "Infrarossi");
        return;
    }
    MenuManager menu("Preferiti", "Infrarossi");
    for (const auto& path : favorites) {
        const std::string label = favoriteLabel(path);
        menu.addItem(label, "fav", [path, label] { openFile(path, label); });
    }
    menu.setHint("ENTER=apri  `=indietro");
    menu.run(true);
}

void runRemotes() {
    const std::vector<std::string> brands = storage::list("/ir", true);
    if (brands.empty()) {
        ui::message("Telecomandi", {"Nessun database in /ir.",
                                    "Carica i file .ir su LittleFS o microSD.",
                                    "Formato: Flipper Zero .ir"},
                    "Infrarossi");
        return;
    }
    MenuManager menu("Telecomandi", "Infrarossi");
    for (const auto& brand : brands) {
        menu.addItem(brand, "ir", [brand] { openBrand(brand); });
    }
    menu.setGrid(3);
    menu.setHint("ENTER=apri  ;.=riga/pagina  `=indietro");
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

    const std::vector<std::string> brands = storage::list("/ir", true);
    if (brands.empty()) {
        ui::message("Cerca", {"!Nessun database in /ir."}, "Infrarossi");
        return;
    }

    constexpr size_t kMaxResults = 40;
    const std::vector<std::string> favorites = loadFavorites();
    std::vector<MenuItem> matches;

    for (const auto& brand : brands) {
        const bool brandHit = lower(brand).find(needle) != std::string::npos;
        for (const auto& file : storage::list("/ir/" + brand, false)) {
            const std::string upperFile = upper(file);
            if (upperFile.size() < 4 || upperFile.substr(upperFile.size() - 3) != ".IR") {
                continue;
            }
            const std::string model = baseName(file);
            if (!brandHit && lower(model).find(needle) == std::string::npos) {
                continue;
            }
            const std::string path = "/ir/" + brand + "/" + file;
            std::string title = brand + " / " + model;
            if (std::find(favorites.begin(), favorites.end(), path) != favorites.end()) {
                title = "* " + title;
            }
            matches.emplace_back(title, "ir",
                                 [path, model] { openFile(path, model); });
            if (matches.size() >= kMaxResults) {
                break;
            }
        }
        if (matches.size() >= kMaxResults) {
            break;
        }
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
    menu.setHint("ENTER=apri  `=indietro");
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
    display::footer("` = interrompi");
    display::push();
}

void runTvBg() {
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
            display::footer("` = interrompi");
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
    menu.setHint("`=indietro  </>=scorri  ENTER=apri");
    menu.run(true);
}

}  // namespace ir

#endif  // ENABLE_MODULE_IR
