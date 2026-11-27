idf_component_register(
  SRCS "display.cpp" "keyboard.cpp" "battery.cpp"
  INCLUDE_DIRS "include"
  REQUIRES lgfx driver esp_timer esp_adc
)
