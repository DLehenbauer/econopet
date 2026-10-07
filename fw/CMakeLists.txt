cmake_minimum_required(VERSION 3.12)

# Distinguish a requested configuration from the SDK's automatic Release default.
set(FW_REQUESTED_BUILD_TYPE "${CMAKE_BUILD_TYPE}")

# Must include the Pico SDK before 'project()' and resolve the target platform.
include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
set (PICO_SDK_REQUIRED_VERSION "2.1.1")
if (PICO_SDK_VERSION_STRING VERSION_LESS PICO_SDK_REQUIRED_VERSION)
    message(FATAL_ERROR "Raspberry Pi Pico SDK version ${PICO_SDK_REQUIRED_VERSION} (or later) required. Your version is ${PICO_SDK_VERSION_STRING}")
endif()

# Host tests exercise paranoid assertions that production deliberately elides.
if(PICO_PLATFORM STREQUAL "host")
    set(FW_BUILD_TYPE Debug)
else()
    set(FW_BUILD_TYPE RelWithDebInfo)
endif()
if("${FW_REQUESTED_BUILD_TYPE}" STREQUAL "")
    set(CMAKE_BUILD_TYPE "${FW_BUILD_TYPE}" CACHE STRING "Build configuration" FORCE)
    message(STATUS "Defaulting firmware build type to '${FW_BUILD_TYPE}'")
endif()
if(NOT CMAKE_BUILD_TYPE STREQUAL FW_BUILD_TYPE)
    message(FATAL_ERROR "Firmware configuration ${PICO_PLATFORM} requires CMAKE_BUILD_TYPE=${FW_BUILD_TYPE}")
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
