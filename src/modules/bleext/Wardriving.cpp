#include "BleExtInternal.h"

#if ENABLE_MODULE_BLEEXT

#include <Arduino.h>
#include <SD.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "Display.h"
#include "Logger.h"
#include "MenuManager.h"
#include "Navigation.h"
#include "PowerTimer.h"
#include "Storage.h"
#include "Ui.h"

namespace bleext {
namespace {

constexpr uint32_t kFlushIntervalMs = 5000;

volatile uint32_t g_packets = 0;
volatile uint32_t g_dropped = 0;

char g_path[48] = {0};
uint32_t g_startMs = 0;
uint32_t g_durationMs = 0;
uint32_t g_lastFlushMs = 0;
// Righe gia' scritte nel CSV: il file viene solo accodato, mai riscritto.
int g_flushedRows = 0;

std::string csvEscapeName(const char* name) {
  std::string clean;
  for (const char* p = name; *p != '\0'; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    clean.push_back((c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '?');
  }
  if (clean.find_first_of(",\"") != std::string::npos) {
    std::string quoted = "\"";
    for (char c : clean) {
      if (c == '"') {
        quoted += "\"\"";
      } else {
        quoted += c;
      }
    }
    quoted += '"';
    return quoted;
  }
  return clean;
}

void wardrivingHandler(const AdvertRecord& record) {
  ++g_packets;
  if (!ingestDevice(record)) {
    ++g_dropped;
  }
}

// storage::openWrite apre in FILE_WRITE, cioe' tronca: riscrivere tutto il CSV
// ogni 5 s perde il log se il device si spegne a meta'. La SD e' montata su
// "/sd" con il path logico passato alla VFS (storage::sdPath non aggiunge il
// mountpoint): FILE_APPEND accoda solo le righe dei MAC non ancora scritti.
bool writeWardrivingCsv() {
  if (g_path[0] == '\0') {
    return false;
  }
  const bool create = !storage::exists(g_path, true);
  File file;
  if (create) {
    file = storage::openWrite(g_path, true);
  } else if (storage::hasSD()) {
    file = SD.open(g_path, FILE_APPEND);
  }
  if (!file) {
    return false;
  }

  const char* header =
      "timestamp_ms,mac,name,vendor_oui,rssi,adv_type,service_uuid_count\n";
  bool ok = !create || file.print(header) == std::strlen(header);

  const int count = snapshotSummaries(g_deviceRows, kMaxDevices, false);
  if (g_flushedRows > count) {
    g_flushedRows = count;
  }
  for (int i = g_flushedRows; ok && i < count; ++i) {
    const std::string name = g_deviceRows[i].hasName
                                 ? csvEscapeName(g_deviceRows[i].name)
                                 : std::string();
    const std::string oui(g_deviceRows[i].address, 8);
    char line[160];
    std::snprintf(line, sizeof(line), "%lu,%s,%s,%s,%d,%u,%u\n",
                  static_cast<unsigned long>(g_deviceRows[i].bestSeenMs),
                  g_deviceRows[i].address, name.c_str(), oui.c_str(),
                  static_cast<int>(g_deviceRows[i].bestRssi),
                  static_cast<unsigned>(advEventToPdu(g_deviceRows[i].advType)),
                  static_cast<unsigned>(g_deviceRows[i].serviceUuidCount));
    if (file.print(line) != std::strlen(line)) {
      ok = false;
      break;
    }
  }
  file.flush();
  file.close();
  if (ok) {
    g_flushedRows = count;
  }
  return ok;
}

void drawWardriving() {
  const uint32_t now = millis();
  const uint32_t elapsed = now - g_startMs;
  const float fraction =
      g_durationMs == 0
          ? 0.0f
          : std::min(1.0f, static_cast<float>(elapsed) /
                               static_cast<float>(g_durationMs));

  display::clear();
  display::header("Wardriving", "BLE+");
  display::text(6, 24, "Scansione passiva", display::kAccent, 0);
  display::progressBar(20, 38, 200, 8, fraction, display::kInfo);
  display::text(8, 54, "MAC unici: " + std::to_string(deviceCount()),
                display::kText, 2);
  display::text(8, 72,
                "Pacchetti: " + std::to_string(g_packets),
                display::kText, 0);
  display::text(8, 82,
                "Scartati: " + std::to_string(g_dropped) + " (max 64)",
                display::kTextDim, 0);
  display::text(8, 94, std::string("File: ") + g_path, display::kTextDim, 0);
  display::text(8, 106,
                "Tempo: " + std::to_string(elapsed / 1000) + "s / " +
                    std::to_string(g_durationMs / 1000) + "s",
                display::kTextDim, 0);
  display::footer("ESC = interrompi");
  display::push();
}

void runWardriving(uint32_t seconds) {
  powertimer::Hold hold;
  if (!storage::hasSD()) {
    ui::message("Wardriving",
                {"!Serve una microSD per salvare il CSV."}, "BLE+");
    return;
  }
  storage::mkdir("/ble", true);

  std::snprintf(g_path, sizeof(g_path), "/ble/ble_wardriving_%08lX.csv",
                static_cast<unsigned long>(esp_random()));
  g_startMs = millis();
  g_durationMs = seconds * 1000UL;
  g_lastFlushMs = g_startMs;
  g_packets = 0;
  g_dropped = 0;
  g_flushedRows = 0;
  // Azzera la tabella prima della prima scrittura: nessun device della
  // sessione precedente deve finire nel nuovo CSV.
  resetDevices();

  if (!writeWardrivingCsv()) {
    ui::message("Wardriving",
                {"!Impossibile creare il file CSV.", "Controlla la microSD."},
                "BLE+");
    return;
  }

  NimBLEScan* scan = scanOrNull();
  if (scan == nullptr) {
    ui::message("Wardriving", {"!Stack BLE non disponibile."}, "BLE+");
    return;
  }

  setScanHandler(wardrivingHandler);
  configureScan(scan, false);  // passivo: solo advertising osservato

  scan->start(seconds, nullptr, false);
  delay(50);
  if (!scan->isScanning()) {
    stopScan(scan);
    ui::message("Wardriving", {"!Avvio scansione fallito."}, "BLE+");
    return;
  }

  navigation::waitRelease();
  bool writeError = false;
  uint32_t lastDraw = 0;
  for (;;) {
    navigation::update();
    if (navigation::poll().type == navigation::Event::Back) {
      break;
    }
    const uint32_t now = millis();
    if (now - g_startMs >= g_durationMs) {
      break;
    }
    if (!scan->isScanning() && (now - g_startMs) > 2000) {
      break;
    }
    if (now - lastDraw > 200) {
      drawWardriving();
      lastDraw = now;
    }
    // Riscrittura periodica: una sola riga per MAC con l'RSSI piu' forte.
    if (now - g_lastFlushMs >= kFlushIntervalMs) {
      if (!writeWardrivingCsv()) {
        writeError = true;
        break;
      }
      g_lastFlushMs = now;
    }
    delay(20);
  }

  stopScan(scan);  // stacca i callback prima dello snapshot finale
  const bool finalWrite = writeWardrivingCsv();
  drawWardriving();

  logger::info("Wardriving: %u pacchetti, %d MAC, %u scartati, heap=%u",
               static_cast<unsigned>(g_packets), deviceCount(),
               static_cast<unsigned>(g_dropped),
               static_cast<unsigned>(ESP.getFreeHeap()));

  std::vector<std::string> lines;
  lines.push_back(std::string("File: ") + g_path);
  lines.push_back("MAC unici: " + std::to_string(deviceCount()));
  lines.push_back("Pacchetti: " + std::to_string(g_packets));
  lines.push_back("Scartati max 64: " + std::to_string(g_dropped));
  if (writeError || !finalWrite) {
    lines.push_back("!Errore scrittura sulla microSD.");
  }
  ui::message("Wardriving", lines, "BLE+");
  navigation::waitRelease();
}

}  // namespace

void runWardrivingMenu() {
  MenuManager menu("Wardriving", "BLE+");
  menu.addItem("30 secondi", "sd", [] { runWardriving(30); });
  menu.addItem("60 secondi", "sd", [] { runWardriving(60); });
  menu.addItem("120 secondi", "sd", [] { runWardriving(120); });
  menu.setHint("ESC=indietro  ENTER=avvia");
  menu.run(true);
}

}  // namespace bleext

#endif  // ENABLE_MODULE_BLEEXT
