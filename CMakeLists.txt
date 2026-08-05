cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Opt-in per-build dependencies.lock (installer-poc uses this to keep the
# tracked repo-root lock untouched across target builds).
if(DEFINED HOMEKEY_DEPENDENCIES_LOCK)
    idf_build_set_property(DEPENDENCIES_LOCK "${HOMEKEY_DEPENDENCIES_LOCK}")
endif()
idf_build_set_property(MINIMAL_BUILD ON)
project(HomeKey-ESP32)
idf_build_get_property(IDF_TARGET IDF_TARGET)
if(NOT IDF_TARGET STREQUAL "esp32" AND CONFIG_ARDUINO_USE_USB_JTAG STREQUAL "y")
idf_build_set_property(COMPILE_OPTIONS "-DARDUINO_USB_CDC_ON_BOOT" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DARDUINO_USB_MODE" APPEND)
endif()
