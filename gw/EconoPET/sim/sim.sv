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

module sim;
    bit clk = '0;
    initial forever #(1000 / (64 * 2)) clk = ~clk;

    spi_tb spi_tb(
        .clk_i(clk)
    );

    initial begin
        $dumpfile("work_sim/out.vcd");
        $dumpvars(0, sim);

        spi_tb.run();

        $display("[%t] Simulation Complete", $time);
        $finish;
    end
 endmodule
 