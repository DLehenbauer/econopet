# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

cmake_minimum_required(VERSION 3.13)
project(reload LANGUAGES NONE)

set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(BIN_DIR ${CMAKE_CURRENT_BINARY_DIR}/bin)

find_program(PETCAT_EXECUTABLE petcat REQUIRED)

set(PRG_NAME reload.prg)

add_custom_command(
    OUTPUT ${BIN_DIR}/${PRG_NAME}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BIN_DIR}
    COMMAND ${PETCAT_EXECUTABLE} -h -v -l 401 -w1p -o ${BIN_DIR}/${PRG_NAME} -- ${SRC_DIR}/reload.bas
    DEPENDS ${SRC_DIR}/reload.bas
    COMMENT "Building ${PRG_NAME}"
    VERBATIM
)

add_custom_target(reload ALL
    DEPENDS ${BIN_DIR}/${PRG_NAME}
)
