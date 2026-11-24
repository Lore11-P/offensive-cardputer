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

// Lettore MP3 locale dalla microSD (stesso decoder della web radio).
#ifndef ENABLE_MODULE_MUSIC
#define ENABLE_MODULE_MUSIC 1
#endif

// BadUSB / HID injection: richiede lo stack TinyUSB (ARDUINO_USB_MODE=0),
// quindi e' attivo solo nell'env dedicato cardputer-adv-hid.
#ifndef ENABLE_MODULE_BADUSB
#define ENABLE_MODULE_BADUSB 0
#endif

// Browser/analisi catture 802.11: PCAP on-device, export hc22000 ed EAPOL.
#ifndef ENABLE_MODULE_PCAPTOOLS
#define ENABLE_MODULE_PCAPTOOLS 1
#endif

// Strumenti BLE: AirTag, Flipper finder, GATT, skimmer, wardriving, cattura.
#ifndef ENABLE_MODULE_BLEEXT
#define ENABLE_MODULE_BLEEXT 1
#endif

// Microfono: spettro FFT, registratore WAV, voice memo.
#ifndef ENABLE_MODULE_AUDIO
#define ENABLE_MODULE_AUDIO 1
#endif

// BLE HID: tastiera BLE e DuckyScript via BLE (Bad BLE).
#ifndef ENABLE_MODULE_BLEHID
#define ENABLE_MODULE_BLEHID 1
#endif

// Utility: clock, calendario, note, calcolatrice, QR, TOTP.
#ifndef ENABLE_MODULE_TOOLS
#define ENABLE_MODULE_TOOLS 1
#endif

// WiFi Lab: PMKID attivo, client monitor, whitelist, waterfall 2.4 GHz.
#ifndef ENABLE_MODULE_WIFILAB
#define ENABLE_MODULE_WIFILAB 1
#endif

// WPA-SEC: upload catture e download potfile.
#ifndef ENABLE_MODULE_WPASEC
#define ENABLE_MODULE_WPASEC 1
#endif

// Sensori: livella, goniometro, pedometro, torcia LED.
#ifndef ENABLE_MODULE_SENSORS
#define ENABLE_MODULE_SENSORS 1
#endif

// WiFi+: Responder, Karma, Auto-Portal.
#ifndef ENABLE_MODULE_WIFIEXT
#define ENABLE_MODULE_WIFIEXT 1
#endif

// ESP-NOW: file, comandi e clipboard mesh.
#ifndef ENABLE_MODULE_ESPNOW
#define ENABLE_MODULE_ESPNOW 1
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
inline constexpr const char* kVersion = "2.3.1";
inline constexpr const char* kTitle = "ORBIT OS v2.3.1";
inline constexpr const char* kShortTitle = "ORBIT OS";
inline constexpr const char* kBuildDate = __DATE__;
}  // namespace fw
