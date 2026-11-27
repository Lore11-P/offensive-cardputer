#include "orbit_audio/Audio.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include <dirent.h>
#include <esp_log.h>
#include <sys/stat.h>

#include "Display.h"
#include "MenuManager.h"
#include "Navigation.h"
#include "Ui.h"
#include "orbit_compat.h"
#include "orbit_store/Store.h"

// Music hub for OrbitOS 3.0. Library roots, album/track model and cover format
// follow the 2.x MusicMenu: /data/Music/<Album>/<track>, optional 64x64 RGB565
// cover.cvr. Only MP3 and WAV can be played; other extensions stay visible and
// explain why they cannot be opened.
namespace orbit_audio {
namespace {

constexpr const char* kTag = "orbit_audio";
constexpr int kMaxAlbums = 40;
constexpr int kMaxTracks = 150;
constexpr size_t kMaxDirEntries = 256;
constexpr int kCoverSide = 64;
constexpr size_t kCoverBytes = static_cast<size_t>(kCoverSide) * kCoverSide * 2;
constexpr uint32_t kPlayerTaskStack = 32 * 1024;  // minimp3 scratch is ~16 KB
constexpr UBaseType_t kPlayerTaskPriority = 5;

struct Entry {
    std::string name;
    bool dir = false;
};

struct Track {
    std::string name;
    std::string path;
};

struct Album {
    std::string name;
    std::string path;
    int count = 0;
};

volatile bool s_stopRequested = false;
volatile bool s_taskDone = false;
volatile bool s_taskOk = false;

struct PlayerTaskArgs {
    std::string path;
    volatile bool* stop = nullptr;
};

void playerTask(void* arg) {
    PlayerTaskArgs* args = static_cast<PlayerTaskArgs*>(arg);
    s_taskOk = playFile(args->path, args->stop);
    delete args;
    s_taskDone = true;
    vTaskDelete(nullptr);
}

std::string joinPath(const std::string& base, const std::string& name) {
    if (base.empty() || base == "/") {
        return "/" + name;
    }
    if (base.back() == '/') {
        return base + name;
    }
    return base + "/" + name;
}

std::string lowerExtension(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) {
        return "";
    }
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return ext;
}

bool isAudioName(const std::string& name) {
    const std::string ext = lowerExtension(name);
    return ext == "mp3" || ext == "wav" || ext == "flac" || ext == "m4a" ||
           ext == "aac" || ext == "ogg" || ext == "opus";
}

bool isPlayableName(const std::string& name) {
    const std::string ext = lowerExtension(name);
    return ext == "mp3" || ext == "wav";
}

std::string asciiLabel(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            out.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?');
            ++i;
            continue;
        }
        size_t length = 0;
        uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            length = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            length = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            length = 4;
        }
        if (length == 0 || i + length > text.size()) {
            out.push_back('?');
            ++i;
            continue;
        }
        bool valid = true;
        for (size_t k = 1; k < length; ++k) {
            const unsigned char cc = static_cast<unsigned char>(text[i + k]);
            if ((cc & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        i += length;
        if (!valid) {
            out.push_back('?');
        } else if (cp == 0x00C0 || cp == 0x00C1 || cp == 0x00C2 || cp == 0x00C3 ||
                   cp == 0x00C4 || cp == 0x00C5) {
            out.push_back('A');
        } else if (cp == 0x00E0 || cp == 0x00E1 || cp == 0x00E2 || cp == 0x00E3 ||
                   cp == 0x00E4 || cp == 0x00E5) {
            out.push_back('a');
        } else if (cp == 0x00C8 || cp == 0x00C9 || cp == 0x00CA || cp == 0x00CB) {
            out.push_back('E');
        } else if (cp == 0x00E8 || cp == 0x00E9 || cp == 0x00EA || cp == 0x00EB) {
            out.push_back('e');
        } else if (cp == 0x00CC || cp == 0x00CD || cp == 0x00CE || cp == 0x00CF) {
            out.push_back('I');
        } else if (cp == 0x00EC || cp == 0x00ED || cp == 0x00EE || cp == 0x00EF) {
            out.push_back('i');
        } else if (cp == 0x00D2 || cp == 0x00D3 || cp == 0x00D4 || cp == 0x00D5 ||
                   cp == 0x00D6 || cp == 0x00D8) {
            out.push_back('O');
        } else if (cp == 0x00F2 || cp == 0x00F3 || cp == 0x00F4 || cp == 0x00F5 ||
                   cp == 0x00F6 || cp == 0x00F8) {
            out.push_back('o');
        } else if (cp == 0x00D9 || cp == 0x00DA || cp == 0x00DB || cp == 0x00DC) {
            out.push_back('U');
        } else if (cp == 0x00F9 || cp == 0x00FA || cp == 0x00FB || cp == 0x00FC) {
            out.push_back('u');
        } else if (cp == 0x00C7) {
            out.push_back('C');
        } else if (cp == 0x00E7) {
            out.push_back('c');
        } else if (cp == 0x00D1) {
            out.push_back('N');
        } else if (cp == 0x00F1) {
            out.push_back('n');
        } else if (cp == 0x00DF) {
            out += "ss";
        } else if (cp == 0x2013 || cp == 0x2014) {
            out.push_back('-');
        } else if (cp == 0x2018 || cp == 0x2019) {
            out.push_back('\'');
        } else if (cp == 0x201C || cp == 0x201D) {
            out.push_back('"');
        } else {
            out.push_back('?');
        }
    }
    return out;
}

std::string displayName(const std::string& file) {
    const size_t dot = file.find_last_of('.');
    if (dot == std::string::npos || dot == 0) {
        return file;
    }
    return file.substr(0, dot);
}

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

std::string formatTime(uint32_t seconds) {
    char buf[12];
    std::snprintf(buf, sizeof(buf), "%lu:%02lu", static_cast<unsigned long>(seconds / 60),
                  static_cast<unsigned long>(seconds % 60));
    return buf;
}

std::vector<Entry> listDir(const std::string& path) {
    std::vector<Entry> out;
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
        return out;
    }
    for (dirent* item = readdir(dir); item != nullptr; item = readdir(dir)) {
        const std::string name = item->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        if (out.size() >= kMaxDirEntries) {
            break;
        }
        Entry entry;
        entry.name = name;
        entry.dir = item->d_type == DT_DIR;
        if (item->d_type == DT_UNKNOWN) {
            struct stat st = {};
            if (stat(joinPath(path, name).c_str(), &st) == 0) {
                entry.dir = S_ISDIR(st.st_mode);
            }
        }
        out.push_back(entry);
    }
    closedir(dir);
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        if (a.dir != b.dir) {
            return a.dir;
        }
        return a.name < b.name;
    });
    return out;
}

int countTracks(const std::string& folder) {
    int count = 0;
    for (const Entry& entry : listDir(folder)) {
        if (!entry.dir && isAudioName(entry.name)) {
            if (++count >= kMaxTracks) {
                break;
            }
        }
    }
    return count;
}

void scanRoot(const std::string& root, std::vector<Album>& albums,
              std::vector<Track>& loose) {
    for (const Entry& entry : listDir(root)) {
        if (entry.dir) {
            if (static_cast<int>(albums.size()) >= kMaxAlbums) {
                continue;
            }
            Album album;
            album.name = entry.name;
            album.path = joinPath(root, entry.name);
            album.count = countTracks(album.path);
            albums.push_back(album);
        } else if (isAudioName(entry.name) &&
                   static_cast<int>(loose.size()) < kMaxTracks) {
            Track track;
            track.name = entry.name;
            track.path = joinPath(root, entry.name);
            loose.push_back(track);
        }
    }
    ESP_LOGI(kTag, "libreria %s: %u album, %u brani sciolti", root.c_str(),
             static_cast<unsigned>(albums.size()), static_cast<unsigned>(loose.size()));
}

bool tryRoot(const char* root, std::vector<Album>& albums, std::vector<Track>& loose,
             std::string* chosen) {
    albums.clear();
    loose.clear();
    scanRoot(root, albums, loose);
    if (albums.empty() && loose.empty()) {
        return false;
    }
    *chosen = root;
    return true;
}

bool findLibrary(std::vector<Album>& albums, std::vector<Track>& loose,
                 std::string* root) {
    const bool haveSd = orbit_store::mountSd();
    if (haveSd) {
        static const char* kSdRoots[] = {"/sd/data/Music", "/sd/music", "/sd/Music"};
        for (const char* candidate : kSdRoots) {
            if (tryRoot(candidate, albums, loose, root)) {
                return true;
            }
        }
    }
    if (orbit_store::mountLittleFs() &&
        tryRoot("/littlefs/data/Music", albums, loose, root)) {
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Cover: 64x64 RGB565 little-endian, kept in RAM as RGB332 (4 KB, or 2 KB at
// 32x32 when heap is tight). A missing or wrong-sized file is not an error.
// ---------------------------------------------------------------------------
uint8_t packRgb332(uint16_t px) {
    const uint8_t r5 = (px >> 11) & 0x1F;
    const uint8_t g6 = (px >> 5) & 0x3F;
    const uint8_t b5 = px & 0x1F;
    return static_cast<uint8_t>(((r5 >> 2) << 5) | ((g6 >> 3) << 2) | (b5 >> 3));
}

class Cover {
public:
    Cover() = default;
    ~Cover() {
        delete[] pixels_;
    }
    Cover(const Cover&) = delete;
    Cover& operator=(const Cover&) = delete;

    void load(const std::string& albumPath) {
        reset();
        FILE* file = std::fopen(joinPath(albumPath, "cover.cvr").c_str(), "rb");
        if (file == nullptr) {
            return;
        }
        if (std::fseek(file, 0, SEEK_END) != 0 || std::ftell(file) != static_cast<long>(kCoverBytes) ||
            std::fseek(file, 0, SEEK_SET) != 0) {
            std::fclose(file);
            return;
        }

        int side = kCoverSide;
        uint8_t* pixels = new (std::nothrow) uint8_t[kCoverSide * kCoverSide];
        if (pixels == nullptr) {
            side = kCoverSide / 2;
            pixels = new (std::nothrow) uint8_t[side * side];
        }
        if (pixels == nullptr) {
            std::fclose(file);
            return;
        }

        uint8_t row[kCoverSide * 2];
        bool ok = true;
        for (int y = 0; y < kCoverSide && ok; ++y) {
            if (std::fread(row, 1, sizeof(row), file) != sizeof(row)) {
                ok = false;
                break;
            }
            if (side == kCoverSide) {
                for (int x = 0; x < kCoverSide; ++x) {
                    pixels[y * kCoverSide + x] = packRgb332(static_cast<uint16_t>(
                        row[x * 2] | (row[x * 2 + 1] << 8)));
                }
            } else if ((y & 1) == 0) {
                for (int x = 0; x < kCoverSide; x += 2) {
                    pixels[(y / 2) * side + x / 2] = packRgb332(static_cast<uint16_t>(
                        row[x * 2] | (row[x * 2 + 1] << 8)));
                }
            }
        }
        std::fclose(file);
        if (!ok) {
            delete[] pixels;
            return;
        }
        pixels_ = pixels;
        side_ = side;
    }

    bool ready() const {
        return pixels_ != nullptr;
    }
    int side() const {
        return side_;
    }
    const uint8_t* pixels() const {
        return pixels_;
    }

private:
    void reset() {
        delete[] pixels_;
        pixels_ = nullptr;
        side_ = kCoverSide;
    }

    uint8_t* pixels_ = nullptr;
    int side_ = kCoverSide;
};

void drawCover(const Cover& cover, int x, int y) {
    auto& d = display::g();
    if (cover.ready()) {
        const uint8_t* pixels = cover.pixels();
        const int side = cover.side();
        for (int row = 0; row < kCoverSide; ++row) {
            const int sy = row * side / kCoverSide;
            for (int col = 0; col < kCoverSide; ++col) {
                const int sx = col * side / kCoverSide;
                const uint8_t v = pixels[sy * side + sx];
                const uint16_t r = (v >> 5) & 0x07;
                const uint16_t g = (v >> 2) & 0x07;
                const uint16_t b = v & 0x03;
                const uint16_t color = static_cast<uint16_t>(
                    (((r << 2) | (r >> 1)) << 11) | (((g << 3) | g) << 5) |
                    ((b << 3) | (b << 1) | (b >> 1)));
                d.drawPixel(x + col, y + row, color);
            }
        }
        d.drawRect(x - 1, y - 1, kCoverSide + 2, kCoverSide + 2,
                   display::C(display::T_line));
        return;
    }
    d.fillRect(x, y, kCoverSide, kCoverSide, display::C(display::T_bg2));
    d.drawRect(x, y, kCoverSide, kCoverSide, display::C(display::T_line));
    d.drawRect(x + 2, y + 2, kCoverSide - 4, kCoverSide - 4,
               display::C(display::T_line2));
    display::drawOrbitIcon("music", x + kCoverSide / 2 - 12, y + 10, 24);
    display::text(x + 8, y + kCoverSide - 14, "NO COVER", display::C(display::T_dim), 0);
}

void drawVolume(int x, int y, int width) {
    constexpr int kSegments = 21;
    const int segW = std::max(3, width / kSegments);
    const int volume = orbit_audio::volume();
    for (int i = 0; i < kSegments; ++i) {
        const uint16_t color =
            i < volume ? display::C(i > 16 ? display::T_err : display::T_ok)
                       : display::C(display::T_bg3);
        display::g().fillRect(x + i * segW, y, segW - 2, 8, color);
    }
}

void drawPlayer(const Track& track, const Album& album, int index, int count,
                const Cover& cover, bool paused) {
    display::clear(display::C(display::T_bg));
    display::header("Musica", fit(asciiLabel(album.name), 100, 0));

    constexpr int kCoverY = 24;
    const int coverX = display::width() - kCoverSide - 8;
    drawCover(cover, coverX, kCoverY);

    const int colX = 8;
    const int colW = coverX - colX - 8;

    display::text(colX, 26, fit(asciiLabel(displayName(track.name)), colW, 2),
                  display::C(display::T_acc), 2);
    const bool running = orbit_audio::active();
    const char* status = paused ? "in pausa" : (running ? "in riproduzione" : "apertura...");
    display::text(colX, 38, status,
                  paused || !running ? display::C(display::T_dim)
                                     : display::C(display::T_text), 0);

    const uint32_t total = orbit_audio::durationMs();
    const uint32_t current = orbit_audio::positionMs();
    const float fraction =
        total > 0 ? std::min(1.0f, static_cast<float>(current) / static_cast<float>(total))
                  : 0.0f;
    display::progressBar(colX, 50, colW, 5, fraction, display::C(display::T_acc));
    display::text(colX, 58, formatTime(current / 1000), display::C(display::T_dim), 0);
    const std::string totalText = total > 0 ? formatTime(total / 1000) : "--:--";
    display::text(colX + colW - display::textWidth(totalText, 0), 58, totalText,
                  display::C(display::T_dim), 0);

    drawVolume(colX, 72, colW);
    char volText[24];
    std::snprintf(volText, sizeof(volText), "volume %d/21", orbit_audio::volume());
    display::text(colX, 83, volText, display::C(display::T_dim), 0);
    char pos[24];
    std::snprintf(pos, sizeof(pos), "%d/%d", index + 1, count);
    display::text(colX + colW - display::textWidth(pos, 0), 83, pos,
                  display::C(display::T_dim), 0);

    display::footer("ENTER=pausa ,/=brano ;.=vol `");
    display::push();
}

void setVolumeRelative(int delta) {
    orbit_audio::setVolume(orbit_audio::volume() + delta);
}

void runPlayer(const std::vector<Track>& tracks, const Album& album, int startIndex) {
    if (tracks.empty()) {
        return;
    }
    if (!begin()) {
        ui::message("Musica", {"!Audio non disponibile.", lastError()}, asciiLabel(album.name));
        return;
    }

    Cover cover;
    cover.load(album.path);

    int index = std::max(0, std::min(startIndex, static_cast<int>(tracks.size()) - 1));
    bool showError = false;
    bool decoderStuck = false;
    std::string errorTrack;
    navigation::waitRelease();

    for (;;) {
        const Track& track = tracks[index];
        ESP_LOGI(kTag, "riproduco %s", track.path.c_str());

        setPaused(false);
        s_stopRequested = false;
        s_taskDone = false;
        s_taskOk = false;
        PlayerTaskArgs* args = new (std::nothrow) PlayerTaskArgs;
        if (args == nullptr) {
            ui::message("Musica", {"!Memoria insufficiente."}, asciiLabel(album.name));
            break;
        }
        args->path = track.path;
        args->stop = &s_stopRequested;
        if (xTaskCreate(playerTask, "orbit_play", kPlayerTaskStack, args,
                        kPlayerTaskPriority, nullptr) != pdPASS) {
            delete args;
            ui::message("Musica", {"!Task audio non creato."}, asciiLabel(album.name));
            break;
        }

        bool paused = false;
        bool started = false;
        int action = 0;  // 0 natural end, 1 next, -1 previous, 2 exit
        uint32_t startedAt = millis();
        uint32_t lastDraw = 0;
        uint32_t nextVolume = 0;

        for (;;) {
            const navigation::EventData ev = navigation::poll();
            if (ev.type == navigation::Event::Back) {
                action = 2;
                break;
            }
            if (ev.type == navigation::Event::Select) {
                paused = !paused;
                setPaused(paused);
                lastDraw = 0;
            } else if (ev.type == navigation::Event::Up) {
                setVolumeRelative(1);
                nextVolume = millis() + 380;
                lastDraw = 0;
            } else if (ev.type == navigation::Event::Down) {
                setVolumeRelative(-1);
                nextVolume = millis() + 380;
                lastDraw = 0;
            } else if (ev.type == navigation::Event::Left) {
                action = -1;
                break;
            } else if (ev.type == navigation::Event::Right) {
                action = 1;
                break;
            }

            if (navigation::pressed(';') || navigation::pressed('.')) {
                if (nextVolume != 0 &&
                    static_cast<int32_t>(millis() - nextVolume) >= 0) {
                    setVolumeRelative(navigation::pressed(';') ? 1 : -1);
                    nextVolume = millis() + 110;
                    lastDraw = 0;
                }
            } else {
                nextVolume = 0;
            }

            if (active()) {
                started = true;
            }
            if (s_taskDone) {
                if (!s_taskOk) {
                    showError = true;
                    errorTrack = asciiLabel(track.name);
                } else {
                    action = 1;
                }
                break;
            }
            if (!started && !paused && millis() - startedAt > 6000) {
                ESP_LOGW(kTag, "timeout avvio %s", track.path.c_str());
                showError = true;
                errorTrack = asciiLabel(track.name);
                action = 2;
                break;
            }

            if (millis() - lastDraw > 250) {
                lastDraw = millis();
                drawPlayer(track, album, index, static_cast<int>(tracks.size()), cover,
                           paused);
            }
            delay(10);
        }

        if (action != 0) {
            s_stopRequested = true;
        }
        const uint32_t stopAt = millis();
        while (!s_taskDone && millis() - stopAt < 2000) {
            delay(10);
        }
        if (!s_taskDone) {
            ESP_LOGE(kTag, "il task audio non si e' fermato");
            decoderStuck = true;
            break;
        }

        if (showError) {
            break;
        }
        if (action == 2) {
            break;
        }

        const int count = static_cast<int>(tracks.size());
        index = action < 0 ? (index - 1 + count) % count : (index + 1) % count;
    }

    setPaused(false);
    if (!decoderStuck) {
        end();
    }
    if (showError && !decoderStuck) {
        ui::message("Musica", {"!Riproduzione non riuscita.", lastError(), errorTrack},
                    asciiLabel(album.name));
    }
    if (decoderStuck) {
        ui::message("Musica", {"!Il decoder non risponde.", "Riavvia il dispositivo."},
                    asciiLabel(album.name));
    }
    navigation::waitRelease();
}

void openAlbum(const Album& album) {
    std::vector<Track> all;
    for (const Entry& entry : listDir(album.path)) {
        if (!entry.dir && isAudioName(entry.name) &&
            static_cast<int>(all.size()) < kMaxTracks) {
            Track track;
            track.name = entry.name;
            track.path = joinPath(album.path, entry.name);
            all.push_back(track);
        }
    }
    if (all.empty()) {
        ui::message("Musica", {"!Album vuoto.", asciiLabel(album.name)}, "Musica");
        return;
    }

    std::vector<Track> playable;
    std::vector<int> playableIndex(all.size(), -1);
    for (size_t i = 0; i < all.size(); ++i) {
        if (isPlayableName(all[i].name)) {
            playableIndex[i] = static_cast<int>(playable.size());
            playable.push_back(all[i]);
        }
    }

    MenuManager menu(asciiLabel(album.name), "Musica");
    for (size_t i = 0; i < all.size(); ++i) {
        const std::string label =
            fit(asciiLabel(displayName(all[i].name)), display::width() - 60, 2);
        const int playIndex = playableIndex[i];
        if (playIndex >= 0) {
            menu.addItem(label, "music",
                         [&playable, &album, playIndex] { runPlayer(playable, album, playIndex); });
        } else {
            const std::string name = asciiLabel(all[i].name);
            const std::string albumName = asciiLabel(album.name);
            menu.addItem(label, "file", [name, albumName] {
                ui::message("Musica",
                            {"!Formato non supportato.", name,
                             "Riproducibili: MP3 e WAV."},
                            albumName);
            });
        }
    }
    menu.setHint("ENTER=riproduci  `=album");
    menu.run(true);
}

void runMusic() {
    std::vector<Album> albums;
    std::vector<Track> loose;
    std::string root;
    if (!findLibrary(albums, loose, &root)) {
        ui::message("Musica",
                    {"!Nessuna libreria trovata.",
                     "Crea /data/Music/<Album> sulla SD.",
                     "Formati: MP3 e WAV."},
                    "Audio");
        return;
    }

    MenuManager menu("Musica", "Audio");
    for (size_t i = 0; i < albums.size(); ++i) {
        const std::string suffix = " (" + std::to_string(albums[i].count) + ")";
        const int nameWidth = display::width() - 60 - display::textWidth(suffix, 2);
        menu.addItem(fit(asciiLabel(albums[i].name), nameWidth, 2) + suffix, "folder",
                     [&albums, i] { openAlbum(albums[i]); }, albums[i].count > 0);
    }

    Album looseAlbum;
    looseAlbum.name = "Senza album";
    looseAlbum.path = root;
    looseAlbum.count = static_cast<int>(loose.size());
    if (!loose.empty()) {
        menu.addItem("Senza album (" + std::to_string(loose.size()) + ")", "music",
                     [&looseAlbum] { openAlbum(looseAlbum); });
    }

    menu.setHint("ENTER=album  `=indietro");
    menu.run(true);
}

}  // namespace

void runMenu() {
    MenuManager menu("Audio", "Home");
    menu.addItem("Musica", "music", [] { runMusic(); });
    menu.setHint("ENTER=apri  `=indietro");
    menu.run(true);
}

}  // namespace orbit_audio
