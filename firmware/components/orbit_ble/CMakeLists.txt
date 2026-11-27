idf_component_register(
  SRCS "src/Ble.cpp"
  INCLUDE_DIRS "include"
  REQUIRES bt orbit_ui orbit_hal orbit_settings orbit_mascot esp_timer
)
