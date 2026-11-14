#include "BleExtInternal.h"

#if ENABLE_MODULE_BLEEXT

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "Config.h"
#include "Display.h"
#include "Logger.h"
#include "Navigation.h"
#include "PowerTimer.h"
#include "Ui.h"

extern "C" int ble_hs_id_set_rnd(const uint8_t* rnd_addr);

namespace bleext {
namespace {

constexpr uint8_t kTagStrong = 1 << 0;   // 4C 00 12 19
constexpr uint8_t kTagOffline = 1 << 1;  // 4C 00 10

// Apple FindMy: manufacturer data con company ID 0x004C e tipo 0x12
// (lunghezza 0x19), oppure il pattern "offline finding" 0x10.
int classifyAirTag(const AdvertRecord& record) {
  if (!record.hasManufacturer || record.companyId != 0x004C ||
      record.manufacturerLen < 3) {
    return 0;
  }
  const uint8_t* data = record.manufacturer;
  if (data[2] == 0x12 && record.manufacturerLen >= 4 && data[3] == 0x19) {
    return 1;
  }
  if (data[2] == 0x10) {
    return 2;
  }
  return 0;
}

void airTagHandler(const AdvertRecord& record) {
  const int kind = classifyAirTag(record);
  if (kind == 0) {
    return;
  }
  AdvertRecord copy = record;
  copy.tags = (kind == 1) ? kTagStrong : kTagOffline;
  ingestDevice(copy);
}

void drawAirTagScreen(const char* title, uint32_t foundStrong,
                      uint32_t foundOffline, int count,
                      const DeviceSummary* rows) {
  display::clear();
  display::header(title, "BLE+");
  display::text(4, 24,
                "Trovati: " + std::to_string(count) + "  (0x12 " +
                    std::to_string(foundStrong) + " / 0x10 " +
                    std::to_string(foundOffline) + ")",
                display::kAccent, 0);

  if (count == 0) {
    display::textCentered(55, "Nessun AirTag/FindMy", display::kTextDim, 2);
    display::textCentered(74, "Scansione passiva in corso...",
                          display::kTextDim, 0);
  } else {
    const int shown = std::min(count, 4);
    for (int i = 0; i < shown; ++i) {
      const int y = 35 + i * 21;
      const bool strong = (rows[i].tags & kTagStrong) != 0;
      const std::string kind = strong ? "AirTag " : "FindMy ";
      const std::string name =
          rows[i].hasName ? rows[i].name : "(senza nome)";
      display::text(4, y, kind + shortenText(name, 15), display::kText, 0);
      display::text(4, y + 9,
                    std::string(rows[i].address) + "  " +
                        std::to_string(rows[i].rssi) + "dBm  " +
                        formatAge(millis() - rows[i].lastSeenMs),
                    display::kTextDim, 0);
    }
  }
  display::footer("`=stop  solo ricerca passiva");
  display::push();
}

#if ENABLE_OFFENSIVE
void drawSpoofScreen(const uint8_t* mac, uint32_t remainingMs) {
  char macText[24];
  std::snprintf(macText, sizeof(macText), "%02X:%02X:%02X:%02X:%02X:%02X",
                mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);

  display::clear();
  display::header("Falsifica AirTag", "BLE+");
  display::textCentered(32, "Trasmissione FindMy attiva", display::kAccent, 2);
  display::textCentered(52, "MAC casuale:", display::kTextDim, 0);
  display::textCentered(64, macText, display::kInfo, 2);
  display::progressBar(20, 86, 200, 8,
                       1.0f - static_cast<float>(remainingMs) / 30000.0f,
                       display::kDanger);
  display::textCentered(100, std::to_string(remainingMs / 1000) + " s",
                        display::kText, 0);
  display::textCentered(114, "Solo test autorizzati", display::kWarning, 0);
  display::footer("` = interrompi");
  display::push();
}
#endif  // ENABLE_OFFENSIVE

}  // namespace

void runAirTagFinder() {
  powertimer::Hold hold;  // schermata di monitoraggio
  NimBLEScan* scan = scanOrNull();
  if (scan == nullptr) {
    ui::message("Trova AirTag",
                {"!Stack BLE non disponibile.", "Riavvia il dispositivo."},
                "BLE+");
    return;
  }

  resetDevices();
  setScanHandler(airTagHandler);
  configureScan(scan, false);  // passivo: nessuna SCAN_REQ trasmessa

  static DeviceSummary rows[kMaxDevices];
  const auto draw = [&]() {
    int count = snapshotSummaries(rows, kMaxDevices, true);
    uint32_t strong = 0;
    uint32_t offline = 0;
    for (int i = 0; i < count; ++i) {
      if (rows[i].tags & kTagStrong) {
        ++strong;
      }
      if (rows[i].tags & kTagOffline) {
        ++offline;
      }
    }
    drawAirTagScreen("Trova AirTag", strong, offline, count, rows);
  };

  navigation::waitRelease();
  for (;;) {
    const navigation::EventData ev = runScanWindow(scan, 900, draw, 120);
    if (ev.type == navigation::Event::Back) {
      break;
    }
  }

  stopScan(scan);
  logger::info("AirTag: %d dispositivi trovati, heap=%u", deviceCount(),
               static_cast<unsigned>(ESP.getFreeHeap()));
  navigation::waitRelease();
}

#if ENABLE_OFFENSIVE
void runAirTagSpoof() {
  powertimer::Hold hold;
  if (!ui::confirm("Falsifica AirTag",
                   {"Trasmette un annuncio FindMy/AirTag",
                    "falsificato con MAC casuale per ~30 s.",
                    "Solo su dispositivi propri o in ambiente",
                    "RF isolato e con autorizzazione."},
                   "BLE+")) {
    return;
  }

  ensureBle();
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  if (advertising == nullptr) {
    ui::message("Falsifica AirTag", {"!Trasmissione non disponibile."}, "BLE+");
    return;
  }

  advertising->stop();
  NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM, false);
  advertising->setAdvertisementType(BLE_GAP_CONN_MODE_NON);
  advertising->setScanResponse(false);

  const uint16_t intervalMs =
      std::max<uint16_t>(config::bleIntervalMs(), 20);
  const uint16_t units = static_cast<uint16_t>(std::min<uint32_t>(
      16384, static_cast<uint32_t>(intervalMs) * 1000UL / 625));
  advertising->setMinInterval(units);
  advertising->setMaxInterval(units);

  uint8_t mac[6] = {0};
  esp_fill_random(mac, sizeof(mac));
  mac[0] |= 0xC0;
  mac[5] |= 0xC0;
  ble_hs_id_set_rnd(mac);

  // Payload legacy da 31 byte: 1E FF 4C 00 12 19 + 25 byte utili.
  uint8_t payload[31] = {0};
  payload[0] = 0x1E;  // lunghezza AD structure
  payload[1] = 0xFF;  // manufacturer specific data
  payload[2] = 0x4C;  // Apple company ID, little endian
  payload[3] = 0x00;
  payload[4] = 0x12;  // FindMy/AirTag
  payload[5] = 0x19;  // lunghezza 25
  payload[6] = 0x00;  // stato
  payload[7] = 0x00;  // hint
  esp_fill_random(&payload[8], 23);

  NimBLEAdvertisementData data;
  data.addData(std::string(reinterpret_cast<const char*>(payload),
                           sizeof(payload)));
  advertising->setAdvertisementData(data);

  if (!advertising->start(0)) {
    advertising->stop();
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC, false);
    ui::message("Falsifica AirTag", {"!Avvio trasmissione fallito."}, "BLE+");
    return;
  }
  logger::info("Trasmissione AirTag avviata: 30 s, MAC casuale");

  const uint32_t start = millis();
  uint32_t lastDraw = 0;
  while (millis() - start < 30000) {
    navigation::update();
    if (navigation::poll().type == navigation::Event::Back) {
      break;
    }
    const uint32_t now = millis();
    if (now - lastDraw > 100) {
      drawSpoofScreen(mac, 30000 - (now - start));
      lastDraw = now;
    }
    delay(10);
  }

  advertising->stop();
  NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC, false);
  logger::info("Trasmissione AirTag terminata");
  navigation::waitRelease();
}
#endif  // ENABLE_OFFENSIVE

}  // namespace bleext

#endif  // ENABLE_MODULE_BLEEXT
