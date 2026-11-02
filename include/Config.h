#pragma once

#include <Arduino.h>
#include <string>

#include "ModuleFlags.h"

// Impostazioni persistenti (NVS) e import/export JSON.
// Nessuna credenziale viene memorizzata qui: le credenziali Wi-Fi vivono nel
// namespace NVS dedicato "cpfw-net" (vedi utils/WifiSetup.cpp).
namespace config {

enum class Module : uint8_t { Ble = 0, Wifi, Ir, Games, Settings, Sd, Radar, Radio, Count };

struct Settings {
    uint8_t brightness = 128;      // 10..255
    uint8_t rotation = 1;          // indice M5GFX
    uint16_t modules = 0xFFFF;     // bitmask Module
    uint16_t bleDurationSec = 30;  // durata spam BLE
    uint16_t bleIntervalMs = 100;  // intervallo advertising
    uint8_t irRepeat = 2;          // ripetizioni comandi IR
    uint16_t irTvbgDelayMs = 120;  // pausa TV-B-Gone
    uint16_t sleepTimeoutSec = 120;  // 0 = disattivato

    // Sensibilita' del radar BLE (1..5): controlla quanto rapidamente il tic
    // del finder accelera al diminuire della distanza. 5 = molto reattivo.
    uint8_t radarSensitivity = 3;
};

void begin();
const Settings& get();
void save();
void reset();

uint8_t brightness();
void setBrightness(uint8_t value);

uint8_t rotation();
void setRotation(uint8_t index);

bool moduleEnabled(Module module);
bool moduleCompiled(Module module);
void setModuleEnabled(Module module, bool enabled);

uint16_t bleDurationSec();
void setBleDurationSec(uint16_t seconds);
uint16_t bleIntervalMs();
void setBleIntervalMs(uint16_t ms);
uint8_t irRepeat();
void setIrRepeat(uint8_t repeats);
uint16_t irTvbgDelayMs();
void setIrTvbgDelayMs(uint16_t ms);
uint16_t sleepTimeoutSec();
void setSleepTimeoutSec(uint16_t seconds);

// Sensibilita' del radar BLE (1..5).
uint8_t radarSensitivity();
void setRadarSensitivity(uint8_t level);

// JSON non sensibile, utile per backup della configurazione.
std::string toJson();
bool applyJson(const char* json, size_t len);
bool exportToFile(const char* path = "/config.json");
bool importFromFile(const char* path = "/config.json");

}  // namespace config
