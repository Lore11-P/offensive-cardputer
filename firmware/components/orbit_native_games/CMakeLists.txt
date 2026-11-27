idf_component_register(
  SRCS "src/Breakout.cpp"
       "src/Chess.cpp"
       "src/Connect4.cpp"
       "src/Doom.cpp"
       "src/Flappy.cpp"
       "src/Game2048.cpp"
       "src/Lander.cpp"
       "src/Minecraft.cpp"
       "src/Minesweeper.cpp"
       "src/Orbital.cpp"
       "src/Pacman.cpp"
       "src/PixelJump.cpp"
       "src/Pong.cpp"
       "src/Simon.cpp"
       "src/Snake.cpp"
       "src/StarSweep.cpp"
       "src/Tetris.cpp"
       "src/GamesUi.cpp"
       "src/NativeGames.cpp"
  INCLUDE_DIRS "include"
  REQUIRES orbit_ui orbit_hal lgfx esp_timer nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE -w)
