// ============================================================================
//  Lettore MP3 - riproduzione di file locali dalla microSD
//
//  I brani si leggono da /music (mount SD "/sd" -> SD.open("/music/...")).
//  Se la SD non e' montata si usa /music su LittleFS, utile per qualche file
//  di prova (la partizione interna e' piccola).
//
//  Il decoder e il bus I2S sono condivisi con la Web Radio tramite
//  utils/AudioOut: una sola istanza di Audio, mai due.
// ============================================================================

#include "MusicMenu.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <SD.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "AudioOut.h"
#include "Display.h"
#include "Logger.h"
#include "MenuManager.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "PowerTimer.h"
#include "Storage.h"
#include "Ui.h"

#if ENABLE_MODULE_MUSIC

namespace music {
namespace {

constexpr int kMaxTracks = 150;
constexpr const char* kFolder = "/music";

struct Track {
    std::string path;  // "/music/brano.mp3"
    std::string name;  // "brano.mp3"
    bool onSd = false;
};

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool isAudioFile(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) {
        return false;
    }
    const std::string ext = lower(name.substr(dot + 1));
    return ext == "mp3" || ext == "wav" || ext == "flac" || ext == "m4a" ||
           ext == "aac" || ext == "ogg" || ext == "opus";
}

std::string shortened(const std::string& text, size_t maxLen) {
    if (text.size() <= maxLen || maxLen < 2) {
        return text;
    }
    return text.substr(0, maxLen - 1) + "~";
}

std::string displayName(const std::string& file) {
    const size_t dot = file.find_last_of('.');
    return dot == std::string::npos ? file : file.substr(0, dot);
}

std::string formatTime(uint32_t seconds) {
    char buf[12];
    std::snprintf(buf, sizeof(buf), "%lu:%02lu",
                  static_cast<unsigned long>(seconds / 60),
                  static_cast<unsigned long>(seconds % 60));
    return buf;
}

std::vector<Track> scanTracks() {
    std::vector<Track> tracks;
    const bool sd = storage::hasSD();
    const auto entries = storage::listEntries(kFolder, sd);
    for (const auto& entry : entries) {
        if (entry.isDir || !isAudioFile(entry.name)) {
            continue;
        }
        Track track;
        track.path = std::string(kFolder) + "/" + entry.name;
        track.name = entry.name;
        track.onSd = sd;
        tracks.push_back(track);
        if (static_cast<int>(tracks.size()) >= kMaxTracks) {
            break;
        }
    }
    return tracks;
}

bool startTrack(const Track& track) {
    if (!audioout::beginPlayback()) {
        return false;
    }
    fs::FS& filesystem = track.onSd ? static_cast<fs::FS&>(SD) : static_cast<fs::FS&>(LittleFS);
    return audioout::decoder().connecttoFS(filesystem, track.path.c_str());
}

void drawVolume(int x, int y, int width) {
    constexpr int kSegments = 21;
    const int segW = std::max(3, width / kSegments);
    const int vol = audioout::volume();
    for (int i = 0; i < kSegments; ++i) {
        const uint16_t color = i < vol ? (i > 16 ? display::kDanger : display::kNeoGreen)
                                       : display::kPanel;
        display::g().fillRect(x + i * segW, y, segW - 2, 8, color);
    }
}

void drawPlayer(const Track& track, bool paused, bool waiting) {
    Audio& audio = audioout::decoder();
    display::clear();
    display::header("Musica", "Lettore");

    display::textCentered(24, shortened(displayName(track.name), 30),
                          display::kAccent, 2);
    display::textCentered(42,
                          waiting ? "apertura..." : (paused ? "in pausa" : "in riproduzione"),
                          paused || waiting ? display::kTextDim : display::kText, 0);

    const uint32_t total = audio.getAudioFileDuration();
    const uint32_t current = audio.getAudioCurrentTime();
    const float fraction = total > 0
                               ? std::min(1.0f, static_cast<float>(current) / static_cast<float>(total))
                               : 0.0f;
    display::progressBar(16, 58, display::width() - 32, 6, fraction);
    display::text(16, 68, formatTime(current), display::kTextDim, 0);
    display::text(display::width() - 52, 68, total > 0 ? formatTime(total) : "--:--",
                  display::kTextDim, 0);

    drawVolume(14, 86, display::width() - 28);
    display::textCentered(100, "volume " + std::to_string(audioout::volume()) + "/21",
                          display::kTextDim, 0);
    display::text(6, 112, shortened(track.path, 38), display::kTextDim, 0);
    display::footer("ENTER=pausa  ,/=brano  ;.=vol  ESC=esci");
    display::push();
}

// Riproduce a partire da startIndex. Gestisce pausa, volume e cambio brano
// finche' l'utente non esce con `.
void runPlayer(const std::vector<Track>& tracks, int startIndex) {
    if (tracks.empty()) {
        return;
    }
    powertimer::Hold hold;  // l'ascolto non deve far scattare lo spegnimento

    int index = startIndex;
    navigation::waitRelease();

    for (;;) {
        const Track& track = tracks[index];
        logger::info("Musica: %s", track.path.c_str());
        if (!startTrack(track)) {
            ui::message("Musica", {"!Impossibile aprire il file.", track.path,
                                   "Controlla che sia un MP3 valido."},
                        "Lettore");
            break;
        }

        bool paused = false;
        bool started = false;
        const uint32_t startedAt = millis();
        uint32_t lastDraw = 0;
        int action = 0;  // 0 = nessuna, 1 = successivo, -1 = precedente, 2 = esci

        for (;;) {
            navigation::update();
            const navigation::EventData ev = navigation::poll();
            if (ev.type == navigation::Event::Back) {
                action = 2;
                break;
            }
            if (ev.type == navigation::Event::Select) {
                if (audioout::decoder().pauseResume()) {
                    paused = !paused;
                }
                lastDraw = 0;
            } else if (ev.type == navigation::Event::Up) {
                audioout::setVolume(audioout::volume() + 1);
                lastDraw = 0;
            } else if (ev.type == navigation::Event::Down) {
                audioout::setVolume(audioout::volume() - 1);
                lastDraw = 0;
            } else if (ev.type == navigation::Event::Left) {
                action = -1;
                break;
            } else if (ev.type == navigation::Event::Right) {
                action = 1;
                break;
            }

            audioout::pump();

            if (audioout::decoder().isRunning()) {
                started = true;
            } else if (started && !paused) {
                action = 1;  // brano finito: passa al successivo
                break;
            } else if (!started && !paused && millis() - startedAt > 6000) {
                ui::message("Musica", {"!Formato non riproducibile.", track.path},
                            "Lettore");
                action = 2;
                break;
            }

            if (millis() - lastDraw > 250) {
                lastDraw = millis();
                drawPlayer(track, paused, !started);
            }
            delay(2);
        }

        audioout::decoder().stopSong();
        if (action == 2) {
            break;
        }
        const int count = static_cast<int>(tracks.size());
        index = (action < 0) ? (index - 1 + count) % count : (index + 1) % count;
    }

    audioout::endPlayback();
    logger::info("Musica: fermata");
}

}  // namespace

void runMenu() {
    std::vector<Track> tracks = scanTracks();

    MenuManager menu("Musica", "Menu principale");
    if (tracks.empty()) {
        menu.addItem("Nessun brano locale", "music", [] {
            ui::message("Musica", {"!Nessun file in /music.",
                                   "Copia i brani sulla microSD",
                                   "nella cartella /music."},
                        "Musica");
        });
    } else {
        for (size_t i = 0; i < tracks.size(); ++i) {
            const Track& track = tracks[i];
            menu.addItem(shortened(displayName(track.name), 22), "music",
                         [&tracks, i] { runPlayer(tracks, static_cast<int>(i)); });
        }
    }

    menu.setHint("ENTER=apri  ESC=indietro");
    menu.run(true);
}

}  // namespace music

#endif  // ENABLE_MODULE_MUSIC
