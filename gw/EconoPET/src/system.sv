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

 module system #(
    parameter DATA_WIDTH = 8,
    parameter WB_ADDR_WIDTH = 20,
    parameter CPU_ADDR_WIDTH = 16,
    parameter RAM_ADDR_WIDTH = 17
) (
    // Wishbone B4 peripheral
    // (See https://cdn.opencores.org/downloads/wbspec_b4.pdf)
    input  logic                  wb_clock_i,
    input  logic                  wb_reset_i,
    input  logic [ADDR_WIDTH-1:0] wb_addr_i,
    input  logic [DATA_WIDTH-1:0] wb_data_i,
    output logic [DATA_WIDTH-1:0] wb_data_o,
    input  logic                  wb_we_i,
    input  logic                  wb_cycle_i,
    input  logic                  wb_strobe_i,
    output logic                  wb_stall_o,
    output logic                  wb_ack_o,

    // CPU
    output logic cpu_be_o,

    input  logic [CPU_ADDR_WIDTH-1:0] cpu_addr_i,
    output logic [CPU_ADDR_WIDTH-1:0] cpu_addr_o,
    output logic                      cpu_addr_oe,

    input  logic [DATA_WIDTH-1:0] cpu_data_i,
    output logic [DATA_WIDTH-1:0] cpu_data_o,
    output logic                  cpu_data_oe,

    // RAM
    output logic ram_addr_a10_o,
    output logic ram_addr_a11_o,
    output logic ram_addr_a15_o,
    output logic ram_addr_a16_o,
    output logic ram_oe_o,
    output logic ram_we_o,

    // IO
    output logic io_oe_o,
    output logic pia1_cs_o,
    output logic pia2_cs_o,
    output logic via_cs_o
);
    logic grant_wb = '0;

    initial begin
        cpu_be_o  = '0;
        ram_oe_o  = '0;
        ram_we_o  = '0;
        io_oe_o   = '0;
        pia1_cs_o = '0;
        pia2_cs_o = '0;
        via_cs_o  = '0;
        wb_stall_o = 1'b1;
    end

    logic [5:0] clock_counter = '0;

    always_ff @(posedge clock_i or posedge wb_reset_i) begin
        if (wb_reset_i) begin
            clock_counter <= '0;
        end else begin
            clock_counter <= clock_counter + 1'b1;
        end
    end

    always_comb begin
        wb_permitted = (clock_counter < 48);
    end

    always_ff @(posedge clock_i) begin
        if (wb_cycle_i && wb_strobe_i) begin
            
        end
    end

endmodule
