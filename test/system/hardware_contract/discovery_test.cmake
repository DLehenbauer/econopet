# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

cmake_minimum_required(VERSION 3.20...3.31)

include("${DISCOVERY_MODULE}")

# Run each candidate in isolation so expected fatal errors do not abort the suite.
if(DEFINED HEADER)
    econopet_discover_hardware_contract("${HEADER}" NAMES)
    if(NOT NAMES STREQUAL "GOOD")
        message(FATAL_ERROR "Unexpected discovered names: ${NAMES}")
    endif()
    return()
endif()

# Check accepted spelling variations and declarations that must fail closed.
set(CASES spaces tabs comments continuation function empty alias expression signed lowercase no_exports preprocessing_error)
set(spaces "#define ECONOPET_GOOD 8u\n")
set(tabs "\t#\tdefine\tECONOPET_GOOD\t8u\t\n")
set(comments "#/**/define/**/ECONOPET_GOOD/**/0x08U /* value */\n")
set(continuation "#def\\\nine ECONOPET_GO\\\nOD \\\n8u\n")
set(function "#define ECONOPET_BAD(x) 8u\n")
set(empty "#define ECONOPET_BAD\n")
set(alias "#define ECONOPET_BAD ECONOPET_GOOD\n")
set(expression "#define ECONOPET_BAD (1u << 3)\n")
set(signed "#define ECONOPET_BAD 8\n")
set(lowercase "#define ECONOPET_bad 8u\n")
set(no_exports "#define UNRELATED 8u\n")
set(preprocessing_error "#error invalid contract\n")
file(MAKE_DIRECTORY "${TEST_DIR}")
foreach(CASE IN LISTS CASES)
    # Keep one valid export beside malformed ones to detect silent omission.
    if(CASE MATCHES "^(function|empty|alias|expression|signed|lowercase)$")
        set(CONTENTS "${spaces}${${CASE}}")
    else()
        set(CONTENTS "${${CASE}}")
    endif()
    set(HEADER "${TEST_DIR}/${CASE}.h")
    file(WRITE "${HEADER}" "${CONTENTS}")
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
        "-DDISCOVERY_MODULE=${DISCOVERY_MODULE}"
        "-DHEADER=${HEADER}"
        -P "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUTPUT ERROR_VARIABLE ERROR)

    # Check the failure reason as well as the exit code.
    if(CASE MATCHES "^(spaces|tabs|comments|continuation)$")
        if(NOT RESULT STREQUAL "0")
            message(FATAL_ERROR "${CASE} should be accepted: ${OUTPUT}${ERROR}")
        endif()
    else()
        if(CASE STREQUAL "no_exports")
            set(EXPECTED "No hardware contract constants found")
        elseif(CASE STREQUAL "preprocessing_error")
            set(EXPECTED "Hardware contract preprocessing failed")
        else()
            set(EXPECTED "Unsupported hardware contract export")
        endif()
        if(RESULT STREQUAL "0" OR NOT ERROR MATCHES "${EXPECTED}")
            message(FATAL_ERROR "${CASE} did not fail as expected: ${OUTPUT}${ERROR}")
        endif()
    endif()
    file(REMOVE "${HEADER}")
endforeach()
