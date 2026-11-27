#pragma once

#include <cstdint>
#include <vector>

#include "IrProtocol.h"

namespace orbit_ir {

// Frame totali per un invio manuale (1 + 2 ripetizioni, come il default 2.x).
constexpr int kExtraRepeats = 2;

// Invia un comando parsed/raw. false se RMT non e' disponibile o il frame
// supera i limiti.
bool sendCommand(const IrCommand& command);

// Invia durate signed (mark > 0, space < 0) ripetute "repeats" volte in piu'.
bool sendDurations(const std::vector<int32_t>& durations, int repeats);

// Rilascia canale ed encoder RMT. Idempotente.
void releaseTx();

}  // namespace orbit_ir
