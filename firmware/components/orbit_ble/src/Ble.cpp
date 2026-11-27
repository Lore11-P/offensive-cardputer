#include "orbit_ble/Ble.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Display.h"
#include "Logger.h"
#include "MenuManager.h"
#include "Navigation.h"
#include "Ui.h"
#include "esp_bt.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "orbit_compat.h"
#include "orbit_mascot/Mascot.h"
#include "services/gap/ble_svc_gap.h"

// Porta su NimBLE (IDF 5.4) del modulo BLE 2.x: scanner e advertising
// falsificati. Nessun GATT ne' connessione: solo observer + broadcaster.
// Le API NimBLE sono quelle host (ble_gap_*), non le esp_ble_gap_* di
// Bluedroid.

namespace orbit_ble {
namespace {

constexpr uint8_t kAdvMax = 31;       // advertising legacy
constexpr size_t kScanMax = 32;       // slot fissi (nessuna crescita heap)
constexpr size_t kDisplayMax = 20;    // voci mostrate
constexpr size_t kNameMax = 20;
constexpr uint8_t kNameComplete = 0x09;
constexpr uint8_t kNameShort = 0x08;
constexpr uint16_t kScanUnit = 80;    // 50 ms
constexpr uint16_t kAdvInterval = 96; // 60 ms
constexpr uint32_t kSpamSlotMs = 140; // cambio payload (100-200 ms)
constexpr uint32_t kScanRefreshMs = 2000;
constexpr uint32_t kScanStaleMs = 15000;
constexpr char kDeviceName[] = "ORBIT";

// ---------------------------------------------------------------------------
// Payload del firmware 2.x. Sono AD structure complete: [len][type][dati].
// Non vanno incapsulate in un manufacturer wrapper.
// ---------------------------------------------------------------------------

struct Payload {
    const char* name;
    const uint8_t* data;
    uint8_t size;
    uint8_t set;
};

enum SetBit : uint8_t {
    kSetApple = 1 << 0,
    kSetSamsung = 1 << 1,
    kSetGoogle = 1 << 2,
    kSetWindows = 1 << 3,
};

#define APPLE_ADV(model)                                                        \
    {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, model, 0x20, 0x75, 0xaa, 0x30,  \
     0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00,  \
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}

constexpr uint8_t kAppleAirPods[] = APPLE_ADV(0x02);
constexpr uint8_t kAppleAirPodsPro[] = APPLE_ADV(0x0e);
constexpr uint8_t kAppleAirPodsMax[] = APPLE_ADV(0x0a);
constexpr uint8_t kAppleAirPodsGen2[] = APPLE_ADV(0x0f);
constexpr uint8_t kAppleAirPodsGen3[] = APPLE_ADV(0x13);
constexpr uint8_t kAppleAirPodsPro2[] = APPLE_ADV(0x14);

// Google Fast Pair: UUID 0xFE2C + model ID di esempio (0x9C0D4A).
constexpr uint8_t kFastPair[] = {
    0x02, 0x01, 0x06,                            // flags
    0x03, 0x03, 0x2c, 0xfe,                      // UUID 0xFE2C
    0x06, 0x16, 0x2c, 0xfe, 0x9c, 0x0d, 0x4a,    // service data
};

// Microsoft Swift Pair.
constexpr uint8_t kSwiftPair[] = {
    0x02, 0x01, 0x06,
    0x06, 0xff, 0x06, 0x00, 0x03, 0x01, 0x80,
    0x0b, 0x09, 'S', 'w', 'i', 'f', 't', ' ', 'P', 'a', 'i', 'r',
};

constexpr uint8_t kSwiftPairSurface[] = {
    0x02, 0x01, 0x06,
    0x06, 0xff, 0x06, 0x00, 0x03, 0x01, 0x80,
    0x13, 0x09, 'S', 'u', 'r', 'f', 'a', 'c', 'e', ' ',
    'H', 'e', 'a', 'd', 'p', 'h', 'o', 'n', 'e', 's',
};

constexpr uint8_t kSwiftPairMouse[] = {
    0x02, 0x01, 0x06,
    0x06, 0xff, 0x06, 0x00, 0x03, 0x01, 0x80,
    0x0e, 0x09, 'S', 'u', 'r', 'f', 'a', 'c', 'e', ' ', 'M', 'o', 'u', 's', 'e',
};

// Samsung (manufacturer ID 0x0075) con nome Galaxy Buds2.
constexpr uint8_t kSamsung[] = {
    0x02, 0x01, 0x06,
    0x09, 0xff, 0x75, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00,
    0x0d, 0x09, 'G', 'a', 'l', 'a', 'x', 'y', ' ', 'B', 'u', 'd', 's', '2',
};

constexpr uint8_t kSamsungBudsPro[] = {
    0x02, 0x01, 0x06,
    0x09, 0xff, 0x75, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00,
    0x10, 0x09, 'G', 'a', 'l', 'a', 'x', 'y', ' ', 'B', 'u', 'd', 's',
    ' ', 'P', 'r', 'o',
};

static_assert(sizeof(kAppleAirPods) <= kAdvMax, "payload Apple > 31 byte");
static_assert(sizeof(kAppleAirPodsPro) <= kAdvMax, "payload Apple > 31 byte");
static_assert(sizeof(kAppleAirPodsMax) <= kAdvMax, "payload Apple > 31 byte");
static_assert(sizeof(kAppleAirPodsGen2) <= kAdvMax, "payload Apple > 31 byte");
static_assert(sizeof(kAppleAirPodsGen3) <= kAdvMax, "payload Apple > 31 byte");
static_assert(sizeof(kAppleAirPodsPro2) <= kAdvMax, "payload Apple > 31 byte");
static_assert(sizeof(kFastPair) <= kAdvMax, "payload Google > 31 byte");
static_assert(sizeof(kSwiftPair) <= kAdvMax, "payload Windows > 31 byte");
static_assert(sizeof(kSwiftPairSurface) <= kAdvMax, "payload Windows > 31 byte");
static_assert(sizeof(kSwiftPairMouse) <= kAdvMax, "payload Windows > 31 byte");
static_assert(sizeof(kSamsung) <= kAdvMax, "payload Samsung > 31 byte");
static_assert(sizeof(kSamsungBudsPro) <= kAdvMax, "payload Samsung > 31 byte");

// Controllo a compile time che ogni payload sia una catena AD valida:
// ogni campo deve stare dentro il buffer e la somma deve chiuderlo esatto.
constexpr bool adChainValid(const uint8_t* data, size_t len) {
    size_t pos = 0;
    while (pos < len) {
        const size_t fieldLen = data[pos];
        if (fieldLen == 0 || pos + 1 + fieldLen > len) {
            return false;
        }
        pos += 1 + fieldLen;
    }
    return pos == len;
}

static_assert(adChainValid(kAppleAirPods, sizeof(kAppleAirPods)), "AD Apple");
static_assert(adChainValid(kAppleAirPodsPro, sizeof(kAppleAirPodsPro)), "AD Apple");
static_assert(adChainValid(kAppleAirPodsMax, sizeof(kAppleAirPodsMax)), "AD Apple");
static_assert(adChainValid(kAppleAirPodsGen2, sizeof(kAppleAirPodsGen2)), "AD Apple");
static_assert(adChainValid(kAppleAirPodsGen3, sizeof(kAppleAirPodsGen3)), "AD Apple");
static_assert(adChainValid(kAppleAirPodsPro2, sizeof(kAppleAirPodsPro2)), "AD Apple");
static_assert(adChainValid(kFastPair, sizeof(kFastPair)), "AD Google");
static_assert(adChainValid(kSwiftPair, sizeof(kSwiftPair)), "AD Windows");
static_assert(adChainValid(kSwiftPairSurface, sizeof(kSwiftPairSurface)), "AD Windows");
static_assert(adChainValid(kSwiftPairMouse, sizeof(kSwiftPairMouse)), "AD Windows");
static_assert(adChainValid(kSamsung, sizeof(kSamsung)), "AD Samsung");
static_assert(adChainValid(kSamsungBudsPro, sizeof(kSamsungBudsPro)), "AD Samsung");

const Payload kPayloads[] = {
    {"AirPods", kAppleAirPods, sizeof(kAppleAirPods), kSetApple},
    {"AirPods Pro", kAppleAirPodsPro, sizeof(kAppleAirPodsPro), kSetApple},
    {"AirPods Max", kAppleAirPodsMax, sizeof(kAppleAirPodsMax), kSetApple},
    {"AirPods Gen2", kAppleAirPodsGen2, sizeof(kAppleAirPodsGen2), kSetApple},
    {"AirPods Gen3", kAppleAirPodsGen3, sizeof(kAppleAirPodsGen3), kSetApple},
    {"AirPods Pro2", kAppleAirPodsPro2, sizeof(kAppleAirPodsPro2), kSetApple},
    {"Fast Pair", kFastPair, sizeof(kFastPair), kSetGoogle},
    {"Swift Pair", kSwiftPair, sizeof(kSwiftPair), kSetWindows},
    {"Surface Headphones", kSwiftPairSurface, sizeof(kSwiftPairSurface),
     kSetWindows},
    {"Surface Mouse", kSwiftPairMouse, sizeof(kSwiftPairMouse), kSetWindows},
    {"Galaxy Buds2", kSamsung, sizeof(kSamsung), kSetSamsung},
    {"Galaxy Buds Pro", kSamsungBudsPro, sizeof(kSamsungBudsPro), kSetSamsung},
};

enum class PayloadSet : uint8_t { Random, Apple, Samsung, Google, Windows, All };

const char* setLabel(PayloadSet set) {
    switch (set) {
        case PayloadSet::Random: return "Random";
        case PayloadSet::Apple: return "Apple (iOS/AirPods)";
        case PayloadSet::Samsung: return "Samsung";
        case PayloadSet::Google: return "Google/Android";
        case PayloadSet::Windows: return "Windows SwiftPair";
        case PayloadSet::All: return "All";
    }
    return "?";
}

// Etichetta corta per l'avviso legale (la riga deve stare nei 240 px).
const char* setShortLabel(PayloadSet set) {
    switch (set) {
        case PayloadSet::Random: return "Random";
        case PayloadSet::Apple: return "Apple";
        case PayloadSet::Samsung: return "Samsung";
        case PayloadSet::Google: return "Google";
        case PayloadSet::Windows: return "SwiftPair";
        case PayloadSet::All: return "All";
    }
    return "?";
}

bool belongsTo(const Payload& payload, PayloadSet set) {
    switch (set) {
        case PayloadSet::Random:
        case PayloadSet::All:
            return true;
        case PayloadSet::Apple: return (payload.set & kSetApple) != 0;
        case PayloadSet::Samsung: return (payload.set & kSetSamsung) != 0;
        case PayloadSet::Google: return (payload.set & kSetGoogle) != 0;
        case PayloadSet::Windows: return (payload.set & kSetWindows) != 0;
    }
    return false;
}

size_t payloadCount(PayloadSet set) {
    size_t count = 0;
    for (const Payload& payload : kPayloads) {
        if (belongsTo(payload, set)) {
            ++count;
        }
    }
    return count;
}

const Payload* payloadAt(PayloadSet set, size_t index) {
    for (const Payload& payload : kPayloads) {
        if (!belongsTo(payload, set)) {
            continue;
        }
        if (index == 0) {
            return &payload;
        }
        --index;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Stato dello stack NimBLE (stesso ciclo di vita della chat).
// ---------------------------------------------------------------------------

bool g_hostInited = false;
volatile bool g_synced = false;
volatile bool g_scanActive = false;
uint8_t g_ownAddrType = BLE_OWN_ADDR_PUBLIC;
uint8_t g_selfAddr[6] = {};
uint8_t g_selfAddrType = BLE_ADDR_PUBLIC;

struct ScanEntry {
    uint8_t addr[6];
    uint8_t addrType;
    int8_t rssi;
    uint32_t lastSeen;
    uint8_t advLen;
    uint8_t adv[kAdvMax];
    char name[kNameMax];
    bool used;
};

// Array fisso: il callback NimBLE non deve allocare e la lista non cresce
// con il numero di advertising ricevuti.
ScanEntry g_entries[kScanMax];
portMUX_TYPE g_scanLock = portMUX_INITIALIZER_UNLOCKED;

void copyText(char* out, size_t cap, const char* text) {
    if (out == nullptr || cap == 0) {
        return;
    }
    size_t written = 0;
    if (text != nullptr) {
        while (written + 1 < cap && text[written] != '\0') {
            out[written] = text[written];
            ++written;
        }
    }
    out[written] = '\0';
}

void formatMac(char out[18], const uint8_t addr[6]) {
    std::snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", addr[5], addr[4],
                  addr[3], addr[2], addr[1], addr[0]);
}

const char* addressTypeName(uint8_t type) {
    switch (type) {
        case BLE_ADDR_PUBLIC: return "Pubblico";
        case BLE_ADDR_RANDOM: return "Casuale";
        case BLE_ADDR_PUBLIC_ID: return "Pubblico ID";
        case BLE_ADDR_RANDOM_ID: return "Casuale ID";
        default: return "Sconosciuto";
    }
}

// Nome da AD 0x09 (completo) o 0x08 (abbreviato). Solo ASCII stampabile:
// il display ha un font 5x7 che non gestisce UTF-8.
bool parseName(const uint8_t* data, uint8_t len, char* out, size_t cap) {
    if (out == nullptr || cap == 0) {
        return false;
    }
    out[0] = '\0';
    if (data == nullptr) {
        return false;
    }
    bool found = false;
    size_t pos = 0;
    while (pos + 1 < len) {
        const size_t fieldLen = data[pos];
        if (fieldLen == 0 || pos + 1 + fieldLen > len) {
            break;
        }
        const uint8_t type = data[pos + 1];
        if ((type == kNameComplete || type == kNameShort) && fieldLen > 1) {
            size_t written = 0;
            for (size_t i = 0; i + 1 < fieldLen && written + 1 < cap; ++i) {
                const char c = static_cast<char>(data[pos + 2 + i]);
                out[written++] = (c >= 0x20 && c < 0x7F) ? c : '?';
            }
            out[written] = '\0';
            found = written > 0;
            if (type == kNameComplete) {
                break;  // il nome completo vince sull'abbreviato
            }
        }
        pos += 1 + fieldLen;
    }
    if (!found) {
        out[0] = '\0';
    }
    return found;
}

int gapEvent(struct ble_gap_event* event, void* arg);

bool startScan() {
    if (!g_hostInited || !g_synced || !g_scanActive) {
        return false;
    }
    struct ble_gap_disc_params params = {};
    params.itvl = kScanUnit;
    params.window = kScanUnit;
    params.passive = 0;  // attivo: i nomi arrivano spesso solo in SCAN_RSP
    params.filter_duplicates = 0;
    const int rc = ble_gap_disc(g_ownAddrType, BLE_HS_FOREVER, &params,
                                gapEvent, nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        logger::warn("BLE: scan rc=%d", rc);
        return false;
    }
    return true;
}

void handleAdvertisement(const struct ble_gap_disc_desc* desc) {
    if (!g_scanActive || desc == nullptr) {
        return;
    }

    char name[kNameMax];
    const bool hasName =
        parseName(desc->data, desc->length_data, name, sizeof(name));
    const bool scanResponse =
        desc->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP;
    const uint32_t now = millis();

    portENTER_CRITICAL(&g_scanLock);
    if (!g_scanActive) {
        portEXIT_CRITICAL(&g_scanLock);
        return;
    }

    int slot = -1;
    for (size_t i = 0; i < kScanMax; ++i) {
        if (g_entries[i].used &&
            std::memcmp(g_entries[i].addr, desc->addr.val, 6) == 0) {
            slot = static_cast<int>(i);
            break;
        }
    }

    if (slot < 0) {
        // Nuovo indirizzo: primo slot libero, altrimenti sostituisce la voce
        // vista meno di recente.
        for (size_t i = 0; i < kScanMax; ++i) {
            if (!g_entries[i].used) {
                slot = static_cast<int>(i);
                break;
            }
        }
        if (slot < 0) {
            slot = 0;
            for (size_t i = 1; i < kScanMax; ++i) {
                if (static_cast<int32_t>(g_entries[i].lastSeen -
                                         g_entries[slot].lastSeen) < 0) {
                    slot = static_cast<int>(i);
                }
            }
        }
        std::memset(&g_entries[slot], 0, sizeof(ScanEntry));
        std::memcpy(g_entries[slot].addr, desc->addr.val, 6);
        g_entries[slot].addrType = desc->addr.type;
        g_entries[slot].used = true;
    }

    ScanEntry& entry = g_entries[slot];
    entry.rssi = desc->rssi;
    entry.lastSeen = now;
    if (hasName) {
        copyText(entry.name, sizeof(entry.name), name);
    }
    if (desc->data != nullptr && desc->length_data > 0) {
        // I dati del SCAN_RSP non sovrascrivono l'advertisement principale.
        if (!scanResponse || entry.advLen == 0) {
            const size_t len = std::min<size_t>(desc->length_data, kAdvMax);
            std::memcpy(entry.adv, desc->data, len);
            entry.advLen = static_cast<uint8_t>(len);
        }
    }
    portEXIT_CRITICAL(&g_scanLock);
}

int gapEvent(struct ble_gap_event* event, void* arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_DISC:
            handleAdvertisement(&event->disc);
            return 0;
        case BLE_GAP_EVENT_DISC_COMPLETE:
            // Con BLE_HS_FOREVER non dovrebbe accadere; se il controller
            // interrompe la scansione la riavviamo finche' e' attiva.
            if (g_scanActive) {
                startScan();
            }
            return 0;
        default:
            return 0;
    }
}

void onReset(int reason) {
    g_synced = false;
    logger::warn("BLE: host reset, reason=%d", reason);
}

void onSync() {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        logger::error("BLE: indirizzo non disponibile, rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &g_ownAddrType);
    if (rc != 0) {
        logger::error("BLE: tipo indirizzo, rc=%d", rc);
        return;
    }
    g_selfAddrType = g_ownAddrType == BLE_OWN_ADDR_PUBLIC ? BLE_ADDR_PUBLIC
                                                          : BLE_ADDR_RANDOM;
    std::memset(g_selfAddr, 0, sizeof(g_selfAddr));
    if (ble_hs_id_copy_addr(g_selfAddrType, g_selfAddr, nullptr) != 0) {
        logger::warn("BLE: indirizzo locale non leggibile");
    }
    g_synced = true;
}

void hostTask(void* param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

bool waitSynced(uint32_t timeoutMs) {
    const uint32_t start = millis();
    while (!g_synced && millis() - start < timeoutMs) {
        delay(10);
    }
    return g_synced;
}

// nimble_port_init() inizializza e abilita anche il controller: non va
// chiamato esp_bt_controller_enable() a mano (come nella chat).
bool ensureBle() {
    if (g_hostInited) {
        return waitSynced(1000);
    }
    const esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        logger::error("BLE: nimble_port_init rc=%d", err);
        return false;
    }
    g_hostInited = true;

    ble_hs_cfg.reset_cb = onReset;
    ble_hs_cfg.sync_cb = onSync;
    if (ble_svc_gap_device_name_set(kDeviceName) != 0) {
        logger::warn("BLE: nome GAP non impostato");
    }

    nimble_port_freertos_init(hostTask);
    if (!waitSynced(2000)) {
        logger::error("BLE: host non sincronizzato");
        return false;
    }
    return true;
}

void clearScanList() {
    portENTER_CRITICAL(&g_scanLock);
    std::memset(g_entries, 0, sizeof(g_entries));
    portEXIT_CRITICAL(&g_scanLock);
}

void stopScan() {
    g_scanActive = false;
    if (g_hostInited && g_synced) {
        ble_gap_disc_cancel();  // BLE_HS_EALREADY se non attiva: ignorato
    }
    delay(30);
    clearScanList();
}

void shutdown() {
    g_scanActive = false;
    if (!g_hostInited) {
        g_synced = false;
        clearScanList();
        return;
    }

    if (g_synced) {
        ble_gap_disc_cancel();
        ble_gap_adv_stop();
    }
    delay(50);

    const int rc = nimble_port_stop();
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        const esp_err_t err = nimble_port_deinit();
        if (err != ESP_OK) {
            logger::error("BLE: nimble_port_deinit rc=%d", err);
        } else {
            g_hostInited = false;
        }
    } else {
        // Non deinizializzare a forza mentre il task host potrebbe essere
        // ancora vivo (stesso criterio della chat).
        logger::error("BLE: nimble_port_stop rc=%d", rc);
    }

    g_synced = false;
    g_ownAddrType = BLE_OWN_ADDR_PUBLIC;
    g_selfAddrType = BLE_ADDR_PUBLIC;
    std::memset(g_selfAddr, 0, sizeof(g_selfAddr));
    clearScanList();
}

// ---------------------------------------------------------------------------
// Scanner
// ---------------------------------------------------------------------------

void shortName(const char* name, char out[12]) {
    if (name == nullptr || name[0] == '\0') {
        std::snprintf(out, 12, "%s", "?");
        return;
    }
    const size_t len = std::strlen(name);
    if (len <= 11) {
        std::snprintf(out, 12, "%s", name);
        return;
    }
    std::memcpy(out, name, 10);
    out[10] = '~';
    out[11] = '\0';
}

void drawScanner(const ScanEntry* list, size_t count, int selected) {
    display::clear();
    display::header("Scanner BLE", "Bluetooth");

    const int top = 26;
    const int rowH = 10;
    int rows = (display::height() - 12 - top) / rowH;
    if (rows < 1) {
        rows = 1;
    }

    if (count == 0) {
        display::textCentered(56, "Nessun dispositivo", display::kTextDim, 2);
        display::textCentered(74, "Scansione in corso...", display::kText, 0);
    } else {
        int first = selected - rows / 2;
        if (first < 0) {
            first = 0;
        }
        if (first > static_cast<int>(count) - rows) {
            first = static_cast<int>(count) - rows;
        }
        if (first < 0) {
            first = 0;
        }
        for (int i = 0; i < rows && first + i < static_cast<int>(count); ++i) {
            const ScanEntry& entry = list[first + i];
            const int y = top + i * rowH;
            char mac[18];
            formatMac(mac, entry.addr);
            char name[12];
            shortName(entry.name, name);
            char line[48];
            std::snprintf(line, sizeof(line), "%-11s %s %4d", name, mac,
                          static_cast<int>(entry.rssi));
            if (first + i == selected) {
                display::g().fillRect(2, y - 1, display::width() - 4, rowH,
                                      display::C(display::T_accD));
                display::text(4, y, line, display::C(display::T_onAcc), 0);
            } else {
                display::text(4, y, line, display::C(display::T_text), 0);
            }
        }
    }

    char trailing[16];
    std::snprintf(trailing, sizeof(trailing), "%u dev",
                  static_cast<unsigned>(count));
    display::footer(count > 0 ? "ESC=esci ;.=scroll ENTER=info" : "ESC=esci",
                    trailing);
    display::push();
}

void showDetails(const ScanEntry& entry, uint32_t now) {
    char mac[18];
    formatMac(mac, entry.addr);

    std::vector<std::string> lines;
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "Nome: %.40s",
                  entry.name[0] != '\0' ? entry.name : "-");
    lines.emplace_back(buffer);
    std::snprintf(buffer, sizeof(buffer), "MAC: %s", mac);
    lines.emplace_back(buffer);
    std::snprintf(buffer, sizeof(buffer), "Tipo: %s",
                  addressTypeName(entry.addrType));
    lines.emplace_back(buffer);
    std::snprintf(buffer, sizeof(buffer), "RSSI: %d dBm",
                  static_cast<int>(entry.rssi));
    lines.emplace_back(buffer);

    const uint32_t age = (now - entry.lastSeen) / 1000;
    if (age == 0) {
        lines.emplace_back("Visto: ora");
    } else {
        std::snprintf(buffer, sizeof(buffer), "Visto: %u s fa",
                      static_cast<unsigned>(age));
        lines.emplace_back(buffer);
    }

    if (entry.advLen == 0) {
        lines.emplace_back("AD: nessun dato");
    } else {
        char chunk[64];
        for (size_t offset = 0; offset < entry.advLen; offset += 31) {
            size_t used = 0;
            for (size_t i = offset; i < entry.advLen && i < offset + 31; ++i) {
                used += std::snprintf(chunk + used, sizeof(chunk) - used,
                                      "%02X", entry.adv[i]);
            }
            lines.emplace_back(std::string("AD: ") + chunk);
        }
    }

    ui::message("Dettagli BLE", lines, "Scanner BLE");
}

void runScanner() {
    if (!ensureBle()) {
        ui::message("Scanner BLE",
                    {"!Stack BLE non disponibile.", "Riprova tra qualche secondo."},
                    "Bluetooth");
        shutdown();
        return;
    }

    ble_gap_adv_stop();  // eventuale advertising lasciato da un altro modulo
    delay(20);
    clearScanList();
    g_scanActive = true;
    if (!startScan()) {
        // Il controller puo' essere ancora occupato dallo stop advertising.
        delay(50);
        if (!startScan()) {
            g_scanActive = false;
            ui::message("Scanner BLE",
                        {"!Scansione non avviata.", "Riprova tra qualche secondo."},
                        "Bluetooth");
            return;
        }
    }

    ScanEntry snapshot[kScanMax];
    size_t count = 0;
    int selected = 0;
    uint8_t selectedAddr[6] = {};
    bool hasSelectedAddr = false;
    uint32_t lastRefresh = 0;
    bool needSnapshot = true;
    bool needDraw = true;
    bool running = true;

    navigation::waitRelease();
    while (running) {
        const navigation::EventData event = navigation::poll();
        const uint32_t now = millis();

        if (event.type == navigation::Event::Back) {
            running = false;
            break;
        }
        if (event.type == navigation::Event::Select && count > 0) {
            showDetails(snapshot[selected], now);
            needDraw = true;
            navigation::waitRelease();
        } else if (event.type == navigation::Event::Up && count > 0) {
            selected = (selected + static_cast<int>(count) - 1) %
                       static_cast<int>(count);
            std::memcpy(selectedAddr, snapshot[selected].addr, 6);
            hasSelectedAddr = true;
            needDraw = true;
        } else if (event.type == navigation::Event::Down && count > 0) {
            selected = (selected + 1) % static_cast<int>(count);
            std::memcpy(selectedAddr, snapshot[selected].addr, 6);
            hasSelectedAddr = true;
            needDraw = true;
        }

        if (needSnapshot || now - lastRefresh >= kScanRefreshMs) {
            // La selezione segue il MAC, non l'indice: l'ordinamento per
            // RSSI cambia a ogni aggiornamento.
            if (count > 0 && selected < static_cast<int>(count)) {
                std::memcpy(selectedAddr, snapshot[selected].addr, 6);
                hasSelectedAddr = true;
            }

            count = 0;
            portENTER_CRITICAL(&g_scanLock);
            for (size_t i = 0; i < kScanMax; ++i) {
                // Le voci non piu' viste liberano lo slot: la lista resta
                // una fotografia del presente, non un accumulatore.
                if (g_entries[i].used &&
                    now - g_entries[i].lastSeen > kScanStaleMs) {
                    std::memset(&g_entries[i], 0, sizeof(ScanEntry));
                    continue;
                }
                if (g_entries[i].used && count < kScanMax) {
                    snapshot[count++] = g_entries[i];
                }
            }
            portEXIT_CRITICAL(&g_scanLock);

            std::sort(snapshot, snapshot + count,
                      [](const ScanEntry& a, const ScanEntry& b) {
                          return a.rssi > b.rssi;
                      });
            if (count > kDisplayMax) {
                count = kDisplayMax;
            }

            int found = -1;
            if (hasSelectedAddr) {
                for (size_t i = 0; i < count; ++i) {
                    if (std::memcmp(snapshot[i].addr, selectedAddr, 6) == 0) {
                        found = static_cast<int>(i);
                        break;
                    }
                }
            }
            if (found >= 0) {
                selected = found;
            } else if (selected >= static_cast<int>(count)) {
                selected = count > 0 ? static_cast<int>(count) - 1 : 0;
            }
            if (count == 0) {
                selected = 0;
                hasSelectedAddr = false;
            } else if (selected < static_cast<int>(count)) {
                std::memcpy(selectedAddr, snapshot[selected].addr, 6);
                hasSelectedAddr = true;
            }

            lastRefresh = now;
            needSnapshot = false;
            needDraw = true;
        }

        if (needDraw) {
            drawScanner(snapshot, count, selected);
            needDraw = false;
        }
        delay(20);
    }

    stopScan();
    logger::info("BLE scanner: %u dispositivi",
                 static_cast<unsigned>(count));
}

// ---------------------------------------------------------------------------
// BLE spam (solo ambienti autorizzati: avviso legale obbligatorio)
// ---------------------------------------------------------------------------

void drawSpam(PayloadSet set, const char* payloadName, uint32_t packets,
              uint32_t remainingMs, uint32_t totalMs) {
    display::clear();
    display::header("BLE Spam", "Bluetooth");

    char line[64];
    std::snprintf(line, sizeof(line), "Set: %s", setLabel(set));
    display::text(8, 28, line, display::kTextDim, 0);
    display::text(8, 40, payloadName, display::kAccent, 2);
    std::snprintf(line, sizeof(line), "TX: %u",
                  static_cast<unsigned>(packets));
    display::text(8, 58, line, display::kText, 0);
    display::text(8, 72, "Solo ambienti autorizzati", display::kWarning, 0);

    const float fraction =
        totalMs == 0
            ? 0.0f
            : 1.0f - static_cast<float>(remainingMs) / static_cast<float>(totalMs);
    display::progressBar(8, 94, 180, 8, fraction, display::kDanger);
    std::snprintf(line, sizeof(line), "%us", static_cast<unsigned>(remainingMs / 1000));
    display::text(196, 94, line, display::kText, 0);
    display::footer("ESC = interrompi");
    display::push();
}

void runSpam(PayloadSet set, uint32_t seconds) {
    const std::string activity = std::string("BLE spam ") + setShortLabel(set);
    if (!ui::legalWarning(activity,
                          {"Popup su dispositivi vicini."},
                          "BLE Spam")) {
        return;  // l'utente ha rifiutato: nessuna trasmissione
    }

    if (!ensureBle()) {
        ui::message("BLE Spam",
                    {"!Stack BLE non disponibile.", "Riprova tra qualche secondo."},
                    "BLE Spam");
        shutdown();
        return;
    }

    ble_gap_adv_stop();
    delay(20);

    struct ble_gap_adv_params params = {};
    params.conn_mode = BLE_GAP_CONN_MODE_NON;
    params.disc_mode = BLE_GAP_DISC_MODE_NON;
    params.itvl_min = kAdvInterval;
    params.itvl_max = kAdvInterval;

    const uint32_t totalMs = seconds * 1000UL;
    uint32_t packets = 0;
    uint32_t failures = 0;
    size_t cursor = 0;
    const Payload* current = payloadAt(set, 0);

    navigation::waitRelease();
    const uint32_t start = millis();
    uint32_t nextPacket = start;
    uint32_t lastDraw = start;
    for (;;) {
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }
        const uint32_t now = millis();
        const uint32_t elapsed = now - start;
        if (elapsed >= totalMs) {
            break;
        }

        if (static_cast<int32_t>(now - nextPacket) >= 0) {
            const Payload* next = nullptr;
            if (set == PayloadSet::Random) {
                const size_t count = payloadCount(set);
                next = count > 0 ? payloadAt(set, esp_random() % count) : nullptr;
            } else {
                const size_t count = payloadCount(set);
                next = count > 0 ? payloadAt(set, cursor % count) : nullptr;
                if (count > 0) {
                    cursor = (cursor + 1) % count;
                }
            }
            if (next != nullptr) {
                // ble_gap_adv_set_data() non accetta dati mentre l'advertising
                // e' attivo: stop, cambio payload, restart.
                ble_gap_adv_stop();
                delay(5);
                int rc = ble_gap_adv_set_data(next->data, next->size);
                if (rc == 0) {
                    rc = ble_gap_adv_start(g_ownAddrType, nullptr,
                                           BLE_HS_FOREVER, &params, gapEvent,
                                           nullptr);
                }
                if (rc == 0) {
                    ++packets;
                } else {
                    ++failures;
                }
                current = next;
            }
            nextPacket = millis() + kSpamSlotMs;
        }

        if (now - lastDraw >= 200) {
            drawSpam(set, current != nullptr ? current->name : "-", packets,
                     totalMs - elapsed, totalMs);
            lastDraw = now;
        }
        delay(20);
    }

    ble_gap_adv_stop();
    delay(10);
    logger::info("BLE spam %s: %u slot, %u errori",
                 setLabel(set), static_cast<unsigned>(packets),
                 static_cast<unsigned>(failures));
    if (packets > 0) {
        mascot::award(mascot::Event::BleSpam);
    }
}

void spamDurationMenu(PayloadSet set) {
    MenuManager menu("Durata", "BLE Spam");
    menu.addSeparator(setLabel(set));
    menu.addItem("30 secondi", "timer", [set] { runSpam(set, 30); });
    menu.addItem("60 secondi", "timer", [set] { runSpam(set, 60); });
    menu.setHint("ESC=indietro  ENTER=avvia");
    menu.run(true);
}

void runSpamMenu() {
    MenuManager menu("BLE Spam", "Bluetooth");
    menu.addItem("Random", "power", [] { spamDurationMenu(PayloadSet::Random); });
    menu.addItem("Apple (iOS/AirPods)", "bluetooth",
                 [] { spamDurationMenu(PayloadSet::Apple); });
    menu.addItem("Samsung", "bluetooth",
                 [] { spamDurationMenu(PayloadSet::Samsung); });
    menu.addItem("Google/Android", "bluetooth",
                 [] { spamDurationMenu(PayloadSet::Google); });
    menu.addItem("Windows SwiftPair", "bluetooth",
                 [] { spamDurationMenu(PayloadSet::Windows); });
    menu.addItem("All", "power", [] { spamDurationMenu(PayloadSet::All); });
    menu.addSeparator("Avviso legale obbligatorio");
    menu.setHint("ESC=indietro  ENTER=apri");
    menu.run(true);
}

// ---------------------------------------------------------------------------
// Info
// ---------------------------------------------------------------------------

void showInfo() {
    char mac[18];
    if (g_selfAddr[0] == 0 && g_selfAddr[1] == 0 && g_selfAddr[2] == 0 &&
        g_selfAddr[3] == 0 && g_selfAddr[4] == 0 && g_selfAddr[5] == 0) {
        std::snprintf(mac, sizeof(mac), "%s", "n/d");
    } else {
        formatMac(mac, g_selfAddr);
    }

    const char* controller = "sconosciuto";
    switch (esp_bt_controller_get_status()) {
        case ESP_BT_CONTROLLER_STATUS_IDLE: controller = "spento"; break;
        case ESP_BT_CONTROLLER_STATUS_INITED: controller = "inizializzato"; break;
        case ESP_BT_CONTROLLER_STATUS_ENABLED: controller = "attivo"; break;
        default: break;
    }

    char line[80];
    std::vector<std::string> lines;
    std::snprintf(line, sizeof(line), "Indirizzo: %s", mac);
    lines.emplace_back(line);
    std::snprintf(line, sizeof(line), "Tipo: %s",
                  addressTypeName(g_selfAddrType));
    lines.emplace_back(line);
    std::snprintf(line, sizeof(line), "Controller BT: %s", controller);
    lines.emplace_back(line);
    lines.emplace_back(g_synced ? "NimBLE: sincronizzato" : "NimBLE: non pronto");
    std::snprintf(line, sizeof(line), "Heap libero: %u B",
                  static_cast<unsigned>(esp_get_free_heap_size()));
    lines.emplace_back(line);
    std::snprintf(line, sizeof(line), "Heap minimo: %u B",
                  static_cast<unsigned>(esp_get_minimum_free_heap_size()));
    lines.emplace_back(line);
    lines.emplace_back("Solo observer + broadcaster.");

    ui::message("BLE info", lines, "Bluetooth");
}

}  // namespace

void runMenu() {
    if (!ensureBle()) {
        ui::message("Bluetooth",
                    {"!Stack BLE non disponibile.", "Riprova tra qualche secondo."},
                    "Home");
        shutdown();
        return;
    }

    MenuManager menu("Bluetooth", "Home");
    menu.addItem("Scanner BLE", "bluetooth", runScanner);
    menu.addItem("BLE Spam", "power", runSpamMenu);
    menu.addItem("BLE info", "info", showInfo);
    menu.setHint("ESC=indietro  ;.=nav  ENTER=apri");
    menu.run(true);

    // Qualunque uscita dal menu lascia il BT spento: l'heap torna ai giochi.
    shutdown();
}

bool selfTest() {
    if (!ensureBle()) {
        logger::error("BLE selftest: stack non disponibile");
        return false;
    }
    clearScanList();
    g_scanActive = true;
    if (!startScan()) {
        delay(50);
        if (!startScan()) {
            g_scanActive = false;
            shutdown();
            logger::error("BLE selftest: scan non avviata");
            return false;
        }
    }
    delay(2500);
    int count = 0;
    portENTER_CRITICAL(&g_scanLock);
    for (size_t i = 0; i < kScanMax; ++i) {
        if (g_entries[i].used) {
            ++count;
        }
    }
    portEXIT_CRITICAL(&g_scanLock);
    stopScan();
    shutdown();
    logger::info("BLE selftest: %d dispositivi", count);
    return count > 0;
}

}  // namespace orbit_ble
