#pragma once

namespace gridmath {

// Prossima voce selezionabile della griglia, con wrap di pagina e clamp
// sull'ultima pagina parziale. Pura, cosi' la si prova sull'host.
template <typename IsEnabled>
int move(int selected, int count, int cols, int rows, int dx, int dy, IsEnabled enabled) {
    if (count <= 0 || cols <= 0 || rows <= 0) {
        return selected;
    }
    const int size = cols * rows;
    if (size <= 0) {
        return selected;
    }

    const int pageStart = (selected / size) * size;
    int row = (selected - pageStart) / cols;
    int col = (selected - pageStart) % cols;

    if (dx != 0) {
        col = (col + dx + cols) % cols;
    }

    int target = pageStart + row * cols + col;

    if (dy != 0) {
        const int nextRow = row + dy;
        if (nextRow >= 0 && nextRow < rows) {
            target = pageStart + nextRow * cols + col;
        } else {
            const int direction = (dy > 0) ? 1 : -1;
            int nextPageStart = pageStart + direction * size;
            if (nextPageStart < 0) {
                nextPageStart = ((count - 1) / size) * size;
            } else if (nextPageStart >= count) {
                nextPageStart = 0;
            }
            const int nextRowInPage = (direction > 0) ? 0 : (rows - 1);
            target = nextPageStart + nextRowInPage * cols + col;
        }
    }

    if (target >= count) {
        target = count - 1;
    }

    for (int attempt = 0; attempt < count; ++attempt) {
        if (target >= 0 && target < count && enabled(target)) {
            return target;
        }
        target = (target + 1) % count;
    }
    return selected;
}

}  // namespace gridmath
