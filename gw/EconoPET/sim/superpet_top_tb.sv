// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

import common_pkg::*;

// Standalone end-to-end smoke test for the SuperPET 6809 side.
//
// Deliberately does not reuse mock_system.sv/mock_cpu.sv (which drive the
// physical-6502 bus role via the m6502 soft core) -- in 6809 mode, the
// physical CPU is permanently tri-stated and the mc6809 core inside 'top'
// is the only active bus master, so there is nothing for a mock 6502 to
// drive. Instead this wires 'top' directly to mock_sram (a real,
// timing-accurate AS6C1008 model) and the SPI1 driver, mirroring
// mock_system.sv's non-CPU wiring.
module superpet_top_tb;
    // Phase 2 (below) validates full 16-bank $9000 switching. SRAM A12-A14
    // come from the shared bus (no dedicated FPGA pins), which main.sv
    // drives with the bank-translated address during the CPU window; the
    // ram_addr concatenation below mirrors that PCB wiring.

    bit sys_clock;
    clock_gen #(SYS_CLOCK_MHZ) sys_clock_gen (.clock_o(sys_clock));
    initial sys_clock_gen.start;

    logic [CPU_ADDR_WIDTH-1:0] bus_addr;
    wire  [DATA_WIDTH-1:0]     bus_data;
    logic bus_we_n;

    logic [DATA_WIDTH-1:0] bus_data_mux;
    logic                  bus_data_mux_oe;

    assign bus_data = bus_data_mux_oe ? bus_data_mux : {DATA_WIDTH{1'bz}};

    // Reset is a wire-OR net: the FPGA can assert it (top_reset_n/oe), and we
    // provide the external/manual side, exactly like mock_system.sv.
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
        .sys_clock_i(sys_clock),

        .cpu_be_o(cpu_be),
        .cpu_ready_o(cpu_ready),
        .cpu_reset_n_i(cpu_reset_n),
        .cpu_reset_n_o(top_reset_n),
        .cpu_reset_n_oe(top_reset_n_oe),
        .cpu_clock_o(cpu_clock),
        .cpu_addr_i (bus_addr),
        .cpu_addr_o (top_addr),
        .cpu_addr_oe(top_addr_oe),
        .cpu_data_i (bus_data),
        .cpu_data_o (top_data),
        .cpu_data_oe(top_data_oe),
        .cpu_we_n_i (1'b1),      // Physical CPU off-bus in 6809 mode
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

        // No mock_cpu in this testbench -- physical CPU role is inactive.
        .cpu_be_i(1'b0),
        .cpu_addr_i({CPU_ADDR_WIDTH{1'b0}}),
        .cpu_data_i({DATA_WIDTH{1'b0}}),
        .cpu_we_n_i(1'b1),

        .ram_oe_n_i(ram_oe_n_o),
        .ram_we_n_i(ram_we_n_o),

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

    task static run;
        logic [DATA_WIDTH-1:0] dout;

        $display("[%t] BEGIN SuperPET 6809 smoke test", $time);
        spi1_driver.reset;

        // Small 6809 test program at $0300:
        //   $0300: 86 42        LDA #$42
        //   $0302: B7 02 00     STA $0200
        //   $0305: 20 FE        BRA $0305 (infinite self-loop)
        $display("[%t]   Loading test program at $0300", $time);
        spi_write_at(common_pkg::wb_ram_addr(17'h00300), 8'h86);
        spi_write_at(common_pkg::wb_ram_addr(17'h00301), 8'h42);
        spi_write_at(common_pkg::wb_ram_addr(17'h00302), 8'hB7);
        spi_write_at(common_pkg::wb_ram_addr(17'h00303), 8'h02);
        spi_write_at(common_pkg::wb_ram_addr(17'h00304), 8'h00);
        spi_write_at(common_pkg::wb_ram_addr(17'h00305), 8'h20);
        spi_write_at(common_pkg::wb_ram_addr(17'h00306), 8'hFE);

        // Poison the target location.
        spi_write_at(common_pkg::wb_ram_addr(17'h00200), 8'h00);

        // MC6809 reset vector is at $FFFE/$FFFF (not $FFFC/$FFFD like 6502).
        $display("[%t]   Writing 6809 reset vector -> $0300", $time);
        spi_write_at(common_pkg::wb_ram_addr(17'h0FFFE), 8'h03);
        spi_write_at(common_pkg::wb_ram_addr(17'h0FFFF), 8'h00);

        // Select the soft 6809 (the power-on default is the physical CPU).
        $display("[%t]   Selecting soft 6809 (REG_CPU_SEL=%0d)", $time, CPU_SEL_SOFT_6809);
        spi_write_at(common_pkg::wb_reg_addr(REG_CPU_SEL), 8'(CPU_SEL_SOFT_6809));

        // REG_CPU defaults to reset-asserted at power-on (register_file.sv);
        // the FPGA actively holds cpu_reset_n_i low via cpu_reset_n_oe until
        // this is cleared -- overriding manual_reset_n entirely. Must clear
        // it via SPI, same as top_tb.sv's own cpu reset sequencing.
        $display("[%t]   Releasing reset via REG_CPU", $time);
        spi_write_at(common_pkg::wb_reg_addr(REG_CPU), 8'b0000_0000);

        // Budget generously for reset sync + 3 instructions.
        $display("[%t]   Waiting for program to execute", $time);
        #200000;

        // Peek RAM directly first -- race-free, sidesteps any SPI/CPU-slot
        // boundary timing in the read-back path itself.
        $display("[%t]   RAM[$0200] (direct peek) = $%02x (expect $42)", $time, mock_sram.mem[17'h00200]);
        `assert_equal(mock_sram.mem[17'h00200], 8'h42);

        spi_read_at(common_pkg::wb_ram_addr(17'h00200), dout);
        $display("[%t]   RAM[$0200] (SPI read) = $%02x (expect $42)", $time, dout);
        `assert_equal(dout, 8'h42);

        // --- Phase 2: SuperPET $9000-$9FFF bank switching ---
        //   $0300: 86 05        LDA #$05        ; select bank 5
        //   $0302: B7 EF FC     STA $EFFC
        //   $0305: 86 AA        LDA #$AA        ; write $AA to bank 5's $9000
        //   $0307: B7 90 00     STA $9000
        //   $030A: 86 00        LDA #$00        ; select bank 0
        //   $030C: B7 EF FC     STA $EFFC
        //   $030F: 86 BB        LDA #$BB        ; write $BB to bank 0's $9000
        //   $0311: B7 90 00     STA $9000
        //   $0314: 86 05        LDA #$05        ; select bank 5 again
        //   $0316: B7 EF FC     STA $EFFC
        //   $0319: B6 90 00     LDA $9000       ; read back -- should be $AA,
        //                                       ; not $BB, if bank 5's data
        //                                       ; survived switching away
        //                                       ; and back.
        //   $031C: B7 02 00     STA $0200
        //   $031F: 20 FE        BRA $031F (infinite self-loop)
        $display("[%t]   Phase 2: bank switching ($EFFC)", $time);
        spi_write_at(common_pkg::wb_ram_addr(17'h00300), 8'h86);
        spi_write_at(common_pkg::wb_ram_addr(17'h00301), 8'h05);
        spi_write_at(common_pkg::wb_ram_addr(17'h00302), 8'hB7);
        spi_write_at(common_pkg::wb_ram_addr(17'h00303), 8'hEF);
        spi_write_at(common_pkg::wb_ram_addr(17'h00304), 8'hFC);
        spi_write_at(common_pkg::wb_ram_addr(17'h00305), 8'h86);
        spi_write_at(common_pkg::wb_ram_addr(17'h00306), 8'hAA);
        spi_write_at(common_pkg::wb_ram_addr(17'h00307), 8'hB7);
        spi_write_at(common_pkg::wb_ram_addr(17'h00308), 8'h90);
        spi_write_at(common_pkg::wb_ram_addr(17'h00309), 8'h00);
        spi_write_at(common_pkg::wb_ram_addr(17'h0030A), 8'h86);
        spi_write_at(common_pkg::wb_ram_addr(17'h0030B), 8'h00);
        spi_write_at(common_pkg::wb_ram_addr(17'h0030C), 8'hB7);
        spi_write_at(common_pkg::wb_ram_addr(17'h0030D), 8'hEF);
        spi_write_at(common_pkg::wb_ram_addr(17'h0030E), 8'hFC);
        spi_write_at(common_pkg::wb_ram_addr(17'h0030F), 8'h86);
        spi_write_at(common_pkg::wb_ram_addr(17'h00310), 8'hBB);
        spi_write_at(common_pkg::wb_ram_addr(17'h00311), 8'hB7);
        spi_write_at(common_pkg::wb_ram_addr(17'h00312), 8'h90);
        spi_write_at(common_pkg::wb_ram_addr(17'h00313), 8'h00);
        spi_write_at(common_pkg::wb_ram_addr(17'h00314), 8'h86);
        spi_write_at(common_pkg::wb_ram_addr(17'h00315), 8'h05);
        spi_write_at(common_pkg::wb_ram_addr(17'h00316), 8'hB7);
        spi_write_at(common_pkg::wb_ram_addr(17'h00317), 8'hEF);
        spi_write_at(common_pkg::wb_ram_addr(17'h00318), 8'hFC);
        spi_write_at(common_pkg::wb_ram_addr(17'h00319), 8'hB6);
        spi_write_at(common_pkg::wb_ram_addr(17'h0031A), 8'h90);
        spi_write_at(common_pkg::wb_ram_addr(17'h0031B), 8'h00);
        spi_write_at(common_pkg::wb_ram_addr(17'h0031C), 8'hB7);
        spi_write_at(common_pkg::wb_ram_addr(17'h0031D), 8'h02);
        spi_write_at(common_pkg::wb_ram_addr(17'h0031E), 8'h00);
        spi_write_at(common_pkg::wb_ram_addr(17'h0031F), 8'h20);
        spi_write_at(common_pkg::wb_ram_addr(17'h00320), 8'hFE);

        spi_write_at(common_pkg::wb_ram_addr(17'h00200), 8'h00);  // poison

        $display("[%t]   Re-asserting and releasing reset for phase 2", $time);
        spi_write_at(common_pkg::wb_reg_addr(REG_CPU), 8'b0000_0010);
        #20000;
        spi_write_at(common_pkg::wb_reg_addr(REG_CPU), 8'b0000_0000);

        #800000;

        $display("[%t]   RAM[$0200] (direct peek) = $%02x (expect $AA)", $time, mock_sram.mem[17'h00200]);
        `assert_equal(mock_sram.mem[17'h00200], 8'hAA);

        $display("[%t] END SuperPET 6809 smoke test", $time);
    endtask

    `TB_INIT
endmodule
