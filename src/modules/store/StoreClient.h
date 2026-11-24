#pragma once

#include <string>
#include <vector>

#include "StoreLogic.h"

// Rete e filesystem dello store. Il download verifica SHA-256 e scrive solo
// dopo la verifica; il catalogo e' limitato per dimensione e numero di voci.
namespace store {

bool fetchCatalog(std::vector<GameInfo>& out, const std::string& parent);
bool downloadGame(const GameInfo& info, const std::string& parent);

// True se il file esiste ed ha la dimensione attesa (controllo veloce per la
// lista). La verifica SHA-256 completa avviene al download.
bool gameInstalled(const GameInfo& info);

}  // namespace store
