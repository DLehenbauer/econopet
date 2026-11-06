# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

enable_language(CXX)
find_package(verilator REQUIRED HINTS $ENV{VERILATOR_ROOT})

# Use the Efinity source manifest, with packages before their consumers.
set(GW_MODEL_PROJECT "${GW_MODEL_DIR}/EconoPET.xml")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GW_MODEL_PROJECT}")
file(STRINGS "${GW_MODEL_PROJECT}" GW_MODEL_ENTRIES
    REGEX "<efx:(design|sim)_file[^>]*name=\"[^\"]+\"")
set(GW_MODEL_PACKAGES)
set(GW_MODEL_SOURCES)
foreach(ENTRY IN LISTS GW_MODEL_ENTRIES)
    string(REGEX REPLACE ".*name=\"([^\"]+)\".*" "\\1" SOURCE_PATH "${ENTRY}")
    if(SOURCE_PATH MATCHES "_pkg\\.sv$")
        list(APPEND GW_MODEL_PACKAGES "${GW_MODEL_DIR}/${SOURCE_PATH}")
    elseif(NOT SOURCE_PATH MATCHES "\\.svh$")
        list(APPEND GW_MODEL_SOURCES "${GW_MODEL_DIR}/${SOURCE_PATH}")
    endif()
endforeach()
set(GW_MODEL_SOURCES ${GW_MODEL_PACKAGES} ${GW_MODEL_SOURCES})
list(REMOVE_DUPLICATES GW_MODEL_SOURCES)

# Build generated-only models with compilation and linking scheduled by Ninja.
function(add_gateware_model NAME)
    set(MODEL_TARGET ${NAME}_vl_model)
    set(EXECUTABLE_TARGET ${NAME}_vl)
    set(MODEL_SOURCES ${GW_MODEL_SOURCES} "${GW_MODEL_DIR}/sim/${NAME}.sv")
    list(REMOVE_DUPLICATES MODEL_SOURCES)
    add_library(${MODEL_TARGET} OBJECT EXCLUDE_FROM_ALL)
    target_compile_features(${MODEL_TARGET} PRIVATE cxx_std_20)
    target_compile_options(${MODEL_TARGET} PRIVATE -O3 -march=native)
    set_target_properties(${MODEL_TARGET} PROPERTIES INTERPROCEDURAL_OPTIMIZATION FALSE)
    verilate(${MODEL_TARGET}
        PREFIX V${NAME}
        TOP_MODULE ${NAME}
        SOURCES ${MODEL_SOURCES} "${GW_MODEL_DIR}/verilator.vlt"
        OPT_FAST -O3
        OPT_SLOW -O3
        OPT_GLOBAL -O3
        # Refresh the native dependency manifest on build-time regeneration too.
        VERILATOR_ARGS --make cmake --main --timing --assert -O3
            --x-assign unique --x-initial unique
            --timescale 1ns/1ps -Wno-fatal -Wno-lint -Wno-style
            "-I${GW_MODEL_DIR}" "-I${GW_MODEL_DIR}/external/m6502/rtl"
            "-DECONOPET_ROMS_DIR=\"${ECONOPET_ROMS_DIR}\"")
    add_executable(${EXECUTABLE_TARGET} EXCLUDE_FROM_ALL)
    target_link_libraries(${EXECUTABLE_TARGET} PRIVATE ${MODEL_TARGET})
    set_target_properties(${EXECUTABLE_TARGET} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        INTERPROCEDURAL_OPTIMIZATION FALSE)
    add_dependencies(gateware_models ${EXECUTABLE_TARGET})
endfunction()
