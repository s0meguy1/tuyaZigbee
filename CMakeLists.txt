set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/cmake/Toolchain.cmake")

CMAKE_MINIMUM_REQUIRED(VERSION 3.22)
PROJECT(tuyaZigbee LANGUAGES C ASM)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Keep OTA A/B versioning out of common/version.h. A decimal cache override is
# deliberately normalized before it reaches the C preprocessor, so `013` can
# never become an accidental octal C literal.
set(MOES_APP_BUILD_OVERRIDE "" CACHE STRING
    "Override application build number for a clean OTA artifact (decimal 0..255; default is Build 12)")
set(MOES_APP_BUILD_EFFECTIVE 12)
if(NOT "${MOES_APP_BUILD_OVERRIDE}" STREQUAL "")
    if(NOT "${MOES_APP_BUILD_OVERRIDE}" MATCHES "^[0-9]+$")
        message(FATAL_ERROR "MOES_APP_BUILD_OVERRIDE must be a decimal integer from 0 to 255")
    endif()
    string(REGEX REPLACE "^0+" "" MOES_APP_BUILD_NORMALIZED "${MOES_APP_BUILD_OVERRIDE}")
    if("${MOES_APP_BUILD_NORMALIZED}" STREQUAL "")
        set(MOES_APP_BUILD_NORMALIZED 0)
    endif()
    if(MOES_APP_BUILD_NORMALIZED GREATER 255)
        message(FATAL_ERROR "MOES_APP_BUILD_OVERRIDE must be in the range 0..255")
    endif()
    set(MOES_APP_BUILD_EFFECTIVE "${MOES_APP_BUILD_NORMALIZED}")
    add_compile_definitions(MOES_APP_BUILD_OVERRIDE=${MOES_APP_BUILD_EFFECTIVE})
endif()
message(STATUS "MOES application build number: ${MOES_APP_BUILD_EFFECTIVE}")

ADD_SUBDIRECTORY(cmake_timestamp timestamp)
ADD_SUBDIRECTORY(bootloader)
ADD_SUBDIRECTORY(IASsensor)
ADD_SUBDIRECTORY(switch)
ADD_SUBDIRECTORY(light)
