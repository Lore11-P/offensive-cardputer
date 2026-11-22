#include "MenuManager.h"

#include <Arduino.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Display.h"
#include "GridMath.h"
#include "Navigation.h"
#include "Orbit.h"

namespace {
constexpr int kHeaderHeight = 22;
constexpr int kFooterHeight = 12;
constexpr int kRowHeight = 20;
constexpr int kIconSize = 16;
constexpr int kGridMargin = 4;
constexpr int kGridGap = 4;
constexpr int kGridIcon = 24;
constexpr int kCarouselFrames = 6;
// Passo angolare tra due voci sull'orbita (radianti).
constexpr float kOrbitStep = 0.5934f;  // 34 gradi

// Hub attivo: i menu radice dei moduli mostrano il hub invece di
// "Menu principale". Solo un livello, nessuna allocazione (letterali).
const char* s_rootContext = nullptr;

std::string toUpperFirst(const std::string& s) {
    if (s.empty()) {
        return s;
    }
    std::string out = s;
    if (out[0] >= 'a' && out[0] <= 'z') {
        out[0] = static_cast<char>(out[0] - 32);
    }
    return out;
}

std::string toUpper(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 32);
        }
    }
    return out;
}

std::string ellipsize(const std::string& text, int maxWidth, uint8_t font = 0) {
    if (display::textWidth(text, font) <= maxWidth) {
        return text;
    }
    // Larghezza lineare nel numero di caratteri: evita il ciclo O(n^2) con
    // una stringa temporanea per ogni iterazione.
    const int adv = font == 4 ? 12 : (font == 0 ? 6 : 7);
    const int pad = adv - (font == 4 ? 10 : 5);
    int keep = (maxWidth + pad) / adv - 2;
    const int minKeep = text.size() > 2 ? 2 : static_cast<int>(text.size());
    if (keep < minKeep) {
        keep = minKeep;
    }
    if (keep > static_cast<int>(text.size())) {
        keep = static_cast<int>(text.size());
    }
    std::string out;
    out.reserve(static_cast<size_t>(keep) + 2);
    out.assign(text, 0, static_cast<size_t>(keep));
    out += "..";
    return out;
}

// Etichetta del carosello: maiuscola e troncata in un buffer di stack, cosi'
// i frame di animazione non allocano stringhe.
void drawCarouselLabel(int y, const std::string& label, int maxWidth, uint16_t color) {
    constexpr size_t kCap = 48;
    if (label.size() + 3 > kCap) {
        display::textCentered(y, ellipsize(toUpper(label), maxWidth, 2), color, 2);
        return;
    }
    char buf[kCap];
    size_t n = label.size();
    for (size_t i = 0; i < n; ++i) {
        char c = label[i];
        buf[i] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
    }
    buf[n] = '\0';
    if (display::textWidth(buf, 2) > maxWidth) {
        constexpr int kAdv = 7;
        constexpr int kPad = 2;
        int keep = (maxWidth + kPad) / kAdv - 2;
        const int minKeep = n > 2 ? 2 : static_cast<int>(n);
        if (keep < minKeep) {
            keep = minKeep;
        }
        if (keep > static_cast<int>(n)) {
            keep = static_cast<int>(n);
        }
        buf[keep] = '.';
        buf[keep + 1] = '.';
        buf[keep + 2] = '\0';
    }
    display::textCentered(y, buf, color, 2);
}

// Prima voce selezionabile da start in avanti (con wrap): -1 se nessuna.
int firstSelectable(const std::vector<MenuItem>& items, int start) {
    const int count = static_cast<int>(items.size());
    if (count == 0) {
        return -1;
    }
    start = std::max(0, std::min(start, count - 1));
    for (int i = 0; i < count; ++i) {
        const int idx = (start + i) % count;
        if (items[idx].enabled) {
            return idx;
        }
    }
    return -1;
}

// Angoli d'accento: la firma visiva dei pannelli Orbit.
void cornerTicks(int x, int y, int w, int h, uint16_t color, int len = 4) {
    auto& d = display::g();
    d.drawFastHLine(x, y, len, color);
    d.drawFastVLine(x, y, len, color);
    d.drawFastHLine(x + w - len, y, len, color);
    d.drawFastVLine(x + w - 1, y, len, color);
    d.drawFastHLine(x, y + h - 1, len, color);
    d.drawFastVLine(x, y + h - len, len, color);
    d.drawFastHLine(x + w - len, y + h - 1, len, color);
    d.drawFastVLine(x + w - 1, y + h - len, len, color);
}

void drawPanel(int x, int y, int w, int h, bool selected, uint16_t fill) {
    auto& d = display::g();
    d.fillRect(x, y, w, h, fill);
    d.drawRect(x, y, w, h, selected ? display::C(display::T_acc) : display::C(display::T_line));
    if (selected) {
        cornerTicks(x + 1, y + 1, w - 2, h - 2, display::C(display::T_accL), 5);
        d.fillRect(x, y + 3, 2, h - 6, display::C(display::T_acc));
    } else {
        d.drawPixel(x, y, display::C(display::T_line2));
        d.drawPixel(x + w - 1, y + h - 1, display::C(display::T_line2));
    }
}

// Badge circolare per il carosello orbitale.
void drawOrbitBadge(const MenuItem& item, int cx, int cy, int iconSize, bool selected, float depth) {
    auto& d = display::g();
    const int r = iconSize / 2 + (selected ? 5 : 3);
    const uint16_t fill = selected ? display::C(display::T_bg3)
                                   : orbit::blend(display::C(display::T_bg2),
                                                  display::C(display::T_bg), 120);
    d.fillCircle(cx, cy, r, fill);
    if (selected) {
        d.drawCircle(cx, cy, r, display::C(display::T_acc));
        d.drawCircle(cx, cy, r + 2, orbit::blend(display::C(display::T_acc),
                                                 display::C(display::T_bg), 170));
        d.drawCircle(cx, cy, r + 4, orbit::blend(display::C(display::T_acc),
                                                 display::C(display::T_bg), 215));
    } else {
        d.drawCircle(cx, cy, r, orbit::blend(display::C(display::T_line),
                                             display::C(display::T_bg),
                                             static_cast<uint8_t>(depth < 0 ? 90 : 20)));
    }
    if (item.icon == "separator") {
        d.drawFastHLine(cx - iconSize / 2, cy, iconSize, display::C(display::T_line));
        return;
    }
    const uint16_t tint = selected ? 0 : orbit::blend(display::C(display::T_text),
                                                      display::C(display::T_bg),
                                                      static_cast<uint8_t>(depth < 0 ? 110 : 30));
    display::drawOrbitIcon(item.icon, cx - iconSize / 2, cy - iconSize / 2, iconSize, tint);
    if (!item.enabled) {
        d.drawLine(cx - r + 2, cy + r - 2, cx + r - 2, cy - r + 2, display::C(display::T_err));
    }
}
}  // namespace

MenuManager::MenuManager(std::string title, std::string parent)
    : title_(std::move(title)), parent_(std::move(parent)) {
    // Un modulo aperto da un hub mantiene il suo breadcrumb reale. "ORBIT"
    // e' il padre storico del menu Audio, anch'esso aperto dal hub.
    if ((parent_ == "Menu principale" || parent_ == "ORBIT") && s_rootContext &&
        *s_rootContext) {
        parent_ = s_rootContext;
    }
    hint_ = "ESC=indietro ;.,/=nav ENTER=apri";
}

void MenuManager::setRootContext(const char* context) {
    s_rootContext = context;
}

void MenuManager::addItem(std::string label, std::string icon, std::function<void()> action,
                          bool enabled) {
    items_.emplace_back(std::move(label), std::move(icon), std::move(action), enabled);
}

void MenuManager::addItem(MenuItem item) {
    items_.push_back(std::move(item));
}

void MenuManager::addSeparator(const std::string& label) {
    MenuItem item;
    item.label = label;
    item.icon = "separator";
    item.enabled = false;
    items_.push_back(std::move(item));
}

void MenuManager::setItems(std::vector<MenuItem> items) {
    items_ = std::move(items);
    selected_ = 0;
    top_ = 0;
    ensureVisible();
}

void MenuManager::setHint(const std::string& hint) {
    hint_ = hint;
}

void MenuManager::setGrid(uint8_t columns) {
    requestedColumns_ = static_cast<uint8_t>(std::max(2, std::min(4, static_cast<int>(columns))));
    mode_ = Mode::Grid;
    selected_ = 0;
    top_ = 0;
    ensureVisible();
}

void MenuManager::setCarousel() {
    mode_ = Mode::Carousel;
    selected_ = 0;
    top_ = 0;
    carouselDir_ = 0;
    carouselAnim_ = 0;
}

void MenuManager::setTextHandler(std::function<bool(char)> handler) {
    textHandler_ = std::move(handler);
}

int MenuManager::gridColumns() const {
    if (display::width() < 200) {
        return std::min(2, static_cast<int>(requestedColumns_));
    }
    return static_cast<int>(requestedColumns_);
}

int MenuManager::gridRows() const {
    const int available = display::height() - kHeaderHeight - kFooterHeight;
    const int minTileHeight = 40;
    return std::max(1, (available - 2 * kGridMargin + kGridGap) /
                           (minTileHeight + kGridGap));
}

int MenuManager::pageSize() const {
    return gridColumns() * gridRows();
}

int MenuManager::rowsPerPage() const {
    if (mode_ == Mode::Grid) {
        return pageSize();
    }
    const int available = display::height() - kHeaderHeight - kFooterHeight;
    return std::max(1, available / kRowHeight);
}

bool MenuManager::selectable(int index) const {
    return index >= 0 && index < static_cast<int>(items_.size()) && items_[index].enabled;
}

void MenuManager::moveSelection(int delta, bool animate) {
    if (items_.empty()) {
        return;
    }
    int idx = selected_;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        idx = (idx + delta + static_cast<int>(items_.size())) % static_cast<int>(items_.size());
        if (selectable(idx)) {
            if (animate && mode_ == Mode::Carousel) {
                carouselDir_ = delta > 0 ? 1 : -1;
                carouselAnim_ = kCarouselFrames;
            }
            selected_ = idx;
            ensureVisible();
            return;
        }
    }
}

void MenuManager::moveGrid(int dx, int dy) {
    if (items_.empty()) {
        return;
    }
    selected_ = gridmath::move(selected_, static_cast<int>(items_.size()), gridColumns(),
                               gridRows(), dx, dy, [this](int index) { return selectable(index); });
    ensureVisible();
}

void MenuManager::ensureVisible() {
    if (mode_ == Mode::Carousel) {
        return;
    }
    if (mode_ == Mode::Grid) {
        const int size = pageSize();
        if (size > 0) {
            top_ = (selected_ / size) * size;
        }
        return;
    }

    visibleRows_ = rowsPerPage();
    if (selected_ < top_) {
        top_ = selected_;
    } else if (selected_ >= top_ + visibleRows_) {
        top_ = selected_ - visibleRows_ + 1;
    }
    if (top_ < 0) {
        top_ = 0;
    }
}

void MenuManager::run(bool allowBack) {
    visibleRows_ = rowsPerPage();
    // La selezione iniziale puo' cadere su un separatore o una voce spenta.
    const int first = firstSelectable(items_, selected_);
    if (first >= 0) {
        selected_ = first;
    }
    ensureVisible();

    uint32_t nextUp = 0;
    uint32_t nextDown = 0;
    uint32_t nextLeft = 0;
    uint32_t nextRight = 0;
    bool upHeld = false;
    bool downHeld = false;
    bool leftHeld = false;
    bool rightHeld = false;

    navigation::waitRelease();
    draw();

    auto repeat = [&](bool& held, uint32_t& next, bool now, int dx, int dy) {
        if (now && !held) {
            held = true;
            next = millis() + 380;
        } else if (!now) {
            held = false;
        }
        if (held && millis() >= next) {
            if (mode_ == Mode::Grid) {
                moveGrid(dx, dy);
            } else if (mode_ == Mode::Carousel) {
                if (dx != 0) {
                    moveSelection(dx, true);
                } else if (dy != 0) {
                    moveSelection(dy, true);
                }
            } else if (dy < 0) {
                moveSelection(-1);
            } else if (dy > 0) {
                moveSelection(1);
            }
            next = millis() + 110;
            return true;
        }
        return false;
    };

    bool running = true;
    uint32_t lastClock = millis() / 1000;
    uint16_t lastBattery = display::batterySignature();
    while (running) {
        // poll() aggiorna gia' la tastiera: un solo update per iterazione.
        const navigation::EventData ev = navigation::poll();

        bool dirty = false;
        const int selBefore = selected_;
        const int topBefore = top_;

        if (ev.type == navigation::Event::Up) {
            if (mode_ == Mode::Grid) {
                moveGrid(0, -1);
            } else if (mode_ == Mode::Carousel) {
                moveSelection(-1, true);
            } else {
                moveSelection(-1);
            }
        } else if (ev.type == navigation::Event::Down) {
            if (mode_ == Mode::Grid) {
                moveGrid(0, 1);
            } else if (mode_ == Mode::Carousel) {
                moveSelection(1, true);
            } else {
                moveSelection(1);
            }
        } else if (ev.type == navigation::Event::Left && mode_ == Mode::Grid) {
            moveGrid(-1, 0);
        } else if (ev.type == navigation::Event::Right && mode_ == Mode::Grid) {
            moveGrid(1, 0);
        } else if (ev.type == navigation::Event::Left && mode_ == Mode::Carousel) {
            moveSelection(-1, true);
        } else if (ev.type == navigation::Event::Right && mode_ == Mode::Carousel) {
            moveSelection(1, true);
        } else if (ev.type == navigation::Event::Select) {
            if (selectable(selected_)) {
                // Copia: l'azione puo' chiamare setItems()/addItem() e
                // ricostruire items_, distruggendo la callback in esecuzione.
                auto action = items_[selected_].action;
                if (action) {
                    navigation::waitRelease();
                    action();
                    navigation::waitRelease();
                }
            }
            dirty = true;
        } else if (ev.type == navigation::Event::Back) {
            if (allowBack) {
                running = false;
            }
        } else if (ev.type == navigation::Event::Text && textHandler_) {
            // Copia anche l'handler: puo' essere sostituito mentre gira.
            auto handler = textHandler_;
            // Ridisegna solo se il tasto e' stato gestito: prima anche un
            // carattere ignorato forzava un frame completo.
            if (handler(ev.ch)) {
                dirty = true;
            }
        }

        if (repeat(upHeld, nextUp, navigation::pressed(';'), 0, -1)) {
            dirty = true;
        }
        if (repeat(downHeld, nextDown, navigation::pressed('.'), 0, 1)) {
            dirty = true;
        }
        if (mode_ == Mode::Grid || mode_ == Mode::Carousel) {
            if (repeat(leftHeld, nextLeft, navigation::pressed(','), -1, 0)) {
                dirty = true;
            }
            if (repeat(rightHeld, nextRight, navigation::pressed('/'), 1, 0)) {
                dirty = true;
            }
        }

        if (selected_ != selBefore || top_ != topBefore) {
            dirty = true;
        }
        const uint32_t clock = millis() / 1000;
        const bool clockTick = clock != lastClock;
        if (clockTick) {
            lastClock = clock;
        }

        if (dirty) {
            draw();
            lastBattery = display::batterySignature();
        } else {
            // Animazione del carosello: cambia solo il corpo, header e footer
            // restano identici. Il LED del footer si aggiorna da solo.
            if (mode_ == Mode::Carousel && carouselAnim_ > 0) {
                drawBody();
                display::push(0, kHeaderHeight, display::width(),
                              display::height() - kHeaderHeight - kFooterHeight);
            }
            if (clockTick) {
                const uint16_t batt = display::batterySignature();
                if (batt != lastBattery) {
                    lastBattery = batt;
                    display::header(title_, parent_);
                    display::push(0, 0, display::width(), kHeaderHeight);
                }
                display::footerTick();
            }
        }
        // 2 ms: il loop deve restare reattivo quanto la lettura della tastiera.
        delay(2);
    }
}

std::string MenuManager::trailingText() const {
    if (mode_ == Mode::Grid) {
        const int size = pageSize();
        const int pages = size > 0 ? (static_cast<int>(items_.size()) + size - 1) / size : 1;
        if (pages > 1) {
            char pageText[16];
            std::snprintf(pageText, sizeof(pageText), "P%d/%d", selected_ / size + 1, pages);
            return pageText;
        }
    } else if (mode_ == Mode::List && !items_.empty()) {
        char posText[16];
        std::snprintf(posText, sizeof(posText), "%d/%d", selected_ + 1,
                      static_cast<int>(items_.size()));
        return posText;
    }
    return std::string();
}

void MenuManager::drawStatus() {
    display::header(title_, parent_);
    display::footer(hint_, trailingText());
}

void MenuManager::drawBody() {
    const int bodyTop = kHeaderHeight;
    const int bodyH = display::height() - bodyTop - kFooterHeight;
    display::g().fillRect(0, bodyTop, display::width(), bodyH, display::C(display::T_bg));
    if (mode_ == Mode::Grid) {
        drawGrid();
    } else if (mode_ == Mode::Carousel) {
        drawCarousel();
    } else {
        drawList();
    }
}

void MenuManager::draw() {
    visibleRows_ = rowsPerPage();
    ensureVisible();
    drawBody();
    drawStatus();
    display::push();
}

void MenuManager::drawList() {
    const int w = display::width();
    const int bodyTop = kHeaderHeight;

    if (items_.empty()) {
        display::drawOrbitIcon("planet", w / 2 - 16, bodyTop + 18, 32);
        display::textCentered(bodyTop + 56, "NESSUN SEGNALE", display::C(display::T_dim), 2);
        return;
    }

    for (int row = 0; row < visibleRows_; ++row) {
        const int idx = top_ + row;
        if (idx >= static_cast<int>(items_.size())) {
            break;
        }
        const MenuItem& item = items_[idx];
        const int y = bodyTop + row * kRowHeight + 1;

        if (item.icon == "separator") {
            auto& d = display::g();
            d.drawFastHLine(8, y + kRowHeight / 2 - 1, 12, display::C(display::T_acc));
            d.drawFastHLine(24, y + kRowHeight / 2 - 1, w - 32, display::C(display::T_line));
            if (!item.label.empty()) {
                display::text(28, y + 4, toUpper(item.label), display::C(display::T_dim), 0);
            }
            continue;
        }

        const bool selected = (idx == selected_);
        drawPanel(2, y, w - 6, kRowHeight - 2, selected,
                  selected ? display::C(display::T_bg3) : display::C(display::T_bg2));

        display::drawOrbitIcon(item.icon, 8, y + 2, kIconSize,
                               item.enabled ? 0 : display::C(display::T_dim));
        const uint16_t fg = item.enabled ? (selected ? display::C(display::T_hi)
                                                     : display::C(display::T_text))
                                         : display::C(display::T_dim);
        display::text(28, y + 5, ellipsize(toUpperFirst(item.label), w - 60, 2), fg, 2);
        // Chevron di apertura.
        auto& d = display::g();
        const int ax = w - 14;
        d.drawLine(ax, y + 6, ax + 4, y + 9, selected ? display::C(display::T_acc) : display::C(display::T_line));
        d.drawLine(ax + 4, y + 9, ax, y + 12, selected ? display::C(display::T_acc) : display::C(display::T_line));
    }

    const int total = static_cast<int>(items_.size());
    if (total > visibleRows_) {
        const int trackX = w - 2;
        const int trackTop = bodyTop + 2;
        const int trackH = visibleRows_ * kRowHeight - 4;
        display::g().fillRect(trackX, trackTop, 1, trackH, display::C(display::T_line));
        const int thumbH = std::max(8, trackH * visibleRows_ / total);
        const int thumbY =
            trackTop + (trackH - thumbH) * top_ / std::max(1, total - visibleRows_);
        display::g().fillRect(trackX - 1, thumbY, 3, thumbH, display::C(display::T_acc));
    }
}

void MenuManager::drawGrid() {
    const int w = display::width();
    const int bodyTop = kHeaderHeight;
    const int cols = gridColumns();
    const int rows = gridRows();
    const int available = display::height() - bodyTop - kFooterHeight;
    const int tileW = (w - 2 * kGridMargin - (cols - 1) * kGridGap) / cols;
    const int tileH = (available - 2 * kGridMargin - (rows - 1) * kGridGap) / rows;
    const int size = pageSize();

    if (items_.empty()) {
        display::textCentered(bodyTop + 26, "NESSUN SEGNALE", display::C(display::T_dim), 2);
        return;
    }

    for (int i = 0; i < size; ++i) {
        const int idx = top_ + i;
        if (idx >= static_cast<int>(items_.size())) {
            break;
        }
        const MenuItem& item = items_[idx];
        if (item.icon == "separator") {
            continue;
        }

        const int x = kGridMargin + (i % cols) * (tileW + kGridGap);
        const int y = bodyTop + kGridMargin + (i / cols) * (tileH + kGridGap);
        const bool selected = (idx == selected_);

        drawPanel(x, y, tileW, tileH, selected,
                  selected ? display::C(display::T_bg3) : display::C(display::T_bg2));

        const int iconSize = std::max(12, std::min(kGridIcon, tileH - 20));
        display::drawOrbitIcon(item.icon, x + (tileW - iconSize) / 2,
                               y + (tileH - iconSize) / 2 - 4, iconSize,
                               item.enabled ? 0 : display::C(display::T_dim));

        std::string label = ellipsize(toUpper(item.label), tileW - 6, 0);
        display::text(x + (tileW - display::textWidth(label, 0)) / 2, y + tileH - 11, label,
                      item.enabled ? display::C(display::T_text) : display::C(display::T_dim), 0);
    }
}

void MenuManager::drawCarousel() {
    const int w = display::width();
    const int h = display::height();
    const int total = static_cast<int>(items_.size());
    const int bodyTop = kHeaderHeight;

    if (total == 0) {
        display::textCentered(bodyTop + 30, "NESSUN SEGNALE", display::C(display::T_dim), 2);
        return;
    }

    if (w < 200) {
        // In portrait il carosello non entra: lista verticale centrata sulla
        // voce selezionata. ensureVisible() non calcola top_ in carosello.
        carouselAnim_ = 0;
        visibleRows_ = rowsPerPage();
        top_ = selected_ - visibleRows_ / 2;
        if (top_ < 0) {
            top_ = 0;
        }
        const int maxTop = std::max(0, total - visibleRows_);
        if (top_ > maxTop) {
            top_ = maxTop;
        }
        drawList();
        return;
    }

    const uint32_t frame = millis() / 55;
    orbit::sky(0, bodyTop, w, h - bodyTop - kFooterHeight, 0xC0DE);

    const int cx = w / 2;
    const int cy = bodyTop + 40;
    const int rx = 84;
    const int ry = 26;

    // Decrementa prima del calcolo: l'ultimo frame cade a riposo.
    if (carouselAnim_ > 0) {
        --carouselAnim_;
    }
    const float anim = static_cast<float>(carouselAnim_) / kCarouselFrames;
    const float phase = static_cast<float>(carouselDir_) * anim * kOrbitStep;

    struct Slot {
        int idx;
        float a;
        int x, y, z, size;
    };
    Slot slots[9];
    int nslots = 0;
    const int visible = std::min(total, 7);
    for (int off = -(visible / 2); off <= (visible / 2); ++off) {
        int idx = (selected_ + off) % total;
        if (idx < 0) idx += total;
        // Con meno di 7 voci gli offset estremi ripetono la stessa voce:
        // disegnala una volta sola.
        bool duplicate = false;
        for (int i = 0; i < nslots; ++i) {
            if (slots[i].idx == idx) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        const float a = off * kOrbitStep + phase;
        if (fabsf(a) > 2.95f) {
            continue;
        }
        Slot& s = slots[nslots++];
        s.idx = idx;
        s.a = a;
        s.z = static_cast<int>(cosf(a) * 100.0f);
        s.x = cx + static_cast<int>(sinf(a) * rx);
        s.y = cy + static_cast<int>(cosf(a) * ry);
        s.size = 16 + static_cast<int>(((cosf(a) + 1.0f) * 0.5f) * 8.0f);
    }
    // Ordinamento per profondita': prima il fondo, poi la scena in primo piano.
    for (int i = 0; i < nslots; ++i) {
        for (int j = i + 1; j < nslots; ++j) {
            if (slots[j].z < slots[i].z) {
                const Slot t = slots[i];
                slots[i] = slots[j];
                slots[j] = t;
            }
        }
    }

    // Orbita puntinata.
    orbit::orbitPath(cx, cy, rx, ry, display::C(display::T_line2), display::C(display::T_line),
                     static_cast<int>(frame / 3));

    // Metà posteriore (dietro al pianeta).
    for (int i = 0; i < nslots; ++i) {
        if (slots[i].z < 0) {
            drawOrbitBadge(items_[slots[i].idx], slots[i].x, slots[i].y, slots[i].size, false,
                           -1.0f);
        }
    }

    // Pianeta centrale.
    orbit::planet(cx, cy, 22, -0.6f, -0.5f, true);

    // Metà anteriore.
    for (int i = 0; i < nslots; ++i) {
        if (slots[i].z >= 0) {
            const bool center = (slots[i].idx == selected_);
            drawOrbitBadge(items_[slots[i].idx], slots[i].x, slots[i].y, slots[i].size, center,
                           1.0f);
        }
    }

    // Satellite "luna": resta fuori dagli anelli dei badge.
    const float satA = frame * 0.09f;
    orbit::satellite(cx + 27 + static_cast<int>(sinf(satA) * 3.0f),
                     cy - 33 + static_cast<int>(cosf(satA) * 2.0f), 11,
                     display::C(display::T_hi), display::C(display::T_sec));

    // Etichetta della voce selezionata + indice.
    if (selected_ >= 0 && selected_ < total) {
        const MenuItem& item = items_[selected_];
        const int labelY = h - kFooterHeight - 15;
        drawCarouselLabel(labelY, item.label, w - 110,
                          item.enabled ? display::C(display::T_hi) : display::C(display::T_dim));
        display::text(6, labelY, "ORBIT", display::C(display::T_dim), 0);
        char pos[12];
        std::snprintf(pos, sizeof(pos), "%02d/%02d", selected_ + 1, total);
        display::text(w - 6 - display::textWidth(pos, 0), labelY, pos,
                      display::C(display::T_acc), 0);
    }
}

int MenuManager::selectedIndex() const {
    return selected_;
}

void MenuManager::selectIndex(int index) {
    if (items_.empty()) {
        return;
    }
    const int clamped = std::max(0, std::min(index, static_cast<int>(items_.size()) - 1));
    const int first = firstSelectable(items_, clamped);
    selected_ = first >= 0 ? first : clamped;
    ensureVisible();
}

const std::vector<MenuItem>& MenuManager::items() const {
    return items_;
}
