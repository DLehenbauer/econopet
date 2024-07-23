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

function(download_rom url hash)
    FetchContent_Declare("download-${hash}"
        URL ${url}
        URL_HASH MD5=${hash}
        DOWNLOAD_DIR ${CMAKE_CURRENT_BINARY_DIR}/bin/roms
        DOWNLOAD_NO_EXTRACT true
    )

    list(APPEND rom_downloads "download-${hash}")
    set (rom_downloads ${rom_downloads} PARENT_SCOPE)
endfunction()

# http://www.zimmers.net/anonftp/pub/cbm/firmware/ALLFILES.html
# http://penguincentral.com/retrocomputing/PET/petroms.pdf

# US Character Set
download_rom(http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/characters-2.901447-10.bin 9880432E633B15998D58884FF34C4E70)

# BASIC 4.0
download_rom(http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/basic-4-b000.901465-23.bin 43B3A9F5E1C762AF0B3BB6CC71AAFB84)
download_rom(http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/basic-4-c000.901465-20.bin 398217F35FA50417C7E84883A93A349B)
download_rom(http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/basic-4-d000.901465-21.bin AB780E94772DCA756A0678A17B5BC3A2)

# Edit 4.0, 40 column, Graphics Keyboard, no CRTC
download_rom(http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-4-n.901447-29.bin 6FE27B43EC550A04D30B2E45F07D51FB)

# Edit 4.0, 40 column, Graphics Keyboard, 60 Hz, CRTC
download_rom(http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-4-40-n-60Hz.901499-01.bin 2E86403FC2AC30E7AF05B9E8607BEF98)

# Edit 4.0, 40 column, Graphics Keyboard, 50 Hz, CRTC
download_rom(http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-4-40-n-50Hz.901498-01.bin B76D756E7AC8752AE0035F3CE5F1383C)

# Edit 4.0, 80 column, Business Keyboard, 60 Hz, CRTC
download_rom(http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-4-80-b-60Hz.901474-03.bin DA56995BE008C5F7DB1094E81E5060AA)

# Kernal 4.0
download_rom(http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/kernal-4.901465-22.bin 16EC21443EA5431AB63D511061054E6F)

FetchContent_MakeAvailable(${rom_downloads})
