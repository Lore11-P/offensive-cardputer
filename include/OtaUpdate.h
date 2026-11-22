#pragma once

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

// Schermata "Aggiorna firmware": verifica la rete, chiede conferma, scarica il
// firmware e riavvia. Mostra l'avanzamento e permette di annullare con `.
void run(const std::string& parent);

}  // namespace ota
