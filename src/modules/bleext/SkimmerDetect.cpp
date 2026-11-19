#include "BleExtInternal.h"

#if ENABLE_MODULE_BLEEXT

#include <algorithm>
#include <string>

#include "Display.h"
#include "Logger.h"
#include "Navigation.h"
#include "PowerTimer.h"
#include "Ui.h"

namespace bleext {
namespace {

constexpr uint8_t kBridgeName = 1 << 0;
constexpr uint8_t kGenericName = 1 << 1;
constexpr uint8_t kNoManufacturer = 1 << 2;
constexpr uint8_t kSppUuid = 1 << 3;

bool isExplicitBridgeName(const char* name) {
  // Moduli serial-to-BLE usati comunemente come bridge da skimmer.
  static const char* kTerms[] = {
      "hc-03", "hc-04", "hc-05", "hc-06", "hc03",  "hc04", "hc05",
      "hc06",  "rn-4",  "rn4",   "bluno",  "serial", "btm",  "spp",
      "linvor", "bolutek"};
  for (const char* term : kTerms) {
    if (containsNoCase(name, term)) {
      return true;
    }
  }
  return false;
}

bool isGenericBridgeName(const char* name) {
  static const char* kTerms[] = {"uart",       "bt module", "btmodule",
                                 "bridge",     "modulo bt", "modulo seriale",
                                 "serial bridge"};
  for (const char* term : kTerms) {
    if (containsNoCase(name, term)) {
      return true;
    }
  }
  return false;
}

void skimmerHandler(const AdvertRecord& record) {
  uint8_t reasons = 0;
  if (record.hasName && isExplicitBridgeName(record.name)) {
    reasons |= kBridgeName;
  } else if (record.hasName && !record.hasManufacturer &&
             isGenericBridgeName(record.name)) {
    reasons |= kGenericName | kNoManufacturer;
  }
  if (record.hasSppUuid) {
    reasons |= kSppUuid;
  }
  if ((reasons & kBridgeName) && !record.hasManufacturer) {
    reasons |= kNoManufacturer;
  }
  if (reasons == 0) {
    return;
  }
  AdvertRecord copy = record;
  copy.tags = reasons;
  ingestDevice(copy);
}

int suspicionScore(uint8_t tags) {
  int score = 0;
  score += (tags & kBridgeName) ? 3 : 0;
  score += (tags & kGenericName) ? 1 : 0;
  score += (tags & kNoManufacturer) ? 1 : 0;
  score += (tags & kSppUuid) ? 2 : 0;
  return score;
}

std::string reasonText(uint8_t tags) {
  std::string text;
  if (tags & kBridgeName) {
    text = "nome bridge";
  } else if (tags & kGenericName) {
    text = "nome generico";
  }
  if (tags & kNoManufacturer) {
    if (!text.empty()) {
      text += "/";
    }
    text += "senza mfr";
  }
  if (tags & kSppUuid) {
    if (!text.empty()) {
      text += "/";
    }
    text += "SPP";
  }
  return text;
}

void drawSkimmer(int count, const DeviceSummary* rows) {
  display::clear();
  display::header("Skimmer", "BLE+");
  display::text(4, 23, "Sospetti: " + std::to_string(count) + "  (euristica)",
                display::kAccent, 0);

  if (count == 0) {
    display::textCentered(58, "Nessun sospetto", display::kTextDim, 2);
    display::textCentered(78, "Criterio euristico, non e' una prova.",
                          display::kTextDim, 0);
  } else {
    const int shown = std::min(count, 3);
    for (int i = 0; i < shown; ++i) {
      const int y = 33 + i * 30;
      const std::string name =
          rows[i].hasName ? rows[i].name : "(senza nome)";
      display::text(4, y, shortenText(name, 28), display::kText, 0);
      display::text(4, y + 9,
                    std::string(rows[i].address) + "  " +
                        std::to_string(rows[i].rssi) + "dBm  " +
                        formatAge(millis() - rows[i].lastSeenMs),
                    display::kTextDim, 0);
      display::text(4, y + 18, shortenText(reasonText(rows[i].tags), 38),
                    display::kWarning, 0);
    }
  }
  display::footer("ESC=stop  euristica, non prova");
  display::push();
}

}  // namespace

void runSkimmerDetect() {
  powertimer::Hold hold;  // schermata di monitoraggio
  NimBLEScan* scan = scanOrNull();
  if (scan == nullptr) {
    ui::message("Rileva skimmer",
                {"!Stack BLE non disponibile."}, "BLE+");
    return;
  }

  resetDevices();
  setScanHandler(skimmerHandler);
  configureScan(scan, false);  // passivo: nessuna richiesta di scan

  static DeviceSummary rows[kMaxDevices];
  const auto draw = [&]() {
    int count = snapshotSummaries(rows, kMaxDevices, false);
    std::sort(rows, rows + count, [](const DeviceSummary& a,
                                     const DeviceSummary& b) {
      const int scoreA = suspicionScore(a.tags);
      const int scoreB = suspicionScore(b.tags);
      if (scoreA != scoreB) {
        return scoreA > scoreB;
      }
      return a.bestRssi > b.bestRssi;
    });
    drawSkimmer(count, rows);
  };

  navigation::waitRelease();
  for (;;) {
    const navigation::EventData ev = runScanWindow(scan, 900, draw, 120);
    if (ev.type == navigation::Event::Back) {
      break;
    }
  }

  stopScan(scan);
  logger::info("Skimmer: %d sospetti, heap=%u", deviceCount(),
               static_cast<unsigned>(ESP.getFreeHeap()));
  navigation::waitRelease();
}

}  // namespace bleext

#endif  // ENABLE_MODULE_BLEEXT
