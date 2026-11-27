idf_component_register(SRCS "main.cpp" "selftest.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES orbit_app orbit_store orbit_wren orbit_settings orbit_debug orbit_hal
                                orbit_mascot orbit_audio orbit_ble orbit_wifi nvs_flash)

option(ORBIT_DIAG_SELFTEST "Auto-test seriale di bring-up (display, NVS, SD, audio, radio)" OFF)
if (ORBIT_DIAG_SELFTEST)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE ORBIT_DIAG_SELFTEST=1)
endif()

option(ORBIT_DIAG_STORE "Auto-test dello store (monta SD, WiFi, catalogo, 2048)" OFF)
if (ORBIT_DIAG_STORE)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE ORBIT_DIAG_STORE=1)
endif()

option(ORBIT_DIAG_CHAT "Auto-test della chat BLE all'avvio" OFF)
if (ORBIT_DIAG_CHAT)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE ORBIT_DIAG_CHAT=1)
endif()
