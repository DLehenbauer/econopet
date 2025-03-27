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

// Implements core of SPI Mode 0 transfers in a controller/peripheral agnostic way.
module spi (
    // SPI bus signals (see https://www.oshwa.org/a-resolution-to-redefine-spi-signal-names/)
    input  logic spi_cs_ni,  // (CS)  Chip Select (active low)
    input  logic spi_sck_i,  // (SCK) Serial Clock
    input  logic spi_sd_i,   // (SDI) Serial Data In (rx)
    output logic spi_sd_o,   // (SDO) Serial Data Out (tx)

    // Relevant WB bus signals in SCK domain.
    input logic [DATA_WIDTH-1:0] data_i,  // Data to transmit.  Captured on falling edge of CS_N and
                                          // falling edge of SCK for LSB.

    output logic [DATA_WIDTH-1:0] data_o, // Data received.  Shifted in from SDO on each positive edge of SCK.

    output logic strobe_o  // Asserted on rising SCK when 'data_o' is valid.  The next 'data_i'
                           // is captured on the following negative SCK edge.
);
    localparam BIT_COUNTER_WIDTH = $clog2(DATA_WIDTH),
               BIT_COUNTER_MAX   = (1 << BIT_COUNTER_WIDTH) - 1;    // Same as DATA_WIDTH-1, but typed as BIT_COUNTER_WIDTH bits.

    logic [BIT_COUNTER_WIDTH-1:0] bits_remaining = BIT_COUNTER_MAX;

    initial begin
        strobe_o = '0;
    end

    wire msb = bits_remaining == BIT_COUNTER_MAX;
    wire lsb = bits_remaining == '0;

    // Note that buffer is one bit wider than DATA_WIDTH to hold the incoming SDI bit.
    logic [DATA_WIDTH:0] buffer_d, buffer_q;
    assign buffer_d = msb
        ? {data_i[6:0], 1'bx, spi_sd_i}
        : {buffer_q[DATA_WIDTH-1:0], spi_sd_i};

    always_ff @(posedge spi_cs_ni or posedge spi_sck_i) begin
        if (spi_cs_ni) begin
            // Deasserting 'spi_cs_ni' completes or aborts the current transfer.
            bits_remaining <= BIT_COUNTER_MAX;
            buffer_q       <= 'x;
            strobe_o       <= '0;
        end else begin
            // SDI is valid on positive edge of SCK.
            buffer_q <= buffer_d;

            if (lsb) data_o <= buffer_d[DATA_WIDTH-1:0];

            strobe_o       <= lsb;
            bits_remaining <= bits_remaining - 1'b1;
        end
    end

    logic preload = 1'b1;

    logic spi_sd_q;
    assign spi_sd_o = preload
        ? data_i[DATA_WIDTH-1]
        : spi_sd_q;

    always_ff @(posedge spi_cs_ni or negedge spi_sck_i) begin
        if (spi_cs_ni) begin
            // Deasserting 'spi_cs_ni' completes or aborts the current transfer.
            spi_sd_q <= 'x;
            preload  <= 1'b1;
        end else begin
            spi_sd_q <= buffer_q[DATA_WIDTH];
            preload  <= msb;
        end
    end
endmodule
