# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

# Discover exported unsigned literals using the C compiler's preprocessor.
function(econopet_discover_hardware_contract HEADER OUTPUT_NAMES)
    # Let GNU/Clang handle legal whitespace, comments, and line continuations.
    execute_process(
        COMMAND "${CMAKE_C_COMPILER}" -std=c11 -dM -E -x c "${HEADER}"
        RESULT_VARIABLE RESULT OUTPUT_VARIABLE MACROS ERROR_VARIABLE ERROR)
    if(NOT RESULT STREQUAL "0")
        message(FATAL_ERROR "Hardware contract preprocessing failed (${RESULT}): ${ERROR}")
    endif()

    # Inspect every exported macro, rejecting anything outside the literal contract.
    string(REGEX MATCHALL "[^\n]+" DEFINITIONS "${MACROS}")
    set(NAMES "")
    foreach(DEFINITION IN LISTS DEFINITIONS)
        if(DEFINITION MATCHES "^#[ \t]*define[ \t]+ECONOPET_")
            if(NOT DEFINITION MATCHES "^#[ \t]*define[ \t]+ECONOPET_([A-Z0-9_]+)[ \t]+(0[xX][0-9A-Fa-f]+|[0-9]+)[uU][ \t]*$")
                message(FATAL_ERROR
                    "Unsupported hardware contract export in ${HEADER}: ${DEFINITION}\n"
                    "Use an object-like ECONOPET_[A-Z0-9_]+ macro with an unsigned decimal or hexadecimal literal (u suffix).")
            endif()
            list(APPEND NAMES "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(NAMES STREQUAL "")
        message(FATAL_ERROR "No hardware contract constants found in ${HEADER}")
    endif()
    set(${OUTPUT_NAMES} "${NAMES}" PARENT_SCOPE)
endfunction()
