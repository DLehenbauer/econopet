# PET Clone - Open hardware implementation of the Commodore PET
# by Daniel Lehenbauer and contributors.
#
# https://github.com/DLehenbauer/commodore-pet-clone
#
# To the extent possible under law, I, Daniel Lehenbauer, have waived all
# copyright and related or neighboring rights to this project. This work is
# published from the United States.
#
# @copyright CC0 http://creativecommons.org/publicdomain/zero/1.0/
# @author Daniel Lehenbauer <DLehenbauer@users.noreply.github.com> and contributors

# Provides git version information for embedding in firmware.
#
# After including this module, the following variables are set:
#   GIT_HASH  - Short git commit hash (e.g., "abc1234")
#   GIT_DIRTY - 1 if working tree has uncommitted changes, 0 otherwise
#
# Usage:
#   include(${FW_DIR}/cmake/GitVersion.cmake)

find_package(Git QUIET)

if(GIT_FOUND)
    # Get the short commit hash
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
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
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        RESULT_VARIABLE GIT_DIRTY_RESULT
    )
    
    if(GIT_DIRTY_RESULT EQUAL 0)
        set(GIT_DIRTY 0)
    else()
        set(GIT_DIRTY 1)
    endif()
else()
    set(GIT_HASH "unknown")
    set(GIT_DIRTY 0)
endif()

message(STATUS "Git hash: ${GIT_HASH}${GIT_DIRTY}")
