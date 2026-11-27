#include "IrRmt.h"

#include <algorithm>
#include <cstring>

#include "driver/gpio.h"
#include "driver/rmt_common.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "Logger.h"
#include "orbit_compat.h"

namespace orbit_ir {
namespace {

constexpr int kIrTxGpio = 44;
constexpr uint32_t kResolutionHz = 1000000;  // 1 tick = 1 us
constexpr uint32_t kCarrierHz = 38000;
constexpr float kCarrierDuty = 0.33f;
constexpr size_t kMaxSymbols = 512;
// Il campo duration e' largo 15 bit: le durate piu' lunghe vengono spezzate.
constexpr uint32_t kMaxHalfDuration = 32767;
// Un frame raw oltre 2 s bloccherebbe il menu: viene rifiutato.
constexpr uint64_t kMaxFrameUs = 2000000;

rmt_channel_handle_t s_channel = nullptr;
rmt_encoder_handle_t s_encoder = nullptr;

struct SymbolBuffer {
    rmt_symbol_word_t words[kMaxSymbols];
    size_t count = 0;
    bool havePending = false;
    uint32_t pendingDuration = 0;
    bool pendingLevel = false;
    bool overflow = false;

    void add(uint32_t usec, bool level) {
        while (usec > 0 && !overflow) {
            const uint32_t chunk = std::min(usec, kMaxHalfDuration);
            if (!havePending) {
                pendingDuration = chunk;
                pendingLevel = level;
                havePending = true;
            } else {
                if (count >= kMaxSymbols) {
                    overflow = true;
                    return;
                }
                rmt_symbol_word_t word = {};
                word.duration0 = static_cast<uint16_t>(pendingDuration);
                word.level0 = pendingLevel ? 1 : 0;
                word.duration1 = static_cast<uint16_t>(chunk);
                word.level1 = level ? 1 : 0;
                words[count++] = word;
                havePending = false;
            }
            usec -= chunk;
        }
    }

    size_t finish() {
        if (havePending) {
            if (count >= kMaxSymbols) {
                overflow = true;
                return count;
            }
            rmt_symbol_word_t word = {};
            word.duration0 = static_cast<uint16_t>(pendingDuration);
            word.level0 = pendingLevel ? 1 : 0;
            word.duration1 = 0;
            word.level1 = 0;
            words[count++] = word;
            havePending = false;
        }
        return count;
    }
};

bool ensureTx() {
    if (s_channel != nullptr && s_encoder != nullptr) {
        return true;
    }
    if (s_channel != nullptr) {
        rmt_del_channel(s_channel);
        s_channel = nullptr;
    }

    rmt_tx_channel_config_t config = {};
    config.gpio_num = static_cast<gpio_num_t>(kIrTxGpio);
    config.clk_src = RMT_CLK_SRC_DEFAULT;
    config.resolution_hz = kResolutionHz;
    config.mem_block_symbols = 64;
    config.trans_queue_depth = 4;

    esp_err_t err = rmt_new_tx_channel(&config, &s_channel);
    if (err != ESP_OK) {
        logger::error("IR: rmt_new_tx_channel: %s", esp_err_to_name(err));
        s_channel = nullptr;
        return false;
    }

    rmt_carrier_config_t carrier = {};
    carrier.frequency_hz = kCarrierHz;
    carrier.duty_cycle = kCarrierDuty;
    carrier.flags.polarity_active_low = 0;
    carrier.flags.always_on = 0;
    err = rmt_apply_carrier(s_channel, &carrier);
    if (err != ESP_OK) {
        logger::error("IR: rmt_apply_carrier: %s", esp_err_to_name(err));
        releaseTx();
        return false;
    }

    rmt_copy_encoder_config_t encoderConfig = {};
    err = rmt_new_copy_encoder(&encoderConfig, &s_encoder);
    if (err != ESP_OK) {
        logger::error("IR: rmt_new_copy_encoder: %s", esp_err_to_name(err));
        s_encoder = nullptr;
        releaseTx();
        return false;
    }

    err = rmt_enable(s_channel);
    if (err != ESP_OK) {
        logger::error("IR: rmt_enable: %s", esp_err_to_name(err));
        releaseTx();
        return false;
    }
    return true;
}

}  // namespace

bool sendDurations(const std::vector<int32_t>& durations, int repeats) {
    if (durations.empty()) {
        return false;
    }

    uint64_t total = 0;
    for (int32_t value : durations) {
        const uint64_t magnitude =
            value >= 0 ? static_cast<uint64_t>(value)
                       : static_cast<uint64_t>(-static_cast<int64_t>(value));
        total += magnitude;
        if (total > kMaxFrameUs) {
            logger::warn("IR: frame raw troppo lungo (%llu us)",
                         static_cast<unsigned long long>(total));
            return false;
        }
    }

    if (!ensureTx()) {
        return false;
    }

    SymbolBuffer symbols;
    for (int32_t value : durations) {
        if (value > 0) {
            symbols.add(static_cast<uint32_t>(value), true);
        } else if (value < 0) {
            symbols.add(static_cast<uint32_t>(-static_cast<int64_t>(value)),
                        false);
        }
        if (symbols.overflow) {
            break;
        }
    }
    const size_t count = symbols.finish();
    if (count == 0 || symbols.overflow) {
        logger::warn("IR: frame oltre %u simboli", static_cast<unsigned>(kMaxSymbols));
        return false;
    }

    repeats = std::max(0, std::min(repeats, 5));
    rmt_transmit_config_t tx = {};
    tx.loop_count = 0;
    tx.flags.eot_level = 0;

    for (int i = 0; i <= repeats; ++i) {
        esp_err_t err = rmt_transmit(
            s_channel, s_encoder, symbols.words,
            count * sizeof(rmt_symbol_word_t), &tx);
        if (err == ESP_OK) {
            err = rmt_tx_wait_all_done(s_channel, 3000);
        }
        if (err != ESP_OK) {
            logger::error("IR: rmt_transmit: %s", esp_err_to_name(err));
            // Canale in stato dubbio: la prossima pressione lo ricrea.
            releaseTx();
            return false;
        }
        // Piccolo spazio tra frame: i raw possono non avere un gap finale.
        if (i < repeats) {
            delay(8);
        }
    }
    return true;
}

bool sendCommand(const IrCommand& command) {
    if (!command.supported) {
        return false;
    }
    std::vector<int32_t> durations = buildDurations(command);
    if (durations.empty()) {
        return false;
    }
    return sendDurations(durations, kExtraRepeats);
}

void releaseTx() {
    if (s_channel != nullptr) {
        rmt_tx_wait_all_done(s_channel, 100);
        rmt_disable(s_channel);
        rmt_del_channel(s_channel);
        s_channel = nullptr;
    }
    if (s_encoder != nullptr) {
        rmt_del_encoder(s_encoder);
        s_encoder = nullptr;
    }
}

}  // namespace orbit_ir
