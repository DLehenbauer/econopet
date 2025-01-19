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

module address_decoding_tb();
    // Intentionally 1 bit larger than the CPU address width to avoid overflow/infinite loop.
    logic [CPU_ADDR_WIDTH:0] addr = 17'hxxxx;

    logic ram_en;
    logic magic_en;
    logic pia1_en;
    logic pia2_en;
    logic via_en;
    logic crtc_en;
    logic sid_en;
    logic io_en;
    logic is_vram;
    logic is_readonly;

    address_decoding address_decoding(
        .sys_clock_i(),
        .cpu_be_i(),
        .cpu_wr_strobe_i(),
        .cpu_data_i(),

        .cpu_addr_i(addr[CPU_ADDR_WIDTH-1:0]),
        .ram_en_o(ram_en),
        .magic_en_o(magic_en),
        .pia1_en_o(pia1_en),
        .pia2_en_o(pia2_en),
        .via_en_o(via_en),
        .crtc_en_o(crtc_en),
        .sid_en_o(sid_en),
        .io_en_o(io_en),
        .is_vram_o(is_vram),
        .is_rom_o(is_readonly)
    );

    task check(
        input expected_ram_en,
        input expected_magic_en,
        input expected_pia1_en,
        input expected_pia2_en,
        input expected_via_en,
        input expected_crtc_en,
        input expected_sid_en,
        input expected_io_en,
        input expected_is_vram,
        input expected_is_readonly
    );
        `assert_equal(ram_en, expected_ram_en);
        `assert_equal(magic_en, expected_magic_en);
        `assert_equal(pia1_en, expected_pia1_en);
        `assert_equal(pia2_en, expected_pia2_en);
        `assert_equal(via_en, expected_via_en);
        `assert_equal(crtc_en, expected_crtc_en);
        `assert_equal(sid_en, expected_sid_en);
        `assert_equal(io_en, expected_io_en);
        `assert_equal(is_vram, expected_is_vram);
        `assert_equal(is_readonly, expected_is_readonly);
    endtask

    task check_range(
        input string name,
        input [16:0] start_addr,
        input [16:0] end_addr,
        input expected_ram_en,
        input expected_magic_en,
        input expected_pia1_en,
        input expected_pia2_en,
        input expected_via_en,
        input expected_crtc_en,
        input expected_sid_en,
        input expected_io_en,
        input expected_is_vram,
        input expected_is_readonly
    );
        $display("%s: $%x-$%x", name, start_addr, end_addr);

        for (addr = start_addr; addr <= end_addr; addr = addr + 1) begin
            #1 check(
                expected_ram_en,
                expected_magic_en,
                expected_pia1_en,
                expected_pia2_en,
                expected_via_en,
                expected_crtc_en,
                expected_sid_en,
                expected_io_en,
                expected_is_vram,
                expected_is_readonly
            );
        end
    endtask

    task run;
        $display("[%t] BEGIN %m", $time);

        check_range(
            /* name                   : */ "RAM",
            /* start_addr             : */ 'h0000,
            /* end_addr               : */ 'h7fff,
            /* expected_ram_en        : */ 1,
            /* expected_magic_en      : */ 0,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 0,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 0
        );

        check_range(
            /* name                   : */ "Display RAM",
            /* start_addr             : */ 'h8000,
            /* end_addr               : */ 'h8eff,
            /* expected_ram_en        : */ 1,
            /* expected_magic_en      : */ 0,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 0,
            /* expected_is_vram       : */ 1,
            /* expected_is_readonly   : */ 0
        );

        check_range(
            /* name                   : */ "SID",
            /* start_addr             : */ 'h8f00,
            /* end_addr               : */ 'h8fff,
            /* expected_ram_en        : */ 0,
            /* expected_magic_en      : */ 0,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 1,
            /* expected_io_en         : */ 0,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 0
        );



        check_range(
            /* name                   : */ "ROM",
            /* start_addr             : */ 'h9000,
            /* end_addr               : */ 'he7ff,
            /* expected_ram_en        : */ 1,
            /* expected_magic_en      : */ 0,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 0,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 1
        );

        check_range(
            /* name                   : */ "MAGIC",
            /* start_addr             : */ 'he800,
            /* end_addr               : */ 'he80f,
            /* expected_ram_en        : */ 0,
            /* expected_magic_en      : */ 1,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 0,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 0
        );

        check_range(
            /* name                   : */ "PIA1",
            /* start_addr             : */ 'he810,
            /* end_addr               : */ 'he81f,
            /* expected_ram_en        : */ 0,
            /* expected_magic_en      : */ 0,
            /* expected_pia1_en       : */ 1,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 1,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 0
        );

        check_range(
            /* name                   : */ "PIA2",
            /* start_addr             : */ 'he820,
            /* end_addr               : */ 'he83f,
            /* expected_ram_en        : */ 0,
            /* expected_magic_en      : */ 0,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 1,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 1,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 0
        );

        check_range(
            /* name                   : */ "VIA",
            /* start_addr             : */ 'he840,
            /* end_addr               : */ 'he87f,
            /* expected_ram_en        : */ 0,
            /* expected_magic_en      : */ 0,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 1,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 1,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 0
        );

        check_range(
            /* name                   : */ "CRTC",
            /* start_addr             : */ 'he880,
            /* end_addr               : */ 'he8ff,
            /* expected_ram_en        : */ 0,
            /* expected_magic_en      : */ 0,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 1,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 0,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 0
        );

        // Currently, this unmapped range is treated as ROM.
        check_range(
            /* name                   : */ "(unmapped)",
            /* start_addr             : */ 'he900,
            /* end_addr               : */ 'hefff,
            /* expected_ram_en        : */ 1,
            /* expected_magic_en      : */ 0,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 0,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 1
        );

        check_range(
            /* name                   : */ "ROM",
            /* start_addr             : */ 'hf000,
            /* end_addr               : */ 'hffff,
            /* expected_ram_en        : */ 1,
            /* expected_magic_en      : */ 0,
            /* expected_pia1_en       : */ 0,
            /* expected_pia2_en       : */ 0,
            /* expected_via_en        : */ 0,
            /* expected_crtc_en       : */ 0,
            /* expected_sid_en        : */ 0,
            /* expected_io_en         : */ 0,
            /* expected_is_vram       : */ 0,
            /* expected_is_readonly   : */ 1
        );

        #1 $display("[%t] END %m", $time);
    endtask
endmodule
