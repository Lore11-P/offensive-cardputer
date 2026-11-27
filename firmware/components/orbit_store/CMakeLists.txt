idf_component_register(
  SRCS "src/Wifi.cpp" "src/Sd.cpp" "src/Http.cpp" "src/Catalog.cpp" "src/Store.cpp"
  INCLUDE_DIRS "include"
  REQUIRES esp_http_client esp-tls mbedtls json fatfs esp_driver_sdspi sdmmc
           esp_wifi esp_netif esp_event nvs_flash esp_timer littlefs
)
