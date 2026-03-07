# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

# Efinity Timing Closure User Guide
# https://www.efinixinc.com/docs/efinity-timing-closure-v7.4.pdf

# ============================================================================
# Board-Level Timing Budget
# ============================================================================

# Max PCB trace delay derived from first principles in common_pkg.sv:
#   250 mm / (299.79 / sqrt(4.5)) mm/ns = ~1.8 ns, rounded up to 2 ns.
set board_delay_max 2.0
set board_delay_min 0.0

# ============================================================================
# Clock Definitions
# ============================================================================

# Calculate period in nanoseconds from frequency in megahertz.
proc ns_from_mhz { freq_mhz } {
    return [expr { 1000.0 / $freq_mhz }]
}

# PLL output clock (64 MHz). Matches create_clock in outflow/EconoPET.pt.sdc.
set sys_freq_mhz 64
set sys_period_ns [ns_from_mhz $sys_freq_mhz]
create_clock -period $sys_period_ns -name sys_clock_i [get_ports {sys_clock_i}]

# SPI0 bus clock from MCU (24 MHz max). External pin, source synchronous.
set spi_freq_mhz 24
set spi_period_ns [ns_from_mhz $spi_freq_mhz]
create_clock -period $spi_period_ns -name spi0_sck_i [get_ports {spi0_sck_i}]

# Declare clock domains asynchronous (must list all groups explicitly).
set_clock_groups -asynchronous \
    -group {sys_clock_i} \
    -group {spi0_sck_i}

# ============================================================================
# SPI Constraints (spi0_sck_i domain)
# ============================================================================

# SPI mode 0 (CPOL=0, CPHA=0) is a center-aligned source synchronous SDR interface.
# Clock is low when idle. Data is sampled on rising edge and shifted out on falling edge.
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

# RX: MCU launches SDI on falling SCK edge, FPGA captures on rising SCK edge.
# Allow +/- 1/16th SCK period for edge-to-data skew.
set spi_skew [expr { $spi_period_ns * 0.0625 }]
set_input_delay  -clock spi0_sck_i -clock_fall -max  $spi_skew [get_ports {spi0_sd_i}]
set_input_delay  -clock spi0_sck_i -clock_fall -min -$spi_skew [get_ports {spi0_sd_i}]

# TX: FPGA shifts SDO on falling SCK edge, MCU captures on rising SCK edge.
set_output_delay -clock spi0_sck_i -clock_fall -max  $spi_skew [get_ports {spi0_sd_o}]
set_output_delay -clock spi0_sck_i -clock_fall -min -$spi_skew [get_ports {spi0_sd_o}]

# CS_N is used as an async reset in spi.sv and crossed via 2-FF synchronizer
# in spi1_controller.sv. No synchronous timing requirement.
set_false_path -from [get_ports {spi0_cs_ni}]

# ============================================================================
# CPU Bus Outputs (sys_clock_i domain)
# ============================================================================
#
# The CPU runs at ~1 MHz (one PHI2 cycle per 64 sys_clock_i cycles). All timing
# relationships between FPGA outputs and external components are multi-cycle,
# managed by the bus arbiter in timing.sv. The output delays below are derived
# from board trace delay only (no single-cycle external setup requirement).
#
# Component timing parameters (from datasheets):
#   W65C02S:  tDSR=15 ns, tDHR=10 ns, tAH=10 ns, tBVD=30 ns
#   SRAM:     tAS=0 ns, tAW=50 ns, tWP=45 ns, tDW=25 ns, tDH=0 ns
#   PIA:      tACR=8 ns, tCAR=0 ns, tDCW=10 ns, tHW=5 ns
#   VIA:      tACR=10 ns, tCAR=10 ns, tDCW=10 ns, tHW=10 ns
#
# These requirements are satisfied by multi-cycle operation (62.5-93.75 ns per
# bus slot at 64 MHz), not by single-cycle output delay constraints. The
# set_output_delay values here ensure clean same-cycle output transitions.

# CPU control signals (directly registered in timing.sv).
set_output_delay -clock sys_clock_i -max $board_delay_max [get_ports {cpu_clock_o}]
set_output_delay -clock sys_clock_i -min 0                [get_ports {cpu_clock_o}]
set_output_delay -clock sys_clock_i -max $board_delay_max [get_ports {cpu_be_o}]
set_output_delay -clock sys_clock_i -min 0                [get_ports {cpu_be_o}]
set_output_delay -clock sys_clock_i -max $board_delay_max [get_ports {cpu_ready_o}]
set_output_delay -clock sys_clock_i -min 0                [get_ports {cpu_ready_o}]

# SRAM address and data (active during FPGA bus-master slots).
set_output_delay -clock sys_clock_i -max $board_delay_max [get_ports {ram_addr_a*_o cpu_addr_o[*]}]
set_output_delay -clock sys_clock_i -min 0                [get_ports {ram_addr_a*_o cpu_addr_o[*]}]
set_output_delay -clock sys_clock_i -max $board_delay_max [get_ports {cpu_data_o[*]}]
set_output_delay -clock sys_clock_i -min 0                [get_ports {cpu_data_o[*]}]

# SRAM control: OE and WE must be tightly aligned with address/data outputs.
# The bus arbiter guarantees tAS (0 ns) and tDW (25 ns) via multi-cycle state machine.
set_output_delay -clock sys_clock_i -max $board_delay_max [get_ports {ram_oe_n_o}]
set_output_delay -clock sys_clock_i -min 0                [get_ports {ram_oe_n_o}]
set_output_delay -clock sys_clock_i -max $board_delay_max [get_ports {ram_we_n_o}]
set_output_delay -clock sys_clock_i -min 0                [get_ports {ram_we_n_o}]

# I/O chip selects and transceiver enable (active during CPU bus slots only).
set_output_delay -clock sys_clock_i -max $board_delay_max [get_ports {io_oe_n_o}]
set_output_delay -clock sys_clock_i -min 0                [get_ports {io_oe_n_o}]
set_output_delay -clock sys_clock_i -max $board_delay_max [get_ports {pia1_cs_n_o}]
set_output_delay -clock sys_clock_i -min 0                [get_ports {pia1_cs_n_o}]
set_output_delay -clock sys_clock_i -max $board_delay_max [get_ports {pia2_cs_n_o}]
set_output_delay -clock sys_clock_i -min 0                [get_ports {pia2_cs_n_o}]
set_output_delay -clock sys_clock_i -max $board_delay_max [get_ports {via_cs_n_o}]
set_output_delay -clock sys_clock_i -min 0                [get_ports {via_cs_n_o}]

# Tristate OE signals (Efinix separate _oe ports) control pad direction. The bus
# arbiter toggles cpu_be_o multiple sys_clock_i cycles before data transitions,
# so OE changes have a multi-cycle budget. False path avoids over-constraining
# these long combinational paths (cpu_be_o -> address decode -> OE).
set_false_path -to [get_ports {cpu_addr_oe[*] cpu_data_oe[*]}]

# ============================================================================
# Video and Audio Outputs (sys_clock_i domain)
# ============================================================================
#
# Video signals feed a CRT monitor. Audio feeds an amplified speaker.
# All are registered or driven through a delay pipeline in the sys_clock_i
# domain. Timing is not critical at audio and display refresh rates.

set_output_delay -clock sys_clock_i -max $board_delay_max [get_ports {h_sync_o}]
set_output_delay -clock sys_clock_i -min 0                [get_ports {h_sync_o}]
set_output_delay -clock sys_clock_i -max $board_delay_max [get_ports {v_sync_o}]
set_output_delay -clock sys_clock_i -min 0                [get_ports {v_sync_o}]
set_output_delay -clock sys_clock_i -max $board_delay_max [get_ports {video_o}]
set_output_delay -clock sys_clock_i -min 0                [get_ports {video_o}]
set_output_delay -clock sys_clock_i -max $board_delay_max [get_ports {audio_o}]
set_output_delay -clock sys_clock_i -min 0                [get_ports {audio_o}]

# ============================================================================
# MCU-Facing Outputs (sys_clock_i domain)
# ============================================================================

# SPI stall (backpressure). MCU polls this before starting a new transaction.
set_output_delay -clock sys_clock_i -max $board_delay_max [get_ports {spi_stall_o}]
set_output_delay -clock sys_clock_i -min 0                [get_ports {spi_stall_o}]

# ============================================================================
# Open-Drain / Constant CPU Control Outputs
# ============================================================================
#
# cpu_reset_n, cpu_irq_n, cpu_nmi_n: active-low open-drain signals driven via
# OE. The _o port is tied to 0 (constant), the _oe port controls assertion.
# cpu_we_n_o is tied low (constant), cpu_we_n_oe is driven.
# These are quasi-static or constant. False path is appropriate.

set_false_path -to [get_ports {cpu_reset_n_o cpu_irq_n_o cpu_nmi_n_o}]
set_false_path -to [get_ports {cpu_we_n_o}]

set_output_delay -clock sys_clock_i -max $board_delay_max [get_ports {cpu_reset_n_oe cpu_nmi_n_oe cpu_irq_n_oe}]
set_output_delay -clock sys_clock_i -min 0                [get_ports {cpu_reset_n_oe cpu_nmi_n_oe cpu_irq_n_oe}]
set_output_delay -clock sys_clock_i -max $board_delay_max [get_ports {cpu_we_n_oe}]
set_output_delay -clock sys_clock_i -min 0                [get_ports {cpu_we_n_oe}]

# ============================================================================
# Debug / Status / Unused Outputs
# ============================================================================
#
# These outputs have no external timing requirement: constant values, debug
# pass-through, or unused ports. False path prevents unnecessary optimization.

set_false_path -to [get_ports {status_no}]
set_false_path -to [get_ports {spare_o[*]}]
set_false_path -to [get_ports {spi1_sd_o}]
set_false_path -to [get_ports {pmod1_o[*] pmod1_oe[*] pmod2_o[*] pmod2_oe[*]}]

# ============================================================================
# CPU Bus Inputs (pad to sys_clock_i domain)
# ============================================================================
#
# cpu_addr_i, cpu_data_i, cpu_we_n_i, cpu_sync_i: sampled during CPU bus slots
# (slots 6-7 of the 64-cycle frame). The FPGA generates PHI2 (cpu_clock_o) and
# waits multiple sys_clock_i cycles before sampling, providing a multi-cycle
# timing budget. Address and data are stable well before the FPGA's sampling
# point at the CPU_DATA_STROBE cycle.
#
# False path is correct because there is no meaningful single-cycle setup/hold
# relationship between these inputs and sys_clock_i. The bus protocol (not the
# SDC) guarantees data validity.

set_false_path -from [get_ports {cpu_addr_i[*]}]
set_false_path -from [get_ports {cpu_data_i[*]}]
set_false_path -from [get_ports {cpu_we_n_i}]
set_false_path -from [get_ports {cpu_sync_i}]

# ============================================================================
# Asynchronous / Quasi-Static Inputs
# ============================================================================
#
# DIP switches, active-low open-drain CPU signals, and VIA-driven audio/graphic
# signals. These are either static (change only at power-on) or asynchronous to
# sys_clock_i. The RTL uses level-sensitive sampling gated by bus slot enables,
# not edge-sensitive synchronizers.

set_false_path -from [get_ports {config_crt_i config_keyboard_i}]
set_false_path -from [get_ports {graphic_i}]
set_false_path -from [get_ports {diag_i via_cb2_i}]
set_false_path -from [get_ports {cpu_reset_n_i}]

# pmod2_i is passed through combinationally to pmod1_o (debug loopback).
# No synchronous timing requirement.
set_false_path -from [get_ports {pmod2_i[*]}]
