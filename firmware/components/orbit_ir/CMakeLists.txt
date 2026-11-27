idf_component_register(
  SRCS "src/Ir.cpp" "src/IrProtocol.cpp" "src/IrRmt.cpp"
  INCLUDE_DIRS "include"
  REQUIRES orbit_ui orbit_hal orbit_store orbit_mascot esp_driver_rmt driver nvs_flash
)
