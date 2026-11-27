#pragma once

#include <Audio.h>
#include <stdint.h>

// Gestore condiviso del decoder ESP32-audioI2S e del bus I2S dello speaker.
//
// Radio e lettore MP3 devono usare UNA sola istanza di Audio: due oggetti
// installerebbero lo stesso driver I2S. Qui vivono anche il rilascio/ripresa
// dello speaker M5Unified (durante la riproduzione il decoder possiede il bus)
// e il volume condiviso tra i due moduli.
namespace audioout {

// Decoder condiviso, creato alla prima richiesta. Da usare solo dopo un
// beginPlayback() riuscito (o dopo decoderReady()): non restituisce un
// riferimento valido se l'allocazione e' fallita.
Audio& decoder();

// Alloca il decoder se non esiste, con new (std::nothrow). false se la heap
// non basta: in quel caso i chiamanti devono rinunciare alla riproduzione.
bool decoderReady();

// Stato del decoder senza costruirlo: usare questa funzione nelle schermate
// informative per non installare I2S0 (~40 KB) solo per leggere lo stato.
bool decoderRunning();

// Volume nella scala del decoder (0..21), persistente per la sessione.
int volume();
void setVolume(int value);

// Prepara la riproduzione: chiude lo speaker, configura i pin I2S dal
// M5Unified della board e applica il volume corrente. Reinstalla anche il
// driver I2S0 se il microfono o un altro modulo lo hanno fermato. Ritorna
// false se il bus non e' pronto.
bool beginPlayback();

// Percorso 1.21 per il solo lettore Musica: non tocca il microfono e non
// cambia la configurazione del decoder nel caso normale. Se Radio ha
// rilasciato I2S0, lo reinstalla; all'uscita lo rilascia per Audio/microfono.
bool beginPlaybackLegacy();

// Ferma la traccia, rilascia I2S0 (per Audio/microfono) e restituisce lo
// speaker alla UI.
void endPlaybackLegacy();

// Reinstalla il driver I2S0 del decoder (config identica a quella della
// libreria). Utile dopo l'uso di M5.Mic, che condivide I2S0.
void reinit();

// Ferma la traccia e restituisce l'I2S a M5Unified (beep della UI). Se una
// registrazione e' ancora aperta la chiude prima di rilasciare il bus.
void endPlayback();

// Distrugge il decoder (~40 KB con i suoi buffer) e libera I2S0: serve a
// restituire memoria a un'altra funzione (es. prima di un gioco Wren). La
// prossima beginPlayback() lo ricrea da zero.
void releaseDecoder();

// Va chiamato spesso durante la riproduzione: pompa lo stream del decoder e
// svuota il ring buffer della registrazione verso la SD.
void pump();

// --- Metadati ICY ------------------------------------------------------------
// I callback weak audio_showstation/audio_showstreamtitle della libreria
// aggiornano questi buffer. Stringhe vuote se lo stream non li fornisce.
const char* streamStation();
const char* streamTitle();
void clearMetadata();

// --- Registrazione WAV (PCM decodificato, 16 bit stereo) --------------------
// Il callback audio_process_extern copia il PCM in un ring buffer da 16 KB;
// pump() lo scrive sulla SD, quindi il task audio non tocca mai il filesystem.
// L'intestazione WAV viene completata alla chiusura con il sample rate reale.
bool startRecording(const char* path);
uint32_t stopRecording();  // byte PCM scritti; 0 se non si e' mai registrato
bool recording();
uint32_t recordingBytes();       // PCM finora (file + ring non ancora scritto)
uint32_t recordingSampleRate();
uint32_t recordingDropped();     // campioni scartati per ring pieno
bool recordingError();           // scrittura SD fallita durante la cattura

}  // namespace audioout
