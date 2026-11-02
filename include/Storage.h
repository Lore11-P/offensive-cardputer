#pragma once

#include <FS.h>
#include <string>
#include <vector>

// Astrazione minima LittleFS + SD.
// Le risorse (icone, database IR, config) vengono cercate prima su LittleFS
// e, se assenti, sulla SD montata in /sd.
namespace storage {

struct Entry {
    std::string name;
    bool isDir = false;
    size_t size = 0;
};

bool begin();
bool hasLittleFS();
bool hasSD();

// Apre un file in sola lettura su LittleFS o SD. Con preferSD=true usa solo
// la microSD (utile per il file browser e i backup).
File openRead(const std::string& path, bool preferSD = false);
bool exists(const std::string& path, bool preferSD = false);

// Elenca file o directory in un path logico (es. "/ir").
// Se dirsOnly e' true restituisce solo directory, altrimenti solo file.
std::vector<std::string> list(const std::string& path, bool dirsOnly,
                              bool preferSD = false);

// Elenca file e directory con dimensione, directory prima dei file.
std::vector<Entry> listEntries(const std::string& path, bool preferSD = true);

// Legge un file di testo con limite massimo (protezione da file enormi).
bool readText(const std::string& path, std::string& out, size_t maxBytes = 16384,
              bool preferSD = false);

// Scrittura e gestione file (default: microSD).
File openWrite(const std::string& path, bool preferSD = true);
bool writeText(const std::string& path, const std::string& data, bool preferSD = true);
bool removeFile(const std::string& path, bool preferSD = true);
bool mkdir(const std::string& path, bool preferSD = true);
size_t fileSize(const std::string& path, bool preferSD = false);

// Nome del filesystem attualmente usato per l'ultima apertura.
const char* lastBackend();

}  // namespace storage
