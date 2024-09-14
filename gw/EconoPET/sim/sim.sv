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

module sim;
    video_tb video_tb ();
    // keyboard_tb keyboard_tb ();
    // register_file_tb register_file_tb ();
    // timing_tb timing_tb ();
    // spi_tb spi_tb ();
    // spi1_tb spi1_tb ();
    // ram_tb ram_tb ();
    // bram_tb bram_tb ();
    // top_tb top_tb ();

    initial begin
        $dumpfile("work_sim/out.vcd");
        $dumpvars(0, sim);

        video_tb.run();
        // keyboard_tb.run();
        // register_file_tb.run();
        // timing_tb.run();
        // spi_tb.run();
        // spi1_tb.run();
        // ram_tb.run();
        // bram_tb.run();
        // top_tb.run();

        $display("[%t] Simulation Complete", $time);
        $finish;
    end
endmodule
