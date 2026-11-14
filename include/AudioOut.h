#pragma once

#include <Audio.h>

// Gestore condiviso del decoder ESP32-audioI2S e del bus I2S dello speaker.
//
// Radio e lettore MP3 devono usare UNA sola istanza di Audio: due oggetti
// installerebbero lo stesso driver I2S. Qui vivono anche il rilascio/ripresa
// dello speaker M5Unified (durante la riproduzione il decoder possiede il bus)
// e il volume condiviso tra i due moduli.
namespace audioout {

// Decoder condiviso, creato alla prima richiesta.
Audio& decoder();

// Volume nella scala del decoder (0..21), persistente per la sessione.
int volume();
void setVolume(int value);

// Prepara la riproduzione: chiude lo speaker, configura i pin I2S dal
// M5Unified della board e applica il volume corrente. Ritorna false se il bus
// non e' stato liberato.
bool beginPlayback();

// Ferma la traccia e restituisce l'I2S a M5Unified (beep della UI).
void endPlayback();

// Va chiamato spesso durante la riproduzione: pompa lo stream del decoder.
void pump();

}  // namespace audioout
