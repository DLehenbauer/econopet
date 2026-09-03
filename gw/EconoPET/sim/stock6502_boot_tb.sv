// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

import common_pkg::*;

// Boot test for the VIRTUAL (soft) 6502 side of the in-fabric CPU mux.
// Phase 1 parks the core in a JMP-self loop (the boot-menu flow), then phase 2
// re-resets it into the real BASIC-4/editor/kernal ROM set and runs the stock
// boot until the "COMMODORE" banner lands in video RAM. cpu_sync_i is tied
// high (the pad floats with the socket empty; that must be harmless for soft
// CPUs). Wiring mirrors superpet_top_tb (top -> mock_sram + SPI1).
module stock6502_boot_tb;
    // Upper bound on the boot, not the time actually spent: the run stops as
    // soon as READY appears (~1.4s).
    localparam int BOOT_BUDGET_MS = 2200;
    bit sys_clock;
    clock_gen #(SYS_CLOCK_MHZ) sys_clock_gen (.clock_o(sys_clock));
    initial sys_clock_gen.start;

    logic [CPU_ADDR_WIDTH-1:0] bus_addr;
    wire  [DATA_WIDTH-1:0]     bus_data;
    logic bus_we_n;

    logic [DATA_WIDTH-1:0] bus_data_mux;
    logic                  bus_data_mux_oe;
    assign bus_data = bus_data_mux_oe ? bus_data_mux : {DATA_WIDTH{1'bz}};

    logic [DATA_WIDTH-1:0] ram_data;
    logic                  ram_data_oe;

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

    top top (
        `TOP_UNUSED_PORTS
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

        .cpu_sync_i(1'b1),  // floats high with the socket empty

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

    task static spi_read (output logic [DATA_WIDTH-1:0] data_o);
        spi1_driver.read_next;
        data_o = spi_rx_data;
    endtask

    task static spi_read_at (
        input  logic [WB_ADDR_WIDTH-1:0] addr_i,
        output logic [   DATA_WIDTH-1:0] data_o
    );
        spi1_driver.read_at(addr_i);
        spi_read(data_o);
    endtask

    task static spi_write_at (
        input logic [WB_ADDR_WIDTH-1:0] addr_i,
        input logic [   DATA_WIDTH-1:0] data_i
    );
        spi1_driver.write_at(addr_i, data_i);
    endtask

    logic [7:0] kernal_vec_lo, kernal_vec_hi;

    task static run;
        int spaces;
        bit found;
        $display("[%t] BEGIN stock BASIC-4 boot on soft 6502", $time);
        spi1_driver.reset;

        $display("[%t]   Loading BASIC-4 ROM set", $time);
        mock_sram.load_rom(17'h0B000, "basic-4-b000.901465-23.bin");
        mock_sram.load_rom(17'h0C000, "basic-4-c000.901465-20.bin");
        mock_sram.load_rom(17'h0D000, "basic-4-d000.901465-21.bin");
        mock_sram.load_rom(17'h0E000, "edit-4-40-n-60Hz.901499-01.bin");
        mock_sram.load_rom(17'h0F000, "kernal-4.901465-22.bin");
        // VROM window ($E800-$EFFF); the 2KB edit ROM above ends at $E7FF.
        // Only needed so the end-of-test screen dump can expand real glyphs.
        mock_sram.load_rom(17'h0E800, "characters-2.901447-10.bin");

        mock_sram.fill(17'h08000, 17'h087FF, 8'hAA);   // VRAM sentinel

        // Pre-init main RAM so no boot code reads x-state bytes.
        mock_sram.fill(17'h00000, 17'h002FF, 8'h01);
        mock_sram.fill(17'h00303, 17'h07FFF, 8'h01);

        // Shorten CINT's ~2.3s bell delay (LDA $03EC -> LDA #$01 / NOP);
        // bell writes stay intact.
        mock_sram.mem[17'h0E671] = 8'hA9;
        mock_sram.mem[17'h0E672] = 8'h01;
        mock_sram.mem[17'h0E673] = 8'hEA;
        kernal_vec_lo = mock_sram.mem[17'h0FFFC];
        kernal_vec_hi = mock_sram.mem[17'h0FFFD];

        // Phase 1: park the core in a JMP-self loop, as the boot menu does,
        // so phase 2 is a re-reset of a live core.
        spi_write_at(common_pkg::wb_ram_addr(17'h00300), 8'h4C);  // JMP $0300
        spi_write_at(common_pkg::wb_ram_addr(17'h00301), 8'h00);
        spi_write_at(common_pkg::wb_ram_addr(17'h00302), 8'h03);
        spi_write_at(common_pkg::wb_ram_addr(17'h0FFFC), 8'h00);
        spi_write_at(common_pkg::wb_ram_addr(17'h0FFFD), 8'h03);
        spi_write_at(common_pkg::wb_reg_addr(REG_CPU_SEL), 8'(CPU_SEL_SOFT_6502));
        spi_write_at(common_pkg::wb_reg_addr(REG_CPU), 8'b0000_0011);
        #20000;
        spi_write_at(common_pkg::wb_reg_addr(REG_CPU), 8'b0000_0001);
        #3000000;   // run the loop ~3ms
        begin
            int in_loop; in_loop = 0;
            for (int k = 0; k < 8; k++) begin
                #10000;
                if (top.main.m6502_addr >= 16'h0300 && top.main.m6502_addr <= 16'h0302) in_loop++;
            end
            $display("[%t]   phase1: %0d/8 samples in the JMP loop", $time, in_loop);
            if (in_loop < 6) $fatal(1, "phase1 broken: first run of the soft 6502 is not looping at $0300");
        end

        // PHASE 2: rewrite the vector to the kernal (ROMs already loaded),
        // then replay pet_reset()'s exact sequence.
        spi_write_at(common_pkg::wb_ram_addr(17'h0FFFC), kernal_vec_lo);
        spi_write_at(common_pkg::wb_ram_addr(17'h0FFFD), kernal_vec_hi);
        $display("[%t]   Re-reset (pet_reset sequence) into stock ROMs", $time);
        spi_write_at(common_pkg::wb_reg_addr(REG_CPU_SEL), 8'(CPU_SEL_SOFT_6502));
        spi_write_at(common_pkg::wb_reg_addr(REG_CPU), 8'b0000_0000);  // ready0 reset0
        #4000;
        spi_write_at(common_pkg::wb_reg_addr(REG_CPU), 8'b0000_0010);  // ready0 reset1
        #4000;
        spi_write_at(common_pkg::wb_reg_addr(REG_CPU), 8'b0000_0001);  // ready1 reset0

        // Early fetch trace.
        for (int us100 = 0; us100 < 60; us100++) begin
            #100000;     // 100 us
            $display("[%t]   early t=%0dus addr=$%h rw=%b", $time, (us100+1)*100,
                top.main.m6502_addr, top.main.m6502_rw);
        end

        // Poll for the READY prompt and stop as soon as it lands, rather than
        // burning the whole budget: READY appears around 1.4s, so a fixed 2.2s
        // wait spent a third of the run watching an idle prompt. The budget is
        // now only an upper bound. ('break' is avoided -- iverilog rejects it.)
        found = 1'b0;
        for (int ms = 0; ms < BOOT_BUDGET_MS && !found; ms += 25) begin
            #25000000;   // 25 ms
            found = mock_sram.screen_contains(17'h08000, 40, 25, "READY.");
            if (!found && ms % 100 == 0) begin
                spaces = 0;
                for (int i = 0; i < 40; i++)
                    if (mock_sram.mem[17'(17'h08000 + i)] == 8'h20) spaces++;
                $display("[%t]   t=%0dms addr=$%h row0_spaces=%0d vram0=%02x %02x %02x %02x",
                    $time, ms, top.main.active_cpu_addr, spaces,
                    mock_sram.mem[17'h08000], mock_sram.mem[17'h08001],
                    mock_sram.mem[17'h08002], mock_sram.mem[17'h08003]);
            end
        end

        // A boot that reached the READY prompt shows all three: the banner, the
        // result of BASIC's RAM sizing, and the prompt itself. Checking only the
        // banner would pass on a boot that stalled part-way through init.
        found = mock_sram.screen_contains(17'h08000, 40, 25, "COMMODORE BASIC 4.0")
             && mock_sram.screen_contains(17'h08000, 40, 25, "BYTES FREE")
             && mock_sram.screen_contains(17'h08000, 40, 25, "READY.");

        if (!found) begin
            mock_sram.dump_screen(17'h08000, 17'h0E800, 40, 25, "");
            $fatal(1, "stock boot did not reach the BASIC READY prompt");
        end
        mock_sram.dump_screen(17'h08000, 17'h0E800, 40, 25,
                              "outflow/stock6502_boot_tb.pgm");
        $display("[%t] END stock BASIC-4 boot (banner, RAM size and READY seen)", $time);
    endtask

    // `TB_INIT without $dumpvars (a full-boot VCD is too large).
    initial begin
        $display("[%t] BEGIN %m", $time);
        run;
        #1 $display("[%t] END %m", $time);
        $finish;
    end
endmodule
