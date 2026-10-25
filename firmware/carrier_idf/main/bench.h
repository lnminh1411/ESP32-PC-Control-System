// Board benchmarks: CPU, SRAM, PSRAM, flash, display SPI, USB CDC, Wi-Fi
#pragma once
#include <esp_http_server.h>

void benchCommand(const char *a1, const char *a2);   // console `bench`
esp_err_t benchHttpHandler(httpd_req_t *req);        // GET /bench?kb=N
