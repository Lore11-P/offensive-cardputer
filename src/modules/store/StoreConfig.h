#pragma once

// Catalogo pubblico dello store. Stesso repository dell'OTA: i file devono
// essere pubblicati su main (store/games.json + store/games/*.wren).
namespace store {

constexpr const char* kCatalogUrl =
    "https://raw.githubusercontent.com/Lore11-P/offensive-cardputer/main/store/games.json";
constexpr const char* kGamesDir = "/games";

}  // namespace store
