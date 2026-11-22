#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ModuleFlags.h"

#if ENABLE_MODULE_BLEHID

namespace blehid {

// Parser Ducky minimo, compatibile con i comandi di BadUSB.
// Vive in blehid perche' la build BLE HID non compila il modulo USB.
struct ScriptResult {
    bool completed = false;
    bool aborted = false;
    bool disconnected = false;
    bool flowError = false;  // report HID scartati dallo stack/controller
    uint32_t lines = 0;
    uint32_t total = 0;
};

// Elenca gli script in /badusb: prima la SD, LittleFS come fallback.
// preferSd dice al chiamante quale backend usare per l'apertura.
std::vector<std::string> listScripts(bool& preferSd);

// Esegue lo script. Richiede hid::connected() gia' attivo: disegna lo stato
// e torna con ESC. Non inizializza ne' ferma NimBLE.
ScriptResult runScript(const std::string& path, const std::string& name, bool preferSd);

}  // namespace blehid

#endif  // ENABLE_MODULE_BLEHID
