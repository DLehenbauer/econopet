# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

# Provides add_version_header() for generating 'version.h' into the build
# directory and compiling it into a target.
#
# Usage:
#   add_version_header(<target>)
#
# The header is regenerated on every build (not while configuring), so the git
# hash it reports follows the tree even when a commit, checkout or rebase
# happens without touching any CMakeLists.txt. It is rewritten only when its
# contents actually change, so an unchanged commit does not trigger a rebuild.
#
# The caller is responsible for adding the build directory to the target's
# include path.

function(add_version_header TARGET)
    get_filename_component(version_header_module_dir
        "${CMAKE_CURRENT_FUNCTION_LIST_FILE}" DIRECTORY)

    set(version_header "${CMAKE_CURRENT_BINARY_DIR}/version.h")

    # A custom target (rather than a custom command with the header as its
    # OUTPUT) so that it runs unconditionally: there is no input file whose
    # timestamp reflects the current commit.
    add_custom_target(${TARGET}_version_header
        BYPRODUCTS "${version_header}"
        COMMAND ${CMAKE_COMMAND}
            -DVERSION_HEADER_IN=${version_header_module_dir}/version.h.in
            -DVERSION_HEADER_OUT=${version_header}
            -DPROJECT_VERSION=${PROJECT_VERSION}
            -DPROJECT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
            -DPROJECT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
            -DPROJECT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
            -P ${version_header_module_dir}/GenerateVersionHeader.cmake
        COMMENT "Generating version.h"
        VERBATIM
    )

    add_dependencies(${TARGET} ${TARGET}_version_header)
endfunction()
