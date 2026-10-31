idf_component_register(
  SRC_DIRS "."
  INCLUDE_DIRS "."
  EMBED_FILES gifs.bin
  PRIV_REQUIRES
    esp_driver_gpio esp_driver_ledc esp_driver_spi esp_driver_uart esp_driver_tsens
    esp_lcd esp_timer esp_event esp_netif esp_wifi nvs_flash
    fatfs wear_levelling vfs app_update esp_partition spi_flash esp_psram bootloader_support
    esp_http_client esp_http_server esp-tls mbedtls json lovyangfx mqtt espcoredump)

# settings_or_example.h picks settings.h with __has_include, which neither ninja nor
# ccache's direct mode notices when settings.h appears or goes away (a placeholder
# image was flashed once). The glob re-runs CMake when it does, and the define
# changes main's compile line, so every object that could include it recompiles.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
  file(GLOB settings_h CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/settings.h")
  if(settings_h)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HAVE_SETTINGS_H=1)
  endif()
endif()
