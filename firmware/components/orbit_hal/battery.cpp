#include "orbit_hal/battery.h"

#include <algorithm>

#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_log.h>

namespace orbit_hal {
namespace {

constexpr const char* kTag = "battery";
constexpr adc_channel_t kChannel = ADC_CHANNEL_9;  // GPIO10 su ESP32-S3
constexpr int kDivider = 2;                        // R 1:1 sul modulo

adc_oneshot_unit_handle_t s_unit = nullptr;
adc_cali_handle_t s_cali = nullptr;
bool s_tried = false;

bool init() {
    if (s_unit != nullptr) {
        return true;
    }
    if (s_tried) {
        return false;
    }
    s_tried = true;

    adc_oneshot_unit_init_cfg_t unitConfig = {};
    unitConfig.unit_id = ADC_UNIT_1;
    unitConfig.ulp_mode = ADC_ULP_MODE_DISABLE;
    if (adc_oneshot_new_unit(&unitConfig, &s_unit) != ESP_OK) {
        ESP_LOGE(kTag, "ADC unit non disponibile");
        return false;
    }
    adc_oneshot_chan_cfg_t channelConfig = {};
    channelConfig.atten = ADC_ATTEN_DB_12;
    channelConfig.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_oneshot_config_channel(s_unit, kChannel, &channelConfig) != ESP_OK) {
        ESP_LOGE(kTag, "ADC canale non configurato");
        return false;
    }

    adc_cali_curve_fitting_config_t caliConfig = {};
    caliConfig.unit_id = ADC_UNIT_1;
    caliConfig.atten = ADC_ATTEN_DB_12;
    caliConfig.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&caliConfig, &s_cali) != ESP_OK) {
        s_cali = nullptr;  // senza calibrazione si usa la conversione grezza
    }
    return true;
}

}  // namespace

int batteryMillivolts() {
    if (!init()) {
        return -1;
    }
    int raw = 0;
    if (adc_oneshot_read(s_unit, kChannel, &raw) != ESP_OK) {
        return -1;
    }
    int mv = 0;
    if (s_cali != nullptr) {
        if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) {
            mv = raw;
        }
    } else {
        // ADC 12 bit, attenuazione 12 dB: circa 3,1 V fondo scala.
        mv = raw * 3100 / 4095;
    }
    return mv * kDivider;
}

int batteryPercent() {
    const int mv = batteryMillivolts();
    if (mv <= 0) {
        return -1;
    }
    const int pct = (mv - 3300) * 100 / (4200 - 3300);
    return std::max(0, std::min(100, pct));
}

}  // namespace orbit_hal
