#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Store di OrbitOS 3.0: catalogo firmato ECDSA P-256 + giochi .wren sulla SD.
namespace orbit_store {

struct GameInfo {
    std::string id;
    std::string name;
    std::string version;
    std::string desc;
    std::string url;
    std::string sha256;
    uint32_t size = 0;
};

constexpr const char* kGamesDir = "/littlefs/games";

bool mountSd();
bool mountLittleFs();
// Driver WiFi condiviso: lo Store lo usa per i download, i moduli di analisi
// per scan/attacchi. wifiEnsureDriver() non connette; wifiShutdownDriver()
// ferma e deinizializza il driver (usare prima di un modulo che cambia modo).
bool wifiEnsureDriver();
void wifiShutdownDriver();
bool ensureWifi(std::string* error);
bool currentSsid(std::string& ssid);
bool saveWifi(const std::string& ssid, const std::string& password);

bool fetchCatalog(std::vector<GameInfo>& out, std::string* error);
bool downloadGame(const GameInfo& game, std::string* error);
bool gameInstalled(const GameInfo& game);
std::vector<std::string> installedGames();
std::string gamePath(const std::string& id);

}  // namespace orbit_store
