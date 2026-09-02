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

# Write via a temporary and only replace the header when it actually changes,
# so that rebuilding at the same commit does not recompile everything that
# includes it. Staying quiet in that case also keeps this off the build log.
set(version_header_tmp "${VERSION_HEADER_OUT}.tmp")

configure_file(
    "${VERSION_HEADER_IN}"
    "${version_header_tmp}"
    @ONLY
)

if(EXISTS "${VERSION_HEADER_OUT}")
    file(READ "${VERSION_HEADER_OUT}" version_header_existing)
    file(READ "${version_header_tmp}" version_header_new)
    string(COMPARE EQUAL "${version_header_existing}" "${version_header_new}"
        version_header_unchanged)
else()
    set(version_header_unchanged FALSE)
endif()

if(NOT version_header_unchanged)
    file(RENAME "${version_header_tmp}" "${VERSION_HEADER_OUT}")
    message(STATUS "Firmware v${PROJECT_VERSION} (${GIT_DESCRIPTION})")
else()
    file(REMOVE "${version_header_tmp}")
endif()
