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

module register_file_tb;
    logic                     clock;
    clock_gen #(SYS_CLOCK_MHZ) clock_gen (.clock_o(clock));
    initial clock_gen.start;

    logic [WB_ADDR_WIDTH-1:0] addr;
    logic [   DATA_WIDTH-1:0] poci;
    logic [   DATA_WIDTH-1:0] pico;
    logic                     we;
    logic                     cycle;
    logic                     strobe;
    logic                     ack;
    logic                     stall;

    // Status register
    logic video_graphic;
    logic config_crt;
    logic config_keyboard;

    // CPU control register
    logic cpu_ready;
    logic cpu_reset;
    logic cpu_nmi;

    // Video control register
    logic video_col_80_mode;
    logic [11:10] video_ram_mask;

    register_file register_file (
        .wb_clock_i(clock),
        .wb_addr_i(common_pkg::wb_reg_addr(addr)),
        .wb_data_i(pico),
        .wb_data_o(poci),
        .wb_we_i(we),
        .wb_cycle_i(cycle),
        .wb_strobe_i(strobe),
        .wb_ack_o(ack),
        .wb_stall_o(stall),
        .wb_sel_i(1'b1),

        // Status register
        .video_graphic_i(video_graphic),
        .config_crt_i(config_crt),
        .config_keyboard_i(config_keyboard),

        // CPU control register
        .cpu_ready_o(cpu_ready),
        .cpu_reset_o(cpu_reset),
        .cpu_nmi_o(cpu_nmi),
        
        // Video control register
        .video_col_80_mode_o(video_col_80_mode),
        .video_ram_mask_o(video_ram_mask)
    );

    wb_driver wb (
        .wb_clock_i(clock),
        .wb_addr_o(addr),
        .wb_data_i(poci),
        .wb_data_o(pico),
        .wb_we_o(we),
        .wb_cycle_o(cycle),
        .wb_strobe_o(strobe),
        .wb_ack_i(ack),
        .wb_stall_i(stall)
    );

    always @(posedge clock or negedge clock) begin
        assert (stall == 0) else $fatal(1, "Register access must not stall Wishbone bus");
    end

    task test_status(input bit graphics, input bit crt, input bit keyboard);
        byte data;

        video_graphic = graphics;
        config_crt = crt;
        config_keyboard = keyboard;

        // Register file copies updated status to BRAM on idle clock cycles.
        @(posedge clock);

        wb.read(REG_STATUS, data);

        `assert_equal(data[REG_STATUS_GRAPHICS_BIT], video_graphic);
        `assert_equal(data[REG_STATUS_CRT_BIT], config_crt);
        `assert_equal(data[REG_STATUS_KEYBOARD_BIT], config_keyboard);
    endtask

    task test_reg(input logic [REG_ADDR_WIDTH-1:0] addr, input logic [DATA_WIDTH-1:0] data);
        logic [DATA_WIDTH-1:0] data_rd;

        wb.write(addr, data);
        wb.read(addr, data_rd);
        `assert_exact_equal(data_rd, data);
    endtask

    task test_cpu_reg(input logic reset, input logic ready, input logic nmi);
        test_reg(REG_CPU, {6'bxxxx_x, nmi, reset, ready});
        `assert_equal(cpu_reset, reset);
        `assert_equal(cpu_ready, ready);
        `assert_equal(cpu_nmi, nmi);
    endtask

    task test_video_reg(input logic col_80_mode);
        test_reg(REG_VIDEO, {7'bxxxx_xxx, col_80_mode});
        `assert_equal(video_col_80_mode, col_80_mode);
    endtask

    task run;
        $display("[%t] BEGIN %m", $time);

        wb.reset;

        // Check power-on state
        `assert_equal(cpu_ready, 1'b0);
        `assert_equal(cpu_reset, 1'b1);
        `assert_equal(video_col_80_mode, 1'b0);

        test_cpu_reg(/* reset: */ 1'b0, /* ready: */ 1'b0, /* nmi: */ 1'b0);
        test_cpu_reg(/* reset: */ 1'b0, /* ready: */ 1'b1, /* nmi: */ 1'b0);
        test_cpu_reg(/* reset: */ 1'b1, /* ready: */ 1'b0, /* nmi: */ 1'b0);
        test_cpu_reg(/* reset: */ 1'b1, /* ready: */ 1'b1, /* nmi: */ 1'b0);
        test_cpu_reg(/* reset: */ 1'b0, /* ready: */ 1'b0, /* nmi: */ 1'b1);
        test_cpu_reg(/* reset: */ 1'b0, /* ready: */ 1'b0, /* nmi: */ 1'b0);

        test_video_reg(/* col_80_mode: */ 1'b1);
        test_video_reg(/* col_80_mode: */ 1'b0);

        test_status(/* graphics: */ 1'b0, /* crt: */ 1'b0, /* keyboard: */ 1'b1);
        test_status(/* graphics: */ 1'b0, /* crt: */ 1'b1, /* keyboard: */ 1'b0);
        test_status(/* graphics: */ 1'b1, /* crt: */ 1'b0, /* keyboard: */ 1'b0);

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
