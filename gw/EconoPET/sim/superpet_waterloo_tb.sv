// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

import common_pkg::*;

// Boots the real SuperPET Waterloo ROM set (not a synthetic test program --
// see superpet_top_tb.sv for that) against the mc6809 core integration, to
// check whether actual SuperPET software gets anywhere. Same non-CPU wiring
// as superpet_top_tb.sv (mock_sram + mock_bus + SPI1, no mock 6502).
//
// ROM images (place in ECONOPET_ROMS_DIR):
//   waterloo-a000-bfff.970018-12.bin  (8KB,  $A000-$BFFF)
//   waterloo-c000-dfff.970019-12.bin  (8KB,  $C000-$DFFF)
//   waterloo-e000-ffff-970034-12.bin  (8KB,  $E000-$FFFF, incl. reset vector)
// Source: https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/SuperPET/
module superpet_waterloo_tb;
    // Bottom entry of the power-on menu (setup/monitor/apl/basic/edit/fortran/
    // pascal/development), and so the marker that the list finished painting.
    localparam string MENU_LAST_ENTRY = "development";

    // Lit pixels in the finished menu. The sim is deterministic, so this is
    // exact, not a range: it catches a render the text assertions cannot see.
    localparam int MENU_LIT_PIXELS = 1074;

    bit sys_clock;
    clock_gen #(SYS_CLOCK_MHZ) sys_clock_gen (.clock_o(sys_clock));
    initial sys_clock_gen.start;

    logic [CPU_ADDR_WIDTH-1:0] bus_addr;
    wire  [DATA_WIDTH-1:0]     bus_data;
    logic bus_we_n;

    logic [DATA_WIDTH-1:0] bus_data_mux;
    logic                  bus_data_mux_oe;

    assign bus_data = bus_data_mux_oe ? bus_data_mux : {DATA_WIDTH{1'bz}};

    bit   manual_reset_n = 1'b1;
    logic cpu_reset_n;
    logic top_reset_n;
    logic top_reset_n_oe;
    assign cpu_reset_n = top_reset_n_oe ? top_reset_n : manual_reset_n;

    logic cpu_be;
    logic cpu_clock;
    logic cpu_ready;

    logic [CPU_ADDR_WIDTH-1:0] top_addr;
    logic [CPU_ADDR_WIDTH-1:0] top_addr_oe;
    logic [DATA_WIDTH-1:0] top_data;
    logic [DATA_WIDTH-1:0] top_data_oe;
    logic top_we_n;
    logic top_we_n_oe;

    logic ram_addr_a10_o, ram_addr_a11_o, ram_addr_a15_o, ram_addr_a16_o;
    logic ram_oe_n_o, ram_we_n_o;
    logic io_oe_n, pia1_cs_n, pia2_cs_n, via_cs_n;

    logic spi_sck, spi_cs_n, spi_pico, spi_poci, spi_stall;
    logic [7:0] spi_rx_data;

    // On the real board PIA1 CB1 turns the 60 Hz vertical drive into the
    // screen interrupt the Waterloo boot waits on. There is no PIA in the
    // mock system, so pulse the IRQ line directly (~100us, self-clearing
    // like the retrace edge). Idle high -- an unconnected pin reads low,
    // which holds IRQ asserted forever.
    logic cpu_irq_n = 1'b1;
    initial begin
        forever begin
            #16666666;             // ~60 Hz
            cpu_irq_n = 1'b0;
            #100000;               // ~100 us
            cpu_irq_n = 1'b1;
        end
    end

    top top (
        `TOP_UNUSED_PORTS
        .cpu_nmi_n_i(1'b1),
        .sys_clock_i(sys_clock),

        .cpu_be_o(cpu_be),
        .cpu_ready_o(cpu_ready),
        .cpu_reset_n_i(cpu_reset_n),
        .cpu_reset_n_o(top_reset_n),
        .cpu_reset_n_oe(top_reset_n_oe),
        .cpu_clock_o(cpu_clock),
        // Tied off, not fed from bus_addr: no physical CPU exists in this
        // bench, and the feedback (cpu_addr_o -> bus -> cpu_addr_i ->
        // active_cpu_addr mux) is a false combinational loop.
        .cpu_addr_i ({CPU_ADDR_WIDTH{1'b0}}),
        .cpu_addr_o (top_addr),
        .cpu_addr_oe(top_addr_oe),
        .cpu_data_i (bus_data),
        .cpu_data_o (top_data),
        .cpu_data_oe(top_data_oe),
        .cpu_we_n_i (1'b1),
        .cpu_we_n_o (top_we_n),
        .cpu_we_n_oe(top_we_n_oe),

        .cpu_sync_i(1'b0),
        .cpu_irq_n_i(cpu_irq_n),

        .ram_addr_a10_o(ram_addr_a10_o),
        .ram_addr_a11_o(ram_addr_a11_o),
        .ram_addr_a15_o(ram_addr_a15_o),
        .ram_addr_a16_o(ram_addr_a16_o),
        .ram_oe_n_o(ram_oe_n_o),
        .ram_we_n_o(ram_we_n_o),

        .io_oe_n_o(io_oe_n),
        .pia1_cs_n_o(pia1_cs_n),
        .pia2_cs_n_o(pia2_cs_n),
        .via_cs_n_o(via_cs_n),

        .spi0_cs_ni (spi_cs_n),
        .spi0_sck_i (spi_sck),
        .spi0_sd_i  (spi_pico),
        .spi0_sd_o  (spi_poci),
        .spi_stall_o(spi_stall),

        .graphic_i(1'b0),

        .config_crt_i(1'b0),
        .config_keyboard_i(1'b0)
    );

    wire [RAM_ADDR_WIDTH-1:0] ram_addr = {
        ram_addr_a16_o,
        ram_addr_a15_o,
        bus_addr[14],
        bus_addr[13],
        bus_addr[12],
        ram_addr_a11_o,
        ram_addr_a10_o,
        bus_addr[9:0]
    };

    // mock_sram drives its read data through mock_bus's mux via an explicit
    // output enable rather than a tri-stated data_io (see docs/dev/verilator.md).
    logic [DATA_WIDTH-1:0] ram_data;
    logic                  ram_data_oe;

    mock_sram mock_sram (
        .addr_i(ram_addr),
        .data_i(bus_data),
        .data_o(ram_data),
        .data_oe_o(ram_data_oe),
        .ce_ni(1'b0),
        .oe_ni(ram_oe_n_o),
        .we_ni(ram_we_n_o)
    );

    mock_bus mock_bus (
        .clock_i(sys_clock),

        .top_addr_i(top_addr),
        .top_addr_oe_i(top_addr_oe[0]),
        .top_data_i(top_data),
        .top_data_oe_i(top_data_oe[0]),
        .top_we_n_i(top_we_n),
        .top_we_n_oe_i(top_we_n_oe),

        .cpu_be_i(1'b0),
        .cpu_addr_i({CPU_ADDR_WIDTH{1'b0}}),
        .cpu_data_i({DATA_WIDTH{1'b0}}),
        .cpu_data_oe_i(1'b0),
        .cpu_we_n_i(1'b1),

        .ram_data_i(ram_data),
        .ram_data_oe_i(ram_data_oe),

        .io_data_i(8'h10),
        .io_oe_n_i(io_oe_n),

        .bus_addr_o(bus_addr),
        .bus_data_o(bus_data_mux),
        .bus_data_oe_o(bus_data_mux_oe),
        .bus_we_n_o(bus_we_n)
    );

    spi1_driver spi1_driver (
        .clock_i(sys_clock),
        .spi_sck_o(spi_sck),
        .spi_cs_no(spi_cs_n),
        .spi_pico_o(spi_pico),
        .spi_poci_i(spi_poci),
        .spi_stall_i(spi_stall),
        .spi_data_o(spi_rx_data)
    );

    task static spi_write_at (
        input logic [WB_ADDR_WIDTH-1:0] addr_i,
        input logic [   DATA_WIDTH-1:0] data_i
    );
        spi1_driver.write_at(addr_i, data_i);
    endtask

    // Waterloo writes ASCII, not PET screen codes. Strip the reverse-video
    // bit and pass printable ASCII through; everything else shows as '.'.
    function automatic byte screen_to_ascii(input byte code);
        byte c;
        c = code & 8'h7F;
        if (c >= 8'd32 && c < 8'd127) return c;
        return ".";
    endfunction

    task static dump_screen_row(input int row);
        string line;
        int base;
        line = "";
        base = row * 80;
        for (int col = 0; col < 80; col++) begin
            line = { line, $sformatf("%c", screen_to_ascii(
                mock_sram.mem[17'h08000 + RAM_ADDR_WIDTH'(base) + RAM_ADDR_WIDTH'(col)])) };
        end
        $display("[%t]   SCREEN[%0d]: %s", $time, row, line);
    endtask

    // Assert that 'text' appears somewhere on the screen (same screen-code
    // mapping as dump_screen_row).
    function static bit screen_contains(input string text);
        int tlen;
        bit found;
        tlen = text.len();
        found = 1'b0;
        for (int i = 0; !found && i + tlen <= 2000; i++) begin
            bit match;
            match = 1'b1;
            for (int j = 0; j < tlen; j++) begin
                if (screen_to_ascii(mock_sram.mem[
                    17'h08000 + RAM_ADDR_WIDTH'(i) + RAM_ADDR_WIDTH'(j)]) != byte'(text[j]))
                    match = 1'b0;
            end
            if (match) found = 1'b1;
        end
        return found;
    endfunction

    task static assert_screen_contains(input string text);
        if (!screen_contains(text)) $fatal(1, "expected '%s' on screen -- boot did not reach the menu", text);
    endtask

    task static run;
        bit menu_seen;
        $display("[%t] BEGIN SuperPET Waterloo boot test", $time);
        spi1_driver.reset;

        $display("[%t]   Loading Waterloo ROMs", $time);
        mock_sram.load_rom(17'h0A000, "waterloo-a000-bfff.970018-12.bin");
        mock_sram.load_rom(17'h0C000, "waterloo-c000-dfff.970019-12.bin");
        mock_sram.load_rom(17'h0E000, "waterloo-e000-ffff-970034-12.bin");
        // Character ROM, only so the end-of-test image can expand real glyphs.
        // $800 is quadrant 2 (SuperPET text) of the four 1K quadrants indexed
        // by {crtc_chr_option, video_graphics}; see video.sv/roms_get_char_rom.
        // Held at $8800 -- past the screen, clear of the $9000 ROM window.
        mock_sram.load_rom(17'h08800, "characters.901640-01.bin", 'h800, 'h800);

        // Clear screen RAM.
        mock_sram.fill(17'h08000, 17'h087CF, 8'h20);  // 80x25 screen, space-filled

        spi_write_at(common_pkg::wb_reg_addr(REG_CPU_SEL), 8'(CPU_SEL_SOFT_6809));

        $display("[%t]   Releasing reset via REG_CPU", $time);
        spi_write_at(common_pkg::wb_reg_addr(REG_CPU), 8'b0000_0000);

        // Stop as soon as the menu lands rather than burning a fixed budget.
        // Wait on the bottom entry, not the banner: the banner is up at ~150ms
        // but entries stream in until ~300ms, so the banner caught a half-drawn
        // screen. Flag loop rather than 'break', which iverilog rejects.
        menu_seen = 1'b0;
        for (int i = 0; i < 40 && !menu_seen; i++) begin
            #25000000;   // 25 ms
            if (screen_contains(MENU_LAST_ENTRY)) menu_seen = 1'b1;
            else if (i % 8 == 0) $display("[%t]   ...still running (PC-ish addr=$%h, E=%b)",
                $time, top.main.mc6809_addr, top.main.cpu6809_e);
        end

        $display("[%t]   Captured screen RAM (all 25 rows):", $time);
        for (int row = 0; row < 25; row++) dump_screen_row(row);

        // Only once the menu is up, so the .pgm always shows the passing state.
        // dump_screen's text render is skipped: it assumes PET screen codes.
        if (menu_seen) begin
            int lit;
            mock_sram.write_screen_pgm(17'h08000, 17'h08800, 80, 25,
                                       "outflow/superpet_waterloo_tb.pgm");
            lit = mock_sram.screen_lit_pixels(17'h08000, 17'h08800, 80, 25);
            $display("[%t]   lit pixels = %0d (expect %0d)", $time, lit, MENU_LIT_PIXELS);
            if (lit != MENU_LIT_PIXELS)
                $fatal(1, "screen render changed: %0d lit pixels, expected %0d", lit, MENU_LIT_PIXELS);
        end

        // video.sv picks its charset from CRTC R12 bit 5, independently of the
        // quadrant dumped above; assert they agree or the image is fiction.
        $display("[%t]   CRTC R12=$%02x chr_option=%b", $time,
            top.main.video.video_crtc.video_crtc_reg.r[12],
            top.main.video.video_crtc.video_crtc_reg.r[12][5]);
        if (top.main.video.video_crtc.video_crtc_reg.r[12][5] !== 1'b1)
            $fatal(1, "CRTC R12 bit5 clear: video would render the stock charset, not the SuperPET set dumped above");

        assert_screen_contains("Waterloo microSystems");
        assert_screen_contains(MENU_LAST_ENTRY);
        $display("[%t]   full menu reached in simulation", $time);

        $display("[%t] END SuperPET Waterloo boot test", $time);
    endtask

    // TB_INIT minus $dumpvars: a full-design VCD makes the long boot impractical.
    initial begin $display("[%t] BEGIN %m", $time); run; #1 $display("[%t] END %m", $time); $finish; end
endmodule
