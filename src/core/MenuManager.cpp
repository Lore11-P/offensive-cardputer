#include "MenuManager.h"

#include <Arduino.h>
#include <algorithm>
#include <cstdio>

#include "Display.h"
#include "Navigation.h"

namespace {
constexpr int kHeaderHeight = 22;
constexpr int kFooterHeight = 12;
constexpr int kRowHeight = 20;
constexpr int kIconSize = 16;
constexpr int kGridMargin = 4;
constexpr int kGridGap = 4;
constexpr int kGridIcon = 24;
constexpr int kCarouselFrames = 6;
constexpr int kCarouselSpacing = 84;

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

std::string ellipsize(const std::string& text, int maxWidth) {
    auto& d = display::g();
    d.setFont(&fonts::Font0);
    if (d.textWidth(text.c_str()) <= maxWidth) {
        return text;
    }
    std::string out = text;
    while (out.size() > 2 && d.textWidth((out + "..").c_str()) > maxWidth) {
        out.pop_back();
    }
    return out + "..";
}

uint16_t accentFor(int index) {
    static const uint16_t kColors[] = {
        display::kNeoCyan,
        display::kNeoPink,
        display::kNeoGreen,
        display::kNeoOrange,
    };
    return kColors[static_cast<size_t>(index) % (sizeof(kColors) / sizeof(kColors[0]))];
}

void drawCard(int x, int y, int w, int h, uint16_t fill, bool shadow) {
    auto& d = display::g();
    if (shadow) {
        d.fillRect(x + 3, y + 3, w, h, display::kInk);
    }
    d.fillRect(x, y, w, h, fill);
    d.drawRect(x, y, w, h, display::kInk);
    d.drawRect(x + 1, y + 1, w - 2, h - 2, display::kInk);
}

void drawCarouselCard(const MenuItem& item, int x, int y, int w, int h, bool center, int index) {
    drawCard(x, y, w, h, center ? display::kNeoYellow : display::kPaperLight, center);
    auto& d = display::g();
    d.fillRect(x + 2, y + 2, w - 4, center ? 5 : 4, accentFor(index));
    d.drawFastHLine(x + 2, y + (center ? 7 : 6), w - 4, display::kInk);

    const int iconSize = center ? 32 : 22;
    display::drawIcon(item.icon, x + (w - iconSize) / 2, y + (center ? 11 : 8), iconSize);

    std::string label = toUpper(item.label);
    label = ellipsize(label, w - 6);
    d.setFont(&fonts::Font0);
    const int textW = d.textWidth(label.c_str());
    display::text(x + (w - textW) / 2, y + h - 10, label,
                  item.enabled ? (center ? display::kInk : display::kMuted)
                               : display::kMuted,
                  0);
}
}  // namespace

MenuManager::MenuManager(std::string title, std::string parent)
    : title_(std::move(title)), parent_(std::move(parent)) {
    hint_ = "`=indietro  ;.,/=nav  ENTER=apri";
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
    const int cols = gridColumns();
    const int rows = gridRows();
    const int size = pageSize();
    if (cols <= 0 || rows <= 0 || size <= 0) {
        return;
    }

    const int count = static_cast<int>(items_.size());
    const int pageStart = (selected_ / size) * size;
    int row = (selected_ - pageStart) / cols;
    int col = (selected_ - pageStart) % cols;

    // Orizzontale: si resta nella pagina, con wrap tra le colonne.
    if (dx != 0) {
        col = (col + dx + cols) % cols;
    }

    int target = pageStart + row * cols + col;

    // Verticale: uscendo dal bordo superiore o inferiore si passa alla pagina
    // precedente o successiva (con wrap), altrimenti si cambia riga. Prima
    // questa funzione faceva il wrap dentro la pagina e le pagine successive
    // restavano irraggiungibili (es. 38 marche, 6 per pagina).
    if (dy != 0) {
        const int nextRow = row + dy;
        if (nextRow >= 0 && nextRow < rows) {
            target = pageStart + nextRow * cols + col;
        } else {
            const int direction = (dy > 0) ? 1 : -1;
            int nextPageStart = pageStart + direction * size;
            if (nextPageStart < 0) {
                nextPageStart = ((count - 1) / size) * size;  // ultima pagina
            } else if (nextPageStart >= count) {
                nextPageStart = 0;                            // prima pagina
            }
            const int nextRowInPage = (direction > 0) ? 0 : (rows - 1);
            target = nextPageStart + nextRowInPage * cols + col;
        }
    }

    if (target >= count) {
        target = count - 1;
    }

    // Se la cella non esiste (ultima pagina incompleta) o non e' selezionabile,
    // si cerca la successiva disponibile scorrendo l'elenco.
    for (int attempt = 0; attempt < count; ++attempt) {
        if (target >= 0 && target < count && selectable(target)) {
            selected_ = target;
            ensureVisible();
            return;
        }
        target = (target + 1) % count;
    }
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
        }
    };

    bool running = true;
    while (running) {
        navigation::update();
        const navigation::EventData ev = navigation::poll();

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
            if (selectable(selected_) && items_[selected_].action) {
                navigation::waitRelease();
                items_[selected_].action();
                navigation::waitRelease();
            }
        } else if (ev.type == navigation::Event::Back) {
            if (allowBack) {
                running = false;
            }
        } else if (ev.type == navigation::Event::Text && textHandler_) {
            textHandler_(ev.ch);
        }

        repeat(upHeld, nextUp, navigation::pressed(';'), 0, -1);
        repeat(downHeld, nextDown, navigation::pressed('.'), 0, 1);
        if (mode_ == Mode::Grid || mode_ == Mode::Carousel) {
            repeat(leftHeld, nextLeft, navigation::pressed(','), -1, 0);
            repeat(rightHeld, nextRight, navigation::pressed('/'), 1, 0);
        }

        draw();
        delay(16);
    }
}

void MenuManager::draw() {
    visibleRows_ = rowsPerPage();
    ensureVisible();

    display::clear(display::kPaper);
    display::header(title_, parent_);

    if (mode_ == Mode::Grid) {
        drawGrid();
    } else if (mode_ == Mode::Carousel) {
        drawCarousel();
    } else {
        drawList();
    }

    display::footer(hint_);

    if (mode_ == Mode::Grid) {
        const int size = pageSize();
        const int pages = size > 0
                              ? (static_cast<int>(items_.size()) + size - 1) / size
                              : 1;
        if (pages > 1) {
            char pageText[16];
            std::snprintf(pageText, sizeof(pageText), "%d/%d",
                          selected_ / size + 1, pages);
            display::text(display::width() - 26, display::height() - 10,
                          pageText, display::kNeoYellow, 0);
        }
    }

    display::push();
}

void MenuManager::drawList() {
    const int w = display::width();
    const int bodyTop = kHeaderHeight;

    if (items_.empty()) {
        display::textCentered(bodyTop + 16, "Nessuna voce disponibile", display::kMuted, 2);
        return;
    }

    for (int row = 0; row < visibleRows_; ++row) {
        const int idx = top_ + row;
        if (idx >= static_cast<int>(items_.size())) {
            break;
        }
        const MenuItem& item = items_[idx];
        const int y = bodyTop + row * kRowHeight;

        if (item.icon == "separator") {
            display::g().drawFastHLine(8, y + kRowHeight / 2, w - 16, display::kInk);
            if (!item.label.empty()) {
                display::text(10, y + 5, item.label, display::kMuted, 0);
            }
            continue;
        }

        const bool selected = (idx == selected_);
        drawCard(2, y + 1, w - 6, kRowHeight - 3,
                 selected ? display::kNeoYellow : display::kPaperLight, selected);

        const uint16_t fg = item.enabled ? display::kInk : display::kMuted;
        display::drawIcon(item.icon, 7, y + 2, kIconSize);
        display::text(29, y + 3, toUpperFirst(item.label), fg, 2);
    }

    const int total = static_cast<int>(items_.size());
    if (total > visibleRows_) {
        const int trackX = w - 4;
        const int trackTop = bodyTop + 1;
        const int trackH = visibleRows_ * kRowHeight - 2;
        display::g().fillRect(trackX, trackTop, 3, trackH, display::kInk);
        const int thumbH = std::max(8, trackH * visibleRows_ / total);
        const int thumbY =
            trackTop + (trackH - thumbH) * top_ / std::max(1, total - visibleRows_);
        display::g().fillRect(trackX, thumbY, 3, thumbH, display::kNeoYellow);
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
        display::textCentered(bodyTop + 16, "Nessuna voce disponibile", display::kMuted, 2);
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

        drawCard(x, y, tileW, tileH,
                 selected ? display::kNeoYellow : display::kPaperLight, selected);

        display::g().fillRect(x + 2, y + 2, tileW - 4, 4, accentFor(idx));
        display::g().drawFastHLine(x + 2, y + 6, tileW - 4, display::kInk);

        const int iconSize = std::max(12, std::min(kGridIcon, tileH - 20));
        display::drawIcon(item.icon, x + (tileW - iconSize) / 2, y + 8, iconSize);

        std::string label = toUpper(item.label);
        label = ellipsize(label, tileW - 6);
        auto& d = display::g();
        d.setFont(&fonts::Font0);
        const int textW = d.textWidth(label.c_str());
        display::text(x + (tileW - textW) / 2, y + tileH - 10, label,
                      item.enabled ? display::kInk : display::kMuted, 0);
    }
}

void MenuManager::drawCarousel() {
    const int w = display::width();
    const int total = static_cast<int>(items_.size());
    if (total == 0) {
        display::textCentered(kHeaderHeight + 20, "Nessuna voce disponibile",
                              display::kMuted, 2);
        return;
    }

    const int centerX = w / 2;
    const int centerW = 104;
    const int centerH = 74;
    const int sideW = 70;
    const int sideH = 58;
    const int centerY = kHeaderHeight + 8;
    const int sideY = kHeaderHeight + 17;
    const int shift = carouselAnim_ > 0
                          ? carouselDir_ * (carouselAnim_ * kCarouselSpacing / kCarouselFrames)
                          : 0;

    auto drawAt = [&](int offset) {
        const int idx = ((selected_ + offset) % total + total) % total;
        const MenuItem& item = items_[idx];
        if (item.icon == "separator") {
            return;
        }
        const bool center = (offset == 0);
        const int cw = center ? centerW : sideW;
        const int ch = center ? centerH : sideH;
        const int x = centerX + offset * kCarouselSpacing - cw / 2 + shift;
        const int y = center ? centerY : sideY;
        drawCarouselCard(item, x, y, cw, ch, center, idx);
    };

    drawAt(-1);
    drawAt(1);
    drawAt(0);

    if (total <= 10) {
        const int spacing = 9;
        const int startX = (w - (total - 1) * spacing) / 2;
        for (int i = 0; i < total; ++i) {
            display::g().fillCircle(startX + i * spacing, display::height() - 17, 2,
                                    i == selected_ ? display::kNeoPink : display::kMuted);
        }
    } else if (carouselAnim_ == 0) {
        const std::string page = std::to_string(selected_ + 1) + "/" + std::to_string(total);
        display::textCentered(display::height() - 24, page, display::kMuted, 0);
    }

    if (carouselAnim_ > 0) {
        --carouselAnim_;
    }
}

int MenuManager::selectedIndex() const {
    return selected_;
}

void MenuManager::selectIndex(int index) {
    if (items_.empty()) {
        return;
    }
    selected_ = std::max(0, std::min(index, static_cast<int>(items_.size()) - 1));
    ensureVisible();
}

const std::vector<MenuItem>& MenuManager::items() const {
    return items_;
}
