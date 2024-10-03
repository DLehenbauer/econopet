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

module arbiter (
    input  logic wb_clock_i,
    input  logic clk8_en_i,

    // Wishbone controllers to arbitrate
    input  logic [WB_ADDR_WIDTH-1:0] spi1_addr_i,
    input  logic [   DATA_WIDTH-1:0] spi1_dout_i,
    output logic [   DATA_WIDTH-1:0] spi1_din_o,
    input  logic                     spi1_we_i,
    input  logic                     spi1_cycle_i,
    input  logic                     spi1_strobe_i,
    output logic                     spi1_stall_o,
    output logic                     spi1_ack_o,

    input  logic [WB_ADDR_WIDTH-1:0] video_addr_i,
    input  logic [   DATA_WIDTH-1:0] video_dout_i,
    output logic [   DATA_WIDTH-1:0] video_din_o,
    input  logic                     video_we_i,
    input  logic                     video_cycle_i,
    input  logic                     video_strobe_i,
    output logic                     video_stall_o,
    output logic                     video_ack_o,

    // Wishbone bus
    output logic [WB_ADDR_WIDTH-1:0] wb_addr_o,
    output logic [   DATA_WIDTH-1:0] wb_dout_o,
    input  logic [   DATA_WIDTH-1:0] wb_din_i,
    output logic                     wb_we_o,
    output logic                     wb_cycle_o,
    output logic                     wb_strobe_o,
    input  logic                     wb_stall_i,
    input  logic                     wb_ack_i,

    output logic cpu_grant_en_o
);
    logic clk8_en_delay;
    logic [2:0] grant = 3'b111;

    always_ff @(posedge wb_clock_i) begin
        clk8_en_delay <= clk8_en_i;

        if (clk8_en_i) begin
            grant <= grant + 1'b1;
        end
    end

    localparam VIDEO_1 = 3'b000,
               VIDEO_2 = 3'b001,
               VIDEO_3 = 3'b010,
               VIDEO_4 = 3'b011,
               CPU_1   = 3'b100,
               CPU_2   = 3'b101,
               SPI_1   = 3'b110,
               SPI_2   = 3'b111;

    wire cpu_grant   = grant == CPU_1   || grant == CPU_2;
    wire video_grant = grant == VIDEO_1 || grant == VIDEO_2 || grant == VIDEO_3 || grant == VIDEO_4;
    wire spi1_grant  = grant == SPI_1   || grant == SPI_2;

    logic [0:0] wbc_sel;

    always_comb begin
        if (spi1_grant) begin
            wbc_sel = 1;
        end else begin
            wbc_sel = 0;
        end
    end

    wb_demux #(
        .COUNT(2)
    ) wb_demux (
        .wb_clock_i(wb_clock_i),

        // Wishbone controllers to demux
        .wbc_cycle_i({ video_cycle_i, spi1_cycle_i}),
        .wbc_strobe_i({ video_strobe_i, spi1_strobe_i}),
        .wbc_addr_i({ video_addr_i, spi1_addr_i}),
        .wbc_din_o({ video_din_o, spi1_din_o}),
        .wbc_dout_i({ video_dout_i, spi1_dout_i}),
        .wbc_we_i({ video_we_i, spi1_we_i}),
        .wbc_stall_o({ video_stall_o, spi1_stall_o}),
        .wbc_ack_o({ video_ack_o, spi1_ack_o}),

        // Wishbone bus
        .wb_cycle_o(wb_cycle_o),
        .wb_strobe_o(wb_strobe_o),
        .wb_addr_o(wb_addr_o),
        .wb_din_i(wb_din_i),
        .wb_dout_o(wb_dout_o),
        .wb_we_o(wb_we_o),
        .wb_stall_i(wb_stall_i),
        .wb_ack_i(wb_ack_i),

        // Control signals
        .wbc_sel(wbc_sel), // Select controller
        .wb_en_i(!cpu_grant && clk8_en_delay)
    );

    assign cpu_grant_en_o = cpu_grant & clk8_en_delay;
endmodule
