#include "BleExtInternal.h"

#if ENABLE_MODULE_BLEEXT

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_timer.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "Display.h"
#include "Logger.h"
#include "Navigation.h"
#include "PowerTimer.h"
#include "Storage.h"
#include "Ui.h"

namespace bleext {
namespace {

constexpr uint32_t kCaptureMs = 30000;
constexpr size_t kMaxFileBytes = 8 * 1024 * 1024;
constexpr int kRingSlots = 64;
constexpr int kMaxAdvData = 31;  // payload legacy: PDU length 6..37

struct CaptureSlot {
  uint64_t tsUs = 0;
  char address[18] = {0};
  uint8_t native[6] = {0};
  uint8_t addressType = 0;
  uint8_t advType = 0;
  int8_t rssi = -127;
  uint8_t length = 0;
  uint8_t data[kMaxAdvData] = {0};
};

CaptureSlot g_ring[kRingSlots];
portMUX_TYPE g_ringMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint8_t g_head = 0;
volatile uint8_t g_tail = 0;
volatile uint8_t g_count = 0;
volatile uint32_t g_dropped = 0;
volatile uint32_t g_truncated = 0;

uint32_t g_written = 0;
uint32_t g_csvBytes = 0;
uint32_t g_pcapBytes = 0;
bool g_limitHit = false;
bool g_writeError = false;

struct PcapGlobalHeader {
  uint32_t magic;
  uint16_t versionMajor;
  uint16_t versionMinor;
  int32_t thisZone;
  uint32_t sigFigs;
  uint32_t snapLen;
  uint32_t network;
} __attribute__((packed));

struct PcapPacketHeader {
  uint32_t tsSec;
  uint32_t tsUsec;
  uint32_t inclLen;
  uint32_t origLen;
} __attribute__((packed));

void captureHandler(const AdvertRecord& record) {
  CaptureSlot slot;
  slot.tsUs = static_cast<uint64_t>(esp_timer_get_time());
  std::memcpy(slot.address, record.address, sizeof(slot.address));
  std::memcpy(slot.native, record.native, sizeof(slot.native));
  slot.addressType = record.addressType;
  slot.advType = record.advType;
  slot.rssi = record.rssi;
  slot.length = static_cast<uint8_t>(
      std::min<uint16_t>(record.payloadLen, kMaxAdvData));
  std::memcpy(slot.data, record.payload, slot.length);

  portENTER_CRITICAL(&g_ringMux);
  if (g_count < kRingSlots) {
    g_ring[g_head] = slot;
    g_head = static_cast<uint8_t>((g_head + 1) % kRingSlots);
    ++g_count;
  } else {
    ++g_dropped;
  }
  if (record.payloadLen > kMaxAdvData) {
    ++g_truncated;
  }
  portEXIT_CRITICAL(&g_ringMux);
}

bool popSlot(CaptureSlot& out) {
  bool have = false;
  portENTER_CRITICAL(&g_ringMux);
  if (g_count > 0) {
    out = g_ring[g_tail];
    g_tail = static_cast<uint8_t>((g_tail + 1) % kRingSlots);
    --g_count;
    have = true;
  }
  portEXIT_CRITICAL(&g_ringMux);
  return have;
}

void toHex(const uint8_t* data, size_t length, char* out, size_t outSize) {
  static const char kDigits[] = "0123456789abcdef";
  size_t j = 0;
  for (size_t i = 0; i < length && (j + 2) < outSize; ++i) {
    out[j++] = kDigits[data[i] >> 4];
    out[j++] = kDigits[data[i] & 0x0F];
  }
  out[j] = '\0';
}

bool writeCaptureHeaders(File& csv, File& pcap) {
  const char* csvHeader =
      "timestamp_us,mac,adv_type,rssi,channel,length,hex_payload\n";
  if (csv.write(reinterpret_cast<const uint8_t*>(csvHeader),
                std::strlen(csvHeader)) != std::strlen(csvHeader)) {
    return false;
  }
  g_csvBytes = static_cast<uint32_t>(std::strlen(csvHeader));

  // Linktype 251 (BLUETOOTH_LE_LL): qui viene scritto il frame grezzo
  // AA + PDU senza pseudo-header, come da specifica del modulo.
  PcapGlobalHeader global = {};
  global.magic = 0xA1B2C3D4;
  global.versionMajor = 2;
  global.versionMinor = 4;
  global.thisZone = 0;
  global.sigFigs = 0;
  global.snapLen = 64;
  global.network = 251;
  if (pcap.write(reinterpret_cast<const uint8_t*>(&global),
                 sizeof(global)) != sizeof(global)) {
    return false;
  }
  g_pcapBytes = sizeof(global);
  return true;
}

bool writeCaptureSlot(File& csv, File& pcap, const CaptureSlot& slot) {
  char hex[kMaxAdvData * 2 + 1];
  toHex(slot.data, slot.length, hex, sizeof(hex));

  char line[160];
  const int lineLen = std::snprintf(
      line, sizeof(line), "%llu,%s,%u,%d,-1,%u,%s\n",
      static_cast<unsigned long long>(slot.tsUs), slot.address,
      static_cast<unsigned>(advEventToPdu(slot.advType)),
      static_cast<int>(slot.rssi), static_cast<unsigned>(slot.length), hex);
  if (lineLen <= 0) {
    return false;
  }

  // Frame BLE LL: access address, header PDU, lunghezza, AdvA, payload.
  uint8_t frame[64];
  frame[0] = 0xD6;
  frame[1] = 0xBE;
  frame[2] = 0x89;
  frame[3] = 0x8E;
  const uint8_t pduType = advEventToPdu(slot.advType);
  // bit 0-3 tipo PDU, bit 4 RFU, bit 5 ChSel, bit 6 TxAdd, bit 7 RxAdd.
  frame[4] = static_cast<uint8_t>(
      (pduType & 0x0F) | ((slot.addressType & 0x01) << 6));
  frame[5] = static_cast<uint8_t>(6 + slot.length);
  std::memcpy(&frame[6], slot.native, sizeof(slot.native));
  std::memcpy(&frame[12], slot.data, slot.length);
  const uint32_t frameLen = 12U + slot.length;

  PcapPacketHeader packet = {};
  packet.tsSec = static_cast<uint32_t>(slot.tsUs / 1000000ULL);
  packet.tsUsec = static_cast<uint32_t>(slot.tsUs % 1000000ULL);
  packet.inclLen = frameLen;
  packet.origLen = frameLen;

  const uint32_t csvAdd = static_cast<uint32_t>(lineLen);
  const uint32_t pcapAdd = sizeof(packet) + frameLen;
  if ((g_csvBytes + csvAdd) > kMaxFileBytes ||
      (g_pcapBytes + pcapAdd) > kMaxFileBytes) {
    g_limitHit = true;
    return false;
  }

  if (csv.write(reinterpret_cast<const uint8_t*>(line),
                static_cast<size_t>(lineLen)) !=
      static_cast<size_t>(lineLen)) {
    g_writeError = true;
    return false;
  }
  if (pcap.write(reinterpret_cast<const uint8_t*>(&packet),
                 sizeof(packet)) != sizeof(packet) ||
      pcap.write(frame, frameLen) != frameLen) {
    g_writeError = true;
    return false;
  }

  g_csvBytes += csvAdd;
  g_pcapBytes += pcapAdd;
  ++g_written;
  return true;
}

bool drainCapture(File& csv, File& pcap) {
  for (int i = 0; i < 32; ++i) {
    CaptureSlot slot;
    if (!popSlot(slot)) {
      break;
    }
    if (!writeCaptureSlot(csv, pcap, slot)) {
      return false;
    }
  }
  return true;
}

void drawCapture(uint32_t elapsed, const char* csvPath, const char* pcapPath) {
  display::clear();
  display::header("Cattura BLE", "BLE+");
  display::text(6, 24, "Ascolto BLE passivo 30 s", display::kAccent, 0);
  display::progressBar(20, 38, 200, 8,
                       std::min(1.0f, static_cast<float>(elapsed) /
                                          static_cast<float>(kCaptureMs)),
                       display::kInfo);
  display::text(8, 54, "Scritti: " + std::to_string(g_written),
                display::kText, 2);
  display::text(8, 72,
                "CSV " + std::to_string(g_csvBytes / 1024) + "K  PCAP " +
                    std::to_string(g_pcapBytes / 1024) + "K",
                display::kTextDim, 0);
  display::text(8, 82,
                "Persi: " + std::to_string(g_dropped) + "  Troncati: " +
                    std::to_string(g_truncated),
                display::kTextDim, 0);
  display::text(8, 94, "Canale: n/d (NimBLE 1.4.3)",
                display::kTextDim, 0);
  display::text(8, 104, std::string("CSV: ") + csvPath,
                display::kTextDim, 0);
  display::text(8, 114, std::string("PCAP: ") + pcapPath,
                display::kTextDim, 0);
  display::footer("` = interrompi");
  display::push();
}

void resetRing() {
  portENTER_CRITICAL(&g_ringMux);
  g_head = 0;
  g_tail = 0;
  g_count = 0;
  g_dropped = 0;
  g_truncated = 0;
  portEXIT_CRITICAL(&g_ringMux);
  g_written = 0;
  g_csvBytes = 0;
  g_pcapBytes = 0;
  g_limitHit = false;
  g_writeError = false;
}

}  // namespace

void runBleCapture() {
  powertimer::Hold hold;
  if (!storage::hasSD()) {
    ui::message("Cattura BLE",
                {"!Serve una microSD per salvare i file."}, "BLE+");
    return;
  }
  storage::mkdir("/ble", true);

  char csvPath[48];
  char pcapPath[48];
  const unsigned long token = static_cast<unsigned long>(esp_random());
  std::snprintf(csvPath, sizeof(csvPath), "/ble/ble_capture_%08lX.csv",
                token);
  std::snprintf(pcapPath, sizeof(pcapPath), "/ble/ble_capture_%08lX.pcap",
                token);

  File csv = storage::openWrite(csvPath, true);
  File pcap = storage::openWrite(pcapPath, true);
  if (!csv || !pcap) {
    if (csv) {
      csv.close();
    }
    if (pcap) {
      pcap.close();
    }
    storage::removeFile(csvPath, true);
    storage::removeFile(pcapPath, true);
    ui::message("Cattura BLE",
                {"!Impossibile creare i file.", "Controlla la microSD."},
                "BLE+");
    return;
  }

  resetRing();
  if (!writeCaptureHeaders(csv, pcap)) {
    csv.close();
    pcap.close();
    storage::removeFile(csvPath, true);
    storage::removeFile(pcapPath, true);
    ui::message("Cattura BLE", {"!Scrittura header fallita."}, "BLE+");
    return;
  }

  NimBLEScan* scan = scanOrNull();
  if (scan == nullptr) {
    csv.close();
    pcap.close();
    ui::message("Cattura BLE", {"!Stack BLE non disponibile."}, "BLE+");
    return;
  }

  setScanHandler(captureHandler);
  configureScan(scan, false);  // passivo: preserva il PDU originale

  scan->start(30, nullptr, false);
  delay(50);
  if (!scan->isScanning()) {
    stopScan(scan);
    csv.close();
    pcap.close();
    ui::message("Cattura BLE", {"!Avvio scansione fallito."}, "BLE+");
    return;
  }

  navigation::waitRelease();
  const uint32_t start = millis();
  uint32_t lastDraw = 0;
  uint32_t lastFlush = start;

  for (;;) {
    navigation::update();
    if (navigation::poll().type == navigation::Event::Back) {
      break;
    }
    if (!drainCapture(csv, pcap)) {
      break;  // limite 8 MB o errore SD
    }
    const uint32_t now = millis();
    if (now - start >= kCaptureMs) {
      break;
    }
    if (!scan->isScanning() && (now - start) > 2000) {
      break;
    }
    if (now - lastDraw > 200) {
      drawCapture(now - start, csvPath, pcapPath);
      lastDraw = now;
    }
    if (now - lastFlush > 1000) {
      csv.flush();
      pcap.flush();
      lastFlush = now;
    }
    delay(5);
  }

  stopScan(scan);  // stacca i callback: nessun nuovo slot in coda
  drainCapture(csv, pcap);  // svuota la coda residua
  csv.flush();
  pcap.flush();
  csv.close();
  pcap.close();

  logger::info("Cattura BLE: %u pacchetti, %u persi, CSV=%u B PCAP=%u B",
               static_cast<unsigned>(g_written),
               static_cast<unsigned>(g_dropped),
               static_cast<unsigned>(g_csvBytes),
               static_cast<unsigned>(g_pcapBytes));

  std::vector<std::string> lines;
  lines.push_back(std::string("CSV: ") + csvPath);
  lines.push_back(std::string("PCAP: ") + pcapPath);
  lines.push_back("Pacchetti: " + std::to_string(g_written) + "  persi: " +
                  std::to_string(g_dropped));
  lines.push_back("Troncati >31B: " + std::to_string(g_truncated));
  lines.push_back("Canale: n/d (NimBLE 1.4.3)");
  if (g_limitHit) {
    lines.push_back("!Limite 8 MB raggiunto.");
  }
  if (g_writeError) {
    lines.push_back("!Errore scrittura microSD.");
  }
  ui::message("Cattura BLE", lines, "BLE+");
  navigation::waitRelease();
}

}  // namespace bleext

#endif  // ENABLE_MODULE_BLEEXT
