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
    logic [DATA_WIDTH-2:0] rx_buffer;
    logic [DATA_WIDTH-2:0] tx_buffer;

    always_latch begin
        if (!spi_sck_i) begin
            data_o    <= { rx_buffer, spi_sd_i };

            if (bit_count == '0) begin
                { spi_sd_o, tx_buffer } <= data_i;
            end else begin
                spi_sd_o = tx_buffer[3'd7 - bit_count];
            end
        end
    end

    always_ff @(posedge spi_cs_ni or posedge spi_sck_i) begin
        if (spi_cs_ni) begin
            // Deasserting 'spi_cs_ni' indicates the transfer has ended.
            bit_count <= '0;
            rx_buffer <= 'x;
        end else begin
            // Positive edge of SCK indicates SDI is valid.  Shift SDI into the LSB of 'data_o' and
            // increment the bit count.  If this was the last bit of the current cycle assert 'cycle_o'.
            bit_count <= bit_count + 1'b1;
            rx_buffer <= { rx_buffer[DATA_WIDTH - 3:0], spi_sd_i };
        end
    end

    always_ff @(negedge spi_cs_ni or negedge spi_sck_i) begin
        // The below if-condition avoids the following error:
        // 'ERROR: if-condition does not match any sensitivity list edge (VERI-1167)'
        if (!spi_cs_ni || !spi_sck_i) begin

            // When the final bit of 'data_i' has been shifted into 'spi_sd_o', we know the next positive
            // SCK edge will transfer the final bits of 'data_i' and 'data_o'.
            cycle_o <= bit_count == DATA_WIDTH - 1;
        end
    end
endmodule
