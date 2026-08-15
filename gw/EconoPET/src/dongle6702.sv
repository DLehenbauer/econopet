// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

import common_pkg::*;

// SuperPET 6702 security dongle at $EFE0 (decoded $EFE0-$EFE3 like the
// board's 74LS08 partial decode; all four addresses behave identically).
//
// Waterloo language startup performs a challenge/response against this
// chip and takes a kill path (wild jump through an uninitialized vector)
// when it fails -- an unemulated dongle is exactly a post-load crash.
// Algorithm ported from VICE petmem.c (dongle6702, reverse-engineered by
// the VICE team): eight circular shift registers of lengths 6,3,7,8,1,3,5,2;
// a write is processed only when its parity matches the expected toggle
// (only the first ODD value after an EVEN one mutates state); each input
// bit that changed since the previous odd write flips its register's
// leftmost bit, then every register rotates right, toggling its output bit
// in `val` when a 1 falls off the end.
module dongle6702 (
    input  logic sys_clock_i,
    input  logic reset_i,

    input  logic                      cpu_be_i,
    input  logic                      cpu_data_strobe_i,
    input  logic [CPU_ADDR_WIDTH-1:0] cpu_addr_i,
    input  logic [    DATA_WIDTH-1:0] cpu_data_i,
    output logic [    DATA_WIDTH-1:0] cpu_data_o,
    output logic                      cpu_data_oe,
    input  logic                      cpu_we_i,
    input  logic                      enable_i        // hidden in MMU flat mode
);
    wire sel = enable_i && cpu_be_i && (cpu_addr_i & 16'hFFFC) == 16'hEFE0;

    localparam logic [7:0] MAGIC = 8'hD6;  // 128+64+16+4+2

    // leftmost[i] = 1 << (len[i]-1), lengths 6,3,7,8,1,3,5,2
    function automatic logic [8:0] leftmost(input int i);
        case (i)
            0: leftmost = 9'h020;
            1: leftmost = 9'h004;
            2: leftmost = 9'h040;
            3: leftmost = 9'h080;
            4: leftmost = 9'h001;
            5: leftmost = 9'h004;
            6: leftmost = 9'h010;
            default: leftmost = 9'h002;
        endcase
    endfunction

    logic [8:0] shift [8];
    logic [7:0] val = MAGIC;
    logic [7:0] prevodd = 8'h01;
    logic wantodd = 1'b0;

    initial begin
        for (int i = 0; i < 8; i++)
            shift[i] = ((8'(1) << i) & (MAGIC | 8'h01)) != 0 ? leftmost(i) : 9'h0;
    end

    always_ff @(posedge sys_clock_i) begin
        if (reset_i) begin
            for (int i = 0; i < 8; i++)
                shift[i] <= ((8'(1) << i) & (MAGIC | 8'h01)) != 0 ? leftmost(i) : 9'h0;
            val     <= MAGIC;
            prevodd <= 8'h01;
            wantodd <= 1'b0;
        end else if (sel && cpu_data_strobe_i && cpu_we_i) begin
            if (cpu_data_i[0] == wantodd) begin
                if (wantodd) begin
                    logic [7:0] v;
                    logic [7:0] changed;
                    logic [8:0] s;
                    v = val;
                    changed = prevodd ^ cpu_data_i;
                    for (int i = 7; i >= 0; i--) begin
                        s = shift[i];
                        if (changed[i]) s = s ^ leftmost(i);
                        if (s[0]) begin
                            v = v ^ (8'(1) << i);
                            s = s | (leftmost(i) << 1);
                        end
                        shift[i] <= s >> 1;
                    end
                    prevodd <= cpu_data_i;
                    val     <= v;
                end
                wantodd <= !wantodd;
            end
        end
    end

    // Registered read injection (same discipline as acia6551/ieee).
    always_ff @(posedge sys_clock_i) begin
        cpu_data_oe <= 1'b0;
        if (sel && !cpu_we_i) begin
            cpu_data_oe <= 1'b1;
            cpu_data_o  <= val;
        end
    end
endmodule
