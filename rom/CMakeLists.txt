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
add_custom_target("PET_ROM" ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BIN_DIR}
    COMMAND ${LD65_EXECUTABLE} -C ${SRC_DIR}/pet.cfg -o ${BIN_DIR}/${ROM_NAME} ${OBJ_DIR}/main.o
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Linking object files with LD65"
    DEPENDS "PET_ROM_Assemble"
)

# =============================================================================
# ColourPET Edit ROM (ACME)
# =============================================================================

# Find the ACME cross-assembler
find_program(ACME_EXECUTABLE acme REQUIRED)
message(STATUS "Found ACME: ${ACME_EXECUTABLE}")

# Define paths for ColourPET edit ROM
set(CBM_EDIT_ROM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/cbm-edit-rom)
set(COLOURPET_SRC_OUTPUT ${CBM_EDIT_ROM_DIR}/binaries/colourpet-test.bin)
set(COLOURPET_OUTPUT ${BIN_DIR}/colourpet-test.bin)

# Collect all assembly source files as dependencies
file(GLOB COLOURPET_ASM_SOURCES 
    ${CBM_EDIT_ROM_DIR}/*.asm
    ${CBM_EDIT_ROM_DIR}/config-custom/*.asm
)

# Custom command to build the ColourPET edit ROM and copy to build directory
add_custom_command(
    OUTPUT ${COLOURPET_OUTPUT}
    COMMAND ${ACME_EXECUTABLE} config-custom/!colourpet40.asm
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BIN_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy ${COLOURPET_SRC_OUTPUT} ${COLOURPET_OUTPUT}
    WORKING_DIRECTORY ${CBM_EDIT_ROM_DIR}
    DEPENDS ${COLOURPET_ASM_SOURCES}
    COMMENT "Assembling ColourPET edit ROM with ACME"
    VERBATIM
)

# Custom target for ColourPET edit ROM
add_custom_target(ColourPET_EditROM ALL
    DEPENDS ${COLOURPET_OUTPUT}
)

# =============================================================================
# Clean target
# =============================================================================

add_custom_target(clean_all
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${OBJ_DIR}
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${BIN_DIR}
    COMMAND ${CMAKE_COMMAND} -E remove -f ${COLOURPET_OUTPUT}
    COMMENT "Cleaning build artifacts"
)
