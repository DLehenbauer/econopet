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

# Provides add_rom_header() function for converting binary ROM files to C headers.
#
# Usage:
#   add_rom_header(<target> <input_bin> <output_header>)
#
# Example:
#   add_rom_header(firmware.elf
#       ${ROMS_DIR}/901447-10.bin
#       ${CMAKE_CURRENT_BINARY_DIR}/roms/901447_10.h
#   )
#
# The generated header can be included in C source as an array initializer:
#   const uint8_t rom_data[] = {
#       #include "901447_10.h"
#   };

function(add_rom_header TARGET INPUT_BIN OUTPUT_HEADER)
    # Get the directory containing this file (where BinToHeader.cmake lives)
    get_filename_component(CMAKE_MODULE_DIR "${CMAKE_CURRENT_FUNCTION_LIST_FILE}" DIRECTORY)
    
    # Ensure output directory exists
    get_filename_component(OUTPUT_DIR "${OUTPUT_HEADER}" DIRECTORY)
    
    add_custom_command(
        OUTPUT "${OUTPUT_HEADER}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OUTPUT_DIR}"
        COMMAND ${CMAKE_COMMAND}
            -DINPUT=${INPUT_BIN}
            -DOUTPUT=${OUTPUT_HEADER}
            -P ${CMAKE_MODULE_DIR}/BinToHeader.cmake
        DEPENDS "${INPUT_BIN}"
        COMMENT "Generating ROM header: ${OUTPUT_HEADER}"
        VERBATIM
    )
    
    # Add the generated header as a source dependency of the target
    target_sources(${TARGET} PRIVATE "${OUTPUT_HEADER}")
endfunction()
