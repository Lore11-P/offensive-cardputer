#include "Storage.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>
#include <algorithm>
#include <cstring>

#include "ModuleFlags.h"
#include "Logger.h"

namespace storage {
namespace {
bool g_littlefs = false;
bool g_sd = false;
const char* g_backend = "none";

std::string trimEntryName(const char* name) {
    std::string s = name ? name : "";
    const size_t slash = s.find_last_of('/');
    if (slash != std::string::npos) {
        s = s.substr(slash + 1);
    }
    return s;
}

File openOn(fs::FS& filesystem, const std::string& path) {
    if (!filesystem.exists(path.c_str())) {
        return File();
    }
    return filesystem.open(path.c_str(), FILE_READ);
}

std::string sdPath(const std::string& path) {
    if (path.rfind("/sd", 0) == 0) {
        return path;
    }
    if (path.empty() || path[0] != '/') {
        return "/sd/" + path;
    }
    return "/sd" + path;
}

std::string joinPath(const std::string& base, const std::string& name) {
    if (base.empty() || base == "/") {
        return "/" + name;
    }
    if (base.back() == '/') {
        return base + name;
    }
    return base + "/" + name;
}
}  // namespace

bool begin() {
    g_littlefs = false;
    g_sd = false;

    // Il primo avvio puo' formattare la partizione dati: non contiene nulla
    // di critico e permette di montare il filesystem anche se vuoto.
    if (LittleFS.begin(true)) {
        g_littlefs = true;
        logger::info("LittleFS montato (%u byte liberi)", static_cast<unsigned>(LittleFS.totalBytes() - LittleFS.usedBytes()));
    } else {
        logger::warn("LittleFS non disponibile");
    }

    // BUS SPI dedicato alla microSD (SPI3 su ESP32-S3). Se non c'e' una card
    // il mount fallisce senza bloccare il firmware.
    SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    if (SD.begin(SD_CS_PIN, SPI, 25000000, "/sd")) {
        g_sd = true;
        logger::info("microSD montata");
    } else {
        logger::info("microSD assente");
    }
    return g_littlefs || g_sd;
}

bool hasLittleFS() {
    return g_littlefs;
}

bool hasSD() {
    return g_sd;
}

File openRead(const std::string& path, bool preferSD) {
    if (path.empty()) {
        return File();
    }
    if (preferSD) {
        if (g_sd) {
            File f = openOn(SD, sdPath(path));
            if (f) {
                g_backend = "SD";
                return f;
            }
        }
        g_backend = "none";
        return File();
    }
    if (g_littlefs) {
        File f = openOn(LittleFS, path);
        if (f) {
            g_backend = "LittleFS";
            return f;
        }
    }
    if (g_sd) {
        File f = openOn(SD, sdPath(path));
        if (f) {
            g_backend = "SD";
            return f;
        }
    }
    g_backend = "none";
    return File();
}

bool exists(const std::string& path, bool preferSD) {
    if (preferSD) {
        return g_sd && SD.exists(sdPath(path).c_str());
    }
    if (g_littlefs && LittleFS.exists(path.c_str())) {
        return true;
    }
    if (g_sd) {
        return SD.exists(sdPath(path).c_str());
    }
    return false;
}

std::vector<std::string> list(const std::string& path, bool dirsOnly, bool preferSD) {
    std::vector<std::string> out;
    auto collect = [&](fs::FS& filesystem, const std::string& base) {
        File dir = filesystem.open(base.c_str(), FILE_READ);
        if (!dir || !dir.isDirectory()) {
            return;
        }
        File entry = dir.openNextFile();
        while (entry) {
            std::string name = trimEntryName(entry.name());
            if (name.size() && name[0] != '.' && entry.isDirectory() == dirsOnly) {
                out.push_back(name);
            }
            entry.close();
            entry = dir.openNextFile();
        }
        dir.close();
    };

    if (preferSD) {
        if (g_sd) {
            collect(SD, sdPath(path));
        }
    } else {
        if (g_littlefs) {
            collect(LittleFS, path);
        }
        if (g_sd) {
            collect(SD, sdPath(path));
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<Entry> listEntries(const std::string& path, bool preferSD) {
    std::vector<Entry> out;
    auto collect = [&](fs::FS& filesystem, const std::string& base) {
        File dir = filesystem.open(base.c_str(), FILE_READ);
        if (!dir || !dir.isDirectory()) {
            return;
        }
        File entry = dir.openNextFile();
        while (entry) {
            std::string name = trimEntryName(entry.name());
            if (name.size() && name[0] != '.') {
                Entry item;
                item.name = name;
                item.isDir = entry.isDirectory();
                item.size = item.isDir ? 0 : static_cast<size_t>(entry.size());
                out.push_back(std::move(item));
            }
            entry.close();
            entry = dir.openNextFile();
        }
        dir.close();
    };

    if (preferSD) {
        if (g_sd) {
            collect(SD, sdPath(path));
        }
    } else {
        if (g_littlefs) {
            collect(LittleFS, path);
        }
        if (g_sd) {
            collect(SD, sdPath(path));
        }
    }

    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        if (a.isDir != b.isDir) {
            return a.isDir;
        }
        return a.name < b.name;
    });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const Entry& a, const Entry& b) {
                              return a.name == b.name && a.isDir == b.isDir;
                          }),
              out.end());
    return out;
}

bool readText(const std::string& path, std::string& out, size_t maxBytes,
              bool preferSD) {
    File f = openRead(path, preferSD);
    if (!f) {
        return false;
    }
    out.clear();
    out.reserve(maxBytes < 4096 ? maxBytes : 4096);
    size_t total = 0;
    uint8_t buffer[256];
    while (f.available() && total < maxBytes) {
        const size_t want = std::min(sizeof(buffer), maxBytes - total);
        const int got = f.read(buffer, want);
        if (got <= 0) {
            break;
        }
        out.append(reinterpret_cast<const char*>(buffer), static_cast<size_t>(got));
        total += static_cast<size_t>(got);
    }
    f.close();
    return true;
}

File openWrite(const std::string& path, bool preferSD) {
    if (path.empty()) {
        return File();
    }
    if (preferSD) {
        if (!g_sd) {
            return File();
        }
        File f = SD.open(sdPath(path).c_str(), FILE_WRITE);
        if (f) {
            g_backend = "SD";
        }
        return f;
    }
    if (g_littlefs) {
        File f = LittleFS.open(path.c_str(), FILE_WRITE);
        if (f) {
            g_backend = "LittleFS";
            return f;
        }
    }
    if (g_sd) {
        File f = SD.open(sdPath(path).c_str(), FILE_WRITE);
        if (f) {
            g_backend = "SD";
        }
        return f;
    }
    return File();
}

bool writeText(const std::string& path, const std::string& data, bool preferSD) {
    File f = openWrite(path, preferSD);
    if (!f) {
        return false;
    }
    const size_t written =
        f.write(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    f.close();
    return written == data.size();
}

bool removeFile(const std::string& path, bool preferSD) {
    if (preferSD) {
        return g_sd && SD.remove(sdPath(path).c_str());
    }
    if (g_littlefs && LittleFS.remove(path.c_str())) {
        return true;
    }
    return g_sd && SD.remove(sdPath(path).c_str());
}

bool mkdir(const std::string& path, bool preferSD) {
    if (preferSD) {
        return g_sd && SD.mkdir(sdPath(path).c_str());
    }
    if (g_littlefs && LittleFS.mkdir(path.c_str())) {
        return true;
    }
    return g_sd && SD.mkdir(sdPath(path).c_str());
}

size_t fileSize(const std::string& path, bool preferSD) {
    File f = openRead(path, preferSD);
    if (!f) {
        return 0;
    }
    const size_t size = static_cast<size_t>(f.size());
    f.close();
    return size;
}

const char* lastBackend() {
    return g_backend;
}

}  // namespace storage
