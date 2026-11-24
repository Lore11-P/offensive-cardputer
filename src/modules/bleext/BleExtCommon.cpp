#include "BleExtInternal.h"

#if ENABLE_MODULE_BLEEXT

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Display.h"
#include "Logger.h"

namespace bleext {
namespace {

struct DeviceTable {
  AdvertRecord items[kMaxDevices];
  int count = 0;
};

DeviceTable g_table;
portMUX_TYPE g_tableMux = portMUX_INITIALIZER_UNLOCKED;

volatile AdvertHandler g_handler = nullptr;

// Lettura diretta delle strutture AD: evita le std::string allocate da
// getName()/getManufacturerData() nel callback del controller.
void buildRecord(NimBLEAdvertisedDevice* device, AdvertRecord& record) {
  record = AdvertRecord{};

  const NimBLEAddress address = device->getAddress();
  record.addressType = address.getType();
  const uint8_t* native = address.getNative();
  if (native != nullptr) {
    std::memcpy(record.native, native, sizeof(record.native));
  }
  formatAddress(record.native, record.address);

  record.advType = device->getAdvType();
  record.rssi = static_cast<int8_t>(device->getRSSI());
  record.bestRssi = record.rssi;
  record.connectable = device->isConnectable();

  const size_t length = device->getPayloadLength();
  // Lunghezza dichiarata (per la telemetria "troncati"), copia max 31 byte.
  record.payloadLen = static_cast<uint8_t>(std::min<size_t>(length, 255));
  const uint8_t* payload = (length > 0) ? device->getPayload() : nullptr;
  if (payload == nullptr) {
    return;
  }
  const size_t copyLen = std::min<size_t>(length, sizeof(record.payload));
  std::memcpy(record.payload, payload, copyLen);

  size_t i = 0;
  while (i + 1 < length) {
    const uint8_t fieldLen = payload[i];
    if (fieldLen == 0) {
      ++i;  // riempimento
      continue;
    }
    if (i + 1 + fieldLen > length) {
      break;
    }
    const uint8_t type = payload[i + 1];
    const uint8_t* data = payload + i + 2;
    const uint8_t dataLen = static_cast<uint8_t>(fieldLen - 1);

    if (type == 0x08 || type == 0x09) {
      const size_t nameLen = std::min<size_t>(dataLen, sizeof(record.name) - 1);
      std::memcpy(record.name, data, nameLen);
      record.name[nameLen] = '\0';
      for (size_t k = 0; k < nameLen; ++k) {
        const unsigned char c = static_cast<unsigned char>(record.name[k]);
        if (c < 0x20 || c > 0x7E) {
          record.name[k] = '?';
        }
      }
      record.hasName = nameLen > 0;
    } else if (type == 0xFF && dataLen >= 2) {
      record.companyId = static_cast<uint16_t>(data[0] | (data[1] << 8));
      // Bastano i primi 4 byte per AirTag/FindMy; il record e' x64.
      record.manufacturerLen =
          static_cast<uint8_t>(std::min<size_t>(dataLen, sizeof(record.manufacturer)));
      std::memcpy(record.manufacturer, data, record.manufacturerLen);
      record.hasManufacturer = record.manufacturerLen > 0;
    } else if (type == 0x02 || type == 0x03) {
      record.serviceUuidCount =
          static_cast<uint8_t>(record.serviceUuidCount + dataLen / 2);
      for (uint8_t k = 0; k + 1 < dataLen; k += 2) {
        const uint16_t uuid16 =
            static_cast<uint16_t>(data[k] | (data[k + 1] << 8));
        if (uuid16 == 0x1101) {  // Serial Port Profile
          record.hasSppUuid = true;
        }
      }
    } else if (type == 0x04 || type == 0x05) {
      record.serviceUuidCount =
          static_cast<uint8_t>(record.serviceUuidCount + dataLen / 4);
    } else if (type == 0x06 || type == 0x07) {
      record.serviceUuidCount =
          static_cast<uint8_t>(record.serviceUuidCount + dataLen / 16);
    }
    i += 1 + fieldLen;
  }
}

class ExtScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
public:
  void onResult(NimBLEAdvertisedDevice* device) override {
    AdvertHandler handler = g_handler;
    if (handler == nullptr || device == nullptr) {
      return;
    }
    AdvertRecord record;
    buildRecord(device, record);
    handler(record);
  }
};

// Oggetto statico, mai sullo stack: lo scanner globale conserva il puntatore.
ExtScanCallbacks g_callbacks;

DeviceSummary summarize(const AdvertRecord& record) {
  DeviceSummary summary;
  std::memcpy(summary.address, record.address, sizeof(summary.address));
  std::memcpy(summary.name, record.name, sizeof(summary.name));
  summary.addressType = record.addressType;
  summary.advType = record.advType;
  summary.serviceUuidCount = record.serviceUuidCount;
  summary.tags = record.tags;
  summary.rssi = record.rssi;
  summary.bestRssi = record.bestRssi;
  summary.hasName = record.hasName;
  summary.hasManufacturer = record.hasManufacturer;
  summary.connectable = record.connectable;
  summary.firstSeenMs = record.firstSeenMs;
  summary.lastSeenMs = record.lastSeenMs;
  summary.bestSeenMs = record.bestSeenMs;
  summary.hits = record.hits;
  return summary;
}

char lowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

}  // namespace

// Definito qui perche' tutte le schermate BLE+ lo usano a turno.
DeviceSummary g_deviceRows[kMaxDevices];

void ensureBle() {
  if (NimBLEDevice::getInitialized()) {
    return;
  }
  NimBLEDevice::init("CardputerFW");
  if (NimBLEDevice::getInitialized()) {
    logger::info("NimBLE: inizializzata, heap=%u",
                 static_cast<unsigned>(ESP.getFreeHeap()));
  } else {
    logger::error("NimBLE: init fallita");
  }
}

NimBLEScan* scanOrNull() {
  ensureBle();
  if (!NimBLEDevice::getInitialized()) {
    return nullptr;
  }
  return NimBLEDevice::getScan();
}

NimBLEAdvertisedDeviceCallbacks* scanCallbacks() {
  return &g_callbacks;
}

void setScanHandler(AdvertHandler handler) {
  g_handler = handler;
}

void clearScanHandler() {
  g_handler = nullptr;
}

void resetDevices() {
  portENTER_CRITICAL(&g_tableMux);
  g_table.count = 0;
  portEXIT_CRITICAL(&g_tableMux);
}

bool ingestDevice(const AdvertRecord& record) {
  const uint32_t now = millis();
  bool accepted = true;

  portENTER_CRITICAL(&g_tableMux);
  for (int i = 0; i < g_table.count; ++i) {
    AdvertRecord& existing = g_table.items[i];
    if (std::strcmp(existing.address, record.address) != 0) {
      continue;
    }
    existing.rssi = record.rssi;
    existing.advType = record.advType;
    existing.addressType = record.addressType;
    existing.connectable = record.connectable;
    existing.serviceUuidCount = record.serviceUuidCount;
    existing.tags |= record.tags;
    existing.lastSeenMs = now;
    ++existing.hits;
    if (record.hasName) {
      std::memcpy(existing.name, record.name, sizeof(existing.name));
      existing.hasName = true;
    }
    if (record.hasManufacturer) {
      existing.companyId = record.companyId;
      existing.manufacturerLen = record.manufacturerLen;
      std::memcpy(existing.manufacturer, record.manufacturer,
                  record.manufacturerLen);
      existing.hasManufacturer = true;
    }
    if (record.rssi > existing.bestRssi) {
      existing.bestRssi = record.rssi;
      existing.bestSeenMs = now;
    }
    portEXIT_CRITICAL(&g_tableMux);
    return true;
  }

  if (g_table.count < kMaxDevices) {
    AdvertRecord& slot = g_table.items[g_table.count];
    slot = record;
    slot.firstSeenMs = now;
    slot.lastSeenMs = now;
    slot.bestSeenMs = now;
    slot.hits = 1;
    if (slot.bestRssi < slot.rssi) {
      slot.bestRssi = slot.rssi;
    }
    ++g_table.count;
  } else {
    accepted = false;
  }
  portEXIT_CRITICAL(&g_tableMux);
  return accepted;
}

int deviceCount() {
  portENTER_CRITICAL(&g_tableMux);
  const int count = g_table.count;
  portEXIT_CRITICAL(&g_tableMux);
  return count;
}

int snapshotSummaries(DeviceSummary* out, int maxOut, bool strongestFirst) {
  if (out == nullptr || maxOut <= 0) {
    return 0;
  }
  portENTER_CRITICAL(&g_tableMux);
  const int count = std::min(g_table.count, maxOut);
  for (int i = 0; i < count; ++i) {
    out[i] = summarize(g_table.items[i]);
  }
  portEXIT_CRITICAL(&g_tableMux);

  if (strongestFirst) {
    std::sort(out, out + count, [](const DeviceSummary& a,
                                   const DeviceSummary& b) {
      return a.bestRssi > b.bestRssi;
    });
  }
  return count;
}

void configureScan(NimBLEScan* scan, bool active) {
  if (scan == nullptr) {
    return;
  }
  scan->stop();  // eventuale finestra precedente rimasta appesa
  scan->setAdvertisedDeviceCallbacks(scanCallbacks(), true);
  scan->setActiveScan(active);
  scan->setInterval(100);
  scan->setWindow(active ? 99 : 100);
  // La lista e' gestita da noi: lo stack non trattiene risultati.
  scan->setMaxResults(0);
}

void stopScan(NimBLEScan* scan) {
  if (scan != nullptr) {
    scan->stop();
    scan->setAdvertisedDeviceCallbacks(nullptr, false);
    scan->clearResults();
  }
  clearScanHandler();
}

navigation::EventData runScanWindow(NimBLEScan* scan, uint32_t durationMs,
                                    const std::function<void()>& draw,
                                    uint32_t drawEveryMs) {
  navigation::EventData relevant;
  if (scan == nullptr) {
    return relevant;
  }
  const uint32_t durationSec = std::max<uint32_t>(1, durationMs / 1000);
  scan->start(durationSec, nullptr, false);
  const uint32_t deadline = millis() + durationMs + 1500;
  uint32_t lastDraw = 0;

  while (millis() < deadline) {
    navigation::update();
    const navigation::EventData ev = navigation::poll();
    if (ev.type == navigation::Event::Back) {
      relevant = ev;
      break;
    }
    if (ev.type == navigation::Event::Select ||
        ev.type == navigation::Event::Left ||
        ev.type == navigation::Event::Right) {
      relevant = ev;
    }
    if (draw && (millis() - lastDraw) >= drawEveryMs) {
      lastDraw = millis();
      draw();
    }
    if (!scan->isScanning()) {
      break;
    }
    delay(10);
  }
  if (scan->isScanning()) {
    scan->stop();
  }
  if (!scan->isScanning()) {
    // Evita un ciclo stretto se lo stack rifiuta di ripartire.
    delay(20);
  }
  return relevant;
}

std::string shortenText(const std::string& text, size_t maxLen) {
  if (text.size() <= maxLen || maxLen < 2) {
    return text;
  }
  return text.substr(0, maxLen - 1) + "~";
}

std::string formatAge(uint32_t ageMs) {
  const uint32_t seconds = ageMs / 1000;
  if (seconds < 60) {
    return std::to_string(seconds) + "s";
  }
  const uint32_t minutes = seconds / 60;
  if (minutes < 60) {
    return std::to_string(minutes) + "m" + std::to_string(seconds % 60) + "s";
  }
  return std::to_string(minutes / 60) + "h" +
         std::to_string(minutes % 60) + "m";
}

void formatAddress(const uint8_t* native, char out[18]) {
  if (native == nullptr || out == nullptr) {
    return;
  }
  std::snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", native[5],
                native[4], native[3], native[2], native[1], native[0]);
}

bool containsNoCase(const char* haystack, const char* needle) {
  if (haystack == nullptr || needle == nullptr || needle[0] == '\0') {
    return false;
  }
  for (const char* h = haystack; *h != '\0'; ++h) {
    const char* a = h;
    const char* b = needle;
    while (*a != '\0' && *b != '\0' &&
           lowerAscii(*a) == lowerAscii(*b)) {
      ++a;
      ++b;
    }
    if (*b == '\0') {
      return true;
    }
  }
  return false;
}

uint8_t advEventToPdu(uint8_t eventType) {
  switch (eventType) {
    case 0:
      return 0;  // ADV_IND
    case 1:
      return 1;  // ADV_DIRECT_IND
    case 2:
      return 3;  // HCI ADV_SCAN_IND -> PDU ADV_SCAN_IND
    case 3:
      return 2;  // HCI ADV_NONCONN_IND -> PDU ADV_NONCONN_IND
    case 4:
      return 4;  // SCAN_RSP
    default:
      return 0;
  }
}

uint16_t rssiColor(int rssi) {
  if (rssi > -48) {
    return display::kDanger;
  }
  if (rssi > -62) {
    return display::kWarning;
  }
  if (rssi > -78) {
    return display::kAccent;
  }
  return display::kInfo;
}

float rssiFraction(int rssi) {
  const int clamped =
      std::max(kRssiFloor, std::min(rssi, kRssiCeil));
  return static_cast<float>(clamped - kRssiFloor) /
         static_cast<float>(kRssiCeil - kRssiFloor);
}

int rssiToMeters(int rssi) {
  if (rssi <= kRssiFloor) {
    return 90;
  }
  constexpr float kRssiAtOneMeter = -59.0f;
  constexpr float kPathLossExponent = 2.2f;
  const float ratio =
      (kRssiAtOneMeter - static_cast<float>(rssi)) /
      (10.0f * kPathLossExponent);
  const float meters = std::pow(10.0f, ratio);
  return static_cast<int>(std::min(meters, 90.0f) + 0.5f);
}

int beepIntervalMs(int level, int rssi) {
  const int clampedLevel = std::max(1, std::min(level, 5));
  const float slope =
      8.0f - static_cast<float>(clampedLevel - 1) * 1.5f;
  const float raw =
      20.0f + static_cast<float>(kRssiCeil - rssi) * slope;
  return std::max(20, std::min(static_cast<int>(raw + 0.5f), 900));
}

void drawSegmentBar(int x, int y, int w, int h, int segments, float fraction,
                    float peak, uint16_t color) {
  const int gap = 2;
  const int segmentWidth =
      std::max(2, (w - (segments - 1) * gap) / segments);
  const int lit = std::max(
      0, std::min(static_cast<int>(fraction * segments + 0.5f), segments));
  for (int i = 0; i < segments; ++i) {
    const int sx = x + i * (segmentWidth + gap);
    const uint16_t fill = (i < lit) ? color : display::kPanel;
    display::g().fillRect(sx, y, segmentWidth, h, fill);
  }
  const int peakIndex =
      std::max(0, std::min(static_cast<int>(peak * segments), segments - 1));
  display::g().fillRect(x + peakIndex * (segmentWidth + gap), y - 3,
                        segmentWidth, 2, display::kWhite);
}

int chooseList(const std::string& title, const std::string& parent,
               const std::vector<std::string>& labels,
               const std::string& hint) {
  if (labels.empty()) {
    return -1;
  }
  constexpr int kHeaderH = 22;
  constexpr int kFooterH = 12;
  constexpr int kRowH = 18;
  const int visible =
      std::max(1, (display::height() - kHeaderH - kFooterH) / kRowH);
  const int total = static_cast<int>(labels.size());
  int selected = 0;
  int top = 0;

  navigation::waitRelease();
  for (;;) {
    navigation::update();
    const navigation::EventData ev = navigation::poll();
    if (ev.type == navigation::Event::Back) {
      navigation::waitRelease();
      return -1;
    }
    if (ev.type == navigation::Event::Select) {
      navigation::waitRelease();
      return selected;
    }
    if (ev.type == navigation::Event::Up) {
      selected = (selected + total - 1) % total;
    } else if (ev.type == navigation::Event::Down) {
      selected = (selected + 1) % total;
    }
    if (selected < top) {
      top = selected;
    } else if (selected >= top + visible) {
      top = selected - visible + 1;
    }

    display::clear();
    display::header(title, parent);
    for (int row = 0; row < visible; ++row) {
      const int index = top + row;
      if (index >= total) {
        break;
      }
      const int y = kHeaderH + row * kRowH;
      const bool isSelected = (index == selected);
      if (isSelected) {
        display::g().fillRoundRect(2, y + 1, display::width() - 6,
                                   kRowH - 2, 3, display::kAccentDark);
        display::g().fillRect(2, y + 4, 2, kRowH - 8, display::kAccent);
      }
      display::text(10, y + 3, shortenText(labels[index], 36),
                    isSelected ? display::kWhite : display::kText, 2);
    }
    const int trackX = display::width() - 4;
    const int trackY = kHeaderH;
    const int trackH = display::height() - kHeaderH - kFooterH;
    display::g().fillRect(trackX, trackY, 3, trackH, display::kPanel);
    if (total > visible) {
      const int thumbH = std::max(10, trackH * visible / total);
      const int thumbY =
          trackY + (trackH - thumbH) * top / std::max(1, total - visible);
      display::g().fillRect(trackX, thumbY, 3, thumbH, display::kAccent);
    }
    display::footer(hint);
    display::push();
    delay(16);
  }
}

}  // namespace bleext

#endif  // ENABLE_MODULE_BLEEXT
