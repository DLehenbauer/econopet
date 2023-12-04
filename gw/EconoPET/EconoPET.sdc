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

# Helpers

# Calculate period in nanoseconds from frequency in megahertz.
proc ns_from_mhz { mhz } {
    set result [expr { 1000 / $mhz }]
    return $result
}

# PLL Constraints
set clock_mhz 64
create_clock -period [ns_from_mhz $clock_mhz] clock_i

# SPI1 Constraints

# SPI mode 0 (CPOL=0, CPHA=0) is a center-aligned source synchronous SDR interface.
# Clock is low when idle.  Data is sampled on rising edge and shifted out on falling edge.
#
#        /CS  ‾‾‾\_______________________________________________________________________/‾‾‾
#              . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : .  
#        SCK  ___________/‾‾‾\___/‾‾‾\___/‾‾‾\___/‾‾‾\___/‾‾‾\___/‾‾‾\___/‾‾‾\___/‾‾‾\_______
#              . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : .  
#         TX  -------<​_​̅_​̅_​̅7​̅_​̅_​̅_X_​̅_​̅_​̅6​̅_​̅_​̅_X_​̅_​̅_​̅5​̅_​̅_​̅_X_​̅_​̅_​̅4​̅_​̅_​̅_X_​̅_​̅_​̅3​̅_​̅_​̅_X_​̅_​̅_​̅2​̅_​̅_​̅_X_​̅_​̅_​̅1​̅_​̅_​̅_X_​̅_​̅_​̅0​̅_​̅_​̅_>-------
#              . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . 
#         RX  ----<​_​̅_​̅_​̅_​̅_​̅_​̅7​̅_​̅_​̅_X_​̅_​̅_​̅6​̅_​̅_​̅_X_​̅_​̅_​̅5​̅_​̅_​̅_X_​̅_​̅_​̅4​̅_​̅_​̅_X_​̅_​̅_​̅3​̅_​̅_​̅_X_​̅_​̅_​̅2​̅_​̅_​̅_X_​̅_​̅_​̅1​̅_​̅_​̅_X_​̅_​̅_​̅0​̅_​̅_​̅_​̅_​̅_​̅_​̅_>---
#              . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : .  
#      STALL  ________/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\___

# (See https://www.intel.com/content/dam/altera-www/global/en_US/pdfs/literature/an/an433.pdf)

set spi1_sck_period_mhz 24
set spi1_sck_period_ns [ns_from_mhz $spi1_sck_period_mhz]

# 'spi1_sck_v' is a virtual clock that models the edges at which TX and RX transition
create_clock -name spi1_sck_v -period $spi1_sck_period_ns

# 'spi1_sck_i' is the incoming SCK used to sample TX/RX between transitions.  Note that
# SCK is center-aligned (phase-shifted 90 degrees from 'spi1_sck_v'.)
create_clock -name "spi1_sck_i" -period $spi1_sck_period_ns -waveform [list [expr { $spi1_sck_period_ns * 0.25 }] [expr { $spi1_sck_period_ns * 0.75 }]] [get_ports spi1_sck_i]

# SPI sampling and data clocks are asynchronous/unrelated to other clocks in the design.
set_clock_groups -asynchronous -group {spi1_sck_v spi1_sck_i}

# Assume RX transitions within +/- 250ps of virtual data clock edge
set_input_delay -clock spi1_sck_v -min -0.250 [get_ports {spi1_sd_i}]
set_input_delay -clock spi1_sck_v -max  0.250 [get_ports {spi1_sd_i}]

# Assume the required TX data valid window is 1/2 the SCK period, centered on the sampling edge.
set_output_delay -clock spi1_sck_i -min [expr { $spi1_sck_period_ns * -0.25 }] [get_ports {spi1_sd_o}]
set_output_delay -clock spi1_sck_i -max [expr { $spi1_sck_period_ns *  0.25 }] [get_ports {spi1_sd_o}]

# Assume CS_N transitions centered on data clock
#
# Under hardware control CS_N asserts on what would be the rising SCK edge prior the first bit
# and deasserts on the rising SCK edge after the last bit (if SCK weren't disabled).
#
# Under software control, CS_N is unsynchronized, but generally delayed more than a clock period
# at 4 MHz or above.
set_input_delay -clock spi1_sck_v -min [expr { $spi1_sck_period_ns * -0.25 }] [get_ports {spi1_cs_ni}]
set_input_delay -clock spi1_sck_v -max [expr { $spi1_sck_period_ns *  0.25 }] [get_ports {spi1_cs_ni}]

# (See also generated file: outflow\PET.pt.sdc)
