# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

# Program selected RP2040 flash images through a CMSIS-DAP probe over SWD.
# Select at least one input path: BOOTLOADER_ELF, FIRMWARE_ELF or FPGA_BITSTREAM.
# With FPGA_BITSTREAM, also supply the FPGA_IMAGE output path for
# PrepareFpgaImage.cmake. ELF inputs retain their embedded load addresses.
# Each invocation checks the connected board, not a local file-change cache.
# Matching images skip writes. Successful completion resets and runs the MCU,
# even if no writes were needed. Invalid inputs or unrecovered probe failures
# stop the calling build.
if(NOT DEFINED BOOTLOADER_ELF AND NOT DEFINED FIRMWARE_ELF AND NOT DEFINED FPGA_BITSTREAM)
    message(FATAL_ERROR "A bootloader ELF, firmware ELF or FPGA bitstream must be selected")
endif()

# Reject missing, directory or empty inputs before connecting to the board.
# The loop entries are variable names, not filenames.
foreach(input BOOTLOADER_ELF FIRMWARE_ELF FPGA_BITSTREAM)
    if(DEFINED ${input})
        if(NOT EXISTS "${${input}}" OR IS_DIRECTORY "${${input}}")
            message(FATAL_ERROR "Missing ${input}: ${${input}}")
        endif()
        file(SIZE "${${input}}" input_size)
        if(input_size EQUAL 0)
            message(FATAL_ERROR "Empty ${input}: ${${input}}")
        endif()
    endif()
endforeach()

# Wrap the raw bitstream in a bounded image and record its deployment address.
if(DEFINED FPGA_BITSTREAM)
    include("${CMAKE_CURRENT_LIST_DIR}/PrepareFpgaImage.cmake")
endif()

# Build one Tcl command that checks flash and programs/verifies on check failure.
# path: image filename (spaces and semicolons are supported, braces are rejected).
# offset: absolute XIP load address for a raw binary, or empty for ELF addresses.
# output: caller-scope variable receiving the command, escaped for a CMake list.
function(program_command path offset output)
    # OpenOCD's program helper encloses filenames in Tcl braces internally.
    if("${path}" MATCHES "[{}]")
        message(FATAL_ERROR "OpenOCD program paths must not contain braces: ${path}")
    endif()

    # Raw binaries need an explicit address and type, while ELF files describe both.
    set(check_options)
    if(NOT "${offset}" STREQUAL "")
        set(check_options "${offset} bin")
    endif()

    # Check immediately before writing. Any check error falls through to programming,
    # whose verification must succeed before the session can continue.
    string(CONCAT command
        "if {[catch {verify_image_checksum {${path}} ${check_options}}]} {"
        "program {${path}} ${offset} verify"
        "} else {echo {Skipping unchanged image: ${path}}}")

    # Preserve literal filename semicolons when expanding the OpenOCD argument list.
    string(REPLACE ";" "\\;" command "${command}")

    # Return the command to the script assembling the selected image operations.
    set(${output} "${command}" PARENT_SCOPE)
endfunction()

# Halt the MCU before checking flash or programming selected images.
set(program_args -c "reset init")

# Prepare the gateware operation first, using its flash partition's absolute address.
if(DEFINED FPGA_BITSTREAM)
    program_command("${FPGA_IMAGE}" "${ECONOPET_FPGA_ADDRESS}" command)
    list(APPEND program_args -c "${command}")
endif()

# Check and program bootloader before firmware. A bootloader write erases
# firmware vectors, so recheck firmware afterward even if its ELF is unchanged.
foreach(input BOOTLOADER_ELF FIRMWARE_ELF)
    if(DEFINED ${input})
        program_command("${${input}}" "" command)
        list(APPEND program_args -c "${command}")
    endif()
endforeach()

# Run the application only after every selected image is checked or programmed.
# This also executes when all images matched and no programming was necessary.
list(APPEND program_args -c "poll off" -c "reset run" -c "shutdown")

# Locate OpenOCD and select the CMSIS-DAP probe and RP2040 target configuration.
find_program(OPENOCD_EXECUTABLE NAMES openocd REQUIRED)
set(probe_args -f interface/cmsis-dap.cfg -f target/rp2040.cfg)

# Bootloader-only writes can invalidate firmware without restoring it in this session.
if(DEFINED BOOTLOADER_ELF AND NOT DEFINED FIRMWARE_ELF)
    message(WARNING "Programming a changed bootloader erases the firmware vector table. Reinstall firmware afterward.")
endif()

# Attempt the ordered operations in one session using normal probe transfers.
execute_process(
    COMMAND "${OPENOCD_EXECUTABLE}" ${probe_args}
        -c "adapter speed 20833" -c "init" ${program_args}
    RESULT_VARIABLE program_result)

# Retry once with recovery settings. Recheck flash so completed writes can be skipped
# and firmware erased by a partial bootloader update can still be restored.
if(NOT "${program_result}" STREQUAL "0")
    execute_process(
        COMMAND "${OPENOCD_EXECUTABLE}" ${probe_args}
            -c "adapter speed 5000" -c "init" -c "cmsis-dap quirk enable" ${program_args}
        RESULT_VARIABLE program_result)

    # Propagate an unrecovered failure rather than report successful deployment.
    if(NOT "${program_result}" STREQUAL "0")
        message(FATAL_ERROR "Normal programming and serialized CMSIS-DAP recovery both failed: ${program_result}")
    endif()

    # Distinguish recovered probe failures from a successful first attempt.
    message(WARNING "Normal programming failed, but serialized CMSIS-DAP recovery succeeded.")
endif()

# Remind the user that a pending SD-card update can supersede these images on reset.
message(NOTICE "\nNote: If the SD-card contains 'firmware.uf2', the bootloader will replace SWD-programmed images on reset.\n")
