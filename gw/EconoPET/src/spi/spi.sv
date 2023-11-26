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

// Implements core of SPI Mode 0 transfers in a controller/peripheral agnostic way.
 module spi #(
    parameter DATA_WIDTH = 8
) (
    // SPI bus signals (see https://www.oshwa.org/a-resolution-to-redefine-spi-signal-names/)
    input  logic spi_cs_ni,             // (CS)  Chip Select (active low)
    input  logic spi_sck_i,             // (SCK) Serial Clock
    input  logic spi_sd_i,              // (SDI) Serial Data In (rx)
    output logic spi_sd_o,              // (SDO) Serial Data Out (tx)
    
    // Relevant WB bus signals in SCK domain.
    input  logic [DATA_WIDTH - 1:0] data_i,     // Data to transmit.  Captured on falling edge of CS_N and
                                                // falling edge of SCK for LSB.

    output logic [DATA_WIDTH - 1:0] data_o,     // Data received.  Shifted in from SDO on each positive edge of SCK.
    
    output logic                    cycle_o     // Asserted on rising SCK when 'data_o' is valid.  The next 'data_i'
                                                // is captured on the following negative SCK edege.
);
    logic [$clog2(DATA_WIDTH - 1) - 1:0] bit_count = '0;
    
    initial begin
        cycle_o   <= '0;
        bit_count <= '0;
    end

    // 'data_o' is latched on the rising edge of SCK.
    logic [DATA_WIDTH - 2:0] dout_q;

    always_latch begin
        if (!spi_sck_i) data_o <= { dout_q, spi_sd_i };
    end

    always_ff @(posedge spi_cs_ni or posedge spi_sck_i) begin
        if (spi_cs_ni) begin
            // Deasserting 'spi_cs_ni' indicates the transfer has ended.
            bit_count <= '0;
            dout_q <= 'x;
        end else begin
            // Positive edge of SCK indicates SDI is valid.  Shift SDI into the LSB of 'data_o' and
            // increment the bit count.  If this was the last bit of the current cycle assert 'cycle_o'.
            bit_count <= bit_count + 1'b1;
            dout_q <= { dout_q[DATA_WIDTH - 3:0], spi_sd_i };
        end
    end

    logic [DATA_WIDTH - 1:0] tx_buffer;
    assign spi_sd_o = tx_buffer[DATA_WIDTH - 1];

    always_ff @(negedge spi_cs_ni or negedge spi_sck_i) begin
        if (spi_sck_i) begin
            tx_buffer <= data_i;
            cycle_o <= '0;
        end else begin
            // Negative edge of SCK indicates we've finished transmitting a bit.  Load the next bit to transmit.
            if (bit_count == '0) begin
                // We've already transmitted the last bit of 'tx_buffer'.  Capture the next bits for trasmission.
                tx_buffer <= data_i;
            end else begin
                // Otherwise shift out the MSB from 'tx_buffer'.
                tx_buffer <= { tx_buffer[DATA_WIDTH - 2:0], 1'bx };
            end

            // When the final bit of 'data_i' has been shifted into 'spi_sd_o', the we know the following positive
            // SCK transfers edge will transfer the final bits of 'data_i' and 'data_o'.
            cycle_o <= bit_count == DATA_WIDTH - 1;
        end;
    end
endmodule
