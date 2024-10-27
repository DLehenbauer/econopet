# PET Clone - Open hardware implementation of the Commodore PET
# by Daniel Lehenbauer and contributors.
# 
# https://github.com/DLehenbauer/commodore-pet-clone
#
# To the extent possible under law, I, Daniel Lehenbauer, have waived all
# copyright and related or neighboring rights to this project. This work is
# published from the United States.
#
# @copyright CC0 http://creativecommons.org/publicdomain/zero/1.0/
# @author Daniel Lehenbauer <DLehenbauer@users.noreply.github.com> and contributors

set(CMAKE_VERBOSE_MAKEFILE ON)
cmake_minimum_required(VERSION 3.13...3.27)

# Include Raspberry Pi Pico SDK (must be before project)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(EconoPET C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

add_compile_options(-Wall -Wextra -Wnull-dereference)

set(PICO_SDK_REQUIRED_VERSION "2.0.0")
if (PICO_SDK_VERSION_STRING VERSION_LESS PICO_SDK_REQUIRED_VERSION)
    message(FATAL_ERROR "Raspberry Pi Pico SDK version ${PICO_SDK_REQUIRED_VERSION} (or later) required. Your version is ${PICO_SDK_VERSION_STRING}")
endif()

# Initialize Raspberry Pi Pico SDK
pico_sdk_init()

# Add projects
add_subdirectory("./fw")
add_subdirectory("./roms")
