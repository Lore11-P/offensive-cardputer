#include "GamesMenu.h"

#include "Breakout.h"
#include "Chess.h"
#include "Config.h"
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
#include "Ui.h"

#if ENABLE_MODULE_GAMES

namespace games {
namespace {
uint8_t g_savedRotation = 1;
bool g_displayOverridden = false;

struct GameEntry {
    const char* label;
    const char* icon;
    void (*run)();
};

// Tabella unica: una sola lambda di ingresso invece di 17 copie.
constexpr GameEntry kGames[] = {
    {"Tetris", "games", runTetris},
    {"Breakout", "games", runBreakout},
    {"Pac-Man", "pacman", runPacman},
    {"Doom Lite", "doom", runDoomLite},
    {"Snake", "snake", runSnake},
    {"Pong", "pong", runPong},
    {"2048", "2048", run2048},
    {"Flappy", "flappy", runFlappy},
    {"Simon", "simon", runSimon},
    {"Minesweeper", "mines", runMinesweeper},
    {"Connect 4", "connect4", runConnect4},
    {"Scacchi", "chess", runChess},
    {"Minecraft 3D", "minecraft", runMinecraft},
    {"StarSweep", "rocket", runStarSweep},
    {"Lander", "planet", runLander},
    {"Orbital", "sat", runOrbital},
    {"Pixel Jump", "games", runPixelJump},
};
constexpr size_t kGameCount = sizeof(kGames) / sizeof(kGames[0]);
}  // namespace

void enterGameDisplay() {
    if (!g_displayOverridden) {
        g_savedRotation = config::rotation();
        g_displayOverridden = true;
    }
    // I giochi disegnano in orizzontale. A seconda del modello e del valore
    // salvato, lo stesso indice puo' corrispondere a portrait o landscape:
    // si prova prima la rotazione dell'utente, poi 0/1/2/3 finche' il
    // framebuffer non e' largo (0 e' il landscape standard sul Cardputer ADV).
    const uint8_t kCandidates[5] = {g_savedRotation, 0, 1, 2, 3};
    for (uint8_t i = 0; i < 5; ++i) {
        if (i > 0 && kCandidates[i] == g_savedRotation) {
            continue;
        }
        display::applyRotation(kCandidates[i]);
        if (display::width() > display::height()) {
            return;
        }
    }
}

void exitGameDisplay() {
    display::applyRotation(g_savedRotation);
    g_displayOverridden = false;
}

void runMenu() {
    MenuManager menu("Giochi", "Menu principale");
    for (size_t i = 0; i < kGameCount; ++i) {
        menu.addItem(kGames[i].label, kGames[i].icon, [i] {
            enterGameDisplay();
            kGames[i].run();
            exitGameDisplay();
        });
    }
    menu.setCarousel();
    menu.setHint("ESC=indietro  </>=scorri  ENTER=gioca");
    menu.run(true);
}

}  // namespace games

#endif  // ENABLE_MODULE_GAMES
