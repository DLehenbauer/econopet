/**
 * PET Clone - Open hardware implementation of the Commodore PET
 * by Daniel Lehenbauer and contributors.
 *
 * https://github.com/DLehenbauer/commodore-pet-clone
 *
 * To the extent possible under law, I, Daniel Lehenbauer, have waived all
 * copyright and related or neighboring rights to this project. This work is
 * published from the United States.
 *
 * @copyright CC0 http://creativecommons.org/publicdomain/zero/1.0/
 * @author Daniel Lehenbauer <DLehenbauer@users.noreply.github.com> and contributors
 */

`include "./src/common_pkg.svh"

import common_pkg::*;

module video (
    // Wishbone B4 controller
    // (See: https://cdn.opencores.org/downloads/wbspec_b4.pdf)
    input  logic wb_clock_i,                      // Bus clock
    output logic [WB_ADDR_WIDTH-1:0] wb_addr_o,   // Address of pending read/write (valid when 'cycle_o' asserted)
    output logic [   DATA_WIDTH-1:0] wb_data_o,   // Data received from MCU to write (valid when 'cycle_o' asserted)
    input  logic [   DATA_WIDTH-1:0] wb_data_i,   // Data to transmit to MCU (captured on 'wb_clock_i' when 'wb_ack_i' asserted)
    output logic wb_we_o,                         // Direction of bus transfer (0 = reading, 1 = writing)
    output logic wb_cycle_o,                      // Requests a bus cycle from the arbiter
    output logic wb_strobe_o,                     // Signals next request ('addr_o', 'data_o', and 'wb_we_o' are valid).
    input  logic wb_stall_i,                      // Signals that peripheral is not ready to accept request
    input  logic wb_ack_i,                        // Signals termination of cycle ('data_i' valid)

    input  logic col_80_mode_i,                   // (0 = 40 col, 1 = 80 col)
    input  logic gfx_mode_i                       // (0 = Gfx, 1 = Lowercase)
);
    initial begin
        wb_we_o     = '0;
        wb_cycle_o  = '0;
        wb_strobe_o = '0;
    end

    logic [13:0] ma;
    logic [ 4:0] ra;

    localparam EVEN_RAM = 0,
               EVEN_ROM = 1,
               ODD_RAM  = 2,
               ODD_ROM  = 3;

    logic [WB_ADDR_WIDTH-1:0] addrs [3:0];
    always_comb begin
        addrs[EVEN_RAM] = common_pkg::wb_vram_addr(col_80_mode_i
            ? { ma[9:0], 1'b0 }     // 80 column mode
            : { 1'b0, ma[9:0] });   // 40 column mode
        addrs[EVEN_ROM] = common_pkg::wb_vrom_addr({ gfx_mode_i, data[EVEN_RAM][6:0], ra[2:0] });
        addrs[ODD_RAM]  = common_pkg::wb_vram_addr({ ma[9:0], 1'b1 });
        addrs[ODD_ROM]  = common_pkg::wb_vrom_addr({ gfx_mode_i, data[ODD_RAM][6:0], ra[2:0] });
    end

    logic [DATA_WIDTH-1:0] data [3:0];

    localparam WB_IDLE = 0,
               WB_AWAIT_ACK = 1;

    logic [1:0] fetch_stage = EVEN_RAM;
    logic [0:0] wb_state = WB_IDLE;

    always_ff @(posedge wb_clock_i) begin
        wb_strobe_o <= '0;
        
        case (wb_state)
            WB_IDLE: begin
                if (!wb_stall_i) begin
                    wb_addr_o   <= addrs[fetch_stage];
                    wb_cycle_o  <= 1'b1;
                    wb_strobe_o <= 1'b1;
                    wb_state    <= WB_AWAIT_ACK;
                end
            end

            WB_AWAIT_ACK: begin
                if (wb_ack_i) begin
                    data[fetch_stage] <= wb_data_i;
                    fetch_stage       <= fetch_stage + 1'b1;
                    wb_state          <= WB_IDLE;
                end
            end
        endcase
    end
endmodule
