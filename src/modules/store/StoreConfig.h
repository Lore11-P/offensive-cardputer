#pragma once

// Catalogo pubblico dello store. Stesso repository dell'OTA: i file devono
// essere pubblicati su main (store/games.json + store/games/*.wren).
namespace store {

constexpr const char* kCatalogUrl =
    "https://raw.githubusercontent.com/Lore11-P/offensive-cardputer/main/store/games.json";
constexpr const char* kCatalogSigUrl =
    "https://raw.githubusercontent.com/Lore11-P/offensive-cardputer/main/store/games.sig";
constexpr const char* kGamesDir = "/games";
// Prefisso esatto dei giochi: il catalogo non puo' puntare a repo esterni.
constexpr const char* kGameBaseUrl =
    "https://raw.githubusercontent.com/Lore11-P/offensive-cardputer/main/store/games/";

}  // namespace store
