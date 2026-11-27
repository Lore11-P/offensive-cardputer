#include "orbit_audio/Audio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "AudioInternal.h"
#include "minimp3.h"

// Streaming decoders. Buffers are heap allocated for the duration of one file
// (MP3: 8 KB input + 4.6 KB PCM + 4 KB resampler; WAV: 4 KB + 4 KB + 4 KB) and
// released before returning, so Wren gets the heap back after the player exits.
namespace orbit_audio {
namespace {

constexpr const char* kTag = "orbit_audio";
constexpr size_t kMp3InputBytes = 8192;
constexpr size_t kWavChunkFrames = 1024;
constexpr size_t kResampleSliceFrames = 128;
constexpr size_t kResampleOutFrames = 1024;

std::string lowerExtension(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return "";
    }
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return ext;
}

long fileSizeOf(FILE* file) {
    if (file == nullptr) {
        return -1;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        return -1;
    }
    const long size = std::ftell(file);
    if (std::fseek(file, 0, SEEK_SET) != 0) {
        return -1;
    }
    return size;
}

// Linear interpolation. State survives chunk boundaries; the tail shorter than
// one output frame is dropped at end of file.
class Resampler {
public:
    void configure(uint32_t sourceRate, uint32_t outputRate) {
        step_ = static_cast<float>(sourceRate) / static_cast<float>(outputRate);
        active_ = sourceRate != outputRate;
        pos_ = 0.0f;
        haveLast_ = false;
        lastL_ = 0;
        lastR_ = 0;
    }

    bool active() const {
        return active_;
    }

    size_t process(const int16_t* in, size_t frames, int16_t* out, size_t capacity) {
        if (!active_ || frames == 0 || capacity == 0) {
            return 0;
        }
        size_t produced = 0;
        float pos = pos_;
        const float limit = static_cast<float>(frames - 1);
        while (produced < capacity && pos <= limit) {
            const int index = static_cast<int>(floorf(pos));
            const float frac = pos - static_cast<float>(index);
            int16_t l;
            int16_t r;
            if (index < 0) {
                const int16_t pl = haveLast_ ? lastL_ : in[0];
                const int16_t pr = haveLast_ ? lastR_ : in[1];
                l = static_cast<int16_t>(static_cast<float>(pl) +
                                         (static_cast<float>(in[0]) - pl) * frac);
                r = static_cast<int16_t>(static_cast<float>(pr) +
                                         (static_cast<float>(in[1]) - pr) * frac);
            } else if (static_cast<size_t>(index + 1) < frames) {
                const int16_t* a = in + index * 2;
                l = static_cast<int16_t>(static_cast<float>(a[0]) +
                                         (static_cast<float>(a[2]) - a[0]) * frac);
                r = static_cast<int16_t>(static_cast<float>(a[1]) +
                                         (static_cast<float>(a[3]) - a[1]) * frac);
            } else {
                l = in[index * 2];
                r = in[index * 2 + 1];
            }
            out[produced * 2] = l;
            out[produced * 2 + 1] = r;
            ++produced;
            pos += step_;
        }
        pos_ = pos - static_cast<float>(frames);
        lastL_ = in[(frames - 1) * 2];
        lastR_ = in[(frames - 1) * 2 + 1];
        haveLast_ = true;
        return produced;
    }

private:
    float step_ = 1.0f;
    float pos_ = 0.0f;
    bool active_ = false;
    bool haveLast_ = false;
    int16_t lastL_ = 0;
    int16_t lastR_ = 0;
};

class PlaybackWriter {
public:
    PlaybackWriter() = default;
    PlaybackWriter(const PlaybackWriter&) = delete;
    PlaybackWriter& operator=(const PlaybackWriter&) = delete;
    ~PlaybackWriter() {
        delete[] out_;
    }

    bool setup(uint32_t sourceRate, volatile bool* stopFlag) {
        stop_ = stopFlag;
        if (sourceRate < 8000 || sourceRate > 96000) {
            internal::setError("Sample rate non supportato");
            return false;
        }
        // Keep the I2S/codec at the boot rate for every file: the ES8311 clock
        // registers are written once, and the tiny speaker does not benefit
        // from native 32/48 kHz. Everything except 44.1 kHz is resampled.
        constexpr uint32_t kOutputRate = 44100;
        if (!internal::setOutputRate(kOutputRate)) {
            internal::setError("I2S: sample rate non impostabile");
            return false;
        }
        resampler_.configure(sourceRate, kOutputRate);
        if (resampler_.active()) {
            out_ = new (std::nothrow) int16_t[kResampleOutFrames * 2];
            if (out_ == nullptr) {
                internal::setError("Memoria insufficiente (resampler)");
                return false;
            }
        }
        ESP_LOGI(kTag, "rate sorgente %u -> uscita %u%s", static_cast<unsigned>(sourceRate),
                 static_cast<unsigned>(kOutputRate),
                 resampler_.active() ? " (resample)" : "");
        return true;
    }

    bool write(const int16_t* stereo, size_t frames) {
        if (frames == 0) {
            return true;
        }
        // Pause holds the current decoded block; stop still breaks the wait.
        while (paused()) {
            if (internal::stopRequested(stop_)) {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (internal::stopRequested(stop_)) {
            return false;
        }
        if (!resampler_.active()) {
            return internal::writeStereo(stereo, frames * 2, stop_);
        }
        size_t done = 0;
        while (done < frames) {
            const size_t slice = std::min(frames - done, kResampleSliceFrames);
            const size_t produced =
                resampler_.process(stereo + done * 2, slice, out_, kResampleOutFrames);
            if (produced > 0 && !internal::writeStereo(out_, produced * 2, stop_)) {
                return false;
            }
            done += slice;
            if (internal::stopRequested(stop_)) {
                return false;
            }
        }
        return true;
    }

private:
    Resampler resampler_;
    int16_t* out_ = nullptr;
    volatile bool* stop_ = nullptr;
};

uint16_t read16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t read32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

struct WavInfo {
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint32_t rate = 0;
    uint32_t dataOffset = 0;
    uint32_t dataSize = 0;
};

bool parseWav(FILE* file, long fileSize, WavInfo* info) {
    uint8_t header[12];
    if (std::fread(header, 1, sizeof(header), file) != sizeof(header) ||
        std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVE", 4) != 0) {
        internal::setError("WAV: intestazione RIFF non valida");
        return false;
    }

    bool haveFormat = false;
    for (int chunk = 0; chunk < 32; ++chunk) {
        uint8_t head[8];
        if (std::fread(head, 1, sizeof(head), file) != sizeof(head)) {
            break;
        }
        const uint32_t size = read32(head + 4);
        const long payload = std::ftell(file);
        if (payload < 0) {
            break;
        }
        const long next = payload + static_cast<long>(size) + static_cast<long>(size & 1);

        if (std::memcmp(head, "fmt ", 4) == 0) {
            uint8_t fmt[40] = {0};
            const size_t wanted = std::min<size_t>(size, sizeof(fmt));
            if (wanted < 16 || std::fread(fmt, 1, wanted, file) != wanted) {
                internal::setError("WAV: blocco fmt troncato");
                return false;
            }
            uint16_t format = read16(fmt);
            if (format == 0xFFFE) {  // WAVE_FORMAT_EXTENSIBLE
                if (wanted < 40 || read16(fmt + 24) != 1) {
                    internal::setError("WAV: formato esteso non PCM");
                    return false;
                }
                format = 1;
            }
            info->channels = read16(fmt + 2);
            info->rate = read32(fmt + 4);
            info->bits = read16(fmt + 14);
            const uint16_t blockAlign = read16(fmt + 12);
            if (format != 1) {
                internal::setError("WAV: solo PCM non compresso");
                return false;
            }
            if (info->channels < 1 || info->channels > 2 || info->bits != 16 ||
                blockAlign != info->channels * 2) {
                internal::setError("WAV: richiesto PCM16 mono/stereo");
                return false;
            }
            if (info->rate < 8000 || info->rate > 96000) {
                internal::setError("WAV: sample rate fuori intervallo");
                return false;
            }
            haveFormat = true;
        } else if (std::memcmp(head, "data", 4) == 0) {
            if (!haveFormat) {
                internal::setError("WAV: data prima di fmt");
                return false;
            }
            info->dataOffset = static_cast<uint32_t>(payload);
            const long remain = fileSize - payload;
            if (remain <= 0) {
                internal::setError("WAV: nessun campione");
                return false;
            }
            const uint32_t available = static_cast<uint32_t>(remain);
            info->dataSize = (size == 0 || size > available) ? available : size;
            if (info->dataSize == 0) {
                internal::setError("WAV: dati vuoti");
                return false;
            }
            return true;
        }

        if (next <= payload || std::fseek(file, next, SEEK_SET) != 0) {
            break;
        }
    }
    internal::setError("WAV: blocco fmt/data assente");
    return false;
}

bool playWav(FILE* file, long fileSize, volatile bool* stopFlag) {
    WavInfo info;
    if (!parseWav(file, fileSize, &info)) {
        return false;
    }

    PlaybackWriter writer;
    if (!writer.setup(info.rate, stopFlag)) {
        return false;
    }

    const uint64_t bytesPerSecond =
        static_cast<uint64_t>(info.rate) * info.channels * sizeof(int16_t);
    uint32_t durationMs = 0;
    if (bytesPerSecond > 0) {
        durationMs = static_cast<uint32_t>(
            std::min<uint64_t>(0xFFFFFFFFULL, static_cast<uint64_t>(info.dataSize) * 1000ULL /
                                                   bytesPerSecond));
    }
    internal::setDurationMs(durationMs);
    internal::setPositionMs(0);

    if (std::fseek(file, static_cast<long>(info.dataOffset), SEEK_SET) != 0) {
        internal::setError("WAV: seek ai dati fallito");
        return false;
    }

    std::unique_ptr<int16_t[]> readBuf(new (std::nothrow) int16_t[kWavChunkFrames * 2]);
    std::unique_ptr<int16_t[]> stereoBuf;
    if (info.channels == 1) {
        stereoBuf.reset(new (std::nothrow) int16_t[kWavChunkFrames * 2]);
    }
    if (readBuf == nullptr || (info.channels == 1 && stereoBuf == nullptr)) {
        internal::setError("Memoria insufficiente (WAV)");
        return false;
    }

    const size_t frameBytes = static_cast<size_t>(info.channels) * sizeof(int16_t);
    uint32_t remaining = info.dataSize;
    uint64_t framesDone = 0;
    bool ok = true;
    internal::markPlaying();

    while (remaining >= frameBytes) {
        if (internal::stopRequested(stopFlag)) {
            ok = false;
            break;
        }
        size_t frames = std::min<size_t>(kWavChunkFrames, remaining / frameBytes);
        const size_t got = std::fread(readBuf.get(), frameBytes, frames, file);
        if (got == 0) {
            internal::setError("WAV: lettura troncata");
            ok = false;
            break;
        }
        const int16_t* source = readBuf.get();
        if (info.channels == 1) {
            int16_t* stereo = stereoBuf.get();
            for (size_t i = 0; i < got; ++i) {
                const int16_t sample = readBuf[i];
                stereo[i * 2] = sample;
                stereo[i * 2 + 1] = sample;
            }
            source = stereo;
        }
        if (!writer.write(source, got)) {
            ok = false;
            break;
        }
        framesDone += got;
        remaining -= static_cast<uint32_t>(got * frameBytes);
        internal::setPositionMs(static_cast<uint32_t>(
            std::min<uint64_t>(0xFFFFFFFFULL, framesDone * 1000ULL / info.rate)));
    }

    if (internal::stopRequested(stopFlag)) {
        ok = false;
    }
    return ok;
}

bool playMp3(FILE* file, long fileSize, volatile bool* stopFlag) {
    if (fileSize <= 0) {
        internal::setError("MP3 vuoto");
        return false;
    }

    long dataStart = 0;
    long dataEnd = fileSize;

    // Skip ID3v2 (syncsafe size, optional footer).
    uint8_t id3[10] = {0};
    if (std::fread(id3, 1, sizeof(id3), file) == sizeof(id3) &&
        std::memcmp(id3, "ID3", 3) == 0) {
        const uint32_t tagSize = (static_cast<uint32_t>(id3[6] & 0x7F) << 21) |
                                 (static_cast<uint32_t>(id3[7] & 0x7F) << 14) |
                                 (static_cast<uint32_t>(id3[8] & 0x7F) << 7) |
                                 static_cast<uint32_t>(id3[9] & 0x7F);
        long skip = static_cast<long>(tagSize) + 10;
        if ((id3[5] & 0x10) != 0) {
            skip += 10;
        }
        if (skip > 0 && skip < fileSize && std::fseek(file, skip, SEEK_SET) == 0) {
            dataStart = skip;
        }
    }

    // Ignore ID3v1 so the decoder does not try to parse the trailing tag.
    if (dataEnd - dataStart > 128) {
        if (std::fseek(file, dataEnd - 128, SEEK_SET) == 0) {
            char tag[3];
            if (std::fread(tag, 1, sizeof(tag), file) == sizeof(tag) &&
                std::memcmp(tag, "TAG", 3) == 0) {
                dataEnd -= 128;
            }
        }
    }
    if (std::fseek(file, dataStart, SEEK_SET) != 0) {
        internal::setError("MP3: seek fallito");
        return false;
    }

    std::unique_ptr<uint8_t[]> input(new (std::nothrow) uint8_t[kMp3InputBytes]);
    std::unique_ptr<int16_t[]> pcm(
        new (std::nothrow) int16_t[MINIMP3_MAX_SAMPLES_PER_FRAME]);
    std::unique_ptr<mp3dec_t> decoder(new (std::nothrow) mp3dec_t);
    if (input == nullptr || pcm == nullptr || decoder == nullptr) {
        internal::setError("Memoria insufficiente (MP3)");
        return false;
    }
    mp3dec_init(decoder.get());

    size_t available = 0;
    size_t pos = 0;
    bool eof = false;
    auto refill = [&]() -> bool {
        if (pos > 0) {
            std::memmove(input.get(), input.get() + pos, available - pos);
            available -= pos;
            pos = 0;
        }
        while (available < kMp3InputBytes && !eof) {
            const long here = std::ftell(file);
            if (here < 0) {
                eof = true;
                break;
            }
            const long remain = dataEnd - here;
            if (remain <= 0) {
                eof = true;
                break;
            }
            size_t want = kMp3InputBytes - available;
            if (static_cast<long>(want) > remain) {
                want = static_cast<size_t>(remain);
            }
            const size_t got = std::fread(input.get() + available, 1, want, file);
            if (got == 0) {
                eof = true;
                break;
            }
            available += got;
        }
        return available > pos;
    };

    if (!refill()) {
        internal::setError("MP3: nessun dato dopo i tag");
        return false;
    }

    PlaybackWriter writer;
    bool started = false;
    bool ok = true;
    uint32_t sourceRate = 0;
    uint64_t positionAccum = 0;

    for (;;) {
        if (internal::stopRequested(stopFlag)) {
            ok = false;
            break;
        }
        if (available - pos < 4096 && !eof) {
            refill();
        }
        if (available == pos) {
            break;
        }

        mp3dec_frame_info_t info = {};
        const int samples = mp3dec_decode_frame(decoder.get(), input.get() + pos,
                                                static_cast<int>(available - pos),
                                                pcm.get(), &info);
        if (samples > 0) {
            pos += static_cast<size_t>(info.frame_bytes);
            if (!started) {
                sourceRate = static_cast<uint32_t>(info.hz);
                if (sourceRate < 8000 || sourceRate > 96000) {
                    internal::setError("MP3: sample rate non valido");
                    ok = false;
                    break;
                }
                if (!writer.setup(sourceRate, stopFlag)) {
                    ok = false;
                    break;
                }
                const uint32_t bitrate = info.bitrate_kbps > 0
                                             ? static_cast<uint32_t>(info.bitrate_kbps)
                                             : 128U;
                const uint64_t bytes =
                    static_cast<uint64_t>(dataEnd > dataStart ? dataEnd - dataStart : 0);
                internal::setDurationMs(static_cast<uint32_t>(std::min<uint64_t>(
                    0xFFFFFFFFULL, bytes * 8ULL / bitrate)));
                internal::markPlaying();
                started = true;
            }

            if (info.channels == 1) {
                // Expand mono to stereo backwards, in place: destination 2i/2i+1
                // is always ahead of the source sample that has not been read.
                int16_t* samplesOut = pcm.get();
                for (int i = samples - 1; i >= 0; --i) {
                    const int16_t sample = samplesOut[i];
                    samplesOut[i * 2] = sample;
                    samplesOut[i * 2 + 1] = sample;
                }
            }
            if (!writer.write(pcm.get(), static_cast<size_t>(samples))) {
                ok = false;
                break;
            }
            positionAccum += static_cast<uint64_t>(samples) * 1000ULL;
            internal::setPositionMs(static_cast<uint32_t>(
                std::min<uint64_t>(0xFFFFFFFFULL, positionAccum / sourceRate)));
        } else if (info.frame_bytes > 0) {
            pos += static_cast<size_t>(info.frame_bytes);
        } else {
            if (eof) {
                break;
            }
            if (available - pos >= kMp3InputBytes - 1) {
                ++pos;  // no sync in a full buffer: drop one byte and retry
            } else {
                refill();
            }
        }
    }

    if (!started) {
        internal::setError("MP3: nessun frame valido");
        return false;
    }
    if (internal::stopRequested(stopFlag)) {
        ok = false;
    }
    return ok;
}

}  // namespace

bool playFile(const std::string& path, volatile bool* stopFlag) {
    struct Guard {
        Guard() {
            internal::resetPlayback();
        }
        ~Guard() {
            internal::finishPlayback();
        }
    } guard;
    (void)guard;

    if (path.empty()) {
        internal::setError("Percorso vuoto");
        return false;
    }
    if (!begin()) {
        return false;
    }

    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        internal::setError("File non apribile");
        return false;
    }
    const long fileSize = fileSizeOf(file);
    const std::string extension = lowerExtension(path);
    bool ok = false;
    if (extension == "mp3") {
        ok = playMp3(file, fileSize, stopFlag);
    } else if (extension == "wav") {
        ok = playWav(file, fileSize, stopFlag);
    } else {
        internal::setError("Formato non supportato (solo MP3/WAV)");
    }
    std::fclose(file);

    if (internal::stopRequested(stopFlag)) {
        internal::setError("Interrotto");
        return false;
    }
    return ok;
}

}  // namespace orbit_audio
