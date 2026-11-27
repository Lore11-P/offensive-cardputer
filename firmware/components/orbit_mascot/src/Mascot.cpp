// Mascotte "Zorbo": i punti arrivano solo dopo il legalWarning e sono fissi
// per azione completata (mai per pacchetto). Qui vivono stato NVS e loop UI.
#include "orbit_mascot/Mascot.h"

#include <cstdio>
#include <cstring>

#include <nvs.h>

#include "Display.h"
#include "MascotDraw.h"
#include "Navigation.h"
#include "orbit_compat.h"

namespace mascot {
namespace {

constexpr const char* kNamespace = "orbit-mascot";

// Stato di sessione (il timestamp dell'ultimo award non viene persistito:
// il fumetto evento vale solo nella sessione corrente).
bool s_loaded = false;
uint32_t s_score = 0;
uint8_t s_affection = 30;  // primo avvio / NVS vuota: Zorbo e' gia' curioso
Event s_lastEvent = Event::Count;
uint16_t s_lastPoints = 0;
bool s_hasAward = false;
uint32_t s_lastAwardMs = 0;
uint32_t s_lastPetMs = 0;  // 0 = mai coccolata in questa sessione

void load() {
    if (s_loaded) {
        return;
    }
    s_loaded = true;  // se NVS non risponde non ritentiamo a ogni uso

    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    uint32_t score = 0;
    uint8_t value = 0;
    uint16_t points = 0;
    if (nvs_get_u32(handle, "score", &score) == ESP_OK) {
        s_score = clampScore(score);
    }
    if (nvs_get_u8(handle, "aff", &value) == ESP_OK) {
        s_affection = clampAffection(value);
    }
    if (nvs_get_u8(handle, "evt", &value) == ESP_OK &&
        value < static_cast<uint8_t>(Event::Count)) {
        s_lastEvent = static_cast<Event>(value);
    }
    if (nvs_get_u16(handle, "pts", &points) == ESP_OK) {
        s_lastPoints = points;
    }
    nvs_close(handle);
}

void save() {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_set_u32(handle, "score", s_score);
    nvs_set_u8(handle, "aff", s_affection);
    nvs_set_u8(handle, "evt", static_cast<uint8_t>(s_lastEvent));
    nvs_set_u16(handle, "pts", s_lastPoints);
    nvs_commit(handle);
    nvs_close(handle);
}

// Battute evento: solo string literals, nessuna allocazione.
const char* eventSpeech(Event event) {
    switch (event) {
        case Event::WifiDeauth:
            return "Deauth +10|Solo reti tue.";
        case Event::WifiBeacon:
            return "Beacon +15|Router confuso.";
        case Event::WifiDeauthFlood:
            return "Flood +25|Rete in tilt.";
        case Event::EvilTwin:
            return "Twin +20|Chi e' il vero?";
        case Event::BleSpam:
            return "BLE spam +10|Che caos!";
        case Event::IrJammer:
            return "Jammer +5|Zap zap!";
        case Event::AirTagSpoof:
            return "AirTag +15|Fantasma!";
        case Event::Count:
        default:
            return "";
    }
}

const char* const kIdle[] = {
    "Che solletico!",
    "Scansione...|Nessun pericolo.",
    "Un grattino?|Sono timido.",
    "Zorbo approva.|Test nel lab.",
    "Le reti altrui?|Non si toccano.",
};
constexpr uint8_t kIdleCount = sizeof(kIdle) / sizeof(kIdle[0]);

}  // namespace

void begin() {
    load();
}

void award(Event event) {
    const uint16_t points = pointsFor(event);
    if (points == 0) {
        return;  // Count o evento senza punti: no-op
    }
    load();
    s_score = clampScore(static_cast<uint64_t>(s_score) + points);
    s_lastEvent = event;
    s_lastPoints = points;
    s_hasAward = true;
    s_lastAwardMs = millis();
    save();
}

void runMenu() {
    load();
    navigation::waitRelease();

    const uint32_t entryMs = millis();
    uint8_t hearts = 0;
    uint32_t lastHeartMs = entryMs;
    const char* petSpeech = nullptr;
    uint32_t petSpeechMs = 0;

    for (;;) {
        const uint32_t now = millis();
        const navigation::EventData ev = navigation::poll();

        if (ev.type == navigation::Event::Back) {
            break;
        }
        if (ev.type == navigation::Event::Select) {
            if (!petReady(now, s_lastPetMs)) {
                petSpeech = "Aspetta un po'...";
                petSpeechMs = now;
            } else {
                const bool earns = canEarnPetPoints(s_affection);
                s_affection = affectionAfterPet(s_affection);
                if (earns) {
                    s_score = clampScore(static_cast<uint64_t>(s_score) + 1);
                }
                s_lastPetMs = now;
                hearts = 10;
                lastHeartMs = now;
                petSpeech = earns ? "Ahhh, che bello!|Che solletico!" : "Cuore pieno!";
                petSpeechMs = now;
                save();
            }
        }

        if (hearts > 0 && now - lastHeartMs >= 100) {
            lastHeartMs = now;
            --hearts;
        }

        const char* speech;
        if (s_hasAward && now - s_lastAwardMs < 10000) {
            speech = eventSpeech(s_lastEvent);
        } else if (petSpeech != nullptr && now - petSpeechMs < 2000) {
            speech = petSpeech;
        } else {
            speech = kIdle[((now - entryMs) / 7000) % kIdleCount];
        }

        char eventLabel[48];
        if (s_lastEvent != Event::Count) {
            snprintf(eventLabel, sizeof(eventLabel), "Ultima: %s +%u", labelFor(s_lastEvent),
                     static_cast<unsigned>(s_lastPoints));
        } else {
            eventLabel[0] = '\0';
        }

        View view;
        view.score = s_score;
        view.affection = s_affection;
        view.hearts = hearts;
        view.blink = ((now - entryMs) % 4000) < 120;
        view.happy = s_affection >= 50;
        view.speech = speech;
        view.event = eventLabel;
        view.nowMs = now;
        drawScene(view);

        delay(33);
    }

    navigation::waitRelease();
}

}  // namespace mascot
