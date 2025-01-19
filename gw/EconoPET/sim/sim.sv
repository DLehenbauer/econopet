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

`define TEST_WB_MUX
`define TEST_VIDEO
`define TEST_VIDEO_CRTC
`define TEST_VIDEO_CRTC_REG
`define TEST_KEYBOARD
`define TEST_REGISTER_FILE
`define TEST_TIMING
`define TEST_ADDRESS_DECODING
`define TEST_SPI
`define TEST_SPI1
`define TEST_RAM
`define TEST_BRAM
`define TEST_TOP

module sim;
`ifdef TEST_WB_MUX
    wb_mux_tb wb_mux_tb ();
`endif
`ifdef TEST_VIDEO
    video_tb video_tb ();
`endif
`ifdef TEST_VIDEO_CRTC
    video_crtc_tb video_crtc_tb ();
`endif
`ifdef TEST_VIDEO_CRTC_REG
    video_crtc_reg_tb video_crtc_reg_tb ();
`endif
`ifdef TEST_KEYBOARD
    keyboard_tb keyboard_tb ();
`endif
`ifdef TEST_REGISTER_FILE
    register_file_tb register_file_tb ();
`endif
`ifdef TEST_TIMING
    timing_tb timing_tb ();
`endif
`ifdef TEST_ADDRESS_DECODING
    address_decoding_tb address_decoding_tb ();
`endif
`ifdef TEST_SPI
    spi_tb spi_tb ();
`endif
`ifdef TEST_SPI1
    spi1_tb spi1_tb ();
`endif
`ifdef TEST_RAM
    ram_tb ram_tb ();
`endif
`ifdef TEST_BRAM
    bram_tb bram_tb ();
`endif 
`ifdef TEST_TOP
    top_tb top_tb ();
`endif

    initial begin
        $printtimescale(sim);
        $dumpfile("work_sim/out.vcd");
        $dumpvars(0, sim);

`ifdef TEST_WB_MUX
        wb_mux_tb.run;
`endif
`ifdef TEST_VIDEO
        video_tb.run;
`endif
`ifdef TEST_VIDEO_CRTC
        video_crtc_tb.run;
`endif
`ifdef TEST_VIDEO_CRTC_REG
        video_crtc_reg_tb.run;
`endif
`ifdef TEST_KEYBOARD
        keyboard_tb.run;
`endif
`ifdef TEST_REGISTER_FILE
        register_file_tb.run;
`endif
`ifdef TEST_TIMING
        timing_tb.run;
`endif
`ifdef TEST_ADDRESS_DECODING
        address_decoding_tb.run;
`endif
`ifdef TEST_SPI
        spi_tb.run;
`endif
`ifdef TEST_SPI1
        spi1_tb.run;
`endif
`ifdef TEST_RAM
        ram_tb.run;
`endif
`ifdef TEST_BRAM
        bram_tb.run;
`endif 
`ifdef TEST_TOP
        top_tb.run;
`endif
        $display("[%t] Simulation Complete", $time);
        $finish;
    end
endmodule
