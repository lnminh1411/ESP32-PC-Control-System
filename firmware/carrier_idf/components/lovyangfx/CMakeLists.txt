# LovyanGFX (submodule) registered with quoted paths: its own
# boards.cmake/esp-idf.cmake uses the legacy register_component(), which
# splits paths on spaces and breaks in "E:\ESP32 PC Controller".
set(LGFX_ROOT "${CMAKE_CURRENT_LIST_DIR}/LovyanGFX")
set(LGFX_SRCS
  "${LGFX_ROOT}/src/lgfx/v1/lgfx_v1.cpp"
  "${LGFX_ROOT}/src/lgfx/v1/lgfx_v1_panel.cpp"
  "${LGFX_ROOT}/src/lgfx/v1/lgfx_v1_touch.cpp"
  "${LGFX_ROOT}/src/lgfx/v1/lgfx_v1_platforms.cpp"
  "${LGFX_ROOT}/src/lgfx/v1/LGFXBase.cpp"
  "${LGFX_ROOT}/src/lgfx/v1/lgfx_fonts.cpp"
  "${LGFX_ROOT}/src/lgfx/utility/lgfx_miniz.c"
  "${LGFX_ROOT}/src/lgfx/utility/lgfx_pngle.c"
  "${LGFX_ROOT}/src/lgfx/utility/lgfx_qoi.c"
  "${LGFX_ROOT}/src/lgfx/utility/lgfx_qrcode.c"
  "${LGFX_ROOT}/src/lgfx/utility/lgfx_tjpgd.c")
idf_component_register(
  SRCS ${LGFX_SRCS}
  INCLUDE_DIRS "LovyanGFX/src"
  REQUIRES nvs_flash efuse esp_lcd driver esp_timer esp_mm)
target_compile_options(${COMPONENT_LIB} PRIVATE -w)   # third-party warnings
