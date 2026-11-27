# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

cmake_minimum_required(VERSION 3.13)
project(drive-test LANGUAGES NONE)

find_program(CL65_EXECUTABLE cl65 REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(BIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/bin")
set(PRG_FILE "${BIN_DIR}/drive-test.prg")
set(MAP_FILE "${BIN_DIR}/drive-test.map")
set(LABEL_FILE "${BIN_DIR}/drive-test.lbl")
set(SHARED_INC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../include")

add_custom_command(
    OUTPUT "${PRG_FILE}" "${MAP_FILE}" "${LABEL_FILE}"
    BYPRODUCTS "${BIN_DIR}/drive-test.s" "${BIN_DIR}/drive-test.o" "${BIN_DIR}/drive_vectors.o"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${BIN_DIR}"
    COMMAND "${CL65_EXECUTABLE}" -t pet --cpu 6502 -Os -Cl -W -unknown-pragma
        -I "${SHARED_INC_DIR}" -S
        -o "${BIN_DIR}/drive-test.s" "${CMAKE_CURRENT_SOURCE_DIR}/drive-test.c"
    COMMAND "${CL65_EXECUTABLE}" -t pet --cpu 6502 -c
        -o "${BIN_DIR}/drive-test.o" "${BIN_DIR}/drive-test.s"
    COMMAND "${CL65_EXECUTABLE}" -t pet --cpu 6502 -c
        -o "${BIN_DIR}/drive_vectors.o" "${CMAKE_CURRENT_SOURCE_DIR}/drive_vectors.s"
    COMMAND "${CL65_EXECUTABLE}" -t pet --cpu 6502
        -C "${CMAKE_CURRENT_SOURCE_DIR}/drive-test.cfg"
        -m "${MAP_FILE}" -Ln "${LABEL_FILE}" -o "${PRG_FILE}"
        "${BIN_DIR}/drive-test.o" "${BIN_DIR}/drive_vectors.o"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_prg.py"
        "${PRG_FILE}" "${MAP_FILE}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/drive-test.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/drive-test.cfg"
        "${CMAKE_CURRENT_SOURCE_DIR}/drive_test_catalog.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/drive_test_items.inc"
        "${SHARED_INC_DIR}/target_diag.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/drive_vectors.s"
        "${CMAKE_CURRENT_SOURCE_DIR}/check_prg.py"
    COMMENT "Building and checking the base-RAM Drive Test PRG"
    VERBATIM)

add_custom_target(drive-test-prg ALL DEPENDS "${PRG_FILE}" "${MAP_FILE}" "${LABEL_FILE}")
