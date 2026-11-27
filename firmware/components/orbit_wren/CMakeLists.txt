idf_component_register(
  SRCS "src/GameRuntime.cpp"
  INCLUDE_DIRS "include"
  REQUIRES wren lgfx orbit_ui orbit_hal orbit_audio orbit_store esp_timer
)
