#include "AudioOut.h"

#include <M5Cardputer.h>

#include <algorithm>

#include "Logger.h"

namespace audioout {
namespace {

Audio* g_decoder = nullptr;
int g_volume = 12;  // 0..21, come la scala del decoder
bool g_speakerReleased = false;
bool g_pinsConfigured = false;

void releaseSpeaker() {
    if (g_speakerReleased) {
        return;
    }
    M5Cardputer.Speaker.stop();
    M5Cardputer.Speaker.end();  // libera il bus I2S per il decoder
    g_speakerReleased = true;
    logger::info("Audio: I2S passato al decoder");
}

void restoreSpeaker() {
    if (!g_speakerReleased) {
        return;
    }
    M5Cardputer.Speaker.begin();
    g_speakerReleased = false;
    g_pinsConfigured = false;
    logger::info("Audio: I2S restituito a M5Unified");
}

}  // namespace

Audio& decoder() {
    if (g_decoder == nullptr) {
        // Il costruttore di Audio installa I2S_NUM_0: lo si crea solo quando
        // serve davvero, cosi' il microfono resta utilizzabile altrimenti.
        g_decoder = new Audio();
    }
    return *g_decoder;
}

int volume() {
    return g_volume;
}

void setVolume(int value) {
    g_volume = std::max(0, std::min(21, value));
    decoder().setVolume(static_cast<uint8_t>(g_volume));
}

bool beginPlayback() {
    releaseSpeaker();
    if (!g_pinsConfigured) {
        // I pin non sono cablati a mano: si leggono dalla board.
        const auto cfg = M5Cardputer.Speaker.config();
        decoder().setPinout(cfg.pin_bck, cfg.pin_ws, cfg.pin_data_out);
        decoder().setVolume(static_cast<uint8_t>(g_volume));
        g_pinsConfigured = true;
        logger::info("Audio: I2S bck=%d ws=%d dout=%d",
                     static_cast<int>(cfg.pin_bck), static_cast<int>(cfg.pin_ws),
                     static_cast<int>(cfg.pin_data_out));
    }
    return g_speakerReleased;
}

void endPlayback() {
    decoder().stopSong();
    restoreSpeaker();
}

void pump() {
    decoder().loop();
}

}  // namespace audioout
