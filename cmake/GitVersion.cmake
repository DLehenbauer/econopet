# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

# Provides git version information for embedding in firmware and in the SD card
# package.
#
# After including this module, the following variables are set:
#   GIT_COMMIT - Full git commit hash (e.g., "abc1234...")
#   GIT_HASH   - Short git commit hash (e.g., "abc1234")
#   GIT_DIRTY  - 1 if working tree has uncommitted changes, 0 otherwise
#
# These are captured when CMake configures, not when the project builds. To keep
# them from going stale, this module asks CMake to reconfigure whenever the
# checked out commit changes. GIT_DIRTY is the exception: nothing in .git changes
# when a tracked file is edited, so a build started by editing a file may still
# report the dirty state as of the last configure.
#
# Usage:
#   include(${CMAKE_CURRENT_LIST_DIR}/../cmake/GitVersion.cmake)

find_package(Git QUIET)

# Subprojects configure with their own CMAKE_SOURCE_DIR, so run git from this
# module's directory, which is inside the working tree regardless of caller.
set(GIT_VERSION_WORK_DIR ${CMAKE_CURRENT_LIST_DIR})

if(GIT_FOUND)
    # Get the full commit hash
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
        WORKING_DIRECTORY ${GIT_VERSION_WORK_DIR}
        OUTPUT_VARIABLE GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_COMMIT_RESULT
    )

    if(NOT GIT_COMMIT_RESULT EQUAL 0)
        set(GIT_COMMIT "unknown")
    endif()

    # Get the short commit hash
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${GIT_VERSION_WORK_DIR}
        OUTPUT_VARIABLE GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_HASH_RESULT
    )
    
    if(NOT GIT_HASH_RESULT EQUAL 0)
        set(GIT_HASH "unknown")
    endif()
    
    # Check if working tree is dirty (has uncommitted changes)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} diff --quiet HEAD
        WORKING_DIRECTORY ${GIT_VERSION_WORK_DIR}
        RESULT_VARIABLE GIT_DIRTY_RESULT
    )
    
    if(GIT_DIRTY_RESULT EQUAL 0)
        set(GIT_DIRTY 0)
    else()
        set(GIT_DIRTY 1)
    endif()

    # Reconfigure when the checked out commit changes, so that the values above
    # do not survive a commit, checkout, merge or rebase. Only files that git
    # rewrites when it moves a ref are watched: the index is deliberately left
    # out, because git also rewrites it while merely reporting status, which
    # would reconfigure the build for no reason.
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --absolute-git-dir
        WORKING_DIRECTORY ${GIT_VERSION_WORK_DIR}
        OUTPUT_VARIABLE GIT_VERSION_GIT_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_VERSION_GIT_DIR_RESULT
    )

    if(GIT_VERSION_GIT_DIR_RESULT EQUAL 0)
        foreach(git_ref_file HEAD logs/HEAD)
            if(EXISTS "${GIT_VERSION_GIT_DIR}/${git_ref_file}")
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                    "${GIT_VERSION_GIT_DIR}/${git_ref_file}")
            endif()
        endforeach()
    endif()
else()
    set(GIT_COMMIT "unknown")
    set(GIT_HASH "unknown")
    set(GIT_DIRTY 0)
endif()

if(GIT_DIRTY)
    message(STATUS "Git hash: ${GIT_HASH}-dirty")
else()
    message(STATUS "Git hash: ${GIT_HASH}")
endif()
