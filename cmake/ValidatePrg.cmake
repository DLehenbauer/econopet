# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

# Require a PRG header and nonempty payload loading in [min_address, end_address).
# Bounds accept decimal or hexadecimal values within the 16-bit address space.
# This checks the loaded bytes, not a BASIC launcher or linker-only segments such as BSS.
function(validate_prg prg_file min_address end_address)
    # Normalize the caller's bounds before comparing them with decoded addresses.
    math(EXPR min_load_address "${min_address}")
    math(EXPR load_limit "${end_address}")
    math(EXPR address_space_end "0x10000")
    if(min_load_address LESS 0 OR min_load_address GREATER_EQUAL load_limit OR load_limit GREATER address_space_end)
        message(FATAL_ERROR "Invalid PRG load bounds: ${min_address}..${end_address}")
    endif()

    # Reject missing files and incomplete load headers or payloads before decoding.
    if(NOT EXISTS "${prg_file}" OR IS_DIRECTORY "${prg_file}")
        message(FATAL_ERROR "Required PRG file is missing or is a directory: ${prg_file}")
    endif()
    set(header_size 2)
    file(SIZE "${prg_file}" prg_size)
    if(prg_size LESS_EQUAL header_size)
        message(FATAL_ERROR "PRG is missing its load address or payload: ${prg_file}")
    endif()

    # Decode the little-endian load address and calculate the exclusive payload end.
    file(READ "${prg_file}" header HEX LIMIT ${header_size})
    string(SUBSTRING "${header}" 0 2 load_address_low)
    string(SUBSTRING "${header}" 2 2 load_address_high)
    math(EXPR load_address "0x${load_address_high}${load_address_low}")
    math(EXPR load_end "${load_address} + ${prg_size} - ${header_size}")
    math(EXPR last_byte "${load_end} - 1")

    # Reject both low-address intrusion and payloads extending past the allowed window.
    if(load_address LESS min_load_address OR load_end GREATER load_limit)
        math(EXPR allowed_last_byte "${load_limit} - 1")
        message(FATAL_ERROR
            "PRG does not fit in ${min_load_address}..${allowed_last_byte}: "
            "${prg_file} loads ${load_address}..${last_byte}")
    endif()
endfunction()
