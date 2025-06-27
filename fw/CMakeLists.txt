cmake_minimum_required(VERSION 3.12)

# Must include the Pico SDK before 'project()'
include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
set (PICO_SDK_REQUIRED_VERSION "2.1.1")
if (PICO_SDK_VERSION_STRING VERSION_LESS PICO_SDK_REQUIRED_VERSION)
    message(FATAL_ERROR "Raspberry Pi Pico SDK version ${PICO_SDK_REQUIRED_VERSION} (or later) required. Your version is ${PICO_SDK_VERSION_STRING}")
endif()

# Initialize Raspberry Pi Pico SDK (must be done before 'project()')
pico_sdk_init()

# Record the root '/fw' directory relative to this file
set(FW_DIR ${CMAKE_CURRENT_SOURCE_DIR})

if (NOT PICO_PLATFORM STREQUAL "host")
    add_subdirectory(src)
else()
    add_subdirectory(test)
endif()
