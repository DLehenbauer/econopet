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

`timescale 1ns / 1ps
`include "./sim/assert.svh"

module spi_driver #(
    parameter SCK_MHZ = 24          // SPI baud rate
) (
    input  logic clock_i,           // Destination clock
    
    // SPI bus signals (see https://www.oshwa.org/a-resolution-to-redefine-spi-signal-names/)
    output logic spi_cs_no,         // (CS)  Chip Select (active low)
    output logic spi_sck_o,         // (SCK) Serial Clock
    input  logic spi_sd_i,          // (SDI) Serial Data In (rx)
    output logic spi_sd_o,          // (SDO) Serial Data Out (tx)

    output logic [7:0] spi_data_o,  // Last received (valid when 'spi_ack_o' asserted)
    output logic       spi_ack_o    // 'spi_data_o' valid pulse
);
    clock_gen#(SCK_MHZ) spi_sck(
        .clock_o(spi_sck_o)
    );

    task reset;
        spi_sck.stop;
        spi_cs_no = '1;
        @(posedge clock_i);         // Hold long enough for destination clock to detect edge.
    endtask

    task send(
        input logic unsigned [7:0] tx[],
        input bit complete_i = 1'b1
    );
        integer byte_index;
        integer bit_index;

        `assert_equal(spi_cs_no, 1'b1);

        tx_byte = tx[0];
        spi_cs_no = '0;
        #1;
        spi_sck.start;

        foreach (tx[byte_index]) begin
            for (bit_index = 0; bit_index < 8; bit_index++) begin
                @(posedge spi_sck_o);
    
                // 'tx_byte' is latched after first rising edge of spi_sck_o.  To test this, we
                // load the next byte to transfer into 'spi_data_i' just afterwards.
                #1;
                tx_byte = tx[byte_index + 1];
            end
        end

        @(negedge spi_sck_o);
        spi_sck.stop;

        if (complete_i) begin
            complete;
        end
    endtask

    task complete;
        `assert_equal(spi_cs_no, '0);
        spi_cs_no = 1'b1;
        @(posedge clock_i);         // Hold CS_N long enough for destination clock to detect edge.
    endtask;

    logic [7:0] spi_rx_data;
    logic       spi_strobe;
    logic [7:0] tx_byte = 8'hxx;

    spi spi_tx(
        .spi_sck_i(spi_sck_o),
        .spi_cs_ni(spi_cs_no),
        .spi_sd_i(spi_sd_i),
        .spi_sd_o(spi_sd_o),
        .data_i(tx_byte),
        .data_o(spi_data_o),
        .strobe_o(spi_strobe)
    );

    sync2_edge_detect sync_valid(
        .clock_i(clock_i),
        .data_i(spi_strobe),
        .pe_o(spi_ack_o)
    );
endmodule
