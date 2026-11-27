idf_component_register(
  SRCS "src/Audio.cpp"
       "src/Playback.cpp"
       "src/MusicUi.cpp"
       "third_party/minimp3_impl.c"
  INCLUDE_DIRS "include"
  PRIV_INCLUDE_DIRS "src" "third_party"
  REQUIRES orbit_ui orbit_hal orbit_store esp_driver_i2s driver
           esp_http_client esp-tls mbedtls fatfs
)

# Unico sorgente C del componente: il decoder third-party, senza warnings.
target_compile_options(${COMPONENT_LIB} PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-w>)

