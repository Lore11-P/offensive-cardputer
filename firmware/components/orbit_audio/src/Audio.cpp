#include "orbit_audio/Audio.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include <driver/i2s_std.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "AudioInternal.h"
#include "orbit_hal/keyboard.h"

// Cardputer ADV internal speaker:
//   BCK GPIO41, WS GPIO43, DOUT GPIO42, ES8311 at I2C 0x18 (shared bus).
//   MCLK is not wired: the codec derives it from BCLK (reg 0x01 = 0xB5).
namespace orbit_audio {
namespace {

constexpr const char* kTag = "orbit_audio";
constexpr uint8_t kCodecAddress = 0x18;
constexpr int kPinBck = 41;
constexpr int kPinWs = 43;
constexpr int kPinDout = 42;
constexpr uint32_t kBootRate = 44100;
constexpr int kVolumeMax = 21;

i2s_chan_handle_t s_tx = nullptr;
uint32_t s_rate = kBootRate;
int s_volume = 12;
bool s_codecReady = false;

volatile bool s_pause = false;
volatile bool s_busy = false;
volatile bool s_playing = false;
volatile uint32_t s_positionMs = 0;
volatile uint32_t s_durationMs = 0;

char s_error[96] = {0};

bool codecWrite(uint8_t reg, uint8_t value) {
    return orbit_hal::i2cWriteReg(kCodecAddress, reg, value);
}

// 0..21 mapped to the ES8311 DAC register: 0x40 (-63.5 dB) .. 0xBF (0 dB),
// 0.5 dB per step. 0 mutes. Writing 0xBF directly would be full 0 dB.
uint8_t volumeRegister(int value) {
    if (value <= 0) {
        return 0x00;
    }
    if (value >= kVolumeMax) {
        return 0xBF;
    }
    const int span = 0xBF - 0x40;
    return static_cast<uint8_t>(0x40 + (value - 1) * span / (kVolumeMax - 1));
}

bool initI2s() {
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan.dma_desc_num = 8;
    chan.dma_frame_num = 256;
    chan.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan, &s_tx, nullptr);
    if (err != ESP_OK) {
        internal::setError("I2S: canale non allocato");
        ESP_LOGE(kTag, "i2s_new_channel: %s", esp_err_to_name(err));
        s_tx = nullptr;
        return false;
    }

    i2s_std_config_t cfg = {};
    cfg.clk_cfg.sample_rate_hz = kBootRate;
    cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
    cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    cfg.slot_cfg.ws_width = 16;
    cfg.slot_cfg.ws_pol = false;
    cfg.slot_cfg.bit_shift = true;
    cfg.slot_cfg.left_align = true;
    cfg.slot_cfg.big_endian = false;
    cfg.slot_cfg.bit_order_lsb = false;
    cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(kPinBck);
    cfg.gpio_cfg.ws = static_cast<gpio_num_t>(kPinWs);
    cfg.gpio_cfg.dout = static_cast<gpio_num_t>(kPinDout);
    cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    cfg.gpio_cfg.invert_flags.mclk_inv = 0;
    cfg.gpio_cfg.invert_flags.bclk_inv = 0;
    cfg.gpio_cfg.invert_flags.ws_inv = 0;

    err = i2s_channel_init_std_mode(s_tx, &cfg);
    if (err != ESP_OK) {
        internal::setError("I2S: config std fallita");
        ESP_LOGE(kTag, "i2s_channel_init_std_mode: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx);
        s_tx = nullptr;
        return false;
    }
    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        internal::setError("I2S: enable fallita");
        ESP_LOGE(kTag, "i2s_channel_enable: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx);
        s_tx = nullptr;
        return false;
    }
    s_rate = kBootRate;
    return true;
}

bool initCodec() {
    if (!orbit_hal::i2cPresent(kCodecAddress)) {
        internal::setError("ES8311 assente sul bus I2C (0x18)");
        ESP_LOGE(kTag, "ES8311 non risponde a 0x%02x", kCodecAddress);
        return false;
    }

    // Full reset, then the proven M5Unified Cardputer ADV speaker sequence with
    // the ES8311 clock/format registers for 16-bit I2S slave, MCLK = BCLK.
    // 0x02..0x08 are the esp-bsp coefficients for MCLK/fs = 32 (32k/44.1k/48k).
    static constexpr uint8_t kSequence[][2] = {
        {0x00, 0x1F},  // reset all registers
        {0x00, 0x00},  // release reset
        {0x00, 0x80},  // CSM power on
        {0x01, 0xB5},  // MCLK = BCLK, M5Unified value
        {0x02, 0x18},  // pre-div 1, MULT_PRE = 8x
        {0x03, 0x10},  // ADC OSR
        {0x04, 0x10},  // DAC OSR
        {0x05, 0x00},  // ADC/DAC clock dividers = 1
        {0x06, 0x03},  // BCLK divider = 4
        {0x07, 0x00},  // LRCK high divider
        {0x08, 0xFF},  // LRCK low divider
        {0x09, 0x0C},  // SDP in: 16 bit, I2S
        {0x0A, 0x0C},  // SDP out: 16 bit, I2S
        {0x0D, 0x01},  // power up analog circuitry
        {0x0E, 0x02},  // analog PGA + ADC modulator
        {0x12, 0x00},  // power-up DAC
        {0x13, 0x10},  // enable output to HP drive
        {0x1C, 0x6A},  // ADC equalizer bypass
        {0x32, 0xBF},  // DAC volume 0 dB
        {0x37, 0x08},  // bypass DAC equalizer
    };

    for (size_t i = 0; i < sizeof(kSequence) / sizeof(kSequence[0]); ++i) {
        if (!codecWrite(kSequence[i][0], kSequence[i][1])) {
            ESP_LOGE(kTag, "ES8311: scrittura 0x%02x fallita", kSequence[i][0]);
            internal::setError("ES8311: scrittura registro fallita");
            return false;
        }
        if (kSequence[i][0] == 0x00 && kSequence[i][1] == 0x1F) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    uint8_t readback = 0;
    if (orbit_hal::i2cReadReg(kCodecAddress, 0x32, &readback)) {
        ESP_LOGI(kTag, "ES8311 pronto, reg 0x32 = 0x%02x", readback);
    } else {
        ESP_LOGW(kTag, "ES8311 pronto, lettura di controllo non riuscita");
    }
    s_codecReady = true;
    return true;
}

void applyVolume() {
    if (s_codecReady) {
        codecWrite(0x32, volumeRegister(s_volume));
    }
}

}  // namespace

namespace internal {

bool setOutputRate(uint32_t rate) {
    if (s_tx == nullptr) {
        return false;
    }
    if (rate < 16000 || rate > 48000) {
        return false;
    }
    if (rate == s_rate) {
        return true;
    }
    esp_err_t err = i2s_channel_disable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "i2s_channel_disable: %s", esp_err_to_name(err));
        return false;
    }
    i2s_std_clk_config_t clk = {};
    clk.sample_rate_hz = rate;
    clk.clk_src = I2S_CLK_SRC_DEFAULT;
    clk.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    const esp_err_t cfg = i2s_channel_reconfig_std_clock(s_tx, &clk);
    const esp_err_t en = i2s_channel_enable(s_tx);
    if (cfg != ESP_OK || en != ESP_OK) {
        ESP_LOGW(kTag, "rate %u non applicato (%s / %s)", static_cast<unsigned>(rate),
                 esp_err_to_name(cfg), esp_err_to_name(en));
        return false;
    }
    s_rate = rate;
    ESP_LOGI(kTag, "I2S rate %u Hz", static_cast<unsigned>(rate));
    return true;
}

bool writeStereo(const int16_t* samples, size_t count, volatile bool* stopFlag) {
    if (s_tx == nullptr || samples == nullptr || count == 0) {
        return false;
    }
    const uint8_t* data = reinterpret_cast<const uint8_t*>(samples);
    size_t remaining = count * sizeof(int16_t);
    int timeouts = 0;
    while (remaining > 0) {
        if (stopRequested(stopFlag)) {
            return false;
        }
        size_t written = 0;
        const esp_err_t err = i2s_channel_write(s_tx, data, remaining, &written, 100);
        if (err == ESP_ERR_TIMEOUT) {
            if (++timeouts >= 20) {
                setError("I2S: timeout di scrittura");
                return false;
            }
        } else if (err != ESP_OK) {
            ESP_LOGE(kTag, "i2s_channel_write: %s", esp_err_to_name(err));
            setError("I2S: errore di scrittura");
            return false;
        } else {
            timeouts = 0;
        }
        if (written > 0) {
            data += written;
            remaining -= written;
        }
    }
    return true;
}

void setPositionMs(uint32_t ms) {
    s_positionMs = ms;
}

void setDurationMs(uint32_t ms) {
    s_durationMs = ms;
}

void markPlaying() {
    s_playing = true;
}

void setError(const char* text) {
    if (text == nullptr) {
        s_error[0] = '\0';
        return;
    }
    std::snprintf(s_error, sizeof(s_error), "%s", text);
}

void resetPlayback() {
    s_playing = false;
    s_busy = true;
    s_positionMs = 0;
    s_durationMs = 0;
    s_error[0] = '\0';
}

void finishPlayback() {
    s_busy = false;
    s_playing = false;
    s_pause = false;
}

bool stopRequested(volatile bool* stopFlag) {
    return stopFlag != nullptr && *stopFlag;
}

}  // namespace internal

bool begin() {
    if (s_tx != nullptr) {
        return true;
    }
    internal::setError("");
    if (!initI2s()) {
        return false;
    }
    if (!initCodec()) {
        i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = nullptr;
        return false;
    }
    applyVolume();
    ESP_LOGI(kTag, "audio pronto (I2S1 %u Hz)", static_cast<unsigned>(s_rate));
    return true;
}

void end() {
    s_pause = false;
    s_busy = false;
    s_playing = false;
    if (s_codecReady) {
        codecWrite(0x32, 0x00);  // mute before stopping the clocks
        codecWrite(0x0D, 0x00);
        codecWrite(0x00, 0x00);
        s_codecReady = false;
    }
    if (s_tx != nullptr) {
        i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = nullptr;
    }
    s_rate = kBootRate;
    ESP_LOGI(kTag, "audio rilasciato");
}

void beep(uint16_t hz, uint16_t ms) {
    if (s_busy || hz == 0 || ms == 0 || s_volume <= 0) {
        return;
    }
    if (!begin()) {
        return;
    }
    if (!internal::setOutputRate(kBootRate)) {
        return;
    }

    constexpr int kFrames = 512;
    int16_t buffer[kFrames * 2];
    const uint32_t total = (kBootRate * ms) / 1000u;
    if (total == 0) {
        return;
    }
    const uint32_t fade = (kBootRate * 5) / 1000u;
    const float step = 6.28318530718f * static_cast<float>(hz) / static_cast<float>(kBootRate);
    const float gain = 32767.0f * 0.6f * static_cast<float>(s_volume) / kVolumeMax;
    float phase = 0.0f;
    for (uint32_t done = 0; done < total;) {
        const uint32_t chunk = (total - done < static_cast<uint32_t>(kFrames))
                                   ? (total - done)
                                   : static_cast<uint32_t>(kFrames);
        for (uint32_t i = 0; i < chunk; ++i) {
            const uint32_t at = done + i;
            float env = 1.0f;
            if (at < fade) {
                env = static_cast<float>(at) / static_cast<float>(fade);
            } else if (at + fade >= total) {
                env = static_cast<float>(total - at - 1) / static_cast<float>(fade);
            }
            const int16_t sample = static_cast<int16_t>(sinf(phase) * gain * env);
            buffer[i * 2] = sample;
            buffer[i * 2 + 1] = sample;
            phase += step;
            if (phase > 6.28318530718f) {
                phase -= 6.28318530718f;
            }
        }
        if (!internal::writeStereo(buffer, chunk * 2, nullptr)) {
            return;
        }
        done += chunk;
    }
}

int volume() {
    return s_volume;
}

void setVolume(int value) {
    if (value < 0) {
        value = 0;
    } else if (value > kVolumeMax) {
        value = kVolumeMax;
    }
    s_volume = value;
    applyVolume();
}

void setPaused(bool value) {
    s_pause = value;
}

bool paused() {
    return s_pause;
}

bool active() {
    return s_playing;
}

uint32_t positionMs() {
    return s_positionMs;
}

uint32_t durationMs() {
    return s_durationMs;
}

const char* lastError() {
    return s_error[0] == '\0' ? "nessun dettaglio" : s_error;
}

}  // namespace orbit_audio
