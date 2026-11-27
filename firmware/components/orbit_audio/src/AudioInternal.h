#pragma once

// Internal bridge between Audio.cpp (I2S/codec) and Playback.cpp (WAV/MP3).

#include <cstddef>
#include <cstdint>

namespace orbit_audio {
namespace internal {

// Reconfigures the I2S clock while the channel is stopped. Only 16000..48000
// are accepted; 44100 is the boot rate.
bool setOutputRate(uint32_t rate);

// Blocking write of interleaved stereo samples. Returns false on stop request
// or I2S failure.
bool writeStereo(const int16_t* samples, size_t count, volatile bool* stopFlag);

void setPositionMs(uint32_t ms);
void setDurationMs(uint32_t ms);
void markPlaying();
void setError(const char* text);

// Playback lifecycle: busy covers file open + decode, playing starts with the
// first PCM block (the UI shows "apertura..." until then).
void resetPlayback();
void finishPlayback();

bool stopRequested(volatile bool* stopFlag);

}  // namespace internal
}  // namespace orbit_audio
