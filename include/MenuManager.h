#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "MenuItem.h"

// Menu gerarchico con breadcrumb, lista o griglia di icone.
// Ogni voce apre un sottomenu/modulo tramite callback.
class MenuManager {
public:
    explicit MenuManager(std::string title, std::string parent = "");

    void addItem(std::string label, std::string icon, std::function<void()> action,
                 bool enabled = true);
    void addItem(MenuItem item);
    void addSeparator(const std::string& label = "");
    void setItems(std::vector<MenuItem> items);
    void setHint(const std::string& hint);

    // Attiva la navigazione a icone. In orizzontale usa 3 colonne, in
    // verticale 2; il numero di righe dipende dallo spazio disponibile.
    void setGrid(uint8_t columns = 3);
    bool gridMode() const { return mode_ == Mode::Grid; }

    // Menu orizzontale a scorrimento: card centrale grande, voci adiacenti
    // in anteprima. Sinistra/destra (o su/giu) scorrono con wrap-around.
    void setCarousel();
    bool carouselMode() const { return mode_ == Mode::Carousel; }

    // Callback opzionale per i tasti stampabili (es. F = preferito).
    // Ritornare true se il tasto e' stato gestito.
    void setTextHandler(std::function<bool(char)> handler);

    // Ciclo di navigazione bloccante fino a ESC (o selezione di una voce,
    // che esegue la callback e poi torna al menu).
    void run(bool allowBack = true);
    void draw();

    int selectedIndex() const;

    // Sposta la selezione su una voce (usato dalle scorciatoie dei moduli:
    // es. il tasto P salta al comando Power e lo invia).
    void selectIndex(int index);

    const std::vector<MenuItem>& items() const;

private:
    enum class Mode { List, Grid, Carousel };

    bool selectable(int index) const;
    void moveSelection(int delta, bool animate = false);
    void moveGrid(int dx, int dy);
    void ensureVisible();
    int rowsPerPage() const;
    int gridColumns() const;
    int gridRows() const;
    int pageSize() const;
    void drawList();
    void drawGrid();
    void drawCarousel();

    std::vector<MenuItem> items_;
    std::string title_;
    std::string parent_;
    std::string hint_;
    std::function<bool(char)> textHandler_;
    int selected_ = 0;
    int top_ = 0;
    int visibleRows_ = 4;
    Mode mode_ = Mode::List;
    uint8_t requestedColumns_ = 3;
    int carouselDir_ = 0;
    int carouselAnim_ = 0;
};
