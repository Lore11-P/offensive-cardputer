#pragma once

#include <cstdint>

// Bussola a heading relativo basata sul giroscopio (BMI270).
//
// La Cardputer ADV non ha magnetometro: non esiste un nord assoluto. Si integra
// la velocita' angolare sull'asse Z, quindi l'angolo e' relativo alla posa
// assunta all'avvio di begin() (o all'ultimo reset()). E' esattamente cio' che
// serve per una ricerca direzionale: "di quanti gradi ho ruotato da quando ho
// sentito il segnale piu' forte".
//
// Il bias del giroscopio viene misurato all'inizio (device fermo) e sottratto;
// una deadband elimina la deriva residua da fermo.
namespace compass {

// Inizializza l'IMU e misura il bias. False se l'IMU non risponde.
bool begin();

// Da chiamare spesso (>= 20 Hz): campiona il giroscopio e integra lo yaw.
void update();

// Heading in gradi, 0..360. Cresce ruotando in senso orario visto dall'alto.
float heading();

// Azzera l'angolo (utile per ripartire da una nuova posa di riferimento).
void reset();

bool available();

}  // namespace compass
