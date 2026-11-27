#include "IrProtocol.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace orbit_ir {
namespace {

constexpr uint32_t kNecHdrMark = 8960;
constexpr uint32_t kNecHdrSpace = 4480;
constexpr uint32_t kNecBitMark = 560;
constexpr uint32_t kNecOneSpace = 1680;
constexpr uint32_t kNecZeroSpace = 560;
constexpr uint32_t kNecMinLength = 108080;

constexpr uint32_t kSamsungHdrMark = 4480;
constexpr uint32_t kSamsungHdrSpace = 4480;
constexpr uint32_t kSamsungMinLength = 108080;

constexpr uint32_t kSonyHdrMark = 2400;
constexpr uint32_t kSonySpace = 600;
constexpr uint32_t kSonyOneMark = 1200;
constexpr uint32_t kSonyZeroMark = 600;
constexpr uint32_t kSonyMinLength = 45000;
constexpr uint32_t kSonyMinGap = 10000;

constexpr uint32_t kLgHdrMark = 8500;
constexpr uint32_t kLgHdrSpace = 4250;
constexpr uint32_t kLgBitMark = 550;
constexpr uint32_t kLgOneSpace = 1600;
constexpr uint32_t kLgZeroSpace = 550;
constexpr uint32_t kLgMinLength = 108050;
constexpr uint32_t kLgMinGap = 39750;

constexpr uint32_t kJvcHdrMark = 8400;
constexpr uint32_t kJvcHdrSpace = 4200;
constexpr uint32_t kJvcBitMark = 525;
constexpr uint32_t kJvcOneSpace = 1725;
constexpr uint32_t kJvcZeroSpace = 525;
constexpr uint32_t kJvcMinLength = 60000;
constexpr uint32_t kJvcMinGap = 10875;

constexpr uint32_t kPanasonicHdrMark = 3456;
constexpr uint32_t kPanasonicHdrSpace = 1728;
constexpr uint32_t kPanasonicBitMark = 432;
constexpr uint32_t kPanasonicOneSpace = 1296;
constexpr uint32_t kPanasonicZeroSpace = 432;
constexpr uint32_t kPanasonicMinLength = 163296;
constexpr uint32_t kPanasonicMinGap = 74736;

constexpr uint32_t kRc5Tick = 889;
constexpr uint32_t kRc5MinLength = 113778;
constexpr uint32_t kRc5MinGap = 88886;

constexpr uint32_t kRc6Tick = 444;
constexpr uint32_t kRc6HdrMark = 6 * kRc6Tick;
constexpr uint32_t kRc6HdrSpace = 2 * kRc6Tick;
constexpr uint32_t kRc6RptLength = 187 * kRc6Tick;

bool startsWith(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

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

std::string upper(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return value;
}

std::string lower(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

uint32_t reverseBits(uint64_t value, int bits) {
    uint32_t result = 0;
    for (int i = 0; i < bits; ++i) {
        result = (result << 1) | static_cast<uint32_t>(value & 1);
        value >>= 1;
    }
    return result;
}

uint8_t reverseBits8(uint8_t value) {
    value = static_cast<uint8_t>(((value & 0xF0) >> 4) | ((value & 0x0F) << 4));
    value = static_cast<uint8_t>(((value & 0xCC) >> 2) | ((value & 0x33) << 2));
    value = static_cast<uint8_t>(((value & 0xAA) >> 1) | ((value & 0x55) << 1));
    return value;
}

// Flipper scrive address/command come byte esadecimali in ordine little-endian:
// "04 00 00 00" -> 0x04. Un singolo token piu' lungo di un byte e' gia' il
// valore completo (es. "0x1234").
uint32_t parseHexBytes(const char* value) {
    uint32_t result = 0;
    int shift = 0;
    bool first = true;
    const char* p = value;
    while (*p != '\0' && shift < 32) {
        while (*p == ' ' || *p == '\t' || *p == ',') {
            ++p;
        }
        if (*p == '\0') {
            break;
        }
        const char* begin = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != ',') {
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

Protocol protocolFromName(const std::string& name) {
    const std::string id = upper(trim(name));
    if (id == "NEC") {
        return Protocol::Nec;
    }
    if (id == "NECEXT") {
        return Protocol::NecExt;
    }
    if (id == "SAMSUNG" || id == "SAMSUNG32") {
        return Protocol::Samsung32;
    }
    if (id == "SONY" || id == "SIRC" || id == "SONY12" || id == "SIRC12") {
        return Protocol::Sony12;
    }
    if (id == "SONY15" || id == "SIRC15") {
        return Protocol::Sony15;
    }
    if (id == "SONY20" || id == "SIRC20") {
        return Protocol::Sony20;
    }
    if (id == "RC5") {
        return Protocol::Rc5;
    }
    if (id == "RC5X") {
        return Protocol::Rc5x;
    }
    if (id == "RC6") {
        return Protocol::Rc6;
    }
    if (id == "LG" || id == "LG2") {
        return Protocol::Lg28;
    }
    if (id == "JVC") {
        return Protocol::Jvc;
    }
    if (id == "KASEIKYO" || id == "PANASONIC") {
        return Protocol::Kaseikyo;
    }
    if (id == "RCA") {
        return Protocol::Rca;
    }
    return Protocol::Unsupported;
}

// Aggiunge i valori di una riga data: al comando. true se il budget/limite
// e' stato superato (il comando diventa non inviabile).
bool addRawValues(const char* value, IrCommand& command, size_t& budget) {
    const char* p = value;
    while (*p != '\0') {
        char* end = nullptr;
        long parsed = std::strtol(p, &end, 10);
        if (end == p) {
            ++p;
            continue;
        }
        p = end;
        if (parsed == 0) {
            continue;
        }
        if (parsed > 1000000L) {
            parsed = 1000000L;
        } else if (parsed < -1000000L) {
            parsed = -1000000L;
        }
        if (command.raw.size() >= kMaxRawValues || budget == 0) {
            command.truncated = true;
            return true;
        }
        command.raw.push_back(static_cast<int32_t>(parsed));
        --budget;
    }
    return false;
}

void normalizeRaw(IrCommand& command) {
    bool hasNegative = false;
    for (int32_t value : command.raw) {
        if (value < 0) {
            hasNegative = true;
            break;
        }
    }
    if (hasNegative) {
        return;
    }
    // Flipper raw positivo: mark, space, mark, space...
    for (size_t i = 1; i < command.raw.size(); i += 2) {
        command.raw[i] = -command.raw[i];
    }
}

uint32_t encodeNec(uint32_t address, uint32_t command) {
    uint32_t cmd = reverseBits(command & 0xFF, 8);
    cmd = (cmd << 8) + (cmd ^ 0xFF);
    if ((address & 0xFFFF) > 0xFF) {
        const uint32_t ext = reverseBits(address & 0xFFFF, 16);
        return (ext << 16) + cmd;
    }
    const uint32_t addr = reverseBits(address & 0xFF, 8);
    return (addr << 24) + ((addr ^ 0xFF) << 16) + cmd;
}

uint32_t encodeSamsung(uint32_t address, uint32_t command) {
    const uint32_t revCustomer = reverseBits(address & 0xFF, 8);
    const uint32_t revCommand = reverseBits(command & 0xFF, 8);
    return (revCommand ^ 0xFF) | (revCommand << 8) | (revCustomer << 16) |
           (revCustomer << 24);
}

uint32_t encodeSony(Protocol protocol, uint32_t address, uint32_t command) {
    int bits = 12;
    uint32_t payload = address & 0x1F;
    if (protocol == Protocol::Sony15) {
        bits = 15;
        payload = address & 0xFF;
    } else if (protocol == Protocol::Sony20) {
        bits = 20;
        payload = address & 0x1FFF;
    }
    const uint32_t message = (payload << 7) | (command & 0x7F);
    return reverseBits(message, bits);
}

uint16_t encodeJvc(uint32_t address, uint32_t command) {
    return static_cast<uint16_t>(
        reverseBits(((command & 0xFF) << 8) | (address & 0xFF), 16));
}

uint32_t encodeLg(uint32_t address, uint32_t command) {
    const uint32_t cmd = command & 0xFFFF;
    const uint32_t checksum =
        ((cmd & 0xF) + ((cmd >> 4) & 0xF) + ((cmd >> 8) & 0xF) +
         ((cmd >> 12) & 0xF)) &
        0xF;
    return ((address & 0xFFF) << 20) | ((cmd & 0xFFFF) << 4) | checksum;
}

uint16_t encodeRc5(uint32_t address, uint32_t command) {
    return static_cast<uint16_t>(((address & 0x1F) << 6) |
                                 (command & 0x3F));
}

uint16_t encodeRc5x(uint32_t address, uint32_t command) {
    const uint16_t s2 = static_cast<uint16_t>((command >> 6) & 1);
    return static_cast<uint16_t>((s2 << 12) | encodeRc5(address, command));
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
    uint8_t parity = static_cast<uint8_t>(bytes[0] ^ bytes[1]);
    parity = static_cast<uint8_t>((parity & 0x0F) ^ (parity >> 4));
    bytes[2] = static_cast<uint8_t>((genre1 << 4) | (parity & 0x0F));
    bytes[3] = static_cast<uint8_t>(((code & 0x0F) << 4) | (genre2 & 0x0F));
    bytes[4] = static_cast<uint8_t>((id << 6) | ((code >> 4) & 0x3F));
    bytes[5] = static_cast<uint8_t>(bytes[2] ^ bytes[3] ^ bytes[4]);

    uint64_t data = 0;
    for (uint8_t byte : bytes) {
        data = (data << 8) | reverseBits8(byte);
    }
    return data;
}

void addMark(std::vector<int32_t>& out, uint32_t usec) {
    if (usec != 0) {
        out.push_back(static_cast<int32_t>(usec));
    }
}

void addSpace(std::vector<int32_t>& out, uint32_t usec) {
    if (usec != 0) {
        out.push_back(-static_cast<int32_t>(usec));
    }
}

// Riproduce IRsend::sendGeneric(): header, bit MSB-first, footer, gap fino
// alla lunghezza minima del messaggio.
void appendGeneric(std::vector<int32_t>& out, uint32_t hdrMark,
                   uint32_t hdrSpace, uint32_t oneMark, uint32_t oneSpace,
                   uint32_t zeroMark, uint32_t zeroSpace, uint32_t footMark,
                   uint32_t gap, uint32_t minLength, uint64_t data, int bits) {
    uint64_t elapsed = 0;
    const auto mark = [&](uint32_t usec) {
        addMark(out, usec);
        elapsed += usec;
    };
    const auto space = [&](uint32_t usec) {
        addSpace(out, usec);
        elapsed += usec;
    };

    if (hdrMark != 0) {
        mark(hdrMark);
    }
    if (hdrSpace != 0) {
        space(hdrSpace);
    }
    for (int bit = bits - 1; bit >= 0; --bit) {
        const bool one = ((data >> bit) & 1) != 0;
        mark(one ? oneMark : zeroMark);
        space(one ? oneSpace : zeroSpace);
    }
    if (footMark != 0) {
        mark(footMark);
    }
    if (minLength != 0 && elapsed < minLength) {
        space(std::max(gap, static_cast<uint32_t>(minLength - elapsed)));
    } else if (gap != 0) {
        space(gap);
    }
}

// Riproduce IRsend::sendRC5(): 14 bit Manchester, primi due bit di start.
void appendRc5(std::vector<int32_t>& out, uint16_t data, int bits,
               uint32_t minLength) {
    bool fieldBit = true;
    if (bits >= 13) {
        fieldBit = (((data >> (bits - 1)) ^ 1) & 1) != 0;
        --bits;
    }
    uint64_t elapsed = 0;
    const auto mark = [&](uint32_t usec) {
        addMark(out, usec);
        elapsed += usec;
    };
    const auto space = [&](uint32_t usec) {
        addSpace(out, usec);
        elapsed += usec;
    };

    mark(kRc5Tick);
    if (fieldBit) {
        space(kRc5Tick);
        mark(kRc5Tick);
    } else {
        mark(kRc5Tick);
        space(kRc5Tick);
    }
    for (int bit = bits - 1; bit >= 0; --bit) {
        if (((data >> bit) & 1) != 0) {
            space(kRc5Tick);
            mark(kRc5Tick);
        } else {
            mark(kRc5Tick);
            space(kRc5Tick);
        }
    }
    const uint32_t want = static_cast<uint32_t>(
        elapsed < minLength ? minLength - elapsed : kRc5MinGap);
    space(std::max(kRc5MinGap, want));
}

void appendRc6(std::vector<int32_t>& out, uint32_t data) {
    addMark(out, kRc6HdrMark);
    addSpace(out, kRc6HdrSpace);
    addMark(out, kRc6Tick);
    addSpace(out, kRc6Tick);
    for (int sent = 1, bit = 19; bit >= 0; ++sent, --bit) {
        const uint32_t bitTime = (sent == 4) ? 2 * kRc6Tick : kRc6Tick;
        if (((data >> bit) & 1) != 0) {
            addMark(out, bitTime);
            addSpace(out, bitTime);
        } else {
            addSpace(out, bitTime);
            addMark(out, bitTime);
        }
    }
    addSpace(out, kRc6RptLength);
}

void appendRca(std::vector<int32_t>& out, uint32_t address,
               uint32_t command) {
    const uint32_t data = (address & 0x0F) | ((command & 0xFF) << 4) |
                          (((~address) & 0x0F) << 12) |
                          (((~command) & 0xFF) << 16);
    addMark(out, 4000);
    addSpace(out, 4000);
    for (int bit = 0; bit < 24; ++bit) {
        addMark(out, 500);
        addSpace(out, ((data >> bit) & 1) != 0 ? 2000 : 1000);
    }
}

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

ParsedFile parseIrFile(const std::string& path) {
    ParsedFile result;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        result.readError = true;
        return result;
    }

    std::string content;
    char chunk[512];
    size_t got = 0;
    while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        if (content.size() + got > kMaxFileBytes) {
            result.tooLarge = true;
            break;
        }
        content.append(chunk, got);
    }
    if (std::ferror(file) != 0) {
        result.readError = true;
    }
    std::fclose(file);
    if (result.tooLarge || result.readError) {
        return result;
    }

    IrCommand current;
    bool active = false;
    bool typeRaw = false;
    std::string protocolName;
    // Budget complessivo per file: 16 KB di durate raw.
    constexpr size_t kRawBudgetValues = 4096;
    size_t rawBudget = kRawBudgetValues;

    const auto flush = [&]() {
        if (active && !current.name.empty() &&
            result.commands.size() < kMaxCommands) {
            if (typeRaw) {
                current.protocol = Protocol::Raw;
                normalizeRaw(current);
                current.supported =
                    !current.raw.empty() && !current.truncated;
            } else {
                current.protocol = protocolFromName(protocolName);
                current.supported = current.protocol != Protocol::Unsupported;
            }
            result.commands.push_back(std::move(current));
        }
        current = IrCommand();
        typeRaw = false;
        protocolName.clear();
        active = false;
    };

    size_t pos = 0;
    while (pos <= content.size() &&
           result.commands.size() < kMaxCommands) {
        const size_t newline = content.find('\n', pos);
        const size_t end =
            newline == std::string::npos ? content.size() : newline;
        std::string line = trim(content.substr(pos, end - pos));
        if (line.empty()) {
            if (newline == std::string::npos) {
                break;
            }
            pos = newline + 1;
            continue;
        }
        if (line[0] == '#') {
            flush();
        } else if (startsWith(line, "name:")) {
            flush();
            current.name = trim(line.substr(5));
            // Un nome ostile non deve diventare una label da 16 KB.
            if (current.name.size() > 48) {
                current.name.resize(48);
            }
            active = true;
        } else if (!active) {
            // Filetype/Version e commenti prima del primo name:.
        } else if (startsWith(line, "type:")) {
            typeRaw = upper(trim(line.substr(5))) == "RAW";
        } else if (startsWith(line, "protocol:")) {
            protocolName = trim(line.substr(9));
            if (protocolName.size() > 32) {
                protocolName.resize(32);
            }
        } else if (startsWith(line, "address:")) {
            current.address = parseHexBytes(line.c_str() + 8);
        } else if (startsWith(line, "command:")) {
            current.command = parseHexBytes(line.c_str() + 8);
        } else if (startsWith(line, "frequency:")) {
            const unsigned long value =
                std::strtoul(line.c_str() + 10, nullptr, 10);
            if (value >= 20000 && value <= 60000) {
                current.frequency = static_cast<uint32_t>(value);
            }
        } else if (startsWith(line, "duty_cycle:")) {
            // La portante e' fissa al 38 kHz / 33%: il valore e' ignorato.
        } else if (startsWith(line, "data:")) {
            if (typeRaw &&
                addRawValues(line.c_str() + 5, current, rawBudget)) {
                // Il comando e' gia' marcato troncato.
            }
        } else if (typeRaw &&
                   (std::isdigit(static_cast<unsigned char>(line[0])) ||
                    line[0] == '-' || line[0] == '+')) {
            addRawValues(line.c_str(), current, rawBudget);
        }

        if (newline == std::string::npos) {
            break;
        }
        pos = newline + 1;
    }
    flush();
    return result;
}

std::vector<int32_t> buildDurations(const IrCommand& command) {
    std::vector<int32_t> out;
    if (command.protocol == Protocol::Raw) {
        return command.raw;
    }
    out.reserve(128);

    switch (command.protocol) {
        case Protocol::Nec:
        case Protocol::NecExt:
            appendGeneric(out, kNecHdrMark, kNecHdrSpace, kNecBitMark,
                          kNecOneSpace, kNecBitMark, kNecZeroSpace, kNecBitMark,
                          0, kNecMinLength,
                          encodeNec(command.address, command.command), 32);
            break;
        case Protocol::Samsung32:
            appendGeneric(out, kSamsungHdrMark, kSamsungHdrSpace, kNecBitMark,
                          kNecOneSpace, kNecBitMark, kNecZeroSpace, kNecBitMark,
                          0, kSamsungMinLength,
                          encodeSamsung(command.address, command.command), 32);
            break;
        case Protocol::Sony12:
        case Protocol::Sony15:
        case Protocol::Sony20: {
            const int bits = command.protocol == Protocol::Sony12
                                 ? 12
                                 : (command.protocol == Protocol::Sony15 ? 15
                                                                        : 20);
            appendGeneric(out, kSonyHdrMark, kSonySpace, kSonyOneMark,
                          kSonySpace, kSonyZeroMark, kSonySpace, 0,
                          kSonyMinGap, kSonyMinLength,
                          encodeSony(command.protocol, command.address,
                                     command.command),
                          bits);
            break;
        }
        case Protocol::Rc5:
            appendRc5(out, encodeRc5(command.address, command.command), 13,
                      kRc5MinLength);
            break;
        case Protocol::Rc5x:
            appendRc5(out, encodeRc5x(command.address, command.command), 13,
                      kRc5MinLength);
            break;
        case Protocol::Rc6: {
            const uint32_t data =
                ((command.address & 0xFFF) << 8) | (command.command & 0xFF);
            appendRc6(out, data);
            break;
        }
        case Protocol::Lg28:
            appendGeneric(out, kLgHdrMark, kLgHdrSpace, kLgBitMark,
                          kLgOneSpace, kLgBitMark, kLgZeroSpace, kLgBitMark,
                          kLgMinGap, kLgMinLength,
                          encodeLg(command.address, command.command), 28);
            break;
        case Protocol::Jvc:
            appendGeneric(out, kJvcHdrMark, kJvcHdrSpace, kJvcBitMark,
                          kJvcOneSpace, kJvcBitMark, kJvcZeroSpace, kJvcBitMark,
                          kJvcMinGap, kJvcMinLength,
                          encodeJvc(command.address, command.command), 16);
            break;
        case Protocol::Kaseikyo:
            appendGeneric(out, kPanasonicHdrMark, kPanasonicHdrSpace,
                          kPanasonicBitMark, kPanasonicOneSpace,
                          kPanasonicBitMark, kPanasonicZeroSpace,
                          kPanasonicBitMark, kPanasonicMinGap,
                          kPanasonicMinLength,
                          encodeKaseikyo(command.address, command.command), 48);
            break;
        case Protocol::Rca:
            appendRca(out, command.address, command.command);
            break;
        case Protocol::Unsupported:
        case Protocol::Raw:
            break;
    }
    return out;
}

void appendJamFrame(uint32_t data, std::vector<int32_t>& out) {
    out.clear();
    appendGeneric(out, kNecHdrMark, kNecHdrSpace, kNecBitMark, kNecOneSpace,
                  kNecBitMark, kNecZeroSpace, kNecBitMark, 5000, 0, data, 32);
}

bool isQuickKey(char key) {
    const char k = static_cast<char>(std::tolower(static_cast<unsigned char>(key)));
    return k == 'p' || k == 'm' || k == '+' || k == '=' || k == '-' ||
           k == 'c' || k == 'x';
}

int quickKeyScore(const std::string& name, char key) {
    const std::string id = lower(name);
    const char k = static_cast<char>(std::tolower(static_cast<unsigned char>(key)));
    switch (k) {
        case 'p':
            if (id == "power" || id == "accensione") {
                return 100;
            }
            if (contains(id, "power") || contains(id, "standby") ||
                contains(id, "on/off") || contains(id, "toggle") ||
                contains(id, "accensione")) {
                return 80;
            }
            if (id == "on" || id == "off") {
                return 60;
            }
            return -1;
        case 'm':
            if (id == "mute") {
                return 100;
            }
            return (contains(id, "mute") || contains(id, "muto")) ? 80 : -1;
        case '+':
        case '=':
            if (id == "vol_up" || id == "volume_up" || id == "vol+") {
                return 100;
            }
            return (contains(id, "vol_up") || contains(id, "vol+") ||
                    contains(id, "volume_up") || contains(id, "volume up") ||
                    contains(id, "vol up"))
                       ? 80
                       : -1;
        case '-':
            if (id == "vol_dn" || id == "vol_down" ||
                id == "volume_down" || id == "vol-") {
                return 100;
            }
            return (contains(id, "vol_dn") || contains(id, "vol-") ||
                    contains(id, "volume_dn") || contains(id, "volume_down") ||
                    contains(id, "volume down") || contains(id, "vol down"))
                       ? 80
                       : -1;
        case 'c':
            if (id == "ch_next" || id == "ch+") {
                return 100;
            }
            return (contains(id, "ch_next") || contains(id, "ch+") ||
                    contains(id, "channel_up") || contains(id, "channel up") ||
                    contains(id, "ch up"))
                       ? 80
                       : -1;
        case 'x':
            if (id == "ch_prev" || id == "ch-") {
                return 100;
            }
            return (contains(id, "ch_prev") || contains(id, "ch-") ||
                    contains(id, "channel_down") ||
                    contains(id, "channel down") ||
                    contains(id, "ch down"))
                       ? 80
                       : -1;
        default:
            return -1;
    }
}

}  // namespace orbit_ir
