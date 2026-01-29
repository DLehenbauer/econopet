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
import common_pkg::*;

module address_decoding_tb();
    logic sys_clock;
    clock_gen #(SYS_CLOCK_MHZ) clock_gen (.clock_o(sys_clock));
    initial clock_gen.start;

    logic reset = 1'b0;
    logic cpu_be = 1'b1;

    logic [CPU_ADDR_WIDTH:0] cpu_addr;  // 'cpu_addr' is intentionally 1-bit larger than CPU_ADDR_WIDTH
                                        // to avoid overflow when looping through the full address range.
    
    logic [DATA_WIDTH-1:0] cpu_data;    // Used to mock  CPU writes to the memory control register.
    logic cpu_wr_strobe = 1'b0;

    logic ram_en;
    logic pia1_en;
    logic pia2_en;
    logic via_en;
    logic crtc_en;
    logic sid_en;
    logic io_en;
    logic is_vram;
    logic is_readonly;

    logic decoded_a15;
    logic decoded_a16;

    address_decoding address_decoding(
        .reset_i(reset),
        .sys_clock_i(sys_clock),
        
        .cpu_be_i(cpu_be),
        .cpu_wr_strobe_i(cpu_wr_strobe),
        .cpu_data_i(cpu_data),

        .cpu_addr_i(cpu_addr[CPU_ADDR_WIDTH-1:0]),
        .ram_en_o(ram_en),
        .pia1_en_o(pia1_en),
        .pia2_en_o(pia2_en),
        .via_en_o(via_en),
        .crtc_en_o(crtc_en),
        .sid_en_o(sid_en),
        .io_en_o(io_en),
        .is_vram_o(is_vram),
        .is_readonly_o(is_readonly),

        .decoded_a15_o(decoded_a15),
        .decoded_a16_o(decoded_a16)
    );

    task check(
        input expected_ram_en,
        input expected_pia1_en,
        input expected_pia2_en,
        input expected_via_en,
        input expected_crtc_en,
        input expected_sid_en,
        input expected_io_en,
        input expected_is_vram,
        input expected_is_readonly,
        input [1:0] expected_a16_15
    );
        `assert_equal(ram_en, expected_ram_en);
        `assert_equal(pia1_en, expected_pia1_en);
        `assert_equal(pia2_en, expected_pia2_en);
        `assert_equal(via_en, expected_via_en);
        `assert_equal(crtc_en, expected_crtc_en);
        `assert_equal(sid_en, expected_sid_en);
        `assert_equal(io_en, expected_io_en);
        `assert_equal(is_vram, expected_is_vram);
        `assert_equal(is_readonly, expected_is_readonly);
        `assert_equal(decoded_a15, expected_a16_15[0]);
        `assert_equal(decoded_a16, expected_a16_15[1]);
    endtask

    task check_range(
        input string name,
        input integer start_addr,
        input integer end_addr,
        input expected_ram_en,
        input expected_pia1_en,
        input expected_pia2_en,
        input expected_via_en,
        input expected_crtc_en,
        input expected_sid_en,
        input expected_io_en,
        input expected_is_vram,
        input expected_is_readonly,
        input [1:0] expected_a16_15
    );
        integer delta;

        delta = 1'b1;
        $display("[%t]   %12s: $%04x-$%04x", $time, name, CPU_ADDR_WIDTH'(start_addr), CPU_ADDR_WIDTH'(end_addr));

        for (cpu_addr = start_addr; cpu_addr <= end_addr; cpu_addr = cpu_addr + delta) begin
            // Clock edge captures new input address.
            @(posedge sys_clock);

            // Signals are valid after edge.
            #1 check(
                expected_ram_en,
                expected_pia1_en,
                expected_pia2_en,
                expected_via_en,
                expected_crtc_en,
                expected_sid_en,
                expected_io_en,
                expected_is_vram,
                expected_is_readonly,
                expected_a16_15
            );

`ifndef PARANOID
            // If not running exhaustive tests, accelerate simulation by randomly skipping addresses.
            // The logic below ensures that we always test the first and last two addresses in the range.
            if (cpu_addr > start_addr) begin
                delta = $urandom_range(1, (end_addr - start_addr) / 2);

                // If the delta is too large, adjust it to ensure we don't skip the last two addresses.
                if (cpu_addr + delta > end_addr - 1) begin
                    delta = end_addr - cpu_addr - 1;

                    // Once we've reached the last couple addresses, ensure forward progress.
                    if (delta < 1) begin
                        delta = 1;
                    end
                end
            end
`endif
        end
    endtask

    task set_mem_ctl(
        enabled,        // Enable expansion memory (1 = enabled, 0 = disabled)
        io_peek,        // I/O peek-through at $E810-$EFFF (1 = enabled, 0 = disabled)
        screen_peek,    // Screen peek-through at $8000-$8FFF (1 = enabled, 0 = disabled)
        select_32,      // Selects 16KB page at $C000-$FFFF (1 = block3, 0 = block2)
        select_10,      // Selects 16KB page at $8000-$BFFF (1 = block1, 0 = block0)
        protect_32,     // Write protects $C000-$FFFF excluding peek-through (1 = read only, 0 = read/write)
        protect_10      // Write protects $8000-$BFFF excluding peek-through (1 = read only, 0 = read/write)
    );
        cpu_addr = 17'h0fff0;
        cpu_data = { enabled, io_peek, screen_peek, 'x, select_32, select_10, protect_32, protect_10 };
        
        @(negedge sys_clock);
        cpu_wr_strobe = 1'b1;
        
        @(negedge sys_clock);
        cpu_wr_strobe = 1'b0;
    endtask

    // Note that this excludes the SID, which maps into VRAM space.
    task check_io();
        // Unmapped ranges in IO space are treated as ROM.
        check_range(
            /* name                   : */ "(unmapped)",
            /* start_addr             : */ 'he800,
            /* end_addr               : */ 'he80f,
            /* expected_ram_en        : */ 1,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 0,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 1,
            /* expected_a16_15        : */ 2'b01
        );

        check_range(
            /* name                   : */ "PIA1",
            /* start_addr             : */ 'he810,
            /* end_addr               : */ 'he81f,
            /* expected_ram_en        : */ 0,
            /* expected_pia1_en       : */ 1,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 1,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 0,
            /* expected_a16_15        : */ 2'b01
        );

        check_range(
            /* name                   : */ "PIA2",
            /* start_addr             : */ 'he820,
            /* end_addr               : */ 'he83f,
            /* expected_ram_en        : */ 0,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 1,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 1,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 0,
            /* expected_a16_15        : */ 2'b01
        );

        check_range(
            /* name                   : */ "VIA",
            /* start_addr             : */ 'he840,
            /* end_addr               : */ 'he87f,
            /* expected_ram_en        : */ 0,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 1,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 1,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 0,
            /* expected_a16_15        : */ 2'b01
        );

        check_range(
            /* name                   : */ "CRTC",
            /* start_addr             : */ 'he880,
            /* end_addr               : */ 'he8ff,
            /* expected_ram_en        : */ 0,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 1,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 0,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 0,
            /* expected_a16_15        : */ 2'b01
        );

        // Unmapped ranges in IO space are treated as ROM.
        check_range(
            /* name                   : */ "(unmapped)",
            /* start_addr             : */ 'he900,
            /* end_addr               : */ 'hefff,
            /* expected_ram_en        : */ 1,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 0,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 1,
            /* expected_a16_15        : */ 2'b01
        );
    endtask

    task check_screen();
        check_range(
            /* name                   : */ "VRAM",
            /* start_addr             : */ 'h8000,
            /* end_addr               : */ 'h8eff,
            /* expected_ram_en        : */ 1,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 0,
            /* expected_is_vram       : */ 1,
            /* expected_is_readonly   : */ 0,
            /* expected_a16_15        : */ 2'b01
        );

        check_range(
            /* name                   : */ "SID",
            /* start_addr             : */ 'h8f00,
            /* end_addr               : */ 'h8fff,
            /* expected_ram_en        : */ 0,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 1,
            /* expected_io_en         : */ 0,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 0,
            /* expected_a16_15        : */ 2'b01
        );
    endtask

    task run;
        $display("[%t] BEGIN %m", $time);

        check_range(
            /* name                   : */ "RAM",
            /* start_addr             : */ 'h0000,
            /* end_addr               : */ 'h7fff,
            /* expected_ram_en        : */ 1,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 0,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 0,
            /* expected_a16_15        : */ 2'b00
        );

        $display("[%t]   Screen: $8000-$8FFF", $time);
        check_screen();

        check_range(
            /* name                   : */ "ROM",
            /* start_addr             : */ 'h9000,
            /* end_addr               : */ 'he7ff,
            /* expected_ram_en        : */ 1,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 0,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 1,
            /* expected_a16_15        : */ 2'b01
        );

        $display("[%t]   IO: $E810-$EFFF", $time);
        check_io();

        check_range(
            /* name                   : */ "ROM",
            /* start_addr             : */ 'hf000,
            /* end_addr               : */ 'hffff,
            /* expected_ram_en        : */ 1,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 0,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 1,
            /* expected_a16_15        : */ 2'b01
        );

        $display("[%t] 64K Expansion: $8000-$FFFF (Blocks 1/2)", $time);
        set_mem_ctl(
            /* enabled      : */ 1,     // (1 = enabled, 0 = disabled)
            /* io_peek      : */ 0,     // $E810-$E8FF: (1 = enabled, 0 = disabled)
            /* screen_peek  : */ 0,     // $8000-$8FFF: (1 = enabled, 0 = disabled)
            /* select_32    : */ 0,     // $C000-$FFFF: (1 = block3, 0 = block2)
            /* select_10    : */ 1,     // $8000-$BFFF: (1 = block1, 0 = block0)
            /* protect_32   : */ 1,     // $C000-$FFFF: (1 = read only, 0 = read/write)
            /* protect_10   : */ 0      // $8000-$BFFF: (1 = read only, 0 = read/write)
        );

        check_range(
            /* name                   : */ "Block 1",
            /* start_addr             : */ 'h8000,
            /* end_addr               : */ 'hbfff,
            /* expected_ram_en        : */ 1,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 0,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 0,
            /* expected_a16_15        : */ 2'b11
        );

        check_range(
            /* name                   : */ "Block 2 (Protected)",
            /* start_addr             : */ 'hc000,
            /* end_addr               : */ 'hffff,
            /* expected_ram_en        : */ 1,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 0,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 1,
            /* expected_a16_15        : */ 2'b10
        );

        $display("[%t] 64K Expansion: $8000-$FFFF (Blocks 0/3)", $time);
        set_mem_ctl(
            /* enabled      : */ 1,     // (1 = enabled, 0 = disabled)
            /* io_peek      : */ 0,     // $E810-$E8FF: (1 = enabled, 0 = disabled)
            /* screen_peek  : */ 0,     // $8000-$8FFF: (1 = enabled, 0 = disabled)
            /* select_32    : */ 1,     // $C000-$FFFF: (1 = block3, 0 = block2)
            /* select_10    : */ 0,     // $8000-$BFFF: (1 = block1, 0 = block0)
            /* protect_32   : */ 0,     // $C000-$FFFF: (1 = read only, 0 = read/write)
            /* protect_10   : */ 1      // $8000-$BFFF: (1 = read only, 0 = read/write)
        );

        check_range(
            /* name                   : */ "Block 0 (Protected)",
            /* start_addr             : */ 'h8000,
            /* end_addr               : */ 'hbfff,
            /* expected_ram_en        : */ 1,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 0,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 1,
            /* expected_a16_15        : */ 2'b10
        );

        check_range(
            /* name                   : */ "Block 3",
            /* start_addr             : */ 'hc000,
            /* end_addr               : */ 'hffff,
            /* expected_ram_en        : */ 1,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 0,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 0,
            /* expected_a16_15        : */ 2'b11
        );

        $display("[%t] 64K Expansion: $8000-$FFFF (Blocks 1/3)", $time);
        set_mem_ctl(
            /* enabled      : */ 1,     // (1 = enabled, 0 = disabled)
            /* io_peek      : */ 1,     // $E810-$E8FF: (1 = enabled, 0 = disabled)
            /* screen_peek  : */ 0,     // $8000-$8FFF: (1 = enabled, 0 = disabled)
            /* select_32    : */ 1,     // $C000-$FFFF: (1 = block3, 0 = block2)
            /* select_10    : */ 1,     // $8000-$BFFF: (1 = block1, 0 = block0)
            /* protect_32   : */ 1,     // $C000-$FFFF: (1 = read only, 0 = read/write)
            /* protect_10   : */ 0      // $8000-$BFFF: (1 = read only, 0 = read/write)
        );

        check_range(
            /* name                   : */ "Block 1",
            /* start_addr             : */ 'h8000,
            /* end_addr               : */ 'hbfff,
            /* expected_ram_en        : */ 1,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 0,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 0,
            /* expected_a16_15        : */ 2'b11
        );

        check_range(
            /* name                   : */ "Block 3 (Protected, IO Peek-Through)",
            /* start_addr             : */ 'hc000,
            /* end_addr               : */ 'he7ff,
            /* expected_ram_en        : */ 1,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 0,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 1,
            /* expected_a16_15        : */ 2'b11
        );

        check_io();

        check_range(
            /* name                   : */ "Block 3 (Protected, IO Peek-Through)",
            /* start_addr             : */ 'hf000,
            /* end_addr               : */ 'hffff,
            /* expected_ram_en        : */ 1,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 0,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 1,
            /* expected_a16_15        : */ 2'b11
        );

        $display("[%t] 64K Expansion: $8000-$FFFF (Blocks 1/3)", $time);
        set_mem_ctl(
            /* enabled      : */ 1,     // (1 = enabled, 0 = disabled)
            /* io_peek      : */ 0,     // $E810-$E8FF: (1 = enabled, 0 = disabled)
            /* screen_peek  : */ 1,     // $8000-$8FFF: (1 = enabled, 0 = disabled)
            /* select_32    : */ 1,     // $C000-$FFFF: (1 = block3, 0 = block2)
            /* select_10    : */ 1,     // $8000-$BFFF: (1 = block1, 0 = block0)
            /* protect_32   : */ 0,     // $C000-$FFFF: (1 = read only, 0 = read/write)
            /* protect_10   : */ 1      // $8000-$BFFF: (1 = read only, 0 = read/write)
        );

        check_screen();

        check_range(
            /* name                   : */ "Block 1 (Protected, Screen Peek-Through)",
            /* start_addr             : */ 'h9000,
            /* end_addr               : */ 'hbfff,
            /* expected_ram_en        : */ 1,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 0,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 1,
            /* expected_a16_15        : */ 2'b11
        );

        check_range(
            /* name                   : */ "Block 3",
            /* start_addr             : */ 'hc000,
            /* end_addr               : */ 'hffff,
            /* expected_ram_en        : */ 1,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 0,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 0,
            /* expected_a16_15        : */ 2'b11
        );

        #1 $display("[%t] END %m", $time);
    endtask

    initial begin
        $dumpfile("work_sim/address_decoding_tb.vcd");
        $dumpvars(0, address_decoding_tb);
        run;
        $finish;
    end
endmodule
