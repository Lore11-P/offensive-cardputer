#include "BleExtInternal.h"

#if ENABLE_MODULE_BLEEXT

#include <Arduino.h>
#include <M5Cardputer.h>
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

namespace bleext {
namespace {

constexpr uint32_t kDiscoveryMs = 6000;
constexpr uint32_t kTrackWindowMs = 900;
constexpr uint32_t kLinkTimeoutMs = 6000;

char g_targetAddress[18] = {0};
volatile bool g_tracking = false;

portMUX_TYPE g_targetMux = portMUX_INITIALIZER_UNLOCKED;
volatile int g_targetRssi = kRssiFloor;
volatile uint32_t g_targetLastMs = 0;
volatile uint32_t g_targetHits = 0;

// OUI note dei moduli Flipper: 80:E1:26 e 80:E1:27 (ST/STM32WB),
// 0C:FA:22 (Flipper Devices).
bool isFlipperOui(const char* address) {
  static const char* kPrefixes[] = {"80:E1:26", "80:E1:27", "0C:FA:22"};
  for (const char* prefix : kPrefixes) {
    if (std::strncmp(address, prefix, 8) == 0) {
      return true;
    }
  }
  return false;
}

bool isFlipperName(const char* name) {
  static const char* kTerms[] = {"flipper", "unleashed", "momentum",
                                 "xtreme", "rogue"};
  for (const char* term : kTerms) {
    if (containsNoCase(name, term)) {
      return true;
    }
  }
  return false;
}

bool isFlipperCandidate(const AdvertRecord& record) {
  return (record.hasName && isFlipperName(record.name)) ||
         isFlipperOui(record.address);
}

void discoveryHandler(const AdvertRecord& record) {
  if (isFlipperCandidate(record)) {
    ingestDevice(record);
  }
}

void trackingHandler(const AdvertRecord& record) {
  if (!g_tracking ||
      std::strcmp(record.address, g_targetAddress) != 0) {
    return;
  }
  const uint32_t now = millis();
  portENTER_CRITICAL(&g_targetMux);
  if (record.rssi > g_targetRssi) {
    g_targetRssi = record.rssi;
  }
  g_targetLastMs = now;
  ++g_targetHits;
  portEXIT_CRITICAL(&g_targetMux);
}

int takeTargetRssi() {
  portENTER_CRITICAL(&g_targetMux);
  const int rssi = g_targetRssi;
  g_targetRssi = kRssiFloor;
  portEXIT_CRITICAL(&g_targetMux);
  return rssi;
}

uint32_t targetLastSeenMs() {
  portENTER_CRITICAL(&g_targetMux);
  const uint32_t last = g_targetLastMs;
  portEXIT_CRITICAL(&g_targetMux);
  return last;
}

uint32_t targetHits() {
  portENTER_CRITICAL(&g_targetMux);
  const uint32_t hits = g_targetHits;
  portEXIT_CRITICAL(&g_targetMux);
  return hits;
}

void drawDiscovery(uint32_t elapsed, int found) {
  display::clear();
  display::header("Trova Flipper", "BLE+");
  display::textCentered(36, "Ricerca Flipper/custom", display::kAccent, 2);
  display::progressBar(20, 60, 200, 8,
                       static_cast<float>(elapsed) /
                           static_cast<float>(kDiscoveryMs),
                       display::kInfo);
  display::textCentered(78, "Trovati: " + std::to_string(found),
                        display::kText, 2);
  display::textCentered(96, "Nome o OUI noto", display::kTextDim, 0);
  display::footer("ESC = annulla");
  display::push();
}

void drawTracker(const std::string& name, const std::string& address,
                 int rssi, int meters, int trend, float peak, bool lost,
                 bool sound, uint32_t hits) {
  display::clear();
  display::header("Flipper", "BLE+");

  const uint16_t color = lost ? display::kTextDim : rssiColor(rssi);
  display::text(6, 25, shortenText(name, 16), display::kText, 2);
  display::text(6, 41, address, display::kTextDim, 0);

  const std::string rssiText =
      (rssi <= kRssiFloor) ? "--" : std::to_string(rssi);
  display::text(6, 52, rssiText, color, 4);
  display::text(6, 84, "dBm", display::kTextDim, 0);

  std::string distance = "--";
  if (!lost && rssi > kRssiFloor) {
    distance = "~ " + std::to_string(meters) + " m";
  }
  display::text(44, 84, distance, lost ? display::kTextDim : display::kText, 2);

  std::string trendText = "-- stabile --";
  uint16_t trendColor = display::kTextDim;
  if (lost) {
    trendText = "SEGNALE PERSO";
    trendColor = display::kDanger;
  } else if (rssi > -48) {
    trendText = "!! CONTATTO !!";
    trendColor = display::kDanger;
  } else if (trend > 2) {
    trendText = "PIU' CALDO >>";
    trendColor = display::kAccent;
  } else if (trend < -2) {
    trendText = "<< PIU' FREDDO";
    trendColor = display::kInfo;
  }
  display::text(6, 100, trendText, trendColor, 2);

  const float fraction =
      lost ? 0.0f : rssiFraction(rssi > kRssiFloor ? rssi : kRssiFloor);
  drawSegmentBar(6, 118, 128, 4, 16, fraction, peak, color);
  display::text(140, 112, sound ? "audio SI" : "audio NO",
                display::kTextDim, 0);
  display::text(196, 112, "visti " + std::to_string(hits),
                display::kTextDim, 0);
  display::footer("ENTER=muto  ESC=stop");
  display::push();
}

}  // namespace

void runFlipperFinder() {
  powertimer::Hold hold;  // schermata di monitoraggio
  NimBLEScan* scan = scanOrNull();
  if (scan == nullptr) {
    ui::message("Trova Flipper", {"!Stack BLE non disponibile."}, "BLE+");
    return;
  }

  resetDevices();
  g_tracking = false;
  setScanHandler(discoveryHandler);
  configureScan(scan, true);  // attivo: il nome arriva con la SCAN_RSP

  navigation::waitRelease();
  const uint32_t start = millis();
  for (;;) {
    const uint32_t elapsed = millis() - start;
    if (elapsed >= kDiscoveryMs) {
      break;
    }
    const auto draw = [&]() {
      drawDiscovery(millis() - start, deviceCount());
    };
    if (runScanWindow(scan, 900, draw, 120).type ==
        navigation::Event::Back) {
      stopScan(scan);
      navigation::waitRelease();
      return;
    }
  }
  stopScan(scan);

  DeviceSummary* rows = g_deviceRows;
  const int count = snapshotSummaries(rows, kMaxDevices, true);
  if (count == 0) {
    ui::message("Trova Flipper",
                {"!Nessun Flipper o custom rilevato.",
                 "Il bersaglio deve essere in trasmissione.",
                 "Verifica nome o OUI noti del firmware."},
                "BLE+");
    return;
  }

  std::vector<std::string> labels;
  labels.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    const std::string name =
        rows[i].hasName ? shortenText(rows[i].name, 12) : "(senza nome)";
    const std::string tail = std::string(rows[i].address).substr(12);
    labels.push_back(name + " " + std::to_string(rows[i].rssi) + " " + tail);
  }
  const int choice =
      chooseList("Bersaglio Flipper", "BLE+", labels,
                 "ENTER=traccia  `=annulla");
  if (choice < 0) {
    return;
  }

  const DeviceSummary target = rows[choice];
  std::snprintf(g_targetAddress, sizeof(g_targetAddress), "%s",
                target.address);
  g_targetRssi = kRssiFloor;
  g_targetLastMs = 0;
  g_targetHits = 0;
  g_tracking = true;

  setScanHandler(trackingHandler);
  configureScan(scan, false);  // passivo: non serve il nome

  int ema = kRssiFloor;
  int previousEma = kRssiFloor;
  int trend = 0;
  float peak = 0.0f;
  bool sound = true;
  uint32_t nextTrend = millis() + 900;
  uint32_t nextBeep = 0;

  const auto draw = [&]() {
    const uint32_t now = millis();
    const uint32_t last = targetLastSeenMs();
    const bool lost = (last == 0) || (now - last > kLinkTimeoutMs);
    const int shownRssi =
        lost ? kRssiFloor : ((ema <= kRssiFloor) ? kRssiFloor : ema);
    drawTracker(target.hasName ? target.name : "(senza nome)",
                target.address, shownRssi, rssiToMeters(shownRssi), trend,
                peak, lost, sound, targetHits());
  };

  navigation::waitRelease();
  for (;;) {
    draw();
    const navigation::EventData ev =
        runScanWindow(scan, kTrackWindowMs, draw, 40);
    if (ev.type == navigation::Event::Back) {
      break;
    }
    if (ev.type == navigation::Event::Select) {
      sound = !sound;
    }

    const int sample = takeTargetRssi();
    const bool heard = sample > kRssiFloor;
    if (heard) {
      if (ema <= kRssiFloor) {
        ema = sample;
      } else {
        ema = (ema * 3 + sample) / 4;  // media mobile esponenziale
      }
      peak = std::max(peak, rssiFraction(sample));
    } else {
      peak *= 0.985f;  // decadimento lento del picco
    }

    const uint32_t now = millis();
    if (now >= nextTrend) {
      trend = (ema > kRssiFloor) ? (ema - previousEma) : 0;
      if (previousEma <= kRssiFloor) {
        trend = 0;
      }
      previousEma = ema;
      nextTrend = now + 900;
    }

    if (sound && heard && now >= nextBeep) {
      const float fraction = rssiFraction(ema);
      const int interval =
          beepIntervalMs(config::radarSensitivity(), ema);
      M5Cardputer.Speaker.tone(700.0f + fraction * 1600.0f, 7);
      nextBeep = now + static_cast<uint32_t>(interval);
    }
  }

  g_tracking = false;
  stopScan(scan);
  logger::info("Flipper finder: %s, heap=%u", g_targetAddress,
               static_cast<unsigned>(ESP.getFreeHeap()));
  navigation::waitRelease();
}

}  // namespace bleext

#endif  // ENABLE_MODULE_BLEEXT
