idf_component_register(
  SRCS "src/Mascot.cpp" "src/MascotDraw.cpp"
  INCLUDE_DIRS "include"
  REQUIRES orbit_ui orbit_hal nvs_flash
)
