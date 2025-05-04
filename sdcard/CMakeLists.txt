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

set(CMAKE_VERBOSE_MAKEFILE ON)
cmake_minimum_required(VERSION 3.13.1)
include(FetchContent)

project(roms)

set(CMAKE_TLS_VERIFY true)
set(sdcard_download_dir ${CMAKE_CURRENT_BINARY_DIR}/download)
set(sdcard_root_dir ${CMAKE_CURRENT_BINARY_DIR}/sdcard_root)
set(sdcard_rom_dir ${sdcard_root_dir}/roms)

function(download_rom filename hash destination_filename)
    set(base_url "http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/")
    set(full_url "${base_url}${filename}")
    set(content_id "download-${filename}-${hash}")

    FetchContent_Declare(${content_id}
        URL ${full_url}
        URL_HASH MD5=${hash}
        DOWNLOAD_DIR ${sdcard_download_dir}
        DOWNLOAD_NO_EXTRACT true
    )

    list(APPEND rom_downloads ${content_id})
    set(rom_downloads ${rom_downloads} PARENT_SCOPE)

    set(rom_file "${sdcard_download_dir}/${filename}")
    set(destination_file "${sdcard_rom_dir}/${destination_filename}")

    list(APPEND binplace_ops "${rom_file}|${destination_file}")
    set(binplace_ops ${binplace_ops} PARENT_SCOPE)
endfunction()

# http://www.zimmers.net/anonftp/pub/cbm/firmware/ALLFILES.html
# http://penguincentral.com/retrocomputing/PET/petroms.pdf
# https://loadrun64.com/zimmers/firmware/computers/pet/Commodore%20ROM%20Genealogy.pdf

#
# BASIC 1
#

# PET 2001 Basic 1    (H1) [6540-011] (same as 901447-01)
download_rom(rom-1-c000.901439-01.bin B45778BDC95D67CCB475008718F4466B 901439-01.bin)
# PET 2001 Basic 1/2  (H2) [6540-013] (same as 901447-03)
download_rom(rom-1-d000.901439-02.bin E13B5675386BEB93E8397DF891113F5B 901439-02.bin)
# PET 2001 Edit 1/2   (H3) [6540-015] (same as 901447-05)
download_rom(rom-1-e000.901439-03.bin EF9BD0E62DFC47EB463FEF20D0344826 901439-03.bin)
# PET 2001 Kernal 1/2 (H4) [6540-016] (same as 901447-06)
download_rom(rom-1-f000.901439-04.bin 2D44AFCAB9713AD4D11118A9E7429A81 901439-04.bin)
# PET 2001 Basic 1/2  (H5) [6540-012] (same as 901447-02)
download_rom(rom-1-c800.901439-05.bin D03EE896C37AD1BD86773655948E1174 901439-05.bin)
# PET 2001 Basic 1/2  (H6) [6540-014] (same as 901447-04)
download_rom(rom-1-d800.901439-06.bin 673B61A8FE11CD5D7E4003F81A2D8453 901439-06.bin)
# PET 2001 Kernal 1/2 (H7) [6540-018] (same as 901447-07)
download_rom(rom-1-f800.901439-07.bin 9E1357C33C0D5A4A89B263F0C3C317FF 901439-07.bin)
# PET 2001 Chargen 1/2 (F10, A2) [6540-010] (same as 901439-08)
download_rom(characters-1.901447-08.bin 29A82EB54E73EBC5673C718C489B174B 901447-08.bin)

#
# BASIC 2
#

# PET 2001 Basic 2  (H1) [6540-019] (same as 901447-09)
download_rom(rom-2-c000.901439-09.bin 9EAF58AA938083AD74117A81AC460E37 901439-09.bin)

#
# BASIC 3
#

# CBM 30xx Basic 3      (D6) [2332-007]
download_rom(basic-2-c000.901465-01.bin 6B13EB6A7E4A2E15F3FFF18461CE9C0D 901465-01.bin)
# CBM 30xx Basic 3      (D7) [2332-008]
download_rom(basic-2-d000.901465-02.bin 8B7779F1AFE3A6542BD17F6B80917D67 901465-02.bin)
# CBM 30xx Edit 3 40 B NoCRTC (D8) [2316-024]
download_rom(edit-2-b.901474-01.bin 7F87889CA7EE2537F0C1993D35D0FB18 901474-01.bin)
# CBM 30xx Edit 3 40 N NoCRTC (D8; H3) [2316-011] [6540-024] (same as 901439-17)
download_rom(edit-2-n.901447-24.bin CB8E8404C0B28EDA10469792DFD1DBC2 901447-24.bin)
# CBM 30xx Kernal PET Kernal 3  (D9) [2332-009]
download_rom(kernal-2.901465-03.bin 51A38BFEF8F9E72CB64BF7D874B4C8C6 901465-03.bin)
# CBM 30xx Chargen 3/4 (F10) [2316-004]
download_rom(characters-2.901447-10.bin 9880432E633B15998D58884FF34C4E70 901447-10.bin)

#
# BASIC 4
#

# CBM 40xx Basic 4  (D5; UD10)
download_rom(basic-4-b000.901465-19.bin 34D6650ACD5DC4A4049F0C189AB5EEB0 901465-19.bin)
# CBM 40xx Basic 4  (D6; UD9) [6332-059]
download_rom(basic-4-c000.901465-20.bin 398217F35FA50417C7E84883A93A349B 901465-20.bin)
# CBM 40xx Basic 4  (D7; UD8) [6332-096]
download_rom(basic-4-d000.901465-21.bin AB780E94772DCA756A0678A17B5BC3A2 901465-21.bin)

# CBM 40xx Edit 4 40 B NoCRTC (D8) [6316-035]
download_rom(edit-4-b.901474-02.bin A09D11163A708B8DEA90F1C5DF33DCA0 901474-02.bin)
# CBM 40xx Edit 4 40 N NoCRTC (D8) [6316-034]
download_rom(edit-4-n.901447-29.bin 6FE27B43EC550A04D30B2E45F07D51FB 901447-29.bin)
# CBM 40xx Edit 4 40 N CRTC 50Hz (UD7)
download_rom(edit-4-40-n-50Hz.901498-01.bin B76D756E7AC8752AE0035F3CE5F1383C 901498-01.bin)
# CBM 40xx Edit 4 40 N CRTC 60Hz (UD7)
download_rom(edit-4-40-n-60Hz.901499-01.bin 2E86403FC2AC30E7AF05B9E8607BEF98 901499-01.bin)
# CBM 80xx Edit 4 80 B CRTC 50Hz (UD7) [2316-059]
download_rom(edit-4-80-b-50Hz.901474-04-3681.bin 3E646194DE7458B05A06159AFBDD9427 901474-04.bin)
# CBM 80xx Edit 4 80 B CRTC 60Hz (UD7) [2316-041]
download_rom(edit-4-80-b-60Hz.901474-03.bin DA56995BE008C5F7DB1094E81E5060AA 901474-03.bin)

# CBM 40xx Kernal PET Kernal 4  (D9; UD6) [6332-075]
download_rom(kernal-4.901465-22.bin 16EC21443EA5431AB63D511061054E6F 901465-22.bin)

#
# BASIC 4.1
#

# CBM 40xx Basic 4.1  (D5; UD10) [6332-120]
download_rom(basic-4-b000.901465-23.bin 43B3A9F5E1C762AF0B3BB6CC71AAFB84 901465-23.bin)

# Ensure all downloads are complete
FetchContent_MakeAvailable(${rom_downloads})

file(MAKE_DIRECTORY ${sdcard_rom_dir})

# Perform the renames
foreach(pair IN LISTS binplace_ops)
    # Split the semicolon-delimited pair into a list
    string(REPLACE "|" ";" split_pair ${pair})
    
    # Extract the individual parts of the pair
    list(GET split_pair 0 downloaded_file)
    list(GET split_pair 1 destination_file)

    if (EXISTS ${downloaded_file})
        # Ensure the destination directory exists
        get_filename_component(destination_dir ${destination_file} DIRECTORY)
        file(MAKE_DIRECTORY ${destination_dir})

        # Copy the file to the destination directory
        file(COPY ${downloaded_file} DESTINATION ${destination_dir})

        # Rename the copied file to the desired name
        get_filename_component(source_filename ${downloaded_file} NAME)
        file(RENAME ${destination_dir}/${source_filename} ${destination_file})
    else()
        message(FATAL_ERROR "File not found: ${downloaded_file}")
    endif()
endforeach()

# Copy config.yaml to the sdcard_root directory
file(COPY ${CMAKE_CURRENT_LIST_DIR}/config.yaml DESTINATION ${sdcard_root_dir})
