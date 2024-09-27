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
    output logic [WB_ADDR_WIDTH-1:0] wb_addr_o,
    output logic [   DATA_WIDTH-1:0] wb_data_o,
    output logic                     wb_we_o,
    output logic                     wb_cycle_o,
    output logic                     wb_strobe_o,
    input  logic                     wb_stall_i,
    input  logic                     wb_ack_i,

    input  logic [WB_ADDR_WIDTH-1:0] spi1_addr_i,
    input  logic [   DATA_WIDTH-1:0] spi1_data_i,
    input  logic                     spi1_we_i,
    input  logic                     spi1_cycle_i,
    input  logic                     spi1_strobe_i,
    output logic                     spi1_stall_o,

    input  logic [WB_ADDR_WIDTH-1:0] video_addr_i,
    input  logic [   DATA_WIDTH-1:0] video_data_i,
    input  logic                     video_we_i,
    input  logic                     video_cycle_i,
    input  logic                     video_strobe_i,
    output logic                     video_stall_o,

    input  logic clk8_en_i,
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

    always_comb begin
        wb_addr_o   = 'x;
        wb_data_o   = 'x;
        wb_we_o     = '0;
        wb_cycle_o  = '0;
        wb_strobe_o = '0;

        spi1_stall_o  = 1'b1;
        video_stall_o = 1'b1;

        if (spi1_grant) begin
            wb_addr_o     = spi1_addr_i;
            wb_data_o     = spi1_data_i;
            wb_we_o       = spi1_we_i;
            wb_cycle_o    = spi1_cycle_i;
            wb_strobe_o   = spi1_strobe_i & clk8_en_delay;
            spi1_stall_o  = wb_stall_i | !clk8_en_delay;
        end else if (video_grant) begin
            wb_addr_o     = video_addr_i;
            wb_data_o     = video_data_i;
            wb_we_o       = video_we_i;
            wb_cycle_o    = video_cycle_i;
            wb_strobe_o   = video_strobe_i & clk8_en_delay;
            video_stall_o = wb_stall_i | !clk8_en_delay;
        end
    end

    assign cpu_grant_en_o = cpu_grant & clk8_en_delay;
endmodule
