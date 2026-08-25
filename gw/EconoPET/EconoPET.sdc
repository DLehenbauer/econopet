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

# The soft-core clocks are short pulses from timing.sv's 64-cycle arbiter, not
# 50%-duty clock divisions. Cycle numbers below are the arbiter's cycle_count
# value matched by the registered assignment that drives each edge; timing.sv
# asserts them against its own constants, so keep the two in sync.
set sys_cycles_per_frame 64

# cycle_count initializes to 63. The first sys_clock_i rising edge processes
# cycle 63, the second processes cycle 0, and arbiter cycle N therefore occurs
# on source clock edge 2N+3.
proc arbiter_clock_edges { rise fall } {
    global sys_cycles_per_frame
    return [list [expr { 2 * $rise + 3 }] \
                 [expr { 2 * $fall + 3 }] \
                 [expr { 2 * ($rise + $sys_cycles_per_frame) + 3 }]]
}

#                                                              rise fall
create_generated_clock -name cpu_phi -source [get_ports {sys_clock_i}] \
    -edges [arbiter_clock_edges 52 61] [get_nets {main/soft_cpu_clock}]
create_generated_clock -name e6809 -source [get_ports {sys_clock_i}] \
    -edges [arbiter_clock_edges 53 61] [get_nets {main/cpu6809_e}]
create_generated_clock -name q6809 -source [get_ports {sys_clock_i}] \
    -edges [arbiter_clock_edges 51 60] [get_nets {main/cpu6809_q}]

# cpu_data_strobe_q is a one-cycle pulse that clocks soft_cpu_data_q at arbiter
# cycle 56. Model that real launch clock so STA derives the five-cycle setup
# and full-frame hold relationships without a data-path exception.
create_generated_clock -name soft_cpu_data_capture -source [get_ports {sys_clock_i}] \
    -edges [arbiter_clock_edges 56 57] [get_nets {main/cpu_data_strobe_q}]

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

set_output_delay -clock sys_clock_i -max $board_delay_max [get_ports {horiz_drive_o}]
set_output_delay -clock sys_clock_i -min 0                [get_ports {horiz_drive_o}]
set_output_delay -clock sys_clock_i -max $board_delay_max [get_ports {vert_drive_o}]
set_output_delay -clock sys_clock_i -min 0                [get_ports {vert_drive_o}]
set_output_delay -clock sys_clock_i -max $board_delay_max [get_ports {jiffy_clock_o}]
set_output_delay -clock sys_clock_i -min 0                [get_ports {jiffy_clock_o}]
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
# cpu_reset_n, cpu_irq_n, cpu_nmi_n: active-low open-drain signals. The _o
# data port is tied to 0 (constant), the _oe port controls assertion.
#
# Constant outputs (tied off in RTL, confirmed in mapped netlist):
#   cpu_reset_n_o, cpu_irq_n_o, cpu_nmi_n_o = 0
#   cpu_we_n_o = 1
#   cpu_irq_n_oe = 0  (IRQ never driven by FPGA)
#   cpu_we_n_oe = 0   (CPU always drives RWB)

set_false_path -to [get_ports {cpu_reset_n_o cpu_irq_n_o cpu_nmi_n_o}]
set_false_path -to [get_ports {cpu_we_n_o}]
set_false_path -to [get_ports {cpu_irq_n_oe cpu_we_n_oe}]

# Quasi-static OE outputs (written by MCU via SPI register file, change only
# during initialization). Board-delay constraint provides a basic fabric check.
set_output_delay -clock sys_clock_i -max $board_delay_max [get_ports {cpu_reset_n_oe cpu_nmi_n_oe}]
set_output_delay -clock sys_clock_i -min 0                [get_ports {cpu_reset_n_oe cpu_nmi_n_oe}]

# ============================================================================
# Debug / Status / Unused Outputs
# ============================================================================
#
# These outputs have no external timing requirement: constant values, debug
# pass-through, or unused ports. False path prevents unnecessary optimization.

set_false_path -to [get_ports {status_no}]
set_false_path -to [get_ports {sp1_o sp2_o sp3_o sp4_o sp5_o sp6_o sp7_o sp8_o}]
set_false_path -to [get_ports {sp1_oe sp2_oe sp3_oe sp4_oe sp5_oe sp6_oe sp7_oe sp8_oe}]
set_false_path -to [get_ports {spi1_sd_o}]
set_false_path -to [get_ports {pmod1_o[*] pmod1_oe[*] pmod2_o[*] pmod2_oe[*]}]

# The following pins are unused in the current design and optimized away by
# synthesis. Constraints are omitted to avoid warnings. If these ports gain
# fanout in a future revision, add false-path constraints here.
#
# set_false_path -from [get_ports {sp1_i sp2_i sp3_i sp4_i sp5_i sp6_i sp7_i sp8_i}]
# set_false_path -from [get_ports {spi1_cs_ni spi1_sck_i spi1_sd_i}]
# set_false_path -from [get_ports {pmod1_i[*]}]
# set_false_path -from [get_ports {audio_det_n_i}]

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
set_false_path -from [get_ports {cpu_we_n_i}]
set_false_path -from [get_ports {cpu_sync_i}]

# cpu_data_i has a multi-cycle bus-valid window, but a false path would let P&R
# stretch the pin-to-FF route without limit. cpu_data_strobe is registered, so
# data guaranteed valid at that phase must reach cpu_data_q by the following
# sys_clock_i edge. Applying the one-period bound to every data-input endpoint
# is conservative for the other raw-pin consumers.
set_max_delay -from [get_ports {cpu_data_i[*]}] $sys_period_ns

# Other crossings are control/status signals with generous protocol budgets.
# The cores' internal paths retain their microsecond clock period.
set_max_delay -from [get_clocks {cpu_phi}] -to [get_clocks {sys_clock_i}] 40.0
set_max_delay -from [get_clocks {e6809}] -to [get_clocks {sys_clock_i}] 40.0
set_max_delay -from [get_clocks {q6809}] -to [get_clocks {sys_clock_i}] 40.0

# READY and interrupt controls are edge-elastic: accepting either level when a
# synchronized control changes on the same source edge is valid. The 6809's
# IRQSample register also provides the first Q-domain synchronizer stage.
set_multicycle_path -hold 1 -start \
    -from [get_cells {main/bp_halted* main/reg_cpu_ready*}] \
    -to [get_clocks {cpu_phi}]
set_multicycle_path -hold 1 -start \
    -from [get_cells {main/cpu_irq_sync*}] \
    -to [get_clocks {cpu_phi q6809}]

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

# cpu_irq_n_i feeds the soft cores through a double-flop synchronizer --
# async by design. cpu_nmi_n_i is still optimized away (no fanout); its
# constraint is omitted to avoid warnings.
set_false_path -from [get_ports {cpu_irq_n_i}]

# pmod2_i is passed through combinationally to pmod1_o (debug loopback).
# No synchronous timing requirement.
set_false_path -from [get_ports {pmod2_i[*]}]

# cpu_sel (main.sv) is quasi-static: it changes only during a CPU swap,
# while both soft cores are held in reset and no bus cycle is in flight,
# so its fanout into the address mux and SID decode has no synchronous
# deadline.
set_false_path -from [get_cells {main/cpu_sel*}]
