#include "AudioOut.h"

#include <M5Cardputer.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <new>

#include "Logger.h"
#include "Storage.h"

namespace audioout {
namespace {

Audio* g_decoder = nullptr;
int g_volume = 12;  // 0..21, come la scala del decoder
bool g_speakerReleased = false;
// true se I2S_NUM_0 e' installato dal decoder. endPlayback() della Radio lo
// rilascia: senza questo flag la Musica (percorso legacy) scriverebbe su un
// driver assente e resterebbe muta.
bool g_i2sOwned = false;

// --- Metadati ICY ------------------------------------------------------------
// Limiti doc: 255 byte stazione, 511 byte titolo (UTF-8, terminatore escluso).
char g_station[256] = {0};
char g_title[512] = {0};

// --- Registrazione -----------------------------------------------------------
// Ring piccolo, allocato solo durante la REC: il callback copia in RAM, la SD
// viene scritta da pump(). Nessun buffer fisso quando la radio non registra.
constexpr uint32_t kRingSamples = 8192;  // 16 KB, circa 170 ms a 48 kHz stereo
constexpr uint32_t kRingBytes = kRingSamples * sizeof(int16_t);
constexpr uint32_t kRingMask = kRingSamples - 1;
// Ogni pump scrive al massimo 2 KB: il loop del decoder non resta fermo per
// una scrittura SD lunga.
constexpr uint32_t kWriteChunkSamples = 1024;  // 2 KB
int16_t* g_ring = nullptr;
volatile uint32_t g_head = 0;
volatile uint32_t g_tail = 0;

File g_file;
bool g_recording = false;
bool g_recError = false;
uint32_t g_recDataBytes = 0;
uint32_t g_recRate = 44100;
uint32_t g_droppedSamples = 0;

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
    logger::info("Audio: I2S restituito a M5Unified");
}

void copyText(char* dst, size_t cap, const char* src) {
    if (dst == nullptr || cap == 0) {
        return;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    size_t len = std::strlen(src);
    if (len >= cap) {
        len = cap - 1;
    }
    std::memcpy(dst, src, len);
    dst[len] = '\0';
}

uint32_t ringUsed() {
    if (g_ring == nullptr) {
        return 0;
    }
    const uint32_t used = g_head - g_tail;
    return used <= kRingSamples ? used : kRingSamples;
}

void put16(uint8_t* p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value & 0xFF);
    p[1] = static_cast<uint8_t>(value >> 8);
}

void put32(uint8_t* p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value & 0xFF);
    p[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

// WAV PCM 16 bit stereo: 44 byte di intestazione, poi i campioni.
bool writeWavHeader(File& file, uint32_t dataBytes, uint32_t sampleRate) {
    uint8_t h[44] = {0};
    std::memcpy(h + 0, "RIFF", 4);
    put32(h + 4, 36 + dataBytes);
    std::memcpy(h + 8, "WAVE", 4);
    std::memcpy(h + 12, "fmt ", 4);
    put32(h + 16, 16);                            // dimensione blocco fmt
    put16(h + 20, 1);                             // PCM
    put16(h + 22, 2);                             // stereo
    put32(h + 24, sampleRate);
    put32(h + 28, sampleRate * 4);                // byte al secondo
    put16(h + 32, 4);                             // block align
    put16(h + 34, 16);                            // bit per campione
    std::memcpy(h + 36, "data", 4);
    put32(h + 40, dataBytes);
    if (!file.seek(0)) {
        return false;
    }
    const size_t wrote = file.write(h, sizeof(h));
    return wrote == sizeof(h) && file.getWriteError() == 0;
}

void pushInterleaved(const int16_t* src, uint32_t count) {
    if (g_ring == nullptr) {
        return;
    }
    const uint32_t freeSamples = kRingSamples - ringUsed();
    if (count > freeSamples) {
        g_droppedSamples += count - freeSamples;
        count = freeSamples;
    }
    if (count == 0) {
        return;
    }
    uint32_t idx = g_head & kRingMask;
    const uint32_t first = std::min(count, kRingSamples - idx);
    std::memcpy(&g_ring[idx], src, first * sizeof(int16_t));
    if (count > first) {
        std::memcpy(&g_ring[0], src + first, (count - first) * sizeof(int16_t));
    }
    g_head += count;
}

// Gli stream mono vengono salvati come stereo (stesso campione L/R), cosi'
// l'intestazione resta sempre 16 bit / 2 canali come richiesto.
void pushMono(const int16_t* src, uint32_t frames) {
    if (g_ring == nullptr) {
        return;
    }
    const uint32_t freeFrames = (kRingSamples - ringUsed()) / 2;
    if (frames > freeFrames) {
        g_droppedSamples += (frames - freeFrames) * 2;
        frames = freeFrames;
    }
    for (uint32_t i = 0; i < frames; ++i) {
        const int16_t sample = src[i];
        g_ring[g_head & kRingMask] = sample;
        ++g_head;
        g_ring[g_head & kRingMask] = sample;
        ++g_head;
    }
}

bool drainChunk() {
    if (!g_file || g_ring == nullptr) {
        return false;
    }
    const uint32_t used = ringUsed();
    if (used == 0) {
        return true;
    }
    const uint32_t idx = g_tail & kRingMask;
    const uint32_t chunk = std::min(used, kRingSamples - idx);
    const size_t want = static_cast<size_t>(chunk) * sizeof(int16_t);
    const size_t wrote =
        g_file.write(reinterpret_cast<const uint8_t*>(&g_ring[idx]), want);
    if (wrote > 0) {
        const uint32_t samples = static_cast<uint32_t>(wrote / sizeof(int16_t));
        g_tail += samples;
        g_recDataBytes += samples * sizeof(int16_t);
    }
    if (wrote != want || g_file.getWriteError() != 0) {
        g_recError = true;
        return false;
    }
    return true;
}

void finishRecording() {
    g_recording = false;
    while (g_file && g_tail != g_head) {
        if (!drainChunk()) {
            break;
        }
    }
    if (g_file) {
        // Il sample rate puo' essere diventato noto dopo l'apertura: il WAV
        // viene completato ora con il valore reale dello stream.
        const uint32_t live = g_decoder ? g_decoder->getSampleRate() : 0;
        if (live >= 8000) {
            g_recRate = live;
        }
        if (!writeWavHeader(g_file, g_recDataBytes, g_recRate)) {
            g_recError = true;
            logger::warn("Audio: header WAV non riscritto (%u byte dati)",
                         static_cast<unsigned>(g_recDataBytes));
        }
        g_file.flush();
        g_file.close();
    }
    if (g_droppedSamples > 0) {
        logger::warn("Audio: REC, %u campioni persi (ring pieno)",
                     static_cast<unsigned>(g_droppedSamples));
    }
    // Il ring vive solo durante la registrazione.
    delete[] g_ring;
    g_ring = nullptr;
    g_head = 0;
    g_tail = 0;
}

void pumpRecording() {
    if (!g_recording || !g_file || g_ring == nullptr) {
        return;
    }
    const uint32_t used = ringUsed();
    if (used == 0) {
        return;
    }
    // Una sola fetta da 2 KB per pump(): decoder().loop() ha sempre la
    // precedenza e il ring assorbe le pause della SD.
    const uint32_t idx = g_tail & kRingMask;
    const uint32_t chunk = std::min(
        std::min(used, kRingSamples - idx), kWriteChunkSamples);
    const size_t want = static_cast<size_t>(chunk) * sizeof(int16_t);
    const size_t wrote =
        g_file.write(reinterpret_cast<const uint8_t*>(&g_ring[idx]), want);
    if (wrote > 0) {
        const uint32_t samples = static_cast<uint32_t>(wrote / sizeof(int16_t));
        g_tail += samples;
        g_recDataBytes += samples * sizeof(int16_t);
    }
    if (wrote != want || g_file.getWriteError() != 0) {
        g_recError = true;
        logger::warn("Audio: scrittura WAV fallita");
        finishRecording();
    }
}

}  // namespace

// Helper dei callback: static per restare interni a questo file.
static void setStation(const char* info) {
    copyText(g_station, sizeof(g_station), info);
}

static void setTitle(const char* info) {
    copyText(g_title, sizeof(g_title), info);
}

static void capturePcm(int16_t* buff, uint16_t len) {
    if (!g_recording || buff == nullptr || len == 0 || g_decoder == nullptr ||
        g_ring == nullptr) {
        return;
    }
    if (g_decoder->getBitsPerSample() != 16) {
        return;  // shortcut: stream 8 bit non registrati, la radio usa MP3/AAC 16 bit
    }
    // Il costruttore della libreria parte da 16000: il rate vero si conosce
    // solo dopo il primo frame decodificato.
    const uint32_t live = g_decoder->getSampleRate();
    if (live >= 8000) {
        g_recRate = live;
    }
    if (g_decoder->getChannels() == 2) {
        pushInterleaved(buff, static_cast<uint32_t>(len) * 2);
    } else {
        pushMono(buff, len);
    }
}

}  // namespace audioout

// Callback weak della libreria ESP32-audioI2S (vedi Audio.h). Devono avere
// linkage globale: qui restano leggeri e non bloccano mai il decoder.
void audio_showstation(const char* info) {
    audioout::setStation(info);
}

void audio_showstreamtitle(const char* info) {
    audioout::setTitle(info);
}

void audio_process_extern(int16_t* buff, uint16_t len, bool* continueI2S) {
    if (continueI2S != nullptr) {
        *continueI2S = true;  // il PCM deve comunque arrivare allo speaker
    }
    audioout::capturePcm(buff, len);
}

namespace audioout {

Audio& decoder() {
    decoderReady();
    assert(g_decoder != nullptr);  // i chiamanti usano beginPlayback()/decoderReady()
    return *g_decoder;
}

bool decoderReady() {
    if (g_decoder == nullptr) {
        // Nothrow: con heap frammentato il firmware non deve abortire per
        // una web radio. I chiamanti controllano il risultato.
        g_decoder = new (std::nothrow) Audio();
        if (g_decoder == nullptr) {
            logger::error("Audio: decoder non allocato (heap %u)",
                          static_cast<unsigned>(ESP.getFreeHeap()));
        } else {
            // Il costruttore di Audio installa I2S_NUM_0.
            g_i2sOwned = true;
        }
    }
    return g_decoder != nullptr;
}

bool decoderRunning() {
    return g_decoder != nullptr && g_decoder->isRunning();
}

int volume() {
    return g_volume;
}

void setVolume(int value) {
    g_volume = std::max(0, std::min(21, value));
    if (g_decoder != nullptr) {
        g_decoder->setVolume(static_cast<uint8_t>(g_volume));
    }
}

bool beginPlayback() {
    if (g_recording || g_file) {
        stopRecording();  // una REC rimasta aperta non deve restare orfana
    }
    releaseSpeaker();
    // Se il modulo Audio sta usando il microfono, I2S0 e' occupato e
    // M5.Mic.end() resetta anche il codec ES8311: la radio deve riprendersi
    // il bus e riaccendere il percorso DAC prima di (ri)creare il decoder.
    const bool micWasRunning = M5Cardputer.Mic.isRunning();
    M5Cardputer.Mic.end();
    if (micWasRunning) {
        M5Cardputer.Speaker.begin();  // riscrive i registri ES8311 del DAC
        M5Cardputer.Speaker.end();    // libera subito I2S1
    }
    if (!decoderReady()) {
        restoreSpeaker();  // ridà almeno lo speaker alla UI
        return false;
    }
    Audio& audio = *g_decoder;
    // Il microfono (M5.Mic) usa I2S_NUM_0 come il decoder: se un altro modulo
    // lo ha fermato o riconfigurato, qui il driver viene ripristinato prima
    // di aprire lo stream.
    reinit();
    if (!g_i2sOwned) {
        restoreSpeaker();
        return false;
    }
    // I pin non sono cablati a mano: si leggono dalla board.
    const auto cfg = M5Cardputer.Speaker.config();
    const bool pinsOk = audio.setPinout(cfg.pin_bck, cfg.pin_ws, cfg.pin_data_out);
    if (!pinsOk) {
        // Non lasciare I2S0 installato ne' lo speaker chiuso se la
        // configurazione dei pin fallisce.
        i2s_driver_uninstall(I2S_NUM_0);
        g_i2sOwned = false;
        restoreSpeaker();
        logger::warn("Audio: setPinout fallito, I2S0 rilasciato");
        return false;
    }
    audio.setVolume(static_cast<uint8_t>(g_volume));
    clearMetadata();
    logger::info("Audio: I2S bck=%d ws=%d dout=%d", static_cast<int>(cfg.pin_bck),
                 static_cast<int>(cfg.pin_ws), static_cast<int>(cfg.pin_data_out));
    return g_speakerReleased && pinsOk;
}

// Percorso 1.21: niente Mic.end() e niente reinit I2S0 nel caso normale.
// Se Radio ha rilasciato I2S0 (endPlayback), viene reinstallato solo allora.
// Ripristina il comportamento del lettore Musica senza toccare Radio/Audio.
bool beginPlaybackLegacy() {
    releaseSpeaker();
    // Solo se il microfono e' davvero acceso (modulo Audio): chiude I2S0 e
    // riaccende il DAC ES8311. Nel percorso normale non fa nulla, quindi la
    // Musica 1.21 resta invariata.
    if (M5Cardputer.Mic.isRunning()) {
        M5Cardputer.Mic.end();
        M5Cardputer.Speaker.begin();
        M5Cardputer.Speaker.end();
        g_i2sOwned = false;
    }
    if (!decoderReady()) {
        restoreSpeaker();
        return false;
    }
    if (!g_i2sOwned) {
        reinit();
        if (!g_i2sOwned) {
            restoreSpeaker();
            return false;
        }
    }
    const auto cfg = M5Cardputer.Speaker.config();
    if (!g_decoder->setPinout(cfg.pin_bck, cfg.pin_ws, cfg.pin_data_out)) {
        i2s_driver_uninstall(I2S_NUM_0);
        g_i2sOwned = false;
        restoreSpeaker();
        logger::warn("Audio legacy: setPinout fallito, I2S0 rilasciato");
        return false;
    }
    g_decoder->setVolume(static_cast<uint8_t>(g_volume));
    clearMetadata();
    logger::info("Audio legacy: I2S bck=%d ws=%d dout=%d",
                 static_cast<int>(cfg.pin_bck), static_cast<int>(cfg.pin_ws),
                 static_cast<int>(cfg.pin_data_out));
    return g_speakerReleased;
}

void endPlaybackLegacy() {
    if (g_decoder != nullptr && g_i2sOwned) {
        g_decoder->stopSong();
    }
    // Rilascia I2S0 a fine ascolto: senza questo il microfono del modulo
    // Audio (stesso I2S_NUM_0) non riuscirebbe ad aprirsi.
    if (g_i2sOwned) {
        i2s_driver_uninstall(I2S_NUM_0);
        g_i2sOwned = false;
    }
    restoreSpeaker();
}

void reinit() {
    // Config identica a quella del costruttore di Audio: 16 DMA buffer da
    // 512 frame, TX master, 16 bit stereo. Il sample rate viene poi impostato
    // dal decoder quando conosce lo stream.
    if (g_i2sOwned) {
        i2s_driver_uninstall(I2S_NUM_0);
        g_i2sOwned = false;
    }
    i2s_config_t cfg = {};
    cfg.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = 16000;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format =
        static_cast<i2s_comm_format_t>(I2S_COMM_FORMAT_STAND_I2S);
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 16;
    cfg.dma_buf_len = 512;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;
    cfg.fixed_mclk = I2S_PIN_NO_CHANGE;
    const esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr);
    g_i2sOwned = (err == ESP_OK);
    if (err != ESP_OK) {
        logger::warn("Audio: i2s_driver_install su I2S0 fallita (%d)",
                     static_cast<int>(err));
    }
}

void endPlayback() {
    // Prima si ferma la sorgente, poi si chiude il WAV: nessun altro PCM
    // arriva nel ring mentre lo si svuota e si riscrive l'header.
    if (g_decoder != nullptr && g_i2sOwned) {
        g_decoder->stopSong();
    }
    if (g_recording || g_file) {
        stopRecording();
    }
    // Libera I2S0: il microfono (M5.Mic) e altri moduli possono riusarlo.
    if (g_i2sOwned) {
        i2s_driver_uninstall(I2S_NUM_0);
        g_i2sOwned = false;
    }
    restoreSpeaker();
}

void pump() {
    if (g_decoder != nullptr) {
        g_decoder->loop();
    }
    // Fette da 2 KB: al massimo 4 per pump (8 KB), così il decoder riprende
    // il controllo prima del frame successivo e non resta fermo su una
    // scrittura unica da 16 KB. I drop sono esposti da recordingDropped().
    for (int i = 0; i < 4 && ringUsed() > 0; ++i) {
        pumpRecording();
    }
}

const char* streamStation() {
    return g_station;
}

const char* streamTitle() {
    return g_title;
}

void clearMetadata() {
    g_station[0] = '\0';
    g_title[0] = '\0';
}

bool startRecording(const char* path) {
    if (g_recording || path == nullptr || path[0] == '\0') {
        return false;
    }
    if (!storage::hasSD()) {
        return false;
    }
    storage::mkdir("/audio");
    storage::mkdir("/audio/rec");

    File file = storage::openWrite(path, true);
    if (!file) {
        return false;
    }
    if (g_ring == nullptr) {
        // 16 KB servono solo mentre si registra: controllo e allocazione qui.
        const uint32_t freeHeap = ESP.getFreeHeap();
        if (freeHeap < kRingBytes + 8192U) {
            logger::warn("Audio: heap insufficiente per ring REC (%u byte)", freeHeap);
            file.close();
            storage::removeFile(path, true);
            return false;
        }
        g_ring = new (std::nothrow) int16_t[kRingSamples];
        if (g_ring == nullptr) {
            logger::warn("Audio: ring REC non allocato");
            file.close();
            storage::removeFile(path, true);
            return false;
        }
        logger::info("Audio: ring REC allocato (%u byte, heap %u)",
                     static_cast<unsigned>(kRingBytes), freeHeap);
    }

    const uint32_t live = g_decoder ? g_decoder->getSampleRate() : 0;
    g_recRate = live >= 8000 ? live : 44100;
    g_recDataBytes = 0;
    g_droppedSamples = 0;
    g_recError = false;
    g_head = 0;
    g_tail = 0;
    g_file = file;
    const bool headerOk = writeWavHeader(g_file, 0, g_recRate);
    g_file.flush();
    if (!headerOk || g_file.getWriteError() != 0) {
        g_file.close();
        g_file = File();
        delete[] g_ring;
        g_ring = nullptr;
        storage::removeFile(path, true);
        return false;
    }
    g_recording = true;
    logger::info("Audio: REC %s (%u Hz)", path, static_cast<unsigned>(g_recRate));
    return true;
}
uint32_t stopRecording() {
    if (g_file) {
        finishRecording();
        logger::info("Audio: REC chiusa, %u byte PCM",
                     static_cast<unsigned>(g_recDataBytes));
    }
    g_recording = false;
    return g_recDataBytes;
}

bool recording() {
    return g_recording;
}

uint32_t recordingBytes() {
    return g_recDataBytes + ringUsed() * sizeof(int16_t);
}

uint32_t recordingSampleRate() {
    return g_recRate;
}

uint32_t recordingDropped() {
    return g_droppedSamples;
}

bool recordingError() {
    return g_recError;
}

}  // namespace audioout
