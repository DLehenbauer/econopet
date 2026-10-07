# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

# Evaluate the host constants and surface execution failures before comparison.
execute_process(COMMAND "${CONTRACT_VALUES}"
    RESULT_VARIABLE HOST_RESULT OUTPUT_VARIABLE HOST_VALUES ERROR_VARIABLE HOST_ERROR)
if(NOT HOST_RESULT STREQUAL "0")
    message(FATAL_ERROR "Hardware contract C evaluation failed (${HOST_RESULT}): ${HOST_ERROR}")
endif()
if(HOST_VALUES STREQUAL "")
    message(FATAL_ERROR "Hardware contract C evaluator emitted no hardware constants")
endif()

# Evaluate the independently compiled SystemVerilog package.
execute_process(COMMAND "${VVP_EXECUTABLE}" "${CONTRACT_IMAGE}"
    RESULT_VARIABLE RTL_RESULT OUTPUT_VARIABLE RTL_VALUES ERROR_VARIABLE RTL_ERROR)
if(NOT RTL_RESULT STREQUAL "0")
    message(FATAL_ERROR "Hardware contract RTL evaluation failed (${RTL_RESULT}): ${RTL_ERROR}")
endif()
if(RTL_VALUES STREQUAL "")
    message(FATAL_ERROR "Hardware contract RTL evaluator emitted no hardware constants")
endif()

# Preserve names and ordering so missing, additional, or changed values fail.
string(REGEX MATCHALL "[^\n]+" HOST_LINES "${HOST_VALUES}")
string(REGEX MATCHALL "[^\n]+" RTL_LINES "${RTL_VALUES}")
list(LENGTH HOST_LINES HOST_COUNT)
list(LENGTH RTL_LINES RTL_COUNT)
if(HOST_COUNT EQUAL 0 OR RTL_COUNT EQUAL 0)
    message(FATAL_ERROR "Hardware contract output contains no entries: C=${HOST_COUNT}, RTL=${RTL_COUNT}")
endif()
if(NOT HOST_COUNT EQUAL RTL_COUNT)
    message(FATAL_ERROR "Hardware contract output count differs: C=${HOST_COUNT}, RTL=${RTL_COUNT}\nC:\n${HOST_VALUES}\nRTL:\n${RTL_VALUES}")
endif()
if(NOT HOST_VALUES STREQUAL RTL_VALUES)
    set(DIFFERENCES "")
    math(EXPR LAST_INDEX "${HOST_COUNT} - 1")
    foreach(INDEX RANGE ${LAST_INDEX})
        list(GET HOST_LINES ${INDEX} HOST_LINE)
        list(GET RTL_LINES ${INDEX} RTL_LINE)
        if(NOT HOST_LINE STREQUAL RTL_LINE)
            string(APPEND DIFFERENCES "C: ${HOST_LINE}, RTL: ${RTL_LINE}\n")
        endif()
    endforeach()
    if(DIFFERENCES STREQUAL "")
        message(FATAL_ERROR "Hardware contract evaluator outputs differ in formatting")
    endif()
    message(FATAL_ERROR "Hardware contract differs from common_pkg.sv:\n${DIFFERENCES}")
endif()
