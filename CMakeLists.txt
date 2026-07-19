cmake_minimum_required(VERSION 3.16)

# Bump this for releases — the create-release workflow tags v<PROJECT_VER>.
# Bump on every functional change so OTA/update-checks see a new version.
set(PROJECT_VER "0.2.61")

# Allow local overrides (WiFi creds, MQTT broker, etc.) — not checked into git.
# Copy sdkconfig.defaults.local.example -> sdkconfig.defaults.local and edit.
if(EXISTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults.local")
    list(APPEND SDKCONFIG_DEFAULTS
         "${CMAKE_SOURCE_DIR}/sdkconfig.defaults"
         "${CMAKE_SOURCE_DIR}/sdkconfig.defaults.local")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mitsubishi-heatpump)
