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

include(ExternalProject)

project(EconoPET C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

add_compile_options(-Wall -Wextra -Wnull-dereference)

# Add projects
add_subdirectory("./sdcard")
add_subdirectory("./rom")
add_subdirectory("./fw/test")

ExternalProject_add(
    firmware
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/fw/targets/rp2040
    # Pass common build arguments to the child project
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_C_STANDARD=${CMAKE_C_STANDARD}
        -DCMAKE_CXX_STANDARD=${CMAKE_CXX_STANDARD}
        -DBUILD_TESTING=OFF  # Disable libyaml tests
    INSTALL_COMMAND ""  # No installation step
    BUILD_ALWAYS 1      # (Ninja will quickly early exit if unchanged.)
    # DEPENDS PET_ROM
)
