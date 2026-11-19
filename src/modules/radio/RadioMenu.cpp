// ============================================================================
//  Web Radio - ascolto di stazioni internet (MP3/AAC via HTTP)
//
//  Decoder: ESP32-audioI2S (schreibfaul1), condiviso con il lettore MP3
//  attraverso utils/AudioOut. Il decoder prende possesso del bus I2S dello
//  speaker, quindi durante l'ascolto M5.Speaker viene chiuso (Speaker.end())
//  e riaperto all'uscita: niente beep del radar mentre suona la radio.
//
//  Gli stream sono HTTP (non HTTPS): il decoder non ha un client TLS adatto e
//  la RAM e' contata. Se una stazione smette di funzionare basta cambiare
//  l'URL nella tabella kStations.
// ============================================================================

#include "RadioMenu.h"

#include <Arduino.h>
#include <M5Cardputer.h>

#include <algorithm>
#include <string>
#include <vector>

#include "AudioOut.h"
#include "Display.h"
#include "Logger.h"
#include "MenuManager.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "PowerTimer.h"
#include "Ui.h"
#include "WifiSetup.h"

#if ENABLE_MODULE_RADIO

namespace radio {
namespace {

struct Station {
    const char* name;
    const char* url;
};

// Stazioni pubbliche in MP3 su HTTP. Se una non risponde piu', sostituisci
// l'URL: non serve ricompilare altro.
constexpr Station kStations[] = {
    {"SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3"},
    {"SomaFM Drone Zone", "http://ice1.somafm.com/dronezone-128-mp3"},
    {"SomaFM Lush", "http://ice1.somafm.com/lush-128-mp3"},
    {"Radio Paradise", "http://stream.radioparadise.com/mp3-128"},
    {"FIP (Radio France)", "http://icecast.radiofrance.fr/fip-midfi.mp3"},
};
constexpr int kStationCount = static_cast<int>(sizeof(kStations) / sizeof(kStations[0]));

std::string shortened(const std::string& text, size_t maxLen) {
    if (text.size() <= maxLen || maxLen < 2) {
        return text;
    }
    return text.substr(0, maxLen - 1) + "~";
}

void drawPlayer(int stationIndex) {
    display::clear();
    display::header("Web Radio", "Radio");

    const Station& station = kStations[stationIndex];
    const bool running = audioout::decoder().isRunning();

    display::textCentered(24, shortened(station.name, 26),
                          running ? display::kAccent : display::kWarning, 2);
    display::textCentered(42, running ? "in riproduzione" : "connessione...",
                          running ? display::kText : display::kTextDim, 0);

    // Bitrate e frequenza di campionamento, quando disponibili.
    const int bitrate = static_cast<int>(audioout::decoder().getBitRate());
    const int rate = static_cast<int>(audioout::decoder().getSampleRate());
    if (bitrate > 0) {
        display::textCentered(56, std::to_string(bitrate) + " kbit/s" +
                                      (rate > 0 ? "  " + std::to_string(rate) + " Hz" : ""),
                              display::kTextDim, 0);
    }

    // Barra del volume a segmenti (0..21).
    constexpr int kSegments = 21;
    constexpr int kBarX = 14;
    constexpr int kBarY = 76;
    constexpr int kSegW = 9;
    constexpr int kSegH = 10;
    const int volume = audioout::volume();
    for (int i = 0; i < kSegments; ++i) {
        const int x = kBarX + i * kSegW;
        const bool lit = i < volume;
        const uint16_t color = lit ? (i > 16 ? display::kDanger : display::kNeoGreen)
                                   : display::kPanel;
        display::g().fillRect(x, kBarY, kSegW - 2, kSegH, color);
    }
    display::textCentered(90, "volume " + std::to_string(volume) + "/21", display::kText, 0);

    display::text(6, 108, shortened(station.url, 38), display::kTextDim, 0);
    display::footer(";.=volume  ENTER=stop  ESC=esci");
    display::push();
}

void playStation(int stationIndex) {
    if (stationIndex < 0 || stationIndex >= kStationCount) {
        return;
    }
    if (!wifisetup::ensureConnected(true, "Radio")) {
        ui::message("Web Radio", {"!Wi-Fi non connesso.", "Configura la rete da qui o da",
                                  "Impostazioni > Rete WiFi."},
                    "Radio");
        return;
    }

    audioout::beginPlayback();
    audioout::setVolume(audioout::volume());
    if (!audioout::decoder().connecttohost(kStations[stationIndex].url)) {
        ui::message("Web Radio",
                    {"!Connessione al flusso fallita.", kStations[stationIndex].url,
                     "La stazione potrebbe essere offline."},
                    "Radio");
        audioout::endPlayback();
        return;
    }

    // Ascolto: il timer di spegnimento non deve attenuare/spegnere.
    powertimer::Hold hold;
    logger::info("Radio: %s", kStations[stationIndex].name);

    const uint32_t startedMs = millis();
    uint32_t lastDraw = 0;
    navigation::waitRelease();
    for (;;) {
        navigation::update();
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back || ev.type == navigation::Event::Select) {
            break;
        }
        if (ev.type == navigation::Event::Up) {
            audioout::setVolume(audioout::volume() + 1);
            lastDraw = 0;
        } else if (ev.type == navigation::Event::Down) {
            audioout::setVolume(audioout::volume() - 1);
            lastDraw = 0;
        }

        audioout::pump();  // pompa lo stream: va chiamato spesso

        // Il decoder spegne isRunning() se il flusso cade (404, rete assente):
        // senza questo controllo la schermata direbbe "in riproduzione" per
        // sempre.
        if (!audioout::decoder().isRunning() && millis() - startedMs > 8000) {
            ui::message("Web Radio",
                        {"!Flusso interrotto.", kStations[stationIndex].url},
                        "Radio");
            break;
        }

        if (millis() - lastDraw > 200) {
            lastDraw = millis();
            drawPlayer(stationIndex);
        }
        delay(2);
    }

    audioout::endPlayback();
    logger::info("Radio: fermata dall'utente");
}

}  // namespace

void runMenu() {
    MenuManager menu("Radio", "Menu principale");
    for (int i = 0; i < kStationCount; ++i) {
        const int index = i;
        menu.addItem(kStations[i].name, "tv", [index] { playStation(index); });
    }
    menu.addSeparator("Ascolto");
    menu.addItem("Ferma / stato", "lock", [] {
        ui::message("Web Radio",
                    {audioout::decoder().isRunning() ? ">Decoder attivo" : ">Decoder fermo",
                     "Volume: " + std::to_string(audioout::volume()) + "/21",
                     "Le stazioni sono in HTTP MP3.",
                     "Gli MP3 locali sono nel menu Musica."},
                    "Radio");
    });
    menu.addItem("WiFi", "wifi", [] { wifisetup::runSettings("Radio"); });
    menu.setHint("ENTER=ascolta  ESC=indietro");
    menu.run(true);
}

}  // namespace radio

#endif  // ENABLE_MODULE_RADIO
