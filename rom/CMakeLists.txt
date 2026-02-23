cmake_minimum_required(VERSION 3.13)
project(PET_ROM LANGUAGES NONE)

# =============================================================================
# Menu ROM (CC65)
# =============================================================================

# Define the output program name
set(ROM_NAME menu.rom)

# Define the source directory and output directory
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(OBJ_DIR ${CMAKE_CURRENT_BINARY_DIR}/obj)
set(BIN_DIR ${CMAKE_CURRENT_BINARY_DIR}/bin)

# Set the cc65 toolchain paths
find_program(CA65_EXECUTABLE ca65 REQUIRED)
find_program(LD65_EXECUTABLE ld65 REQUIRED)

# Add a custom target to assemble the source files
add_custom_target("PET_ROM_Assemble" ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${OBJ_DIR}
    COMMAND ${CA65_EXECUTABLE} -t pet -g -o ${OBJ_DIR}/main.o ${SRC_DIR}/main.s
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Assembling source files with CA65"
)

# Add a custom target to link the object files
# Uses copy_if_different to avoid updating timestamp when content hasn't changed
add_custom_target("PET_ROM" ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BIN_DIR}
    COMMAND ${LD65_EXECUTABLE} -C ${SRC_DIR}/pet.cfg -o ${BIN_DIR}/${ROM_NAME}.tmp ${OBJ_DIR}/main.o
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${BIN_DIR}/${ROM_NAME}.tmp ${BIN_DIR}/${ROM_NAME}
    COMMAND ${CMAKE_COMMAND} -E remove -f ${BIN_DIR}/${ROM_NAME}.tmp
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Linking object files with LD65"
    DEPENDS "PET_ROM_Assemble"
)

# =============================================================================
# ColourPET Edit ROMs (ACME)
# =============================================================================
# Build 4 variants: graphics/business keyboard x 40/80 columns.
# The config file uses !ifndef guards so ACME -D flags override defaults.

# Find the ACME cross-assembler
find_program(ACME_EXECUTABLE acme REQUIRED)
message(STATUS "Found ACME: ${ACME_EXECUTABLE}")

# Define paths for ColourPET edit ROM
set(CBM_EDIT_ROM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/cbm-edit-rom)

# Collect all assembly source files as dependencies
file(GLOB COLOURPET_ASM_SOURCES 
    ${CBM_EDIT_ROM_DIR}/*.asm
    ${CBM_EDIT_ROM_DIR}/config-custom/*.asm
)

# Variant definitions: name, CODEBASE, KEYSCAN, KEYBOARD, COLUMNS, FONTSET
#   n = normal/graphics keyboard, b = business keyboard
# NOTE: 80-column ColourPET variants (CODEBASE=1) require upstream cbm-edit-rom
# to add editrom80cls.asm, editrom80lm.asm, and editrom80scrollup.asm.
set(COLOURPET_VARIANTS
    colourpet-n40
    colourpet-b40
)

# Per-variant settings (parallel arrays)
#                       n40 b40
set(COLOURPET_CODEBASE   0   0)
set(COLOURPET_KEYSCAN    0   1)
set(COLOURPET_KEYBOARD   0   1)
set(COLOURPET_COLUMNS   40  40)
set(COLOURPET_FONTSET    1   1)

set(COLOURPET_OUTPUTS)
set(_idx 0)
foreach(VARIANT_NAME ${COLOURPET_VARIANTS})
    list(GET COLOURPET_CODEBASE ${_idx} _CODEBASE)
    list(GET COLOURPET_KEYSCAN  ${_idx} _KEYSCAN)
    list(GET COLOURPET_KEYBOARD ${_idx} _KEYBOARD)
    list(GET COLOURPET_COLUMNS  ${_idx} _COLUMNS)
    list(GET COLOURPET_FONTSET  ${_idx} _FONTSET)

    set(VARIANT_OUTPUT ${BIN_DIR}/${VARIANT_NAME}.bin)

    add_custom_command(
        OUTPUT ${VARIANT_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${BIN_DIR}
        COMMAND ${ACME_EXECUTABLE}
            -DCODEBASE=${_CODEBASE}
            -DKEYSCAN=${_KEYSCAN}
            -DKEYBOARD=${_KEYBOARD}
            -DCOLUMNS=${_COLUMNS}
            -DFONTSET=${_FONTSET}
            -o ${VARIANT_OUTPUT}
            -f plain
            config-custom/!colourpet40.asm
        WORKING_DIRECTORY ${CBM_EDIT_ROM_DIR}
        DEPENDS ${COLOURPET_ASM_SOURCES}
        COMMENT "Assembling ColourPET edit ROM: ${VARIANT_NAME}"
        VERBATIM
    )

    list(APPEND COLOURPET_OUTPUTS ${VARIANT_OUTPUT})
    math(EXPR _idx "${_idx} + 1")
endforeach()

# Custom target for all ColourPET edit ROM variants
add_custom_target(ColourPET_EditROM ALL
    DEPENDS ${COLOURPET_OUTPUTS}
)

# =============================================================================
# Clean target
# =============================================================================

add_custom_target(clean_all
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${OBJ_DIR}
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${BIN_DIR}
    COMMENT "Cleaning build artifacts"
)
