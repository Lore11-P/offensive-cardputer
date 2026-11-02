// ============================================================================
//  Web Radio - ascolto di stazioni internet (MP3/AAC via HTTP)
//
//  Decoder: ESP32-audioI2S (schreibfaul1). Il decoder prende possesso del bus
//  I2S dello speaker, quindi durante l'ascolto M5.Speaker viene chiuso
//  (Speaker.end()) e riaperto all'uscita: niente beep del radar mentre suona
//  la radio, e nessun conflitto sul peripherale.
//
//  I pin I2S non sono cablati a mano: si leggono a runtime da
//  M5.Speaker.config() (pin_bck / pin_ws / pin_data_out), cosi' il modulo
//  segue la board invece di indovinare.
//
//  Gli stream sono HTTP (non HTTPS): il decoder non ha un client TLS adatto e
//  la RAM e' contata. Se una stazione smette di funzionare basta cambiare
//  l'URL nella tabella kStations.
// ============================================================================

#include "RadioMenu.h"

#include <Arduino.h>
#include <Audio.h>
#include <M5Cardputer.h>

#include <algorithm>
#include <string>
#include <vector>

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

Audio g_audio;
int g_volume = 12;  // scala del decoder: 0..21
bool g_speakerReleased = false;
bool g_decoderConfigured = false;

std::string shortened(const std::string& text, size_t maxLen) {
    if (text.size() <= maxLen || maxLen < 2) {
        return text;
    }
    return text.substr(0, maxLen - 1) + "~";
}

void releaseSpeaker() {
    if (g_speakerReleased) {
        return;
    }
    M5Cardputer.Speaker.stop();
    M5Cardputer.Speaker.end();  // libera il bus I2S per il decoder
    g_speakerReleased = true;
    logger::info("Radio: I2S passato al decoder");
}

void restoreSpeaker() {
    if (!g_speakerReleased) {
        return;
    }
    M5Cardputer.Speaker.begin();  // riprende l'I2S per i beep dell'interfaccia
    g_speakerReleased = false;
    g_decoderConfigured = false;
    logger::info("Radio: I2S restituito a M5Unified");
}

void configureDecoder() {
    if (g_decoderConfigured) {
        return;
    }
    releaseSpeaker();
    const auto cfg = M5Cardputer.Speaker.config();
    g_audio.setPinout(cfg.pin_bck, cfg.pin_ws, cfg.pin_data_out);
    g_audio.setVolume(static_cast<uint8_t>(g_volume));
    g_decoderConfigured = true;
    logger::info("Radio: I2S bck=%d ws=%d dout=%d", static_cast<int>(cfg.pin_bck),
                 static_cast<int>(cfg.pin_ws), static_cast<int>(cfg.pin_data_out));
}

void drawPlayer(int stationIndex) {
    display::clear();
    display::header("Web Radio", "Radio");

    const Station& station = kStations[stationIndex];
    const bool running = g_audio.isRunning();

    display::textCentered(24, shortened(station.name, 26),
                          running ? display::kAccent : display::kWarning, 2);
    display::textCentered(42, running ? "in riproduzione" : "connessione...",
                          running ? display::kText : display::kTextDim, 0);

    // Bitrate e frequenza di campionamento, quando disponibili.
    const int bitrate = g_audio.getBitRate();
    const int rate = g_audio.getSampleRate();
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
    for (int i = 0; i < kSegments; ++i) {
        const int x = kBarX + i * kSegW;
        const bool lit = i < g_volume;
        const uint16_t color = lit ? (i > 16 ? display::kDanger : display::kNeoGreen)
                                   : display::kPanel;
        display::g().fillRect(x, kBarY, kSegW - 2, kSegH, color);
    }
    display::textCentered(90, "volume " + std::to_string(g_volume) + "/21", display::kText, 0);

    display::text(6, 108, shortened(station.url, 38), display::kTextDim, 0);
    display::footer(";.=volume  ENTER=stop  `=esci");
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

    configureDecoder();
    g_audio.setVolume(static_cast<uint8_t>(g_volume));
    if (!g_audio.connecttohost(kStations[stationIndex].url)) {
        ui::message("Web Radio",
                    {"!Connessione al flusso fallita.", kStations[stationIndex].url,
                     "La stazione potrebbe essere offline."},
                    "Radio");
        restoreSpeaker();
        return;
    }

    // Ascolto: il timer di spegnimento non deve attenuare/spegnere.
    powertimer::Hold hold;
    logger::info("Radio: %s", kStations[stationIndex].name);

    uint32_t lastDraw = 0;
    navigation::waitRelease();
    for (;;) {
        navigation::update();
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back || ev.type == navigation::Event::Select) {
            break;
        }
        if (ev.type == navigation::Event::Up) {
            g_volume = std::min(21, g_volume + 1);
            g_audio.setVolume(static_cast<uint8_t>(g_volume));
            lastDraw = 0;
        } else if (ev.type == navigation::Event::Down) {
            g_volume = std::max(0, g_volume - 1);
            g_audio.setVolume(static_cast<uint8_t>(g_volume));
            lastDraw = 0;
        }

        g_audio.loop();  // pompa lo stream: va chiamato spesso

        if (millis() - lastDraw > 200) {
            lastDraw = millis();
            drawPlayer(stationIndex);
        }
        delay(2);
    }

    g_audio.stopSong();
    restoreSpeaker();
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
                    {g_speakerReleased ? ">Decoder attivo" : ">Speaker libero",
                     "Volume: " + std::to_string(g_volume) + "/21",
                     "Le stazioni sono in HTTP MP3."},
                    "Radio");
    });
    menu.addItem("WiFi", "wifi", [] { wifisetup::runSettings("Radio"); });
    menu.setHint("ENTER=ascolta  `=indietro");
    menu.run(true);
}

}  // namespace radio

#endif  // ENABLE_MODULE_RADIO
