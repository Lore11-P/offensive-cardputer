#include "orbit_native_games/NativeGames.h"

#include <cstddef>

#include "Breakout.h"
#include "Chess.h"
#include "Connect4.h"
#include "Display.h"
#include "Doom.h"
#include "Flappy.h"
#include "Game2048.h"
#include "Lander.h"
#include "MenuManager.h"
#include "Minecraft.h"
#include "Minesweeper.h"
#include "Orbital.h"
#include "Pacman.h"
#include "PixelJump.h"
#include "Pong.h"
#include "Simon.h"
#include "Snake.h"
#include "StarSweep.h"
#include "Tetris.h"

namespace orbit_native_games {
namespace {

struct GameEntry {
    const char* label;
    const char* icon;
    void (*run)();
};

// Stessa tabella (etichette e icone) del menu giochi del firmware 2.x.
constexpr GameEntry kGames[] = {
    {"2048", "2048", games::run2048},
    {"Breakout", "games", games::runBreakout},
    {"Flappy", "flappy", games::runFlappy},
    {"Pong", "pong", games::runPong},
    {"Snake", "snake", games::runSnake},
    {"Tetris", "games", games::runTetris},
    {"Pac-Man", "pacman", games::runPacman},
    {"Doom Lite", "doom", games::runDoomLite},
    {"Simon", "simon", games::runSimon},
    {"Minesweeper", "mines", games::runMinesweeper},
    {"Connect 4", "connect4", games::runConnect4},
    {"Scacchi", "chess", games::runChess},
    {"Minecraft 3D", "minecraft", games::runMinecraft},
    {"StarSweep", "rocket", games::runStarSweep},
    {"Lander", "planet", games::runLander},
    {"Orbital", "sat", games::runOrbital},
    {"Pixel Jump", "games", games::runPixelJump},
};
constexpr size_t kGameCount = sizeof(kGames) / sizeof(kGames[0]);

// I giochi disegnano in orizzontale. Non si puo' assumere che la rotazione
// "1" sia landscape su questa variante del pannello: si sceglie la rotazione
// con width > height e si ripristina quella utente all'uscita.
uint8_t landscapeRotation() {
    const uint8_t saved = display::rotation();
    if (display::width() > display::height()) {
        return saved;
    }
    for (uint8_t r = 0; r < 4; ++r) {
        display::applyRotation(r);
        if (display::width() > display::height()) {
            return r;
        }
    }
    display::applyRotation(saved);
    return saved;
}

void runGame(const GameEntry& game) {
    const uint8_t saved = display::rotation();
    landscapeRotation();
    game.run();
    if (display::rotation() != saved) {
        display::applyRotation(saved);
    }
}

}  // namespace

void runMenu() {
    MenuManager menu("Giochi nativi", "Giochi");
    for (size_t i = 0; i < kGameCount; ++i) {
        menu.addItem(kGames[i].label, kGames[i].icon, [i] { runGame(kGames[i]); });
    }
    menu.setCarousel();
    menu.setHint("ESC=indietro  </>=scorri  ENTER=gioca");
    menu.run(true);
}

}  // namespace orbit_native_games
