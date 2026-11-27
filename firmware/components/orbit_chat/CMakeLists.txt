idf_component_register(
  SRCS "src/Chat.cpp"
  INCLUDE_DIRS "include"
  REQUIRES bt mbedtls orbit_ui orbit_hal orbit_settings esp_timer
)

target_compile_options(${COMPONENT_LIB} PRIVATE -w)
