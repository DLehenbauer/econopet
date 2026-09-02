# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

# Writes the firmware 'version.h'. Invoked as a build step by
# add_version_header() (see AddVersionHeader.cmake), so that the git hash it
# records describes the tree as it is when the firmware is built.
#
# Expects the following on the command line:
#   VERSION_HEADER_IN  - Path to version.h.in
#   VERSION_HEADER_OUT - Path of the header to write
#   PROJECT_VERSION, PROJECT_VERSION_MAJOR/MINOR/PATCH - Firmware version

foreach(required_var VERSION_HEADER_IN VERSION_HEADER_OUT PROJECT_VERSION)
    if(NOT DEFINED ${required_var})
        message(FATAL_ERROR "${required_var} must be defined (pass -D${required_var}=...)")
    endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/GitVersion.cmake")

# Write via a temporary so that an unchanged header keeps its timestamp and
# does not force a rebuild of everything that includes it.
configure_file(
    "${VERSION_HEADER_IN}"
    "${VERSION_HEADER_OUT}.tmp"
    @ONLY
)

execute_process(
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${VERSION_HEADER_OUT}.tmp"
            "${VERSION_HEADER_OUT}"
    RESULT_VARIABLE copy_result
)

file(REMOVE "${VERSION_HEADER_OUT}.tmp")

if(NOT copy_result EQUAL 0)
    message(FATAL_ERROR "Failed to write ${VERSION_HEADER_OUT}")
endif()
