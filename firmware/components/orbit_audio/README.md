# orbit_audio

Audio output, local music player and media menu for OrbitOS 3.0 on the M5Stack
Cardputer ADV (ESP32-S3FN8, no PSRAM). ESP-IDF 5.4, C++17, no Arduino and no
M5Unified.

## Status

| Feature | State |
| --- | --- |
| I2S output + ES8311 codec | Implemented |
| WAV (PCM16 mono/stereo) | Implemented, any rate 8–96 kHz |
| MP3 via minimp3 | Implemented, streaming from `FILE*` |
| Volume 0..21 | Implemented (ES8311 DAC register) |
| Album/track browser + player screen | Implemented |
| Optional `cover.cvr` | Implemented (64x64 RGB565, 32x32 fallback) |
| Web Radio | **Not implemented** (deliberately omitted; the hub has no Radio entry) |
| FLAC / M4A / AAC / OGG / Opus | Listed but not decoded; the UI explains the limit |

## API

Header: `orbit_audio/Audio.h`

```cpp
bool begin();                                   // idempotent I2S + codec init
void end();                                     // stop I2S, power down codec
bool playFile(const std::string& path, volatile bool* stopFlag);
void beep(uint16_t hz, uint16_t ms);
int  volume();                                  // 0..21
void setVolume(int value);
void runMenu();                                 // "Audio" hub -> "Musica"

// used by the built-in player
void setPaused(bool paused);
bool paused();
bool active();                                  // first PCM block written
uint32_t positionMs();
uint32_t durationMs();
const char* lastError();
```

`playFile()` blocks until the end of the file, an error, or `*stopFlag == true`.
It returns `true` only on a natural end. It allocates and frees all decoder
buffers internally; call `end()` when leaving the audio module so I2S and the
codec are released before Wren games start.

Only one playback may run at a time. `begin()`/`end()` are not reentrant; the
built-in UI runs `playFile()` in a dedicated FreeRTOS task. A direct caller
must also provide a task stack of at least 24 KB because minimp3 keeps its
~16 KB scratch buffer on the stack (the built-in player uses 32 KB). Do not call
`playFile()` directly from the 16 KB main task.

## Hardware / pinout

| Signal | GPIO |
| --- | --- |
| I2S BCK | 41 |
| I2S WS | 43 |
| I2S DOUT | 42 |
| ES8311 I2C address | 0x18 on the shared internal bus (SDA 8, SCL 9) |

- I2S port: `I2S_NUM_1`, master, 16-bit stereo, 44.1 kHz at boot.
- The codec has no MCLK wire: it derives MCLK from BCLK (`REG01 = 0xB5`).
- The I2C bus is created by `orbit_hal::keyboardBegin()`. This component never
  installs a second driver; it uses `orbit_hal::i2cPresent`, `i2cWrite`,
  `i2cWriteReg` and `i2cReadReg`.
- There is no amplifier enable GPIO on this board; the M5Unified Cardputer ADV
  speaker callback only writes the codec, and this component mirrors it.

### ES8311 sequence used

Full reset, then the M5Unified Cardputer ADV speaker values plus the standard
clock/format registers for a 16-bit I2S slave with MCLK = BCLK:

```
00=1F (reset, 20 ms) 00=00 00=80
01=B5  02=18  03=10  04=10  05=00  06=03  07=00  08=FF
09=0C  0A=0C
0D=01  0E=02  12=00  13=10  1C=6A
32=BF  37=08
```

`0x02..0x08` are the esp-bsp coefficients for MCLK/fs = 32, which is what the
I2S master produces for 16-bit stereo (BCLK = 32 x fs). The DAC volume is then
set through register `0x32`:

- volume 0 -> `0x00` (mute)
- volume 1..21 -> `0x40` (-63.5 dB) .. `0xBF` (0 dB), 0.5 dB steps

If a future board revision has no sound, the first thing to try is `01=BF`
(enable all internal clocks, as in esp-bsp) instead of the M5Unified `B5`.

## Library layout

The browser scans the first non-empty root in this order:

1. `/sd/data/Music`
2. `/sd/music`
3. `/sd/Music`
4. `/littlefs/data/Music`

`orbit_store::mountSd()` mounts the card at `/sd`; `orbit_store::mountLittleFs()`
mounts `/littlefs` (note: the store's LittleFS configuration formats the
partition if the mount fails). Each subdirectory is an album; loose audio files
in the root become "Senza album". The optional album cover is
`<album>/cover.cvr`, exactly 64x64 RGB565 little-endian (8192 bytes). It is kept
in RAM as RGB332 (4 KB, 2 KB at 32x32 if the full allocation fails).

Extensions listed: `.mp3 .wav .flac .m4a .aac .ogg .opus`. Only `.mp3` and
`.wav` are decoded; the others open a "Formato non supportato" message.

## Player keys

| Key | Action |
| --- | --- |
| ENTER | play / pause |
| `,` | previous track |
| `/` | next track |
| `;` | volume up (auto-repeat) |
| `.` | volume down (auto-repeat) |
| `` ` `` | exit (stops within one I2S chunk, well under 200 ms) |

The duration of an MP3 is estimated from the first frame's bitrate and the file
size (CBR estimate; VBR may be slightly off). Elapsed time is derived from the
decoded sample count and is accurate. WAV duration is exact.

## Memory

- MP3: 8 KB input, 4.6 KB PCM, 4 KB resampler (4 KB only when the rate differs),
  ~6.6 KB minimp3 state, all heap allocated and freed per file.
- WAV: 4 KB read buffer + 4 KB mono expansion + 4 KB resampler, per file.
- Playback task stack: 32 KB (minimp3's `mp3dec_scratch_t` is ~16 KB on the
  stack; ESP-IDF `xTaskCreate` stack sizes are in bytes).
- Cover: 4 KB (or 2 KB fallback).
- All of the above are released when the player exits (and `end()` releases the
  I2S DMA buffers), leaving the heap to Wren games.

## Sample-rate handling

The I2S clock and the ES8311 stay at the boot configuration (44.1 kHz, 16-bit
stereo) for every file. Sources at 44.1 kHz are written directly; rates from
8 kHz to 96 kHz are linearly resampled to 44.1 kHz. This avoids stopping and
restarting the codec clock between tracks. Resampling is simple linear
interpolation, not a windowed sinc; acceptable for the Cardputer speaker, not
audiophile-grade.

## Known limits / not done

- Web Radio is not implemented. No `runRadio()` symbol is exposed. A future
  version can reuse the MP3 decoder if it is refactored to read from a generic
  byte source instead of `FILE*`; the current MP3 loop is `FILE*`-specific.
- No seek, no shuffle/repeat modes, no ID3 title/artist display.
- MP3 duration is a CBR estimate; VBR files may show a slightly wrong total.
- No FLAC/AAC/OGG/Opus decoder.
- Album/track caps: 40 albums, 150 tracks per album, 256 directory entries.
- The display font is ASCII. File/album names are transliterated (common
  accented Latin letters) or replaced with `?`.
- Only one audio stream at a time; `begin`/`end`/`playFile` are not thread-safe
  beyond the built-in single player task.

## Integration

1. The component lives under `firmware/components/orbit_audio`. A CMake
   reconfigure (`idf.py reconfigure`) or a full build discovers it. It is
   already referenced by `orbit_app` (REQUIRES) and `Home.cpp` ("Media" item);
   `main/selftest.cpp` uses `begin`/`beep`/`end`.
2. Add `orbit_audio` to the `REQUIRES` of any other component that calls it
   (for example `orbit_app` if the Audio item is wired into `Home.cpp`).
3. Add the menu entry, e.g. `menu.addItem("Audio", "music", [] { orbit_audio::runMenu(); });`
   and `#include "orbit_audio/Audio.h"`.
4. No `sdkconfig` change is required. `CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384`
   in the current project is enough for the menus; the decoder uses its own
   32 KB task.
5. Component dependencies are declared in `CMakeLists.txt`:
   `orbit_ui orbit_hal orbit_store esp_driver_i2s driver esp_http_client esp-tls
   mbedtls fatfs`. The HTTP/TLS entries are reserved for the future radio and
   are already linked by `orbit_store`; removing them is safe.

## Third-party

`third_party/minimp3.h` and `minimp3_ex.h` are from
https://github.com/lieff/minimp3 (CC0 / public domain). Only `minimp3.h` is
compiled (via `minimp3_impl.c`); `minimp3_ex.h` is vendored but unused.
