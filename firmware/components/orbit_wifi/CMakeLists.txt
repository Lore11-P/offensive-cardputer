idf_component_register(
  SRCS "src/Wifi.cpp" "src/Attacks.cpp"
  INCLUDE_DIRS "include"
  PRIV_INCLUDE_DIRS "src"
  REQUIRES orbit_ui orbit_hal orbit_store orbit_mascot esp_wifi esp_netif esp_event nvs_flash
           esp_http_server driver lwip esp_timer
)
