idf_component_register(
  SRC_DIRS "."
  INCLUDE_DIRS "."
  PRIV_REQUIRES
    esp_driver_gpio esp_driver_ledc esp_driver_spi esp_driver_uart esp_driver_tsens
    esp_lcd esp_timer esp_event esp_netif esp_wifi nvs_flash
    fatfs wear_levelling vfs app_update esp_partition spi_flash esp_psram bootloader_support
    esp_http_client esp_http_server esp-tls mbedtls json lovyangfx mqtt espcoredump)
