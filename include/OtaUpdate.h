#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Aggiornamento firmware via Wi-Fi (OTA) dal repository GitHub del progetto.
//
// Scarica flasher/bin/firmware.json (versione, dimensione, SHA-256), poi
// flasher/bin/firmware.bin e firmware.sig. Rifiuta i downgrade rispetto a
// fw::kVersion, verifica SHA-256 e firma ECDSA P-256 prima di spostare il boot
// sulla nuova partizione. Richiede la tabella partizioni a doppio slot
// (app0/app1 in partitions.csv): i dispositivi con il layout precedente devono
// fare un flash USB una volta sola.
namespace ota {

// URL del firmware pubblicato sul repository del progetto.
extern const char* const kFirmwareUrl;

// Helper TLS condivisi con lo store giochi: CA pinnata del progetto,
// allowlist GitHub e sincronizzazione NTP (necessaria per la validita' del
// certificato sui device senza orologio).
const char* rootCaPem();
bool githubUrlAllowed(const std::string& url);
bool ensureClock(const std::string& parent);

// Verifica una firma ECDSA P-256/SHA-256 (64 byte r||s) su un blob arbitrario.
// Usata dallo store per autenticare il catalogo con la stessa chiave offline
// dell'OTA.
bool verifyBlob(const uint8_t* data, size_t len, const uint8_t* signature,
                size_t signatureLen);

// Schermata "Aggiorna firmware": verifica la rete, chiede conferma, scarica il
// firmware e riavvia. Mostra l'avanzamento e permette di annullare con `.
void run(const std::string& parent);

}  // namespace ota
