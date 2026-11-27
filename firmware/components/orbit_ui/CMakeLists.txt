idf_component_register(
  SRCS "src/Display.cpp"
       "src/MenuManager.cpp"
       "src/Ui.cpp"
       "src/Orbit.cpp"
       "src/OrbitIcons.cpp"
       "src/Navigation.cpp"
  INCLUDE_DIRS "include"
  REQUIRES lgfx orbit_hal driver esp_timer heap
)
