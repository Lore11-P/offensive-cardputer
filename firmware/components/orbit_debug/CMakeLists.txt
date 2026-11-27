idf_component_register(
  SRCS "src/Console.cpp"
  INCLUDE_DIRS "include"
  REQUIRES driver esp_driver_usb_serial_jtag orbit_store orbit_wren orbit_ui orbit_hal esp_timer fatfs
)
