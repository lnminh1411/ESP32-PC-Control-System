# Carrier firmware, ESP-IDF build (idf.py). See README.md.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Build only the components main (and what it requires) pulls in, not all of ESP-IDF
idf_build_set_property(MINIMAL_BUILD ON)
project(carrier)
