idf_component_register(
  SRC_DIRS "."
  INCLUDE_DIRS "."
  PRIV_REQUIRES
    esp_driver_gpio esp_driver_ledc esp_driver_spi esp_driver_uart esp_driver_tsens
    esp_lcd esp_timer esp_event esp_netif esp_wifi nvs_flash
    fatfs wear_levelling vfs app_update esp_partition spi_flash esp_psram bootloader_support
    esp_http_client esp_http_server esp-tls mbedtls json lovyangfx mqtt espcoredump)

# secrets_or_example.h picks secrets.h with __has_include, which neither ninja nor
# ccache's direct mode notices when secrets.h appears or goes away (a placeholder
# image was flashed once). The glob re-runs CMake when it does, and the define
# changes main's compile line, so every object that could include it recompiles.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
  file(GLOB secrets_h CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/secrets.h")
  if(secrets_h)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HAVE_SECRETS_H=1)
  endif()
endif()
