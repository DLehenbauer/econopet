# PET emulator (software model of the "rest of the machine").
#
# Vendored from https://github.com/dlehenbauer/pet-emulator (branch: io).
# See sim/README.md for origin, upstream commit, and local modifications.

cmake_minimum_required(VERSION 3.13.1)
project(pet_emulator CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(pet_emulator
    src/main.cpp
    src/clock.cpp
    src/display.cpp
    src/trace.cpp
)
target_precompile_headers(pet_emulator PRIVATE src/pch.hpp)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Boost.Asio (steady_timer) and Boost.Lockfree (spsc_queue); see src/pch.hpp.
find_package(Boost 1.74 COMPONENTS system REQUIRED)

find_package(SDL2 REQUIRED)

target_include_directories(pet_emulator PRIVATE
    ${Boost_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS})

target_link_libraries(pet_emulator PRIVATE
    ${Boost_LIBRARIES}
    ${SDL2_LIBRARIES}
    Threads::Threads)
