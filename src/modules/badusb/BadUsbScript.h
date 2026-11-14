#pragma once

#include <string>

#include "ModuleFlags.h"

#if ENABLE_MODULE_BADUSB

namespace badusb {

// Esegue uno script da /badusb. Ritorna true se arriva alla fine.
bool runScript(const std::string& path, const std::string& name, bool preferSd);

}  // namespace badusb

#endif  // ENABLE_MODULE_BADUSB
