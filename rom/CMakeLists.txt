cmake_minimum_required(VERSION 3.13)
project(PET_ROM LANGUAGES NONE)

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

# Add a clean target to remove generated files
add_custom_target(clean_all
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${OBJ_DIR}
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${BIN_DIR}
    COMMENT "Cleaning build artifacts"
)
