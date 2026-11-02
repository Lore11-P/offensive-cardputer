#pragma once

#include <Arduino.h>

// Payload BLE di esempio per i test di advertising.
// I dati sono strutture AD complete (length, type, value): non aggiungere
// prefissi. I modelli Apple/Fast Pair cambiano nel tempo: aggiorna gli ID
// per ottenere i popup sui dispositivi piu' recenti.
namespace ble {

struct BlePayload {
    const char* name;
    const uint8_t* data;
    uint8_t size;
};

#define APPLE_ADV(model)                                                        \
    {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, model, 0x20, 0x75, 0xaa, 0x30,  \
     0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00,  \
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}

const uint8_t kAppleAirPods[] = APPLE_ADV(0x02);
const uint8_t kAppleAirPodsPro[] = APPLE_ADV(0x0e);
const uint8_t kAppleAirPodsMax[] = APPLE_ADV(0x0a);
const uint8_t kAppleAirPodsGen2[] = APPLE_ADV(0x0f);
const uint8_t kAppleAirPodsGen3[] = APPLE_ADV(0x13);
const uint8_t kAppleAirPodsPro2[] = APPLE_ADV(0x14);

// Google Fast Pair: UUID 0xFE2C + model ID di esempio (0x9C0D4A).
const uint8_t kFastPair[] = {
    0x02, 0x01, 0x06,                                      // flags
    0x03, 0x03, 0x2c, 0xfe,                                // UUID 0xFE2C
    0x06, 0x16, 0x2c, 0xfe, 0x9c, 0x0d, 0x4a,              // service data
};

// Microsoft Swift Pair.
const uint8_t kSwiftPair[] = {
    0x02, 0x01, 0x06,
    0x06, 0xff, 0x06, 0x00, 0x03, 0x01, 0x80,
    0x0b, 0x09, 'S', 'w', 'i', 'f', 't', ' ', 'P', 'a', 'i', 'r',
};

const uint8_t kSwiftPairSurface[] = {
    0x02, 0x01, 0x06,
    0x06, 0xff, 0x06, 0x00, 0x03, 0x01, 0x80,
    0x13, 0x09, 'S', 'u', 'r', 'f', 'a', 'c', 'e', ' ',
    'H', 'e', 'a', 'd', 'p', 'h', 'o', 'n', 'e', 's',
};

const uint8_t kSwiftPairMouse[] = {
    0x02, 0x01, 0x06,
    0x06, 0xff, 0x06, 0x00, 0x03, 0x01, 0x80,
    0x0e, 0x09, 'S', 'u', 'r', 'f', 'a', 'c', 'e', ' ', 'M', 'o', 'u', 's', 'e',
};

// Samsung (manufacturer ID 0x0075) con nome Galaxy Buds2.
const uint8_t kSamsung[] = {
    0x02, 0x01, 0x06,
    0x09, 0xff, 0x75, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00,
    0x0d, 0x09, 'G', 'a', 'l', 'a', 'x', 'y', ' ', 'B', 'u', 'd', 's', '2',
};

const uint8_t kSamsungBudsPro[] = {
    0x02, 0x01, 0x06,
    0x09, 0xff, 0x75, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00,
    0x10, 0x09, 'G', 'a', 'l', 'a', 'x', 'y', ' ', 'B', 'u', 'd', 's',
    ' ', 'P', 'r', 'o',
};

const BlePayload kApplePayloads[] = {
    {"AirPods", kAppleAirPods, sizeof(kAppleAirPods)},
    {"AirPods Pro", kAppleAirPodsPro, sizeof(kAppleAirPodsPro)},
    {"AirPods Max", kAppleAirPodsMax, sizeof(kAppleAirPodsMax)},
    {"AirPods Gen2", kAppleAirPodsGen2, sizeof(kAppleAirPodsGen2)},
    {"AirPods Gen3", kAppleAirPodsGen3, sizeof(kAppleAirPodsGen3)},
    {"AirPods Pro2", kAppleAirPodsPro2, sizeof(kAppleAirPodsPro2)},
};

const BlePayload kGooglePayloads[] = {
    {"Fast Pair", kFastPair, sizeof(kFastPair)},
};

const BlePayload kWindowsPayloads[] = {
    {"SwiftPair", kSwiftPair, sizeof(kSwiftPair)},
    {"Surface Headphones", kSwiftPairSurface, sizeof(kSwiftPairSurface)},
    {"Surface Mouse", kSwiftPairMouse, sizeof(kSwiftPairMouse)},
};

const BlePayload kSamsungPayloads[] = {
    {"Galaxy Buds2", kSamsung, sizeof(kSamsung)},
    {"Galaxy Buds Pro", kSamsungBudsPro, sizeof(kSamsungBudsPro)},
};

}  // namespace ble
