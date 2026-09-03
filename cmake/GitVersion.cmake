# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

# Provides git version information for embedding in firmware and in the SD card
# package.
#
# After including this module, the following variables are set:
#   GIT_COMMIT      - Full git commit hash (e.g., "abc1234...")
#   GIT_HASH        - Short git commit hash (e.g., "abc1234")
#   GIT_DIRTY       - 1 if working tree has uncommitted changes, 0 otherwise
#   GIT_DESCRIPTION - GIT_HASH, with a "-dirty" suffix when GIT_DIRTY is set
#
# A tree that is not a git checkout (an exported archive, say) reports the
# commit as "unknown" and is treated as clean.
#
# These are captured whenever this module is included, so including it while
# configuring would report the commit as of the last configure. Both callers
# therefore include it from a script run as a build step instead, keeping the
# recorded commit in step with the tree being built. This module is therefore
# on the build path and stays silent: callers report what they generate.
#
# Usage:
#   include(${CMAKE_CURRENT_LIST_DIR}/../cmake/GitVersion.cmake)

find_package(Git QUIET)

# Subprojects configure with their own CMAKE_SOURCE_DIR, so run git from this
# module's directory, which is inside the working tree regardless of caller.
set(GIT_VERSION_WORK_DIR ${CMAKE_CURRENT_LIST_DIR})

# Values used when the source is not a git checkout at all, such as a build from
# an exported archive.
set(GIT_COMMIT "unknown")
set(GIT_HASH "unknown")
set(GIT_DIRTY 0)

if(GIT_FOUND)
    # Get the full commit hash. This also serves to detect whether we are in a
    # working tree: if it fails, the remaining queries are skipped and the
    # 'unknown' values above stand.
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
        WORKING_DIRECTORY ${GIT_VERSION_WORK_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_OUTPUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_COMMIT_RESULT
    )

    if(GIT_COMMIT_RESULT EQUAL 0)
        set(GIT_COMMIT "${GIT_COMMIT_OUTPUT}")

        # Get the short commit hash
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
            WORKING_DIRECTORY ${GIT_VERSION_WORK_DIR}
            OUTPUT_VARIABLE GIT_HASH_OUTPUT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE GIT_HASH_RESULT
        )

        if(GIT_HASH_RESULT EQUAL 0)
            set(GIT_HASH "${GIT_HASH_OUTPUT}")
        endif()

        # Check if working tree is dirty (has uncommitted changes)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} diff --quiet HEAD
            WORKING_DIRECTORY ${GIT_VERSION_WORK_DIR}
            OUTPUT_QUIET
            ERROR_QUIET
            RESULT_VARIABLE GIT_DIRTY_RESULT
        )

        if(NOT GIT_DIRTY_RESULT EQUAL 0)
            set(GIT_DIRTY 1)
        endif()
    endif()
endif()

if(GIT_DIRTY)
    set(GIT_DESCRIPTION "${GIT_HASH}-dirty")
else()
    set(GIT_DESCRIPTION "${GIT_HASH}")
endif()
