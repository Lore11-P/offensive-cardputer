// ============================================================================
//  Web Radio - ascolto di stazioni internet (MP3/AAC via HTTP)
//
//  Decoder: ESP32-audioI2S (schreibfaul1), condiviso con il lettore MP3
//  attraverso utils/AudioOut. Il decoder prende possesso del bus I2S dello
//  speaker, quindi durante l'ascolto M5.Speaker viene chiuso (Speaker.end())
//  e riaperto all'uscita: niente beep del radar mentre suona la radio.
//
//  Playlist: /radio/stations.txt sulla microSD, una riga per stazione in
//  formato "Nome|URL" (accettato anche "Nome,URL"). Senza file valido si
//  usano le stazioni predefinite qui sotto.
//
//  Gli stream sono HTTP (non HTTPS): il decoder non ha un client TLS adatto e
//  la RAM e' contata. I metadati ICY arrivano dai callback weak della
//  libreria (implementati in AudioOut.cpp).
// ============================================================================

#include "RadioMenu.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <SD.h>
#include <stdio.h>
#include <time.h>

#include <algorithm>
#include <string>
#include <vector>

#include "AudioOut.h"
#include "Display.h"
#include "Logger.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "PowerTimer.h"
#include "Storage.h"
#include "Ui.h"
#include "WifiSetup.h"

#if ENABLE_MODULE_RADIO

namespace radio {
namespace {

constexpr const char* kPlaylistPath = "/radio/stations.txt";
constexpr size_t kPlaylistMaxBytes = 8192;
constexpr int kMaxStations = 40;
constexpr int kVisibleRows = 4;
constexpr int kRowHeight = 22;
constexpr int kFirstRowY = 26;

// Stazioni pubbliche in MP3 su HTTP. Usate quando la playlist SD manca.
struct DefaultStation {
    const char* name;
    const char* url;
};
constexpr DefaultStation kDefaultStations[] = {
    {"SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3"},
    {"SomaFM Drone Zone", "http://ice1.somafm.com/dronezone-128-mp3"},
    {"SomaFM Lush", "http://ice1.somafm.com/lush-128-mp3"},
    {"Radio Paradise", "http://stream.radioparadise.com/mp3-128"},
    {"FIP (Radio France)", "http://icecast.radiofrance.fr/fip-midfi.mp3"},
};

struct Station {
    std::string name;
    std::string url;
};

// Etichetta breve e descrizione della sorgente della playlist.
std::string g_sourceTag = "DEF";
std::string g_sourceNote = "predefinita";

std::string trim(const std::string& in) {
    size_t begin = 0;
    size_t end = in.size();
    while (begin < end && (in[begin] == ' ' || in[begin] == '\t' || in[begin] == '\r' ||
                           in[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (in[end - 1] == ' ' || in[end - 1] == '\t' ||
                           in[end - 1] == '\r' || in[end - 1] == '\n')) {
        --end;
    }
    return in.substr(begin, end - begin);
}

// Il font ORBIT copre solo ASCII: traslittera accenti e punteggiatura UTF-8
// (o Latin-1) invece di disegnare una fila di '?'.
void appendAscii(std::string& out, uint32_t cp) {
    if (cp == 0x00A0) {
        out += ' ';
        return;
    }
    if (cp >= 0x00C0 && cp <= 0x00C5) { out += 'A'; return; }
    if (cp >= 0x00E0 && cp <= 0x00E5) { out += 'a'; return; }
    if (cp == 0x00C6) { out += "AE"; return; }
    if (cp == 0x00E6) { out += "ae"; return; }
    if (cp == 0x00C7) { out += 'C'; return; }
    if (cp == 0x00E7) { out += 'c'; return; }
    if (cp >= 0x00C8 && cp <= 0x00CB) { out += 'E'; return; }
    if (cp >= 0x00E8 && cp <= 0x00EB) { out += 'e'; return; }
    if (cp >= 0x00CC && cp <= 0x00CF) { out += 'I'; return; }
    if (cp >= 0x00EC && cp <= 0x00EF) { out += 'i'; return; }
    if (cp == 0x00D0) { out += 'D'; return; }
    if (cp == 0x00F0) { out += 'd'; return; }
    if (cp == 0x00D1) { out += 'N'; return; }
    if (cp == 0x00F1) { out += 'n'; return; }
    if (cp >= 0x00D2 && cp <= 0x00D6) { out += 'O'; return; }
    if (cp >= 0x00F2 && cp <= 0x00F6) { out += 'o'; return; }
    if (cp == 0x00D7) { out += 'x'; return; }
    if (cp == 0x00D8) { out += 'O'; return; }
    if (cp == 0x00F8) { out += 'o'; return; }
    if (cp >= 0x00D9 && cp <= 0x00DC) { out += 'U'; return; }
    if (cp >= 0x00F9 && cp <= 0x00FC) { out += 'u'; return; }
    if (cp == 0x00DD || cp == 0x00FF) { out += 'Y'; return; }
    if (cp == 0x00FD) { out += 'y'; return; }
    if (cp == 0x00DE) { out += "TH"; return; }
    if (cp == 0x00FE) { out += "th"; return; }
    if (cp == 0x00DF) { out += "ss"; return; }
    if (cp == 0x2018 || cp == 0x2019 || cp == 0x201A) { out += '\''; return; }
    if (cp == 0x201C || cp == 0x201D || cp == 0x00AB || cp == 0x00BB) {
        out += '"';
        return;
    }
    if (cp == 0x2013 || cp == 0x2014 || cp == 0x2212) { out += '-'; return; }
    if (cp == 0x2026) { out += "..."; return; }
    if (cp == 0x20AC) { out += "EUR"; return; }
    if (cp >= 0x20 && cp < 0x7F) {
        out += static_cast<char>(cp);
        return;
    }
    out += '?';
}

std::string toDisplayAscii(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c < 0x80) {
            out += static_cast<char>(c);
            ++i;
            continue;
        }
        size_t len = 0;
        uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            len = 4;
        }
        if (len == 0) {  // byte Latin-1 isolato
            appendAscii(out, c);
            ++i;
            continue;
        }
        if (i + len > in.size()) {
            break;
        }
        bool valid = true;
        for (size_t k = 1; k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(in[i + k]);
            if ((cc & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!valid) {
            appendAscii(out, c);
            ++i;
            continue;
        }
        appendAscii(out, cp);
        i += len;
    }
    return out;
}

std::string fitText(const std::string& text, int maxWidth, uint8_t font) {
    if (display::textWidth(text, font) <= maxWidth) {
        return text;
    }
    std::string out = text;
    while (!out.empty() &&
           display::textWidth(out + "~", font) > maxWidth) {
        out.pop_back();
    }
    return out + "~";
}

std::string fileName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool parsePlaylist(const std::string& text, std::vector<Station>& out) {
    out.clear();
    size_t start = 0;
    // BOM UTF-8: alcuni editor lo aggiungono al primo nome.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        start = 3;
    }
    while (start <= text.size() && static_cast<int>(out.size()) < kMaxStations) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string line = trim(text.substr(start, end - start));
        start = end + 1;

        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        size_t sep = line.find('|');
        if (sep == std::string::npos) {
            // Formato "Nome,URL": la virgola separatrice e' quella seguita da
            // http(s), cosi' le virgole dentro l'URL non vengono spezzate.
            size_t pos = 0;
            while ((pos = line.find(',', pos)) != std::string::npos) {
                size_t urlPos = pos + 1;
                while (urlPos < line.size() &&
                       (line[urlPos] == ' ' || line[urlPos] == '\t')) {
                    ++urlPos;
                }
                if (line.compare(urlPos, 7, "http://") == 0 ||
                    line.compare(urlPos, 8, "https://") == 0) {
                    sep = pos;
                    break;
                }
                ++pos;
            }
            if (sep == std::string::npos) {
                sep = line.find(',');
            }
        }
        if (sep == std::string::npos || sep == 0) {
            continue;
        }
        std::string name = toDisplayAscii(trim(line.substr(0, sep)));
        std::string url = trim(line.substr(sep + 1));
        if (name.empty() || url.empty() || url.size() > 220) {
            continue;
        }
        if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
            continue;
        }
        if (name.size() > 48) {
            name.resize(48);
        }
        out.push_back({name, url});
    }
    return !out.empty();
}

// Carica la playlist SD; in caso di problemi ripiega sulle stazioni
// predefinite. Ritorna il numero di stazioni disponibili.
int loadStations(std::vector<Station>& out) {
    out.clear();
    std::string text;
    if (!storage::hasSD()) {
        g_sourceTag = "DEF";
        g_sourceNote = "SD assente";
    } else if (!storage::exists(kPlaylistPath, true)) {
        g_sourceTag = "DEF";
        g_sourceNote = "file assente";
    } else {
        const size_t size = storage::fileSize(kPlaylistPath, true);
        if (size == 0) {
            g_sourceTag = "DEF";
            g_sourceNote = "file vuoto";
        } else if (size > kPlaylistMaxBytes) {
            // readText tronca e ritorna false: meglio dirlo che ignorare in
            // silenzio il resto della playlist.
            g_sourceTag = "DEF";
            g_sourceNote = "file oltre 8 KB";
        } else if (!storage::readText(kPlaylistPath, text, kPlaylistMaxBytes, true)) {
            g_sourceTag = "DEF";
            g_sourceNote = "errore di lettura";
        } else if (parsePlaylist(text, out)) {
            g_sourceTag = "SD";
            g_sourceNote = "SD";
            return static_cast<int>(out.size());
        } else {
            g_sourceTag = "DEF";
            g_sourceNote = "nessuna stazione valida";
        }
    }

    for (const auto& station : kDefaultStations) {
        out.push_back({station.name, station.url});
    }
    return static_cast<int>(out.size());
}

uint64_t sdFreeBytes() {
    if (!storage::hasSD()) {
        return 0;
    }
    const uint64_t total = SD.totalBytes();
    const uint64_t used = SD.usedBytes();
    return total > used ? total - used : 0;
}

std::string freeText(uint64_t bytes) {
    if (bytes >= (1ULL << 30)) {
        return std::to_string(static_cast<unsigned>(bytes >> 30)) + " GB";
    }
    return std::to_string(static_cast<unsigned>(bytes >> 20)) + " MB";
}

std::string sizeText(uint32_t bytes) {
    if (bytes >= (1024U * 1024U)) {
        return std::to_string(bytes / (1024U * 1024U)) + " MB";
    }
    return std::to_string(bytes / 1024U) + " KB";
}

std::string clockText(uint32_t bytes, uint32_t rate) {
    const uint32_t seconds = rate > 0 ? bytes / (rate * 4U) : 0;
    char buffer[12];
    if (seconds >= 3600) {
        std::snprintf(buffer, sizeof(buffer), "%u:%02u:%02u",
                      static_cast<unsigned>(seconds / 3600),
                      static_cast<unsigned>((seconds / 60) % 60),
                      static_cast<unsigned>(seconds % 60));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%02u:%02u",
                      static_cast<unsigned>(seconds / 60),
                      static_cast<unsigned>(seconds % 60));
    }
    return buffer;
}

// Nome radio_YYYYmmdd_HHMMSS.wav; se l'orologio non e' ancora sincronizzato
// usa il tempo dall'avvio, e in caso di collisione aggiunge _2, _3, ...
std::string makeRecordingPath() {
    char stamp[32];
    const time_t now = time(nullptr);
    struct tm parts;
    localtime_r(&now, &parts);
    if (parts.tm_year + 1900 >= 2024) {
        strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &parts);
    } else {
        const uint32_t uptime = millis() / 1000U;
        std::snprintf(stamp, sizeof(stamp), "19700101_%02u%02u%02u",
                      static_cast<unsigned>((uptime / 3600U) % 24U),
                      static_cast<unsigned>((uptime / 60U) % 60U),
                      static_cast<unsigned>(uptime % 60U));
    }
    const std::string base = std::string("/audio/rec/radio_") + stamp;
    std::string path = base + ".wav";
    for (int n = 2; n < 100 && storage::exists(path, true); ++n) {
        path = base + "_" + std::to_string(n) + ".wav";
    }
    return path;
}

void drawMarquee(int y, const std::string& text, uint16_t color) {
    const int margin = 6;
    const int maxWidth = display::width() - margin * 2;
    if (display::textWidth(text, 2) <= maxWidth) {
        display::textCentered(y, text, color, 2);
        return;
    }
    const int advance = std::max(1, display::textWidth("M", 2));
    const int columns = maxWidth / advance;
    const std::string padded = text + "    ";
    const size_t offset = (millis() / 220U) % padded.size();
    std::string window;
    window.reserve(static_cast<size_t>(columns));
    for (int i = 0; i < columns; ++i) {
        window.push_back(padded[(offset + static_cast<size_t>(i)) % padded.size()]);
    }
    display::text(margin, y, window, color, 2);
}

void drawPlayer(const Station& station, bool recActive, uint32_t recBytes,
                uint32_t recRate, const std::string& notice, bool noticeActive) {
    Audio& audio = audioout::decoder();
    const bool running = audio.isRunning();

    display::clear();
    display::header("Web Radio", "Radio");

    display::textCentered(24, fitText(station.name, display::width() - 12, 2),
                          running ? display::kAccent : display::kWarning, 2);

    // Indicatore REC lampeggiante a sinistra, stato al centro.
    if (recActive && (millis() / 500U) % 2U == 0U) {
        display::text(6, 36, "REC", display::kDanger, 0);
    }
    display::textCentered(36, running ? "in onda" : "connessione...",
                          running ? display::kText : display::kTextDim, 0);

    // Metadati ICY: artista/titolo, con scorrimento se troppo lunghi.
    const char* title = audioout::streamTitle();
    const char* icyStation = audioout::streamStation();
    std::string metadata;
    if (title != nullptr && title[0] != '\0') {
        metadata = title;
    } else if (icyStation != nullptr && icyStation[0] != '\0') {
        metadata = icyStation;
    } else {
        metadata = "nessun metadato";
    }
    drawMarquee(48, toDisplayAscii(metadata),
                metadata == "nessun metadato" ? display::kTextDim : display::kText);

    const int bitrate = static_cast<int>(audio.getBitRate());
    const int rate = static_cast<int>(audio.getSampleRate());
    if (bitrate > 0) {
        // Il rate non e' attendibile prima del primo frame (default 16000).
        std::string tech = std::to_string(bitrate) + " kbit/s";
        if (rate >= 8000) {
            tech += "  " + std::to_string(rate) + " Hz";
        }
        display::textCentered(62, tech, display::kTextDim, 0);
    }

    // Barra del volume a segmenti (0..21).
    constexpr int kSegments = 21;
    constexpr int kBarX = 14;
    constexpr int kBarY = 74;
    constexpr int kSegW = 9;
    constexpr int kSegH = 9;
    const int volume = audioout::volume();
    for (int i = 0; i < kSegments; ++i) {
        const int x = kBarX + i * kSegW;
        const bool lit = i < volume;
        const uint16_t color = lit ? (i > 16 ? display::kDanger : display::kNeoGreen)
                                   : display::kPanel;
        display::g().fillRect(x, kBarY, kSegW - 2, kSegH, color);
    }
    display::textCentered(87, "volume " + std::to_string(volume) + "/21",
                          display::kText, 0);

    // Riga inferiore: avviso, stato REC con spazio SD, oppure URL.
    if (noticeActive) {
        const bool error = !notice.empty() && notice[0] == '!';
        const std::string text = error ? notice.substr(1) : notice;
        display::text(6, 103, fitText(text, display::width() - 12, 0),
                      error ? display::kDanger : display::kAccent, 0);
    } else if (recActive) {
        std::string line = "REC " + clockText(recBytes, recRate);
        if (storage::hasSD()) {
            line += "   SD libera " + freeText(sdFreeBytes());
        }
        display::text(6, 103, fitText(line, display::width() - 12, 0),
                      display::kDanger, 0);
    } else {
        display::text(6, 103, fitText(station.url, display::width() - 12, 0),
                      display::kTextDim, 0);
    }

    display::footer("R=rec  ;.=volume  INVIO=stop");
    display::push();
}

void playStation(const std::vector<Station>& stations, int stationIndex) {
    if (stationIndex < 0 || stationIndex >= static_cast<int>(stations.size())) {
        return;
    }
    const Station& station = stations[stationIndex];
    if (!wifisetup::ensureConnected(true, "Radio")) {
        ui::message("Web Radio", {"!Wi-Fi non connesso.", "Configura la rete da qui o da",
                                  "Impostazioni > Rete WiFi."},
                    "Radio");
        return;
    }

    // Orologio per i nomi dei WAV: SNTP in background, la radio e' comunque
    // online. Se non risponde si usa il tempo dall'avvio.
    static bool sntpStarted = false;
    if (!sntpStarted) {
        configTime(0, 0, "pool.ntp.org", "time.google.com");
        sntpStarted = true;
    }

    if (!audioout::beginPlayback()) {
        ui::message("Web Radio",
                    {"!Bus I2S non disponibile.", "Riprova; se persiste, riavvia."},
                    "Radio");
        return;
    }
    audioout::setVolume(audioout::volume());
    if (!audioout::decoder().connecttohost(station.url.c_str())) {
        ui::message("Web Radio",
                    {"!Connessione al flusso fallita.", station.url,
                     "La stazione potrebbe essere offline."},
                    "Radio");
        audioout::endPlayback();
        return;
    }

    // Ascolto: il timer di spegnimento non deve attenuare/spegnere.
    powertimer::Hold hold;
    logger::info("Radio: %s", station.name.c_str());

    const uint32_t startedMs = millis();
    uint32_t lastDraw = 0;
    bool recActive = false;
    bool streamLost = false;
    bool savedOnExit = false;
    uint32_t savedBytes = 0;
    std::string recPath;
    std::string notice;
    uint32_t noticeUntil = 0;

    auto showNotice = [&](std::string text, uint32_t durationMs) {
        notice = std::move(text);
        noticeUntil = millis() + durationMs;
        lastDraw = 0;
    };

    auto toggleRecording = [&]() {
        if (!recActive) {
            if (!storage::hasSD()) {
                showNotice("!SD assente: registrazione non disponibile", 3500);
                return;
            }
            if (audioout::decoder().getBitRate() == 0) {
                showNotice("!Attendi l'avvio del flusso", 2500);
                return;
            }
            if (sdFreeBytes() < (4ULL << 20)) {
                showNotice("!Spazio SD insufficiente (min 4 MB)", 3500);
                return;
            }
            recPath = makeRecordingPath();
            if (!audioout::startRecording(recPath.c_str())) {
                showNotice("!Registrazione non avviata", 3500);
                return;
            }
            recActive = true;
            showNotice("REC avviata", 1500);
        } else {
            savedBytes = audioout::stopRecording();
            recActive = false;
            if (savedBytes > 0) {
                showNotice("WAV salvato: " + fileName(recPath), 3500);
            } else {
                showNotice("!Registrazione vuota", 3000);
            }
        }
    };

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
        } else if (ev.type == navigation::Event::Text &&
                   (ev.ch == 'r' || ev.ch == 'R')) {
            toggleRecording();
        }

        audioout::pump();  // pompa lo stream e scrive il PCM registrato su SD

        // Se la SD fallisce la registrazione si chiude da sola: avvisa senza
        // fermare la radio.
        if (recActive && !audioout::recording()) {
            recActive = false;
            savedBytes = audioout::recordingBytes();
            if (audioout::recordingError()) {
                showNotice("!Errore SD: registrazione interrotta", 4000);
            } else if (savedBytes > 0) {
                showNotice("WAV salvato: " + fileName(recPath), 3500);
            }
        }

        // Il decoder spegne isRunning() se il flusso cade (404, rete assente):
        // senza questo controllo la schermata direbbe "in onda" per sempre.
        if (!audioout::decoder().isRunning() && millis() - startedMs > 8000) {
            streamLost = true;
            break;
        }

        if (millis() - lastDraw > 200) {
            lastDraw = millis();
            const bool noticeActive =
                !notice.empty() && static_cast<int32_t>(millis() - noticeUntil) < 0;
            drawPlayer(station, recActive, audioout::recordingBytes(),
                       audioout::recordingSampleRate(), notice, noticeActive);
        }
        delay(2);
    }

    const bool wasRecording = recActive;
    // endPlayback ferma prima lo stream e poi finalizza il WAV: nessun
    // campione arriva a file gia' chiuso.
    audioout::endPlayback();
    if (wasRecording) {
        savedBytes = audioout::recordingBytes();
        savedOnExit = savedBytes > 0;
    }
    recActive = false;
    logger::info("Radio: fermata dall'utente");

    if (savedOnExit) {
        ui::message("Web Radio", {"Registrazione salvata:", recPath,
                                  sizeText(savedBytes)},
                    "Radio");
    } else if (streamLost) {
        ui::message("Web Radio", {"!Flusso interrotto.", station.url}, "Radio");
    }
}

// Elenco stazioni + comandi. Scritto a mano invece di usare MenuManager per
// poter aggiornare la playlist nella stessa schermata (setItems durante
// l'azione del menu distruggerebbe la callback in esecuzione).
void browseStations(std::vector<Station>& stations) {
    int selected = 0;
    int top = 0;
    navigation::waitRelease();

    for (;;) {
        const int stationCount = static_cast<int>(stations.size());
        const int total = stationCount + 3;  // + ricarica, stato, wifi
        const int reloadRow = stationCount;
        const int statusRow = stationCount + 1;
        const int wifiRow = stationCount + 2;

        auto reloadPlaylist = [&]() {
            loadStations(stations);
            selected = 0;
            top = 0;
            if (g_sourceTag != "SD") {
                ui::message("Web Radio",
                            {"!Playlist non caricata.", g_sourceNote,
                             "Uso le stazioni predefinite."},
                            "Radio");
            }
        };

        selected = std::max(0, std::min(selected, total - 1));
        if (selected < top) {
            top = selected;
        }
        if (selected >= top + kVisibleRows) {
            top = selected - kVisibleRows + 1;
        }
        top = std::max(0, std::min(top, std::max(0, total - kVisibleRows)));

        display::clear();
        display::header("Radio", "Menu principale");
        for (int row = 0; row < kVisibleRows; ++row) {
            const int index = top + row;
            if (index >= total) {
                break;
            }
            const int y = kFirstRowY + row * kRowHeight;
            const bool isSelected = index == selected;
            if (isSelected) {
                display::g().fillRect(4, y - 3, display::width() - 8, kRowHeight - 1,
                                      display::kPanel);
                display::g().fillRect(4, y - 3, 2, kRowHeight - 1, display::kAccent);
            }
            const uint16_t color = isSelected ? display::kText : display::kTextDim;
            if (index < stationCount) {
                char number[8];
                std::snprintf(number, sizeof(number), "%02d", index + 1);
                display::text(10, y + 4, number,
                              isSelected ? display::kAccent : display::kTextDim, 0);
                display::text(30, y + 3,
                              fitText(stations[index].name, display::width() - 40, 2),
                              color, 2);
            } else if (index == reloadRow) {
                display::text(10, y + 3, "Ricarica playlist", color, 2);
            } else if (index == statusRow) {
                display::text(10, y + 3, "Stato / ferma", color, 2);
            } else {
                display::text(10, y + 3, "WiFi", color, 2);
            }
        }
        display::footer("INVIO=ascolta  R=ricarica  ESC=esci",
                        g_sourceTag + " " + std::to_string(stationCount));
        display::push();

        navigation::update();
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            return;
        }
        if (ev.type == navigation::Event::Up && selected > 0) {
            --selected;
            continue;
        }
        if (ev.type == navigation::Event::Down && selected < total - 1) {
            ++selected;
            continue;
        }
        if (ev.type == navigation::Event::Text && (ev.ch == 'r' || ev.ch == 'R')) {
            reloadPlaylist();
            continue;
        }
        if (ev.type != navigation::Event::Select) {
            delay(5);
            continue;
        }

        if (selected < stationCount) {
            playStation(stations, selected);
        } else if (selected == reloadRow) {
            reloadPlaylist();
        } else if (selected == statusRow) {
            std::vector<std::string> lines;
            lines.push_back(audioout::decoderRunning() ? ">Decoder attivo"
                                                       : ">Decoder fermo");
            lines.push_back("Volume: " + std::to_string(audioout::volume()) + "/21");
            lines.push_back("Playlist: " + std::to_string(stationCount) + " stazioni (" +
                            g_sourceNote + ")");
            if (storage::hasSD()) {
                lines.push_back("SD libera: " + freeText(sdFreeBytes()));
            } else {
                lines.push_back("!microSD assente");
            }
            lines.push_back("Stream HTTP MP3/AAC, niente TLS.");
            ui::message("Web Radio", lines, "Radio");
        } else if (selected == wifiRow) {
            wifisetup::runSettings("Radio");
        }
        delay(5);
    }
}

}  // namespace

void runMenu() {
    std::vector<Station> stations;
    loadStations(stations);
    browseStations(stations);
}

}  // namespace radio

#endif  // ENABLE_MODULE_RADIO
