#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ModuleFlags.h"
#include "Navigation.h"

// Infrastruttura condivisa degli strumenti BLE estesi.
//
// Il callback di advertising gira nel task del controller NimBLE: le liste
// sono protette da critical section e contengono solo array a dimensione
// fissa, senza allocazioni per pacchetto.
namespace bleext {

// Limite esplicito a 64 dispositivi: niente crescita illimitata con traffico
// BLE intenso o attacchi di flooding pubblicitario.
constexpr int kMaxDevices = 64;

// Scala RSSI usata da barre e stime di distanza (come il modulo Radar).
constexpr int kRssiFloor = -95;
constexpr int kRssiCeil = -35;

struct AdvertRecord {
  char address[18] = {0};
  uint8_t native[6] = {0};
  char name[32] = {0};
  uint8_t addressType = 0;
  uint8_t advType = 0;
  uint16_t companyId = 0xFFFF;
  int8_t rssi = -127;
  int8_t bestRssi = -127;
  uint8_t manufacturerLen = 0;
  uint8_t manufacturer[32] = {0};
  uint16_t payloadLen = 0;
  uint8_t payload[62] = {0};
  uint8_t serviceUuidCount = 0;
  uint8_t tags = 0;
  bool hasName = false;
  bool hasManufacturer = false;
  bool connectable = false;
  bool hasSppUuid = false;
  uint32_t firstSeenMs = 0;
  uint32_t lastSeenMs = 0;
  uint32_t bestSeenMs = 0;
  uint32_t hits = 0;
};

struct DeviceSummary {
  char address[18] = {0};
  char name[32] = {0};
  uint8_t addressType = 0;
  uint8_t advType = 0;
  uint8_t serviceUuidCount = 0;
  uint8_t tags = 0;
  int8_t rssi = -127;
  int8_t bestRssi = -127;
  bool hasName = false;
  bool hasManufacturer = false;
  bool connectable = false;
  uint32_t firstSeenMs = 0;
  uint32_t lastSeenMs = 0;
  uint32_t bestSeenMs = 0;
  uint32_t hits = 0;
};

// Handler statico invocato dal callback unico di scansione. Tutte le funzioni
// registrate hanno durata pari al programma: nessun puntatore a stack.
using AdvertHandler = void (*)(const AdvertRecord&);

void ensureBle();
NimBLEScan* scanOrNull();

void setScanHandler(AdvertHandler handler);
void clearScanHandler();
NimBLEAdvertisedDeviceCallbacks* scanCallbacks();

void resetDevices();
// Ritorna false se la tabella e' piena o il record e' scartato.
bool ingestDevice(const AdvertRecord& record);
int deviceCount();
// Copia fino a maxOut riassunti; con strongestFirst ordina per bestRssi.
int snapshotSummaries(DeviceSummary* out, int maxOut, bool strongestFirst);

void configureScan(NimBLEScan* scan, bool active);
// Ferma la scansione, stacca i callback e azzera l'handler.
void stopScan(NimBLEScan* scan);

// Finestra non bloccante: start() di NimBLEScan vuole i secondi, quindi la
// durata effettiva e' almeno 1 s; il deadline evita attese appese.
navigation::EventData runScanWindow(NimBLEScan* scan, uint32_t durationMs,
                                    const std::function<void()>& draw = nullptr,
                                    uint32_t drawEveryMs = 40);

std::string shortenText(const std::string& text, size_t maxLen);
std::string formatAge(uint32_t ageMs);
void formatAddress(const uint8_t* native, char out[18]);
bool containsNoCase(const char* haystack, const char* needle);

// Converte il tipo di evento HCI (getAdvType) nel tipo di PDU sul canale
// advertising: HCI 2 = ADV_SCAN_IND, HCI 3 = ADV_NONCONN_IND.
uint8_t advEventToPdu(uint8_t eventType);

uint16_t rssiColor(int rssi);
float rssiFraction(int rssi);
int rssiToMeters(int rssi);
int beepIntervalMs(int level, int rssi);
void drawSegmentBar(int x, int y, int w, int h, int segments, float fraction,
                    float peak, uint16_t color);

int chooseList(const std::string& title, const std::string& parent,
               const std::vector<std::string>& labels, const std::string& hint);

void runAirTagFinder();
#if ENABLE_OFFENSIVE
void runAirTagSpoof();
#endif
void runFlipperFinder();
void runGattEnum();
void runSkimmerDetect();
void runWardrivingMenu();
void runBleCapture();

}  // namespace bleext
