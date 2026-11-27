#pragma once

// OrbitOS 3.0 audio: ES8311 codec + I2S output, WAV/MP3 playback and the
// Musica hub. The codec shares the internal I2C bus initialized by
// orbit_hal::keyboardBegin(); this component never installs a second bus.
//
// Threading: begin(), end() and playFile() are not reentrant. The built-in UI
// runs playFile() in a worker task; a caller that invokes it directly gets a
// blocking call (stopFlag is polled while decoding).

#include <cstdint>
#include <string>

namespace orbit_audio {

// Initializes I2S_NUM_1 (44.1 kHz, stereo, 16 bit) and the ES8311 at 0x18.
// Idempotent: repeated calls return true. Requires keyboardBegin() at boot.
bool begin();

// Stops the channel and powers down the codec. Call only when no playback is
// running. Idempotent.
void end();

// Plays a .wav (PCM16 mono/stereo) or .mp3 file until the end, an error or
// *stopFlag == true. Returns true only when the stream reached its end.
// Free all decoder memory before returning.
// minimp3 keeps ~16 KB of scratch on the calling task's stack: call it from a
// task with at least 24 KB of stack (the built-in player uses 32 KB), or use
// runMenu(). Do not call playFile() directly from the 16 KB main task.
bool playFile(const std::string& path, volatile bool* stopFlag);

// Short UI tone. No-op while a file is playing or if audio is unavailable.
void beep(uint16_t hz, uint16_t ms);

// 0..21 (legacy scale), 0 = mute.
int volume();
void setVolume(int value);

// Media hub menu ("Musica"; Radio is not implemented in this component).
void runMenu();

// Extras used by the built-in player. Safe to poll from the UI task.
void setPaused(bool paused);
bool paused();
bool active();
uint32_t positionMs();
uint32_t durationMs();
const char* lastError();

}  // namespace orbit_audio
