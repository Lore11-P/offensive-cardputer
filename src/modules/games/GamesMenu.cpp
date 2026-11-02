#include "GamesMenu.h"

#include "Breakout.h"
#include "Config.h"
#include "Connect4.h"
#include "Display.h"
#include "Doom.h"
#include "Flappy.h"
#include "Game2048.h"
#include "MenuManager.h"
#include "Minesweeper.h"
#include "Pong.h"
#include "Simon.h"
#include "Snake.h"
#include "Tetris.h"
#include "Ui.h"

#if ENABLE_MODULE_GAMES

namespace games {
namespace {
uint8_t g_savedRotation = 1;
bool g_displayOverridden = false;
}  // namespace

void enterGameDisplay() {
    if (!g_displayOverridden) {
        g_savedRotation = config::rotation();
        g_displayOverridden = true;
    }
    display::applyRotation(1);
}

void exitGameDisplay() {
    display::applyRotation(g_savedRotation);
    g_displayOverridden = false;
}

void runMenu() {
    MenuManager menu("Giochi", "Menu principale");
    menu.addItem("Tetris", "games", [] {
        enterGameDisplay();
        runTetris();
        exitGameDisplay();
    });
    menu.addItem("Breakout", "games", [] {
        enterGameDisplay();
        runBreakout();
        exitGameDisplay();
    });
    menu.addItem("Doom Lite", "doom", [] {
        enterGameDisplay();
        runDoomLite();
        exitGameDisplay();
    });
    menu.addItem("Snake", "snake", [] {
        enterGameDisplay();
        runSnake();
        exitGameDisplay();
    });
    menu.addItem("Pong", "pong", [] {
        enterGameDisplay();
        runPong();
        exitGameDisplay();
    });
    menu.addItem("2048", "2048", [] {
        enterGameDisplay();
        run2048();
        exitGameDisplay();
    });
    menu.addItem("Flappy", "flappy", [] {
        enterGameDisplay();
        runFlappy();
        exitGameDisplay();
    });
    menu.addItem("Simon", "simon", [] {
        enterGameDisplay();
        runSimon();
        exitGameDisplay();
    });
    menu.addItem("Minesweeper", "mines", [] {
        enterGameDisplay();
        runMinesweeper();
        exitGameDisplay();
    });
    menu.addItem("Connect 4", "connect4", [] {
        enterGameDisplay();
        runConnect4();
        exitGameDisplay();
    });
    menu.setCarousel();
    menu.setHint("`=indietro  </>=scorri  ENTER=gioca");
    menu.run(true);
}

}  // namespace games

#endif  // ENABLE_MODULE_GAMES
