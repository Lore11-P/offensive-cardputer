// ============================================================================
//  Lettore MP3 - riproduzione di file locali dalla microSD
//
//  I brani si leggono da /data/Music/<Album> (mount SD "/sd" ->
//  SD.open("/data/Music/...")); i file audio sciolti nella root compaiono
//  nella voce "Senza album". Se la SD non e' montata si usa /data/Music su
//  LittleFS, utile per qualche file di prova (la partizione interna e' piccola).
//
//  La cover opzionale di un album e' <Album>/cover.cvr: 64x64 pixel RGB565
//  little-endian, 8192 byte esatti (4096 pixel x 2 byte).
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
#include <new>
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

// Cap difensivi: la SRAM e' 320 KB e la scansione avviene all'ingresso.
// shortcut: oltre 40 album o 150 brani per album il resto non e' elencato,
// passare a una scansione pigra/su pagina se serve una libreria piu' grande.
constexpr int kMaxAlbums = 40;
constexpr int kMaxTracks = 150;
constexpr int kCoverSide = 64;
constexpr size_t kCoverBytes = static_cast<size_t>(kCoverSide) * kCoverSide * 2;
constexpr const char* kFolder = "/data/Music";
constexpr const char* kCoverName = "cover.cvr";

struct Track {
    std::string path;  // "/data/Music/Album/brano.mp3"
    std::string name;  // "brano.mp3"
    bool onSd = false;
};

struct Album {
    std::string path;  // "/data/Music/Album"
    std::string name;  // "Album"
    int count = 0;
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

std::string joinPath(const std::string& base, const std::string& name) {
    return base + "/" + name;
}

// Accorcia il testo alla larghezza disponibile (in pixel) con una tilde.
std::string fit(const std::string& text, int maxWidth, uint8_t font = 2) {
    if (maxWidth <= 0) {
        return "";
    }
    if (display::textWidth(text, font) <= maxWidth) {
        return text;
    }
    std::string out = text;
    while (!out.empty() && display::textWidth(out + "~", font) > maxWidth) {
        out.pop_back();
    }
    return out.empty() ? "" : out + "~";
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

std::vector<Track> scanTracks(const std::string& folder, bool onSd) {
    std::vector<Track> tracks;
    for (const auto& entry : storage::listEntries(folder, onSd)) {
        if (entry.isDir || !isAudioFile(entry.name)) {
            continue;
        }
        Track track;
        track.path = joinPath(folder, entry.name);
        track.name = entry.name;
        track.onSd = onSd;
        tracks.push_back(track);
        if (static_cast<int>(tracks.size()) >= kMaxTracks) {
            break;
        }
    }
    return tracks;
}

int countTracks(const std::string& folder, bool onSd) {
    int count = 0;
    for (const auto& entry : storage::listEntries(folder, onSd)) {
        if (entry.isDir || !isAudioFile(entry.name)) {
            continue;
        }
        if (++count >= kMaxTracks) {
            break;
        }
    }
    return count;
}

// Sottocartelle di /data/Music = album. I file audio nella root finiscono in
// `loose` e vengono mostrati come album virtuale "Senza album".
std::vector<Album> scanAlbums(std::vector<Track>& loose) {
    const bool onSd = storage::hasSD();
    std::vector<Album> albums;
    for (const auto& entry : storage::listEntries(kFolder, onSd)) {
        if (!entry.isDir) {
            if (isAudioFile(entry.name) &&
                static_cast<int>(loose.size()) < kMaxTracks) {
                Track track;
                track.path = joinPath(kFolder, entry.name);
                track.name = entry.name;
                track.onSd = onSd;
                loose.push_back(track);
            }
            continue;
        }
        if (static_cast<int>(albums.size()) >= kMaxAlbums) {
            continue;
        }
        Album album;
        album.path = joinPath(kFolder, entry.name);
        album.name = entry.name;
        album.onSd = onSd;
        album.count = countTracks(album.path, onSd);
        albums.push_back(album);
    }
    return albums;
}

std::string albumLabel(const std::string& name, int count) {
    const std::string suffix = " (" + std::to_string(count) + ")";
    const int nameWidth = display::width() - 60 - display::textWidth(suffix, 2);
    return fit(name, nameWidth, 2) + suffix;
}

bool startTrack(const Track& track) {
    if (!audioout::beginPlaybackLegacy()) {
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

// Cover caricata una sola volta per album: 64x64 RGB565 little-endian.
// Su ESP32 (little-endian) i byte del file si leggono direttamente in uint16_t.
class Cover {
public:
    Cover() = default;
    ~Cover() { reset(); }
    Cover(const Cover&) = delete;
    Cover& operator=(const Cover&) = delete;

    void load(const std::string& albumPath, bool onSd) {
        reset();
        pixels_ = new (std::nothrow) uint16_t[kCoverSide * kCoverSide];
        if (pixels_ == nullptr) {
            return;
        }
        File file = storage::openRead(joinPath(albumPath, kCoverName), onSd);
        if (!file || static_cast<size_t>(file.size()) != kCoverBytes) {
            file.close();
            reset();
            return;
        }
        size_t total = 0;
        while (total < kCoverBytes) {
            const int got = file.read(reinterpret_cast<uint8_t*>(pixels_) + total,
                                      kCoverBytes - total);
            if (got <= 0) {
                break;
            }
            total += static_cast<size_t>(got);
        }
        file.close();
        if (total != kCoverBytes) {
            reset();
        }
    }

    bool ready() const { return pixels_ != nullptr; }
    const uint16_t* pixels() const { return pixels_; }

private:
    void reset() {
        delete[] pixels_;
        pixels_ = nullptr;
    }

    uint16_t* pixels_ = nullptr;
};

void drawCover(const Cover& cover, int x, int y) {
    auto& d = display::g();
    if (cover.ready()) {
        // drawPixel passa dalla conversione colore del display: pushImage
        // riswappa i 16 bit e inverte i canali RGB della cover.
        const uint16_t* px = cover.pixels();
        for (int row = 0; row < kCoverSide; ++row) {
            for (int col = 0; col < kCoverSide; ++col) {
                d.drawPixel(x + col, y + row, px[row * kCoverSide + col]);
            }
        }
        d.drawRect(x - 1, y - 1, kCoverSide + 2, kCoverSide + 2,
                   display::C(display::T_line));
        return;
    }
    // Segnaposto procedurale: cornice + nota, nessun crash senza cover.cvr.
    d.fillRect(x, y, kCoverSide, kCoverSide, display::C(display::T_bg2));
    d.drawRect(x, y, kCoverSide, kCoverSide, display::C(display::T_line));
    d.drawRect(x + 2, y + 2, kCoverSide - 4, kCoverSide - 4,
               display::C(display::T_line2));
    display::drawOrbitIcon("music", x + kCoverSide / 2 - 12, y + 10, 24);
    display::text(x + 8, y + kCoverSide - 14, "NO COVER", display::C(display::T_dim), 0);
}

void drawPlayer(const Track& track, const std::string& albumName, int index, int count,
                const Cover& cover, bool paused, bool waiting) {
    Audio& audio = audioout::decoder();
    display::clear();
    display::header("Musica", fit(albumName, 90, 0));

    constexpr int kCoverX = 8;
    // y=24 invece del consigliato 20: la barra header occupa i primi 22 px.
    constexpr int kCoverY = 24;
    drawCover(cover, kCoverX, kCoverY);

    const int colX = kCoverX + kCoverSide + 8;
    const int colW = display::width() - colX - 8;

    display::text(colX, 26, fit(displayName(track.name), colW, 2),
                  display::kAccent, 2);
    display::text(colX, 38,
                  waiting ? "apertura..." : (paused ? "in pausa" : "in riproduzione"),
                  paused || waiting ? display::kTextDim : display::kText, 0);

    const uint32_t total = audio.getAudioFileDuration();
    const uint32_t current = audio.getAudioCurrentTime();
    const float fraction = total > 0
                               ? std::min(1.0f, static_cast<float>(current) /
                                                    static_cast<float>(total))
                               : 0.0f;
    display::progressBar(colX, 50, colW, 5, fraction);
    display::text(colX, 58, formatTime(current), display::kTextDim, 0);
    const std::string totalText = total > 0 ? formatTime(total) : "--:--";
    display::text(display::width() - 8 - display::textWidth(totalText, 0), 58,
                  totalText, display::kTextDim, 0);

    drawVolume(colX, 72, colW);
    display::text(colX, 83, "volume " + std::to_string(audioout::volume()) + "/21",
                  display::kTextDim, 0);
    char pos[16];
    std::snprintf(pos, sizeof(pos), "%d/%d", index + 1, count);
    display::text(display::width() - 8 - display::textWidth(pos, 0), 83, pos,
                  display::kTextDim, 0);

    display::footer("ENTER=pausa ,/=brano ;.=vol ESC");
    display::push();
}

// Riproduce a partire da startIndex. Gestisce pausa, volume (con auto-repeat)
// e cambio brano finche' l'utente non esce con ESC.
void runPlayer(const std::vector<Track>& tracks, const std::string& albumName,
               const std::string& albumPath, bool onSd, int startIndex) {
    if (tracks.empty()) {
        return;
    }
    powertimer::Hold hold;  // l'ascolto non deve far scattare lo spegnimento

    Cover cover;  // una lettura di cover.cvr per album, non per frame
    cover.load(albumPath, onSd);

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
        uint32_t nextVolume = 0;
        int action = 0;  // 0 = nessuna, 1 = successivo, -1 = precedente, 2 = esci

        for (;;) {
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
                nextVolume = millis() + 380;
                lastDraw = 0;
            } else if (ev.type == navigation::Event::Down) {
                audioout::setVolume(audioout::volume() - 1);
                nextVolume = millis() + 380;
                lastDraw = 0;
            } else if (ev.type == navigation::Event::Left) {
                action = -1;
                break;
            } else if (ev.type == navigation::Event::Right) {
                action = 1;
                break;
            }

            // Auto-repeat del volume mentre ;/. restano premuti.
            if (navigation::pressed(';') || navigation::pressed('.')) {
                if (nextVolume != 0 &&
                    static_cast<int32_t>(millis() - nextVolume) >= 0) {
                    const int delta = navigation::pressed(';') ? 1 : -1;
                    audioout::setVolume(audioout::volume() + delta);
                    nextVolume = millis() + 110;
                    lastDraw = 0;
                }
            } else {
                nextVolume = 0;
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
                drawPlayer(track, albumName, index, static_cast<int>(tracks.size()),
                           cover, paused, !started);
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

    audioout::endPlaybackLegacy();
    logger::info("Musica: fermata");
}

void openTracks(const std::string& title, const std::string& albumPath, bool onSd,
                const std::vector<Track>& tracks) {
    MenuManager menu(title, "Musica");
    for (size_t i = 0; i < tracks.size(); ++i) {
        menu.addItem(fit(displayName(tracks[i].name), display::width() - 60, 2),
                     "music",
                     [&tracks, &albumPath, &title, onSd, i] {
                         runPlayer(tracks, title, albumPath, onSd, static_cast<int>(i));
                     });
    }
    menu.setHint("ENTER=riproduci  ESC=album");
    menu.run(true);
}

void openAlbum(const Album& album) {
    const std::vector<Track> tracks = scanTracks(album.path, album.onSd);
    if (tracks.empty()) {
        ui::message("Musica", {"!Album vuoto.", album.path,
                               "Aggiungi brani supportati."},
                    "Musica");
        return;
    }
    openTracks(album.name, album.path, album.onSd, tracks);
}

}  // namespace

void runMenu() {
    std::vector<Track> loose;
    const std::vector<Album> albums = scanAlbums(loose);

    if (albums.empty() && loose.empty()) {
        ui::message("Musica", {"!Nessun file in /data/Music.",
                               "Copia i brani sulla microSD",
                               "in /data/Music/<Album>/."},
                    "Musica");
        return;
    }

    // Un solo MenuManager per gli album: la selezione resta invariata quando
    // si torna qui dall'elenco brani (run() conserva selected_).
    MenuManager menu("Musica", "Libreria");
    for (size_t i = 0; i < albums.size(); ++i) {
        menu.addItem(albumLabel(albums[i].name, albums[i].count), "folder",
                     [&albums, i] { openAlbum(albums[i]); },
                     albums[i].count > 0);
    }
    if (!loose.empty()) {
        menu.addItem(albumLabel("Senza album", static_cast<int>(loose.size())), "music",
                     [&loose] {
                         openTracks("Senza album", kFolder, storage::hasSD(), loose);
                     });
    }
    menu.setHint("ENTER=album  ESC=indietro");
    menu.run(true);
}

}  // namespace music

#endif  // ENABLE_MODULE_MUSIC
