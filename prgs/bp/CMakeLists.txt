# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

cmake_minimum_required(VERSION 3.13)
project(bp LANGUAGES NONE)

set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(BIN_DIR ${CMAKE_CURRENT_BINARY_DIR}/bin)

find_program(CL65_EXECUTABLE cl65 REQUIRED)

set(PRG_NAME bp.prg)

add_custom_command(
    OUTPUT ${BIN_DIR}/${PRG_NAME}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BIN_DIR}
    COMMAND ${CL65_EXECUTABLE} -t pet --cpu 65C02 -o ${BIN_DIR}/${PRG_NAME} ${SRC_DIR}/bp.c
    DEPENDS ${SRC_DIR}/bp.c
    COMMENT "Building ${PRG_NAME}"
    VERBATIM
)

add_custom_target(bp ALL
    DEPENDS ${BIN_DIR}/${PRG_NAME}
)
