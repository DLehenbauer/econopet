cmake_minimum_required(VERSION 3.13)

set(FAMILY rp2040)
set(BOARD pico_sdk)

add_executable(firmware
    main.c
    driver.c
    test/mem.c
)

target_precompile_headers(firmware PRIVATE pch.h)

# pull in common dependencies
target_link_libraries(
    firmware
    pico_stdlib
    pico_multicore
    pico_util
    hardware_pwm
    hardware_spi
)

# create map/bin/hex/uf2 file etc.
pico_add_extra_outputs(firmware)
