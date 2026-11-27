idf_component_register(
  SRCS "src/Home.cpp" "src/StoreUi.cpp" "src/GamesUi.cpp" "src/WifiUi.cpp"
       "src/SettingsUi.cpp" "src/InputUi.cpp" "src/SdUi.cpp"
  INCLUDE_DIRS "include"
  REQUIRES orbit_ui orbit_store orbit_wren orbit_settings orbit_chat orbit_native_games
           orbit_mascot orbit_ble orbit_audio orbit_wifi orbit_ir orbit_tools
)
