cmake_minimum_required(VERSION 3.16)

# sdkconfig.defaults.local ist gitignored — dort kommen WiFi-Credentials rein.
# Reihenfolge: defaults → local overrides → sdkconfig (generiert)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.local")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.local")
else()
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(studio-panel)
