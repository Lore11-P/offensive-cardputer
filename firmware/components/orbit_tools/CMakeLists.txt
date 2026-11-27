idf_component_register(
  SRCS "src/ToolsMenu.cpp" "src/Util.cpp" "src/ClockUi.cpp" "src/NotesUi.cpp"
       "src/CalcUi.cpp" "src/TotpUi.cpp" "src/QrUi.cpp" "src/InfoUi.cpp"
       "src/SdBrowserUi.cpp"
       "third_party/lgfx_qrcode.c"
  INCLUDE_DIRS "include"
  PRIV_INCLUDE_DIRS "src" "third_party"
  REQUIRES orbit_ui orbit_hal orbit_settings nvs_flash mbedtls
  PRIV_REQUIRES orbit_store esp_timer spi_flash esp_hw_support esp_system vfs
)
