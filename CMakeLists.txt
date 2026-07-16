cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/version.cmake)
if(NOT IDF_VERSION_MAJOR EQUAL 5)
    message(FATAL_ERROR
        "ESP-BH1750 is pinned to ESP-IDF 5.x. "
        "Source /Users/soham/.espressif/v5.5.4/esp-idf/export.sh before building.")
endif()

# Set path to the shared HomeKit SDK from the peer project
set(HOMEKIT_PATH "${CMAKE_CURRENT_LIST_DIR}/../ESP-ARC452A21/external/esp-homekit-sdk")
set(EXTRA_COMPONENT_DIRS
    "${HOMEKIT_PATH}/components"
    "${HOMEKIT_PATH}/components/homekit")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_bh1750)


