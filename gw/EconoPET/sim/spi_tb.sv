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

`include "./sim/assert.svh"

module spi_tb;
    logic       sck;
    logic       cs;
    logic       pico;
    logic       poci;

    logic [7:0] pi;             // Controller (tx) -> Peripheral (rx)
    logic [7:0] expected_pi;
    logic [7:0] po;             // Peripheral (tx) -> Controller (rx)

    logic [7:0] ci;             // Peripheral (tx) -> Controller (rx)
    logic [7:0] expected_ci;
    logic [7:0] co;             // Controller (tx) -> Peripheral (rx)

    logic c_strobe;             // Controller output (co) is valid
    logic p_strobe;             // Peripheral output (po) is valid

    spi dut_spi_controller (
        .spi_sck_i(sck),
        .spi_cs_ni(!cs),
        .spi_sd_i(poci),
        .spi_sd_o(pico),
        .data_o(ci),
        .data_i(co),
        .strobe_o(c_strobe)
    );

    spi dut_spi_peripheral (
        .spi_sck_i(sck),
        .spi_cs_ni(!cs),
        .spi_sd_i(pico),
        .spi_sd_o(poci),
        .data_o(pi),
        .data_i(po),
        .strobe_o(p_strobe)
    );

    logic [2:0] bit_count = '0;

    always @(negedge cs) begin
        $display("[%t]     Deasserting CS resets SPI", $time);
        bit_count <= '0;

        #1;
        `assert_equal(c_strobe, '0);
        `assert_equal(p_strobe, '0);
    end

    always @(posedge cs) begin
        `assert_equal(c_strobe, '0);
        `assert_equal(p_strobe, '0);

        $display("[%t]     Asserting CS_N loads MSB of 'data_i' to SDO", $time);
        #1;
        `assert_equal(pico, expected_pi[7]);
        `assert_equal(poci, expected_ci[7]);
    end

    always @(posedge cs or negedge sck) begin
        if (bit_count == 0) begin
            expected_ci <= po;
            expected_pi <= co;
        end
    end

    always @(posedge sck) begin
        if (cs) begin
            #1;

            if (bit_count != 7) begin
                $display("[%t]     'strobe_o' must not be asserted until last bit received", $time);
                `assert_equal(c_strobe, '0);
                `assert_equal(p_strobe, '0);
            end else begin
                $display("[%t]     'strobe_o' must be asserted on rising SCK of last bit", $time);
                `assert_equal(c_strobe, 1'b1);
                `assert_equal(p_strobe, 1'b1);
            end
        end

        bit_count <= bit_count + 1;
    end

    always @(posedge c_strobe) begin
        $display("[%t]     'ci' must contain received data.", $time);
        `assert_equal(ci, expected_ci);
    end

    always @(posedge p_strobe) begin
        $display("[%t]     'pi' must contain received data.", $time);
        `assert_equal(pi, expected_pi);
    end

    always @(negedge sck) begin
        $display("[%t]     Falling edge of SCK shifts 'data_i[%0d]' out to SDO", $time, 7 - bit_count);

        #1;
        `assert_equal(poci, expected_ci[7 - bit_count]);
        `assert_equal(pico, expected_pi[7 - bit_count]);
    end

    task transfer_bit;
        `assert_equal(cs, '1);
        `assert_equal(sck, '0);

        #10 sck = 1'b1;
        #10 sck = '0;
    endtask

    task run;
        $display("[%t] BEGIN %m", $time);

        // Check power on state prior to setting cs_n
        $display("[%t]   Vet power on state", $time);
        #1;
        `assert_equal(c_strobe, '0);
        `assert_equal(p_strobe, '0);

        $display("[%t]   Drive cs_n and sck", $time);
        sck = '0;
        cs = '0;

        $display("[%t]   BEGIN: Transfer 1 byte", $time);
        co = 8'haa;
        po = 8'h55;

        #1 cs = '1;
        transfer_bit;
        transfer_bit;
        transfer_bit;
        transfer_bit;
        transfer_bit;
        transfer_bit;
        transfer_bit;
        transfer_bit;
        #1 cs = '0;
        #1 $display("[%t]   END: Transfer 1 byte", $time);

        $display("[%t]   BEGIN: Transfer 2 bytes", $time);
        co = 8'haa;
        po = 8'h55;

        #1 cs = '1;

        transfer_bit;

        $display("[%t]   'data_i' must be captured once first bit is transfered", $time);
        co = 8'h7e;
        po = 8'h81;

        transfer_bit;
        transfer_bit;
        transfer_bit;
        transfer_bit;
        transfer_bit;
        transfer_bit;
        transfer_bit;

        transfer_bit;
        transfer_bit;
        transfer_bit;
        transfer_bit;
        transfer_bit;
        transfer_bit;
        transfer_bit;
        transfer_bit;

        #1 cs = '0;

        #1 $display("[%t]   END: Transfer 2 bytes", $time);
        #1 $display("[%t] END %m", $time);
    endtask
endmodule
