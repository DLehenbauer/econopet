# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

# Writes 'build-info.txt' into the SD card root and zips the result.
#
# This runs as a build step (via 'cmake -P') rather than while configuring, so
# that the recorded commit and the zip filename always describe the tree as it
# is when the package is built. Doing the same work while configuring would
# leave both stale after a commit, and forcing a reconfigure to refresh them
# would rebuild far more than the package.
#
# Expects the following on the command line:
#   SDCARD_ROOT_DIR - Directory holding the SD card contents to package
#   SDCARD_ZIP_DIR  - Directory to write the zip into

foreach(required_var SDCARD_ROOT_DIR SDCARD_ZIP_DIR)
    if(NOT DEFINED ${required_var})
        message(FATAL_ERROR "${required_var} must be defined (pass -D${required_var}=...)")
    endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/GitVersion.cmake")

string(TIMESTAMP sdcard_version "%y%m%d" UTC)
string(TIMESTAMP sdcard_build_time "%Y-%m-%d %H:%M:%S UTC" UTC)

# A package built from a modified tree cannot be reproduced from its commit
# alone, so say so rather than implying the commit tells the whole story.
if(GIT_DIRTY)
    set(sdcard_commit "${GIT_COMMIT} (plus uncommitted changes)")
else()
    set(sdcard_commit "${GIT_COMMIT}")
endif()

configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/build-info.txt.in"
    "${SDCARD_ROOT_DIR}/build-info.txt"
    @ONLY
)

# The commit is part of the name so that packages built from different commits
# on the same day remain distinguishable.
set(sdcard_zip_path "${SDCARD_ZIP_DIR}/EconoPET-40-8096-A-firmware-${sdcard_version}-${GIT_HASH}.zip")

file(MAKE_DIRECTORY "${SDCARD_ZIP_DIR}")

# Zip only the contents of ${SDCARD_ROOT_DIR}
execute_process(
    COMMAND ${CMAKE_COMMAND} -E tar cf "${sdcard_zip_path}" --format=zip .
    WORKING_DIRECTORY "${SDCARD_ROOT_DIR}"
    RESULT_VARIABLE sdcard_zip_result
)

if(NOT sdcard_zip_result EQUAL 0)
    message(FATAL_ERROR "Failed to create ${sdcard_zip_path}")
endif()

message(STATUS "Packaged SD card contents to ${sdcard_zip_path}")
