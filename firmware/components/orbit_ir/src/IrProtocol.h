#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Parser e codificatore puri del modulo IR: nessuna dipendenza ESP-IDF,
// cosi' la logica si puo' verificare anche fuori dal target.
namespace orbit_ir {

constexpr size_t kMaxFileBytes = 16 * 1024;
constexpr size_t kMaxCommands = 64;
constexpr size_t kMaxRawValues = 512;
constexpr size_t kMaxFavorites = 8;

enum class Protocol : uint8_t {
    Unsupported = 0,
    Nec,
    NecExt,
    Samsung32,
    Sony12,
    Sony15,
    Sony20,
    Rc5,
    Rc5x,
    Rc6,
    Lg28,
    Jvc,
    Kaseikyo,
    Rca,
    Raw,
};

struct IrCommand {
    std::string name;
    Protocol protocol = Protocol::Unsupported;
    uint32_t address = 0;
    uint32_t command = 0;
    uint32_t frequency = 38000;
    // Durate in microsecondi: > 0 mark, < 0 space. Solo per i comandi raw.
    std::vector<int32_t> raw;
    bool truncated = false;
    bool supported = false;
};

struct ParsedFile {
    std::vector<IrCommand> commands;
    bool tooLarge = false;
    bool readError = false;
};

// Legge al massimo kMaxFileBytes e restituisce i primi kMaxCommands comandi.
ParsedFile parseIrFile(const std::string& path);

// Converte un comando parsed/raw in durate signed (mark > 0, space < 0).
std::vector<int32_t> buildDurations(const IrCommand& command);

// Frame NEC con dati arbitrari, senza gap lungo finale (jammer).
void appendJamFrame(uint32_t data, std::vector<int32_t>& out);

bool isQuickKey(char key);
int quickKeyScore(const std::string& name, char key);

}  // namespace orbit_ir
