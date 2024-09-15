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
`include "./src/common_pkg.svh"

import common_pkg::*;

module video_tb;
    logic clock;
    clock_gen #(SYS_CLOCK_MHZ) clock_gen (.clock_o(clock));
    initial clock_gen.start;

    logic [ WB_ADDR_WIDTH-1:0] addr;
    logic [    DATA_WIDTH-1:0] dout;
    logic [    DATA_WIDTH-1:0] din;
    logic                      we;
    logic                      cycle;
    logic                      strobe;
    logic                      stall = 1'b0;
    logic                      ack   = 1'b1;

    logic                      v_sync;
    logic                      h_sync;

    logic                     cpu_grant;
    logic                     video_grant;
    logic                     spi_grant;

    timing timing (
        .clock_i(clock),
        .cpu_grant_o(cpu_grant),
        .video_grant_o(video_grant),
        .spi_grant_o(spi_grant)
    );

    video video (
        .wb_clock_i(clock),
        .wb_addr_o(addr),
        .wb_data_i(dout),
        .wb_data_o(din),
        .wb_we_o(we),
        .wb_cycle_o(cycle),
        .wb_strobe_o(strobe),
        .wb_stall_i(stall),
        .wb_ack_i(ack),

        .cclk_en_i(cpu_grant),
        .wr_strobe_i(cpu_grant),

        .col_80_mode_i(1'b0),
        .graphic_i(1'b1),
        .v_sync_o(v_sync),
        .h_sync_o(h_sync)
    );

    task run;
        $display("[%t] BEGIN %m", $time);

        //@(posedge v_sync);

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
