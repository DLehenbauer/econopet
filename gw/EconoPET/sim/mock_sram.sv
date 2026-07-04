// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

//
// Asynchronous SRAM simulation model with parameterizable timing.
//
// Models the behavior of a real SRAM chip (default: AS6C1008-55PCN) including:
//
//   - Address-to-output propagation delay (tAA)
//   - OE-to-output propagation delay (tOE)
//   - Output hold after address change (tOH)
//   - Chip enable access time (tACE)
//   - Transition to/from high-Z on OE/CE changes (tOHZ, tOLZ, tCHZ, tCLZ)
//   - Write pulse width checking (tWP)
//   - Address setup relative to end of write (tAW)
//   - Data setup relative to end of write (tDW)
//   - Data hold after end of write (tDH)
//   - Write cycle time (tWC)
//   - Read cycle time (tRC)
//
// The active-low control pins (ce_ni, oe_ni, we_ni) model a real SRAM. The data bus
// is modeled with explicit direction signals rather than a tri-state 'inout' so the
// model simulates identically under both Icarus Verilog and Verilator (which is
// 2-state and cannot represent 'z or 'x):
//
//   - data_i      : bus value presented to the SRAM (sampled during writes)
//   - data_o      : value the SRAM drives during reads
//   - data_oe     : asserted when the SRAM drives the bus (deasserted == high-Z)
//   - data_valid  : asserted when 'data_o' is determinate (deasserted == indeterminate,
//                   modeling the SRAM's 'x output during access-time windows)
//
// A caller resolves the shared bus externally, e.g.:
//   assign bus = sram_data_oe ? sram_data_o : (other_oe ? other_data : '0);
//

import common_pkg::*;

module mock_sram #(
    // -------------------------------------------------------------------------
    // Address and data widths
    // -------------------------------------------------------------------------
    parameter int ADDR_WIDTH = RAM_ADDR_WIDTH,
    parameter int DW         = DATA_WIDTH,

    // -------------------------------------------------------------------------
    // Read-cycle timing (defaults from common_pkg, in ns)
    // -------------------------------------------------------------------------
    parameter int tRC   = SRAM_tRC,
    parameter int tAA   = SRAM_tAA,
    parameter int tACE  = SRAM_tACE,
    parameter int tOE   = SRAM_tOE,
    parameter int tCLZ  = SRAM_tCLZ,
    parameter int tOLZ  = SRAM_tOLZ,
    parameter int tCHZ  = SRAM_tCHZ,
    parameter int tOHZ  = SRAM_tOHZ,
    parameter int tOH   = SRAM_tOH,

    // -------------------------------------------------------------------------
    // Write-cycle timing (defaults from common_pkg, in ns)
    // -------------------------------------------------------------------------
    parameter int tWC   = SRAM_tWC,
    parameter int tAW   = SRAM_tAW,
    parameter int tAS   = SRAM_tAS,
    parameter int tWP   = SRAM_tWP,
    parameter int tWR   = SRAM_tWR,
    parameter int tDW   = SRAM_tDW,
    parameter int tDH   = SRAM_tDH,
    parameter int tOW   = SRAM_tOW,
    parameter int tWHZ  = SRAM_tWHZ
) (
    input  logic [ADDR_WIDTH-1:0] addr_i,
    input  logic [      DW-1:0]   data_i,      // Bus value presented to the SRAM (write data)
    output logic [      DW-1:0]   data_o,      // Value the SRAM drives during reads
    output logic                  data_oe,     // 1 when the SRAM is driving the bus (else high-Z)
    output logic                  data_valid,  // 1 when 'data_o' is determinate (else indeterminate)
    input  logic                  ce_ni,
    input  logic                  oe_ni,
    input  logic                  we_ni
);
    // -------------------------------------------------------------------------
    // Storage
    // -------------------------------------------------------------------------
    logic [DW-1:0] mem [0:(2**ADDR_WIDTH)-1];

    // -------------------------------------------------------------------------
    // Internal tracking signals
    // -------------------------------------------------------------------------
    // Timestamps for timing checks
    time addr_change_time;
    time we_fall_time;
    time we_rise_time;
    time data_stable_time;

    logic [ADDR_WIDTH-1:0] addr_latched;    // Address captured for write
    logic [DW-1:0]         data_latched;    // Data captured for write

    // -------------------------------------------------------------------------
    // Data bus outputs (explicit direction; see module header)
    // -------------------------------------------------------------------------
    // 'data_oe' asserted == chip drives the bus; 'data_valid' asserted == the driven
    // value is determinate. When the chip is not driving or the value is indeterminate,
    // 'data_o' is left at an arbitrary value that callers must ignore.
    initial begin
        data_oe    = 1'b0;
        data_valid = 1'b0;
    end

    // -------------------------------------------------------------------------
    // Determine whether the chip should be driving the bus
    // -------------------------------------------------------------------------
    // The chip drives the bus during a read: CE asserted, OE asserted, WE
    // deasserted. Transitions to/from high-Z have propagation delays (tOHZ,
    // tCHZ for disable and tOLZ, tCLZ for enable).

    wire read_active = !ce_ni && !oe_ni && we_ni;

    always @(read_active) begin
        if (read_active) begin
            // Output transitions from high-Z to low-Z after tOLZ/tCLZ, but
            // data is not yet valid (indeterminate until the tOE/tACE path
            // resolves).
            #(common_pkg::max2(tOLZ, tCLZ));
            if (read_active) begin
                data_o     <= {DW{1'bx}};
                data_valid <= 1'b0;
                data_oe    <= 1'b1;
            end
        end else begin
            // Output becomes indeterminate immediately when the read is
            // deactivated, then transitions to high-Z after tOHZ/tCHZ.
            data_o     <= {DW{1'bx}};
            data_valid <= 1'b0;
            #(common_pkg::max2(tOHZ, tCHZ));
            data_oe <= 1'b0;
        end
    end

    // -------------------------------------------------------------------------
    // Read path: address-to-output propagation
    // -------------------------------------------------------------------------
    // When read_active, data_o reflects mem[addr_i] after tAA from the last
    // address change (or tOE from OE assertion, whichever is later). On an
    // address change while read_active, the previous data is held for tOH
    // before transitioning.

    always @(addr_i) begin
        addr_change_time = $time;

        if (read_active) begin
            // Previous data is held for tOH after the address change.
            // Between tOH and tAA the output is indeterminate.
            #(tOH);
            if (read_active) begin
                data_o     <= {DW{1'bx}};
                data_valid <= 1'b0;
            end

            // New data becomes valid at tAA from the address change.
            #(tAA - tOH);
            if (read_active) begin
                data_o     <= mem[addr_i];
                data_valid <= 1'b1;
            end
        end
    end

    always @(negedge oe_ni) begin
        if (!ce_ni && we_ni) begin
            // OE just asserted while CE is active and WE is not: start a read.
            #(tOE);
            if (read_active) begin
                data_o     <= mem[addr_i];
                data_valid <= 1'b1;
            end
        end
    end

    always @(negedge ce_ni) begin
        if (!oe_ni && we_ni) begin
            #(tACE);
            if (read_active) begin
                data_o     <= mem[addr_i];
                data_valid <= 1'b1;
            end
        end
    end

    // -------------------------------------------------------------------------
    // Write path
    // -------------------------------------------------------------------------
    // A write occurs on the rising edge of WE (or CE, whichever rises first)
    // while the other is still asserted. The data written is the value present
    // on the bus during the active write pulse.
    //
    // Timing checks are performed to verify that the controller satisfies
    // minimum setup and pulse-width requirements.

    always @(negedge we_ni) begin
        we_fall_time = $time;
    end

    // Track data changes for setup checking.
    always @(data_i) begin
        data_stable_time = $time;
    end

    // Capture the presented data continuously while the write pulse is active
    // (a real SRAM cell follows the input during the transparent write window
    // and holds the last value at the end of the pulse). Latching here (rather
    // than sampling 'data_i' at the WE rising edge) makes the commit immune to a
    // controller that releases the data bus coincident with the WE rising edge
    // (a zero-hold-time condition that a real device tolerates).
    // verilator lint_off LATCH
    always @(data_i or we_ni or ce_ni) begin
        if (!we_ni && !ce_ni) begin
            data_latched = data_i;
        end
    end
    // verilator lint_on LATCH

    task automatic commit_write(input time write_end_time);
        we_rise_time = write_end_time;

        // Check write pulse width (tWP)
        if ((we_rise_time - we_fall_time) < tWP) begin
            $display("[%t] SRAM WARNING: Write pulse width violation: %0d ns < tWP(%0d ns)",
                     $time, we_rise_time - we_fall_time, tWP);
        end

        // Check address setup (tAW): address must be valid tAW before end of write
        if ((we_rise_time - addr_change_time) < tAW) begin
            $display("[%t] SRAM WARNING: Address setup violation: %0d ns < tAW(%0d ns)",
                     $time, we_rise_time - addr_change_time, tAW);
        end

        // Check data setup (tDW): data must be valid tDW before end of write
        if ((we_rise_time - data_stable_time) < tDW) begin
            $display("[%t] SRAM WARNING: Data setup violation: %0d ns < tDW(%0d ns)",
                     $time, we_rise_time - data_stable_time, tDW);
        end

        // Commit the write (the value latched during the write pulse).
        mem[addr_i] = data_latched;
        $display("[%t]        SRAM[%h] <- %h", $time, addr_i, data_latched);
    endtask

    // Write is committed when write mode exits: whichever rises first (WE or CE)
    // while the other control signal is still asserted.
    always @(posedge we_ni) begin
        if (!ce_ni) begin
            commit_write($time);
        end
    end

    always @(posedge ce_ni) begin
        if (!we_ni) begin
            commit_write($time);
        end
    end

    // -------------------------------------------------------------------------
    // Utility tasks (for testbench initialization)
    // -------------------------------------------------------------------------

    task automatic load_rom(
        input bit [ADDR_WIDTH-1:0] address,
        input string filename
    );
        int file, status;

    `ifndef ECONOPET_ROMS_DIR
        $fatal(1, "ECONOPET_ROMS_DIR not defined. Specify the ROM directory with -DECONOPET_ROMS_DIR=\"/path/to/roms\" on the Verilog compiler command line.");
    `endif
        file = $fopen({ `ECONOPET_ROMS_DIR, "/", filename }, "rb");
        if (file == 0) begin
            $fatal(1, "Unable to open ROM '%s' in '%s'. Verify ECONOPET_ROMS_DIR is set and points to the ROM directory.", filename, `ECONOPET_ROMS_DIR);
        end

        status = $fread(mem, file, 32'(address));
        if (status < 1) begin
            $fatal(1, "Error reading file '%s' (status=%0d).", filename, status);
        end

        $display("[%t] Loaded ROM '%s' ($%x-%x, %0d bytes)", $time, filename, address, address + ADDR_WIDTH'(status - 1), status);
        $fclose(file);
    endtask

    task automatic fill(
        input bit [ADDR_WIDTH-1:0] start_addr,
        input bit [ADDR_WIDTH-1:0] stop_addr,
        input bit [DW-1:0] data
    );
        bit [ADDR_WIDTH-1:0] a = start_addr;

        while (a <= stop_addr) begin
            mem[a] = data;
            a = a + 1;
        end

        $display("[%t] Filled SRAM [$%x-$%x] with $%h", $time, start_addr, stop_addr, data);
    endtask
endmodule
