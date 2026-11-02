#pragma once

#include <string>
#include <vector>

// Gestione della rete Wi-Fi: credenziali in NVS, scansione delle reti,
// inserimento password e connessione.
//
// E' l'unico punto del firmware che conserva una password di rete: la salva
// nel namespace NVS "cpfw-net" per poter riconnettere i moduli senza
// richiederla a ogni avvio. Si cancella da "Dimentica rete".
//
// Il codice e' usato da Impostazioni > Rete WiFi ed e' l'unico punto del
// firmware che salva una password di rete: cosi' credenziali e comportamento
// restano coerenti per i moduli che useranno la rete.
namespace wifisetup {

struct Network {
    std::string ssid;
    std::string bssid;
    int rssi = 0;
    int channel = 1;
    int auth = 0;
};

// Nome leggibile del tipo di cifratura (stessa terminologia del modulo WiFi).
const char* securityName(int auth);

// --- Credenziali (namespace NVS "cpfw-net") ---------------------------------
bool loadCredentials(std::string& ssid, std::string& password);
bool saveCredentials(const std::string& ssid, const std::string& password);
void forgetCredentials();
bool hasCredentials();

// --- Operazioni con interfaccia ---------------------------------------------
// Scansiona le reti con schermata di avanzamento. False se annullato o errore.
bool scanNetworks(std::vector<Network>& networks, const std::string& parent);

// Mostra l'elenco delle reti e restituisce l'indice scelto (-1 se annullato).
int chooseNetwork(const std::vector<Network>& networks, const std::string& parent);

// Scansiona, fa scegliere la rete, chiede la password, salva e connette.
bool scanAndConnect(const std::string& parent);

// Connette con le credenziali indicate, mostrando l'avanzamento.
bool connect(const std::string& ssid, const std::string& password,
             const std::string& parent);

// Connette usando le credenziali salvate; se mancano e interactive=true avvia
// il flusso di configurazione.
bool ensureConnected(bool interactive, const std::string& parent);

void disconnect();

// Schermata completa di gestione della rete (Impostazioni > Rete WiFi).
void runSettings(const std::string& parent);

}  // namespace wifisetup
