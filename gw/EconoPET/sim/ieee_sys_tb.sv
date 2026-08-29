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
module ieee_sys_tb;
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
        // Tied off, not fed from bus_addr: no physical CPU exists in this
        // bench, and the feedback (cpu_addr_o -> bus -> cpu_addr_i ->
        // active_cpu_addr mux) is a false combinational loop.
        .cpu_addr_i ({CPU_ADDR_WIDTH{1'b0}}),
        .cpu_addr_o (top_addr),
        .cpu_addr_oe(top_addr_oe),
        .cpu_data_i (bus_data),
        .cpu_data_o (top_data),
        .cpu_data_oe(top_data_oe),
        .cpu_we_n_i (1'b1),      // Physical CPU off-bus in 6809 mode
        .cpu_we_n_o (top_we_n),
        .cpu_we_n_oe(top_we_n_oe),

        // No physical CPU: tie the interrupt pads inactive. Left unconnected
        // these read 0 under Verilator, asserting IRQ to the soft core forever.
        .cpu_irq_n_i(1'b1),
        .cpu_nmi_n_i(1'b1),
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

        // No mock_cpu in this testbench -- physical CPU role is inactive.
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
        logic [DATA_WIDTH-1:0] st, rx, marker;
        int i;

        $display("[%t] BEGIN IEEE full-system test (soft 6809 -> snoop -> fabric)", $time);
        spi1_driver.reset;

        // 6809 program at $0300: the PET kernal's ATN send of LISTEN $28,
        // register-exact (VIA $E840 PB bit2 = ATN out; PIA2 $E822 = ~DIO;
        // PIA2 $E823 CRB CB2 = DAV). Then poll NDAC-in and park.
        spi_write_at(common_pkg::wb_ram_addr(17'h00300), 8'h86);
        spi_write_at(common_pkg::wb_ram_addr(17'h00301), 8'h00);
        spi_write_at(common_pkg::wb_ram_addr(17'h00302), 8'hB7);
        spi_write_at(common_pkg::wb_ram_addr(17'h00303), 8'hE8);
        spi_write_at(common_pkg::wb_ram_addr(17'h00304), 8'h23);
        spi_write_at(common_pkg::wb_ram_addr(17'h00305), 8'h86);
        spi_write_at(common_pkg::wb_ram_addr(17'h00306), 8'hFF);
        spi_write_at(common_pkg::wb_ram_addr(17'h00307), 8'hB7);
        spi_write_at(common_pkg::wb_ram_addr(17'h00308), 8'hE8);
        spi_write_at(common_pkg::wb_ram_addr(17'h00309), 8'h22);
        spi_write_at(common_pkg::wb_ram_addr(17'h0030A), 8'h86);
        spi_write_at(common_pkg::wb_ram_addr(17'h0030B), 8'h3C);
        spi_write_at(common_pkg::wb_ram_addr(17'h0030C), 8'hB7);
        spi_write_at(common_pkg::wb_ram_addr(17'h0030D), 8'hE8);
        spi_write_at(common_pkg::wb_ram_addr(17'h0030E), 8'h23);
        spi_write_at(common_pkg::wb_ram_addr(17'h0030F), 8'h86);
        spi_write_at(common_pkg::wb_ram_addr(17'h00310), 8'h06);
        spi_write_at(common_pkg::wb_ram_addr(17'h00311), 8'hB7);
        spi_write_at(common_pkg::wb_ram_addr(17'h00312), 8'hE8);
        spi_write_at(common_pkg::wb_ram_addr(17'h00313), 8'h42);
        spi_write_at(common_pkg::wb_ram_addr(17'h00314), 8'h86);
        spi_write_at(common_pkg::wb_ram_addr(17'h00315), 8'h02);
        spi_write_at(common_pkg::wb_ram_addr(17'h00316), 8'hB7);
        spi_write_at(common_pkg::wb_ram_addr(17'h00317), 8'hE8);
        spi_write_at(common_pkg::wb_ram_addr(17'h00318), 8'h40);
        spi_write_at(common_pkg::wb_ram_addr(17'h00319), 8'h86);
        spi_write_at(common_pkg::wb_ram_addr(17'h0031A), 8'hD7);
        spi_write_at(common_pkg::wb_ram_addr(17'h0031B), 8'hB7);
        spi_write_at(common_pkg::wb_ram_addr(17'h0031C), 8'hE8);
        spi_write_at(common_pkg::wb_ram_addr(17'h0031D), 8'h22);
        spi_write_at(common_pkg::wb_ram_addr(17'h0031E), 8'h86);
        spi_write_at(common_pkg::wb_ram_addr(17'h0031F), 8'h34);
        spi_write_at(common_pkg::wb_ram_addr(17'h00320), 8'hB7);
        spi_write_at(common_pkg::wb_ram_addr(17'h00321), 8'hE8);
        spi_write_at(common_pkg::wb_ram_addr(17'h00322), 8'h23);
        spi_write_at(common_pkg::wb_ram_addr(17'h00323), 8'hB6);
        spi_write_at(common_pkg::wb_ram_addr(17'h00324), 8'hE8);
        spi_write_at(common_pkg::wb_ram_addr(17'h00325), 8'h40);
        spi_write_at(common_pkg::wb_ram_addr(17'h00326), 8'h84);
        spi_write_at(common_pkg::wb_ram_addr(17'h00327), 8'h01);
        spi_write_at(common_pkg::wb_ram_addr(17'h00328), 8'h27);
        spi_write_at(common_pkg::wb_ram_addr(17'h00329), 8'hF9);
        spi_write_at(common_pkg::wb_ram_addr(17'h0032A), 8'h86);
        spi_write_at(common_pkg::wb_ram_addr(17'h0032B), 8'h3C);
        spi_write_at(common_pkg::wb_ram_addr(17'h0032C), 8'hB7);
        spi_write_at(common_pkg::wb_ram_addr(17'h0032D), 8'hE8);
        spi_write_at(common_pkg::wb_ram_addr(17'h0032E), 8'h23);
        spi_write_at(common_pkg::wb_ram_addr(17'h0032F), 8'h86);
        spi_write_at(common_pkg::wb_ram_addr(17'h00330), 8'h5A);
        spi_write_at(common_pkg::wb_ram_addr(17'h00331), 8'hB7);
        spi_write_at(common_pkg::wb_ram_addr(17'h00332), 8'h02);
        spi_write_at(common_pkg::wb_ram_addr(17'h00333), 8'h00);
        spi_write_at(common_pkg::wb_ram_addr(17'h00334), 8'h20);
        spi_write_at(common_pkg::wb_ram_addr(17'h00335), 8'hFE);
        spi_write_at(common_pkg::wb_ram_addr(17'h00200), 8'h00);   // poison marker
        spi_write_at(common_pkg::wb_ram_addr(17'h0FFFE), 8'h03);
        spi_write_at(common_pkg::wb_ram_addr(17'h0FFFF), 8'h00);

        spi_write_at(common_pkg::wb_reg_addr(REG_CPU_SEL), 8'(CPU_SEL_SOFT_6809));
        spi_write_at(common_pkg::wb_ieee_addr(IEEE_REG_CTRL), 8'h01);   // enable
        spi_write_at(common_pkg::wb_reg_addr(REG_CPU), 8'b0000_0000);   // release

        // Poll fabric status over SPI like the firmware does; print the same
        // fields as the F8 debug line for direct hardware comparison.
        st = '0;
        for (i = 0; i < 200; i++) begin
            #100000;   // 100us
            spi_read_at(common_pkg::wb_ieee_addr(IEEE_REG_STATUS), st);
            if (st[0]) i = 200;                  // RX has a byte (no 'break': iverilog)
        end
        $display("[%t]   ST%02X (atn=%b rx_atn=%b rx_avail=%b)", $time, st, st[4], st[1], st[0]);

        `assert_equal(st[4], 1'b1)               // fabric saw ATN
        `assert_equal(st[0], 1'b1)               // command byte captured
        `assert_equal(st[1], 1'b1)               // ... flagged as ATN command

        spi_read_at(common_pkg::wb_ieee_addr(IEEE_REG_RX), rx);
        $display("[%t]   RX head = $%02X (expect $28 LISTEN 8)", $time, rx);
        `assert_equal(rx, 8'h28)

        // Acceptor completed the handshake (NDAC released), so the CPU escaped
        // its poll loop and wrote the marker.
        #2000000;
        spi_read_at(common_pkg::wb_ram_addr(17'h00200), marker);
        $display("[%t]   marker = $%02X (expect $5A: CPU escaped NDAC poll)", $time, marker);
        `assert_equal(marker, 8'h5A)

        $display("[%t] END IEEE full-system test", $time);
    endtask

    `TB_INIT
endmodule
