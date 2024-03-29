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

# Efinity Timing Closure User Guide
# https://www.efinixinc.com/docs/efinity-timing-closure-v5.0.pdf
#
# reset_timing; delete_timing_results; read_sdc; report_timing_summary
# report_timing -from_clock clock_i -to_clock clock_i -setup
# report_timing -from_clock clock_i -to_clock clock_i -hold

# Helpers

# Calculate period in nanoseconds from frequency in megahertz.
proc ns_from_mhz { mhz } {
    set result [expr { 1000 / $mhz }]
    return $result
}

# PLL Constraints

set clock_mhz 64
set clock_period [ns_from_mhz $clock_mhz]
create_clock -period $clock_period clock_i

# SPI1 Constraints

# SPI mode 0 (CPOL=0, CPHA=0) is a center-aligned source synchronous SDR interface.
# Clock is low when idle.  Data is sampled on rising edge and shifted out on falling edge.
#
#        /CS  ‾‾‾\_______________________________________________________________________/‾‾‾
#              . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : .  
#        SCK  ___________/‾‾‾\___/‾‾‾\___/‾‾‾\___/‾‾‾\___/‾‾‾\___/‾‾‾\___/‾‾‾\___/‾‾‾\_______
#              . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . 
#        SDI  -------<​_​̅_​̅_​̅7​̅_​̅_​̅_X_​̅_​̅_​̅6​̅_​̅_​̅_X_​̅_​̅_​̅5​̅_​̅_​̅_X_​̅_​̅_​̅4​̅_​̅_​̅_X_​̅_​̅_​̅3​̅_​̅_​̅_X_​̅_​̅_​̅2​̅_​̅_​̅_X_​̅_​̅_​̅1​̅_​̅_​̅_X_​̅_​̅_​̅0​̅_​̅_​̅_>-------
#              . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : .  
#        SDO  -------<​_​̅_​̅_​̅7​̅_​̅_​̅_X_​̅_​̅_​̅6​̅_​̅_​̅_X_​̅_​̅_​̅5​̅_​̅_​̅_X_​̅_​̅_​̅4​̅_​̅_​̅_X_​̅_​̅_​̅3​̅_​̅_​̅_X_​̅_​̅_​̅2​̅_​̅_​̅_X_​̅_​̅_​̅1​̅_​̅_​̅_X_​̅_​̅_​̅0​̅_​̅_​̅_>-------
#              . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : . : .  
#      STALL  ________/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\___

# (See https://www.intel.com/content/dam/altera-www/global/en_US/pdfs/literature/an/an433.pdf)

set spi1_sck_period_mhz 24
set spi1_sck_period_ns [ns_from_mhz $spi1_sck_period_mhz]

# 'spi1_sck_i' is the incoming SCK used to sample TX/RX between transitions.
create_clock -name spi1_sck_i -period $spi1_sck_period_ns [get_ports spi1_sck_i]

# SPI sampling and data clocks are asynchronous/unrelated to other clocks in the design.
set_clock_groups -asynchronous -group { spi1_sck_i }

# SDO/SDI are sampled on the rising edge of SCK and transition on the falling edge of SCK.

# We assume incoming transitions may be skewed +/- 1/8th the SCK period.
set_input_delay  -clock spi1_sck_i -clock_fall -min [expr { $spi1_sck_period_ns * -0.0625 }] [get_ports {spi1_sd_i}]
set_input_delay  -clock spi1_sck_i -clock_fall -max [expr { $spi1_sck_period_ns *  0.0625 }] [get_ports {spi1_sd_i}]

# We allow outgoing transitions to be skewed by +/- 1/8th the SCK period.
set_output_delay -clock spi1_sck_i -clock_fall -min [expr { $spi1_sck_period_ns * -0.0625 }] [get_ports {spi1_sd_o}]
set_output_delay -clock spi1_sck_i -clock_fall -max [expr { $spi1_sck_period_ns *  0.0625 }] [get_ports {spi1_sd_o}]

# Assume CS_N transitions centered on data clock
#
# Under hardware control CS_N asserts on what would be the rising SCK edge prior the first bit
# and deasserts on the rising SCK edge after the last bit (if SCK weren't disabled).
#
# Under software control, CS_N is unsynchronized, but generally delayed more than an SCK period
# at 4 MHz or above.
set_false_path -from [get_ports spi1_cs_ni]

# (See also generated file: outflow\PET.pt.sdc)
