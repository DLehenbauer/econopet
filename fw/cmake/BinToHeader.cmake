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

# Converts a binary file to a C header containing a byte array initializer.
#
# Called via: cmake -DINPUT=<binary_file> -DOUTPUT=<header_file> -P BinToHeader.cmake
#
# The output is a comma-separated list of hex bytes (e.g., "0x1c,0x22,0x4a,...")
# suitable for use as a C array initializer. Uses copy_if_different to avoid
# triggering rebuilds when the content hasn't changed.

if(NOT DEFINED INPUT)
    message(FATAL_ERROR "INPUT must be defined (path to binary file)")
endif()

if(NOT DEFINED OUTPUT)
    message(FATAL_ERROR "OUTPUT must be defined (path to output header)")
endif()

# Read binary file as hex string
file(READ "${INPUT}" ROM_HEX HEX)

# Convert hex pairs to C byte format: "1c22" -> "0x1c,0x22,"
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," ROM_BYTES "${ROM_HEX}")

# Write to temporary file
set(TEMP_FILE "${OUTPUT}.tmp")
file(WRITE "${TEMP_FILE}" "${ROM_BYTES}\n")

# Only update output if content changed (preserves timestamp for unchanged content)
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TEMP_FILE}" "${OUTPUT}")
file(REMOVE "${TEMP_FILE}")
