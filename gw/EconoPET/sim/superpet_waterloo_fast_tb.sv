// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

import common_pkg::*;

// Same boot test as superpet_waterloo_tb.sv, but overrides timing_6809's
// STRETCH_ROUNDS via defparam for faster simulated progress per wall-clock
// second. Functional verification only; use superpet_waterloo_tb.sv (which
// runs main.sv's actual configuration) for anything timing-sensitive.
module superpet_waterloo_fast_tb;

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

    top #(.CPU_STRETCH_ROUNDS(2)) top (
        .sys_clock_i(sys_clock),

        .cpu_be_o(cpu_be),
        .cpu_ready_o(cpu_ready),
        .cpu_reset_n_i(cpu_reset_n),
        .cpu_irq_n_i(1'b1),
        .cpu_nmi_n_i(1'b1),
        .cpu_reset_n_o(top_reset_n),
        .cpu_reset_n_oe(top_reset_n_oe),
        .cpu_clock_o(cpu_clock),
        .cpu_addr_i (bus_addr),
        .cpu_addr_o (top_addr),
        .cpu_addr_oe(top_addr_oe),
        .cpu_data_i (bus_data),
        .cpu_data_o (top_data),
        .cpu_data_oe(top_data_oe),
        .cpu_we_n_i (1'b1),
        .cpu_we_n_o (top_we_n),
        .cpu_we_n_oe(top_we_n_oe),

        .cpu_sync_i(1'b0),

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

    mock_sram mock_sram (
        .addr_i(ram_addr),
        .data_io(bus_data),
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
        .cpu_we_n_i(1'b1),

        .ram_oe_n_i(ram_oe_n_o),
        .ram_we_n_i(ram_we_n_o),

        .io_data_i(8'hFF),
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

    // PET screen code -> printable ASCII. 0-31 +64, 32-63 direct, graphics
    // glyphs '.'. Bit 7 is reverse video: shown lowercase.
    function automatic byte screen_to_ascii(input byte raw_code);
        byte code;
        byte c;
        bit  reverse;
        code = raw_code;
        reverse = code[7];
        c = byte'(code & 8'h7F);

        if (c < 8'd32) c = byte'(c + 8'd64);        // 0-31 -> '@'/A-Z/symbols
        else if (c < 8'd64) c = c;                  // 32-63 -> direct ASCII
        else return reverse ? "#" : ".";             // 64-127 -> graphics (no reliable glyph)

        // Reverse-video letters shown lowercase (only letters are affected;
        // digits/punctuation have no case to flip, left as-is).
        if (reverse && c >= "A" && c <= "Z") c = byte'(c - "A" + "a");
        return c;
    endfunction

    task static dump_screen_row(input int row);
        string line;
        int base;
        line = "";
        base = row * 80;
        for (int col = 0; col < 80; col++) begin
            line = { line, $sformatf("%c", screen_to_ascii(mock_sram.mem[17'(base + col)])) };
        end
        $display("[%t]   SCREEN[%0d]: %s", $time, row, line);
    endtask

    // PET business-keyboard matrix (row = this project's "column" 0-9,
    // selected via PIA1 PORTA; bit = this project's "row" 0-7, read via
    // PORTB). Values from VICE's data/PET/gtk3_buuk_sym.vkm (business
    // keyboard symbolic keymap) -- this project's keyboard.sv uses the
    // opposite row/column naming convention from the usual PET documentation
    // (KBD_COL_COUNT=10 is the PORTA-selected line, KBD_ROW_COUNT=8 is the
    // PORTB bit), so "column"/"row" below match keyboard.sv, not VICE's.
    task static press_key(
        input int unsigned column,
        input int unsigned row_bit
    );
        logic [KBD_ROW_COUNT-1:0] bitmap;
        bitmap = '1;
        bitmap[row_bit] = 1'b0;
        $display("[%t]   Key press: column=%0d row_bit=%0d", $time, column, row_bit);
        spi_write_at(common_pkg::wb_kbd_addr(column[KBD_COL_WIDTH-1:0]), bitmap);
        #200000;
        spi_write_at(common_pkg::wb_kbd_addr(column[KBD_COL_WIDTH-1:0]), 8'hFF);  // release
    endtask

    task static press_letter(input byte letter);
        case (letter)
            "a": press_key(3, 0);
            "b": press_key(6, 2);
            "c": press_key(6, 1);
            "d": press_key(3, 1);
            "e": press_key(5, 1);
            "f": press_key(2, 2);
            "g": press_key(3, 2);
            "h": press_key(2, 3);
            "i": press_key(4, 5);
            "j": press_key(3, 3);
            "k": press_key(2, 5);
            "l": press_key(3, 5);
            "m": press_key(8, 3);
            "n": press_key(7, 2);
            "o": press_key(5, 5);
            "p": press_key(4, 6);
            "q": press_key(5, 0);
            "r": press_key(4, 2);
            "s": press_key(2, 1);
            "t": press_key(5, 2);
            "u": press_key(5, 3);
            "v": press_key(7, 1);
            "w": press_key(4, 1);
            "x": press_key(8, 1);
            "y": press_key(4, 3);
            "z": press_key(7, 0);
            default: $display("[%t]   press_letter: no mapping for '%c'", $time, letter);
        endcase
    endtask

    task static press_return;
        press_key(3, 4);
    endtask

    // Assert that 'text' appears somewhere on the screen (same screen-code
    // mapping as dump_screen_row).
    task static assert_screen_contains(input string text);
        int tlen;
        bit found;
        tlen = text.len();
        found = 1'b0;
        for (int i = 0; !found && i + tlen <= 2000; i++) begin
            int match;
            match = 1;
            for (int j = 0; j < tlen; j++) begin
                if (screen_to_ascii(mock_sram.mem[17'(17'h08000 + i + j)]) != byte'(text[j]))
                    match = 0;
            end
            if (match) found = 1'b1;
        end
        if (!found) $fatal(1, "expected '%s' on screen -- boot did not reach the menu", text);
    endtask

    task static run;
        $display("[%t] BEGIN SuperPET Waterloo boot test (FAST/functional-only, STRETCH_ROUNDS=2)", $time);
        spi1_driver.reset;

        $display("[%t]   Loading Waterloo ROMs", $time);
        mock_sram.load_rom(17'h0A000, "waterloo-a000-bfff.970018-12.bin");
        mock_sram.load_rom(17'h0C000, "waterloo-c000-dfff.970019-12.bin");
        mock_sram.load_rom(17'h0E000, "waterloo-e000-ffff-970034-12.bin");

        mock_sram.fill(17'h08000, 17'h087CF, 8'h20);  // 80x25 screen, space-filled

        spi_write_at(common_pkg::wb_reg_addr(REG_CPU_SEL), 8'(CPU_SEL_SOFT_6809));

        $display("[%t]   Releasing reset via REG_CPU", $time);
        spi_write_at(common_pkg::wb_reg_addr(REG_CPU), 8'b0000_0000);

        // 600ms: past the scratch-buffer clear loop.
        for (int i = 0; i < 2400; i++) begin
            #250000;
            if (i % 40 == 0) $display("[%t]   ...still running (PC-ish addr=$%h, E=%b)",
                $time, top.main.mc6809_addr, top.main.cpu6809_e);
        end

        $display("[%t]   Captured screen RAM (all 25 rows) BEFORE keypress:", $time);
        for (int row = 0; row < 25; row++) dump_screen_row(row);

        // The Waterloo power-on menu must be on screen by now.
        assert_screen_contains("Waterloo microSystems");
        assert_screen_contains("Select :");

        // Press RETURN and see if anything changes.
        $display("[%t]   Pressing RETURN to test whether boot is waiting for input", $time);
        press_return;

        for (int i = 0; i < 400; i++) begin
            #250000;
            if (i % 40 == 0) $display("[%t]   ...post-keypress (PC-ish addr=$%h, E=%b)",
                $time, top.main.mc6809_addr, top.main.cpu6809_e);
        end

        $display("[%t]   Captured screen RAM (all 25 rows) AFTER keypress:", $time);
        for (int row = 0; row < 25; row++) dump_screen_row(row);

        $display("[%t] END SuperPET Waterloo boot test", $time);
    endtask

    `TB_INIT
endmodule
