#pragma once

#include <cstdint>

// Logica pura della mascotte: nessuna dipendenza Arduino/display, cosi' i
// test host-native possono verificarla. I punti sono fissi per azione
// completata (non per pacchetto): il volume non deve incentivare danni.
namespace mascot {

enum class Event : uint8_t {
    WifiDeauth = 0,
    WifiBeacon,
    WifiDeauthFlood,
    EvilTwin,
    BleSpam,
    IrJammer,
    AirTagSpoof,
    Count
};

constexpr uint32_t kPetCooldownMs = 12000;
constexpr uint8_t kPetAffection = 2;
constexpr uint8_t kMaxAffection = 100;
constexpr uint32_t kMaxScore = 9999999;

constexpr uint16_t pointsFor(Event event) {
    switch (event) {
        case Event::WifiDeauth:
            return 10;
        case Event::WifiBeacon:
            return 15;
        case Event::WifiDeauthFlood:
            return 25;
        case Event::EvilTwin:
            return 20;
        case Event::BleSpam:
            return 10;
        case Event::IrJammer:
            return 5;
        case Event::AirTagSpoof:
            return 15;
        case Event::Count:
        default:
            return 0;
    }
}

constexpr const char* labelFor(Event event) {
    switch (event) {
        case Event::WifiDeauth:
            return "Deauth";
        case Event::WifiBeacon:
            return "Beacon spam";
        case Event::WifiDeauthFlood:
            return "Deauth flood";
        case Event::EvilTwin:
            return "Evil twin";
        case Event::BleSpam:
            return "BLE spam";
        case Event::IrJammer:
            return "Jammer IR";
        case Event::AirTagSpoof:
            return "AirTag spoof";
        case Event::Count:
        default:
            return "?";
    }
}

constexpr uint8_t clampAffection(int value) {
    return value < 0 ? 0 : (value > kMaxAffection ? kMaxAffection : static_cast<uint8_t>(value));
}

constexpr uint32_t clampScore(uint64_t value) {
    return value > kMaxScore ? kMaxScore : static_cast<uint32_t>(value);
}

constexpr uint8_t affectionAfterPet(uint8_t current) {
    return clampAffection(static_cast<int>(current) + kPetAffection);
}

constexpr bool canEarnPetPoints(uint8_t affection) {
    return affection < kMaxAffection;
}

// Unsigned subtraction: gestisce anche il wrap di millis().
constexpr bool petReady(uint32_t nowMs, uint32_t lastPetMs) {
    return lastPetMs == 0 || (nowMs - lastPetMs) >= kPetCooldownMs;
}

}  // namespace mascot
