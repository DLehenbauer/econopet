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

project(EconoPET C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

add_compile_options(-Wall -Wextra -Wnull-dereference)

# Add projects
add_subdirectory("./sdcard")
add_subdirectory("./rom")
add_subdirectory("./gw")
add_subdirectory("./fw")
