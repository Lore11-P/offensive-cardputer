#pragma once

#include <string>

// Aggiornamento firmware via Wi-Fi (OTA) dal repository GitHub del progetto.
//
// Scarica flasher/bin/firmware.bin dal ramo main del repository e lo scrive
// nella partizione applicativa inattiva tramite Update.h. Richiede la tabella
// partizioni a doppio slot (app0/app1 in partitions.csv): i dispositivi con il
// layout precedente devono fare un flash USB una volta sola.
namespace ota {

// URL del firmware pubblicato sul repository del progetto.
extern const char* const kFirmwareUrl;

// Schermata "Aggiorna firmware": verifica la rete, chiede conferma, scarica il
// firmware e riavvia. Mostra l'avanzamento e permette di annullare con `.
void run(const std::string& parent);

}  // namespace ota
