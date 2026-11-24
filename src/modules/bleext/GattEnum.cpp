#include "BleExtInternal.h"

#if ENABLE_MODULE_BLEEXT

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Display.h"
#include "Logger.h"
#include "Navigation.h"
#include "PowerTimer.h"
#include "Ui.h"

namespace bleext {
namespace {

constexpr uint32_t kDiscoveryMs = 6000;
constexpr int kMaxGattLines = 40;
constexpr int kMaxServices = 16;
constexpr uint32_t kWorkerStack = 8192;

struct GattLine {
  char text[48] = {0};
  uint8_t kind = 0;  // 0 = servizio, 1 = caratteristica
};

GattLine g_lines[kMaxGattLines];
int g_lineCount = 0;
// Il worker di enumerazione puo' restare vivo dopo un timeout della UI: le
// voci condivise vanno sempre lette e scritte sotto lo stesso lock, altrimenti
// un worker orfano puo' sforare il limite o far leggere una riga a meta'.
SemaphoreHandle_t g_linesMtx = nullptr;
// true finche' il worker precedente non ha davvero chiuso il client.
volatile bool g_jobActive = false;

void linesLock() {
  if (g_linesMtx == nullptr) {
    g_linesMtx = xSemaphoreCreateMutex();
  }
  if (g_linesMtx != nullptr) {
    xSemaphoreTake(g_linesMtx, portMAX_DELAY);
  }
}

void linesUnlock() {
  if (g_linesMtx != nullptr) {
    xSemaphoreGive(g_linesMtx);
  }
}

struct GattJob {
  DeviceSummary target;
  volatile bool done = false;
  volatile bool abortRequested = false;
  volatile bool connectOk = false;
  volatile bool servicesOk = false;
  volatile bool aborted = false;
  volatile bool truncated = false;
  volatile int phase = 0;  // 0 connessione, 1 enumerazione, 2 chiusura
  volatile int serviceCount = 0;
  volatile int charCount = 0;
  volatile int lineCount = 0;
  volatile uint32_t heapAfter = 0;
};

GattJob g_job;

void gattScanHandler(const AdvertRecord& record) {
  ingestDevice(record);
}

std::string propertyString(NimBLERemoteCharacteristic* characteristic) {
  std::string properties;
  if (characteristic->canRead()) {
    properties += "R";
  }
  if (characteristic->canWrite()) {
    properties += "W";
  }
  if (characteristic->canWriteNoResponse()) {
    properties += "w";
  }
  if (characteristic->canNotify()) {
    properties += "N";
  }
  if (characteristic->canIndicate()) {
    properties += "I";
  }
  if (characteristic->canBroadcast()) {
    properties += "B";
  }
  if (properties.empty()) {
    properties = "-";
  }
  return properties;
}

void appendLine(const std::string& text, uint8_t kind) {
  linesLock();
  if (g_lineCount >= kMaxGattLines) {
    linesUnlock();
    return;
  }
  std::snprintf(g_lines[g_lineCount].text,
                sizeof(g_lines[g_lineCount].text), "%s", text.c_str());
  g_lines[g_lineCount].kind = kind;
  ++g_lineCount;
  g_job.lineCount = g_lineCount;
  linesUnlock();
}

bool releaseClient(NimBLEClient* client) {
  if (client == nullptr) {
    return true;
  }
  // deleteClient disconnette e libera servizi/client. Se il controller non
  // completa la disconnessione si ritenta: non deve restare un client appeso.
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (NimBLEDevice::deleteClient(client)) {
      return true;
    }
    delay(150);
  }
  logger::warn("GATT: deleteClient fallito");
  return false;
}

// Tutta la sessione client (connessione, scoperta, disconnessione) gira in
// questo task: NimBLEClient 1.4.3 e' sincrono e bloccherebbe la UI.
void gattWorker(void*) {
  NimBLEClient* client = NimBLEDevice::createClient();
  if (client == nullptr) {
    g_job.phase = 2;
    g_job.heapAfter = ESP.getFreeHeap();
    g_job.done = true;
    g_jobActive = false;
    vTaskDelete(nullptr);
    return;
  }
  client->setConnectTimeout(10);

  if (g_job.abortRequested) {
    g_job.aborted = true;
    releaseClient(client);
    g_job.phase = 2;
    g_job.heapAfter = ESP.getFreeHeap();
    g_job.done = true;
    g_jobActive = false;
    vTaskDelete(nullptr);
    return;
  }

  const NimBLEAddress address(std::string(g_job.target.address),
                              g_job.target.addressType);
  const bool connected = client->connect(address, true);
  g_job.connectOk = connected;

  if (!connected || g_job.abortRequested) {
    g_job.aborted = g_job.abortRequested;
    if (connected) {
      client->disconnect();
    }
    releaseClient(client);
    g_job.phase = 2;
    g_job.heapAfter = ESP.getFreeHeap();
    g_job.done = true;
    g_jobActive = false;
    vTaskDelete(nullptr);
    return;
  }

  g_job.phase = 1;
  bool truncated = false;
  std::vector<NimBLERemoteService*>* services = client->getServices(true);
  if (services != nullptr) {
    g_job.servicesOk = true;
    for (NimBLERemoteService* service : *services) {
      if (g_job.abortRequested) {
        break;
      }
      if (g_job.lineCount >= kMaxGattLines ||
          g_job.serviceCount >= kMaxServices) {
        truncated = true;
        break;
      }
      ++g_job.serviceCount;
      appendLine("S " + shortenText(service->getUUID().toString(), 36), 0);

      std::vector<NimBLERemoteCharacteristic*>* characteristics =
          service->getCharacteristics(true);
      if (characteristics != nullptr) {
        for (NimBLERemoteCharacteristic* characteristic : *characteristics) {
          if (g_job.abortRequested) {
            break;
          }
          if (g_job.lineCount >= kMaxGattLines) {
            truncated = true;
            break;
          }
          ++g_job.charCount;
          appendLine("  " + propertyString(characteristic) + " " +
                         shortenText(characteristic->getUUID().toString(), 28),
                     1);
        }
      }
    }
  }

  g_job.aborted = g_job.abortRequested;
  g_job.truncated = truncated;
  g_job.phase = 2;
  client->disconnect();
  releaseClient(client);
  g_job.heapAfter = ESP.getFreeHeap();
  g_job.done = true;
  g_jobActive = false;
  vTaskDelete(nullptr);
}

void drawDiscovery(uint32_t elapsed, int found) {
  display::clear();
  display::header("Servizi GATT", "BLE+");
  display::textCentered(36, "Ricerca dispositivi BLE", display::kAccent, 2);
  display::progressBar(20, 60, 200, 8,
                       std::min(1.0f, static_cast<float>(elapsed) /
                                          static_cast<float>(kDiscoveryMs)),
                       display::kInfo);
  display::textCentered(78, "Trovati: " + std::to_string(found),
                        display::kText, 2);
  display::footer("ESC = annulla");
  display::push();
}

void drawGattProgress(const DeviceSummary& target, uint32_t elapsedMs,
                      bool cancelRequested) {
  display::clear();
  display::header("Servizi GATT", "BLE+");
  display::text(4, 23, target.address, display::kTextDim, 0);

  std::string line;
  uint16_t color = display::kText;
  if (g_job.phase == 0) {
    line = cancelRequested ? "Annullamento connessione..." : "Connessione...";
    color = display::kAccent;
    display::progressBar(20, 52, 200, 8,
                         std::min(1.0f, static_cast<float>(elapsedMs) /
                                            static_cast<float>(12000)),
                         display::kInfo);
    display::textCentered(72, "Attesa max 10 s, un client alla volta",
                          display::kTextDim, 0);
  } else if (g_job.phase == 1) {
    line = "Servizi: " + std::to_string(g_job.serviceCount) + "  Caratt.: " +
           std::to_string(g_job.charCount);
    color = display::kAccent;
    display::progressBar(20, 52, 200, 8,
                         static_cast<float>(g_job.lineCount) /
                             static_cast<float>(kMaxGattLines),
                         display::kInfo);
    display::textCentered(72, "Enumerazione in corso...", display::kTextDim, 0);
    display::textCentered(88, "Massimo 40 voci", display::kTextDim, 0);
  } else {
    line = "Chiusura client...";
    color = display::kTextDim;
    display::textCentered(64, "Disconnessione e liberazione",
                          display::kTextDim, 0);
  }
  display::textCentered(38, line, color, 2);
  display::textCentered(108, cancelRequested ? "` = annullamento..." :
                                             "` = interrompi",
                        display::kTextDim, 0);
  display::footer("attendi");
  display::push();
}

void showGattResults(const DeviceSummary& target, bool truncated,
                     uint32_t heapAfter) {
  linesLock();
  const int initialCount = g_lineCount;
  linesUnlock();
  if (initialCount == 0) {
    ui::message("Servizi GATT",
                {"!Nessun servizio o caratteristica.", "Il peer potrebbe",
                 "richiedere accoppiamento o non esporre GATT."},
                "BLE+");
    return;
  }

  constexpr int kVisible = 7;
  constexpr int kRowH = 10;
  int top = 0;
  navigation::waitRelease();
  for (;;) {
    navigation::update();
    const navigation::EventData ev = navigation::poll();
    if (ev.type == navigation::Event::Back) {
      break;
    }
    // Il worker orfano puo' ancora accodare voci: la lettura va sotto lock.
    linesLock();
    GattLine visible[kVisible];
    const int lineCount = g_lineCount;
    if (ev.type == navigation::Event::Up && top > 0) {
      --top;
    } else if (ev.type == navigation::Event::Down &&
               top + kVisible < lineCount) {
      ++top;
    }
    for (int row = 0; row < kVisible; ++row) {
      const int index = top + row;
      if (index >= lineCount) {
        break;
      }
      visible[row] = g_lines[index];
    }
    linesUnlock();

    display::clear();
    display::header("Servizi GATT", "BLE+");
    display::text(4, 23, target.address, display::kTextDim, 0);
    display::text(4, 33,
                  "S " + std::to_string(g_job.serviceCount) + "  C " +
                      std::to_string(g_job.charCount) + "  lib " +
                      std::to_string(heapAfter / 1024) + "K" +
                      (truncated ? "  ~" : ""),
                  display::kAccent, 0);
    for (int row = 0; row < kVisible; ++row) {
      if (top + row >= lineCount) {
        break;
      }
      const int y = 45 + row * kRowH;
      display::text(4, y, visible[row].text,
                    visible[row].kind == 0 ? display::kAccent
                                           : display::kText,
                    0);
    }
    display::footer("S/N=scorri  ESC=esci");
    display::push();
    delay(30);
  }
  navigation::waitRelease();
}

}  // namespace

void runGattEnum() {
  powertimer::Hold hold;
  NimBLEScan* scan = scanOrNull();
  if (scan == nullptr) {
    ui::message("Servizi GATT",
                {"!Stack BLE non disponibile."}, "BLE+");
    return;
  }

  resetDevices();
  setScanHandler(gattScanHandler);
  configureScan(scan, true);

  navigation::waitRelease();
  const uint32_t start = millis();
  for (;;) {
    if (millis() - start >= kDiscoveryMs) {
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

  const int count = snapshotSummaries(g_deviceRows, kMaxDevices, true);
  if (count == 0) {
    ui::message("Servizi GATT",
                {"!Nessun dispositivo BLE rilevato.",
                 "Avvicinati e riprova."},
                "BLE+");
    return;
  }

  std::vector<std::string> labels;
  labels.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    const std::string name =
        g_deviceRows[i].hasName ? shortenText(g_deviceRows[i].name, 12)
                                : "(senza nome)";
    const std::string tail = std::string(g_deviceRows[i].address).substr(12);
    labels.push_back(name + " " + std::to_string(g_deviceRows[i].rssi) + " " +
                     tail);
  }
  const int choice =
      chooseList("Scegli dispositivo", "BLE+", labels,
                 "ENTER=connetti  `=annulla");
  if (choice < 0) {
    return;
  }
  const DeviceSummary target = g_deviceRows[choice];

  if (NimBLEDevice::getClientListSize() > 0) {
    ui::message("Servizi GATT",
                {"!Client BLE gia' in uso.", "Riprova quando e' libero."},
                "BLE+");
    return;
  }
  // g_job e' unico: se il worker di un giro precedente e' ancora vivo (timeout
  // della UI) non si puo' sovrascrivere il target sotto i suoi piedi.
  if (g_jobActive) {
    ui::message("Servizi GATT",
                {"!Enumerazione precedente ancora attiva.",
                 "Attendi che il client si chiuda."},
                "BLE+");
    return;
  }

  const uint32_t heapBefore = ESP.getFreeHeap();
  linesLock();
  g_lineCount = 0;
  linesUnlock();
  g_job.target = target;
  g_job.done = false;
  g_job.abortRequested = false;
  g_job.connectOk = false;
  g_job.servicesOk = false;
  g_job.aborted = false;
  g_job.phase = 0;
  g_job.serviceCount = 0;
  g_job.charCount = 0;
  g_job.lineCount = 0;
  g_job.truncated = false;
  g_job.heapAfter = 0;

  navigation::waitRelease();
  TaskHandle_t worker = nullptr;
  g_jobActive = true;
  if (xTaskCreate(gattWorker, "ble_gatt", kWorkerStack, nullptr, 1, &worker) !=
      pdPASS) {
    g_jobActive = false;
    ui::message("Servizi GATT",
                {"!Task client non avviabile.", "Memoria insufficiente."},
                "BLE+");
    return;
  }

  const uint32_t connectStart = millis();
  const uint32_t waitStart = millis();
  bool cancelRequested = false;
  bool waitTimedOut = false;
  uint32_t lastDraw = 0;
  while (!g_job.done) {
    navigation::update();
    if (navigation::poll().type == navigation::Event::Back &&
        !cancelRequested) {
      cancelRequested = true;
      g_job.abortRequested = true;
      if (g_job.phase == 0) {
        ble_gap_conn_cancel();  // sblocca subito il connect in corso
      }
    }
    // Il worker puo' restare bloccato dentro NimBLE: limite massimo di attesa
    // per non lasciare la UI appesa e permettere comunque l'uscita.
    if (millis() - waitStart > 25000) {
      g_job.abortRequested = true;
      waitTimedOut = true;
      break;
    }
    const uint32_t now = millis();
    if (now - lastDraw > 100) {
      lastDraw = now;
      drawGattProgress(target, now - connectStart, cancelRequested);
    }
    delay(25);
  }

  if (waitTimedOut) {
    // g_job potrebbe essere in scrittura: non leggere i campi parziali.
    ui::message("Servizi GATT", {"!Dispositivo non risponde."}, "BLE+");
    return;
  }

  vTaskDelay(pdMS_TO_TICKS(30));  // il worker termina dopo aver segnato done

  const uint32_t heapAfter = g_job.heapAfter;
  const bool truncated = g_job.truncated;

  if (!g_job.connectOk) {
    char heapLine[40];
    std::snprintf(heapLine, sizeof(heapLine), "Heap libero: %u B",
                  static_cast<unsigned>(ESP.getFreeHeap()));
    ui::message("Servizi GATT",
                {g_job.abortRequested ? "Connessione annullata."
                                      : "!Connessione fallita o timeout.",
                 target.address, heapLine},
                "BLE+");
    return;
  }
  if (g_job.aborted) {
    ui::message("Servizi GATT",
                {"Operazione interrotta.", "Client chiuso."}, "BLE+");
    return;
  }

  linesLock();
  const int lineTotal = g_lineCount;
  for (int i = 0; i < lineTotal; ++i) {
    Serial.println(g_lines[i].text);
  }
  linesUnlock();
  logger::info("GATT: %d servizi, %d caratteristiche, %d voci, heap %u -> %u",
               static_cast<int>(g_job.serviceCount),
               static_cast<int>(g_job.charCount), lineTotal,
               static_cast<unsigned>(heapBefore),
               static_cast<unsigned>(heapAfter));

  if (!g_job.servicesOk) {
    ui::message("Servizi GATT",
                {"!Scoperta servizi fallita.", "Nessun dato disponibile."},
                "BLE+");
    return;
  }
  showGattResults(target, truncated, heapAfter);
}

}  // namespace bleext

#endif  // ENABLE_MODULE_BLEEXT
