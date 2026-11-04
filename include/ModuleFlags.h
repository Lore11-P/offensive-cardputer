#pragma once

// ============================================================================
//  Flag di compilazione dei moduli.
//  I valori possono essere sovrascritti da platformio.ini (build_flags).
// ============================================================================

#ifndef ENABLE_MODULE_BLE
#define ENABLE_MODULE_BLE 1
#endif

// Chat BLE tra Cardputer (richiede ENABLE_MODULE_BLE=1).
#ifndef ENABLE_MODULE_CHAT
#define ENABLE_MODULE_CHAT 1
#endif

#ifndef ENABLE_MODULE_WIFI
#define ENABLE_MODULE_WIFI 1
#endif

#ifndef ENABLE_MODULE_IR
#define ENABLE_MODULE_IR 1
#endif

#ifndef ENABLE_MODULE_GAMES
#define ENABLE_MODULE_GAMES 1
#endif

#ifndef ENABLE_MODULE_SETTINGS
#define ENABLE_MODULE_SETTINGS 1
#endif

#ifndef ENABLE_MODULE_SD
#define ENABLE_MODULE_SD 1
#endif

// Radar Wi-Fi/BLE passivo (finder BLE, vista banda 2.4 GHz, PPI).
#ifndef ENABLE_MODULE_RADAR
#define ENABLE_MODULE_RADAR 1
#endif

// Web radio (decoder MP3/AAC via ESP32-audioI2S).
#ifndef ENABLE_MODULE_RADIO
#define ENABLE_MODULE_RADIO 1
#endif

// Presenza via Wi-Fi CSI: sperimentale, richiede traffico 802.11 reale.
// Le API esp_wifi_set_csi* esistono su ESP32-S3 in arduino-esp32 2.0.x
// (CONFIG_ESP32_WIFI_CSI_ENABLED=1), ma la sensibilita' dipende dai frame
// ricevuti: senza un AP che generi traffico arrivano solo i beacon vicini.
#ifndef ENABLE_RADAR_CSI
#define ENABLE_RADAR_CSI 1
#endif

// Le funzioni offensive (BLE spam, deauth, evil twin, evil portal, jammer)
// sono compilate solo se questo flag vale 1. Default: disabilitate.
#ifndef ENABLE_OFFENSIVE
#define ENABLE_OFFENSIVE 0
#endif

// Ricevitore IR esterno, opzionale: la Cardputer ADV ha solo il trasmettitore.
#ifndef ENABLE_IR_LEARN
#define ENABLE_IR_LEARN 0
#endif

// Pin e costanti hardware Cardputer ADV (verificati sulla documentazione M5Stack).
#ifndef IR_TX_PIN
#define IR_TX_PIN 44
#endif

#ifndef SD_CS_PIN
#define SD_CS_PIN 12
#endif
#ifndef SD_MOSI_PIN
#define SD_MOSI_PIN 14
#endif
#ifndef SD_SCK_PIN
#define SD_SCK_PIN 40
#endif
#ifndef SD_MISO_PIN
#define SD_MISO_PIN 39
#endif

namespace fw {
inline constexpr const char* kVersion = "1.0.0";
inline constexpr const char* kTitle = "CardputerFirmware v1.0";
inline constexpr const char* kShortTitle = "CardputerFirmware";
inline constexpr const char* kBuildDate = __DATE__;
}  // namespace fw
