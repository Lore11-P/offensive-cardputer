idf_component_register(
  SRCS "lgfx/v1/LGFXBase.cpp"
       "lgfx/v1/lgfx_fonts.cpp"
       "lgfx/v1/lgfx_v1.cpp"
       "lgfx/v1/lgfx_v1_panel.cpp"
       "lgfx/v1/lgfx_v1_platforms.cpp"
       "lgfx/v1/lgfx_v1_touch.cpp"
  INCLUDE_DIRS "."
  REQUIRES driver esp_lcd esp_timer efuse esp_hw_support esp_rom spi_flash
)
