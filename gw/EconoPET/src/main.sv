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

`include "./src/common_pkg.svh"

import common_pkg::*;

module main (
    // FPGA
    input  logic sys_clock_i,   // 64 MHz clock (from PLL)

    // CPU
    input  logic cpu_reset_i,
    output logic cpu_reset_o,
    output logic cpu_be_o,
    output logic cpu_ready_o,
    output logic cpu_clock_o,
    input  logic cpu_irq_i,
    output logic cpu_irq_o,
    input  logic cpu_nmi_i,
    output logic cpu_nmi_o,

    input  logic [CPU_ADDR_WIDTH-1:0] cpu_addr_i,
    output logic [CPU_ADDR_WIDTH-1:0] cpu_addr_o,
    output logic                      cpu_addr_oe,

    input  logic [DATA_WIDTH-1:0] cpu_data_i,
    output logic [DATA_WIDTH-1:0] cpu_data_o,
    output logic                  cpu_data_oe,

    input  logic cpu_we_i,
    output logic cpu_we_o,
    output logic cpu_we_oe,

    // RAM
    output logic ram_addr_a10_o,
    output logic ram_addr_a11_o,
    output logic ram_addr_a15_o,
    output logic ram_addr_a16_o,
    output logic ram_oe_o,
    output logic ram_we_o,

    output logic io_oe_o,
    output logic pia1_cs_o,
    output logic pia2_cs_o,
    output logic via_cs_o,

    // Config from DIP switch
    input logic config_crt_i,       // Display type (0 = 12"/CRTC/20kHz, 1 = 9"/non-CRTC/15kHz)
    input logic config_keyboard_i,  // Keyboard type (0 = Business, 1 = Graphics)

    // Video
    input  logic graphic_i,         // VIA CA2 pin 39: Character ROM A10 (0 = graphics, 1 = text)
    output logic v_sync_o,
    output logic h_sync_o,
    output logic video_o,

    // Audio
    input  logic diag_i,
    input  logic via_cb2_i,
    output logic audio_o,

    // SPI buses
    input  logic spi0_cs_ni,        // (CS)  Chip Select (active low)
    input  logic spi0_sck_i,        // (SCK) Serial Clock
    input  logic spi0_sd_i,         // (SDI) Serial Data In (MCU -> FPGA)
    output logic spi0_sd_o,         // (SDO) Serial Data Out (FPGA -> MCU)
    
    input  logic spi1_cs_ni,        // (CS)  Chip Select (active low)
    input  logic spi1_sck_i,        // (SCK) Serial Clock
    input  logic spi1_sd_i,         // (SDI) Serial Data In (MCU -> FPGA)
    output logic spi1_sd_o,         // (SDO) Serial Data Out (FPGA -> MCU)

    output logic spi_stall_o        // Flow control for SPI (0 = Ready, 1 = Busy)
);
    // WB Bus Declarations

    logic [WB_ADDR_WIDTH-1:0] wb_addr;
    logic [   DATA_WIDTH-1:0] wb_din;
    logic [   DATA_WIDTH-1:0] wb_dout;
    logic                     wb_we;
    logic                     wb_cycle;
    logic                     wb_strobe;
    logic                     wb_stall;
    logic                     wb_ack;

    logic ram_wb_sel;
    logic reg_wb_sel;
    logic kbd_wb_sel;

    always_comb begin
        ram_wb_sel = 1'b0;
        reg_wb_sel = 1'b0;
        kbd_wb_sel = 1'b0;

        unique casez (wb_addr)
            {WB_RAM_BASE, {(WB_ADDR_WIDTH - $bits(WB_RAM_BASE)){1'b?}}}: ram_wb_sel = 1'b1;
            {WB_REG_BASE, {(WB_ADDR_WIDTH - $bits(WB_REG_BASE)){1'b?}}}: reg_wb_sel = 1'b1;
            {WB_KBD_BASE, {(WB_ADDR_WIDTH - $bits(WB_KBD_BASE)){1'b?}}}: kbd_wb_sel = 1'b1;
            default: begin end
        endcase
    end

    //
    // SPI <-> Wishbone Bridge
    //

    // TODO: At the moment, this is actually connected to the RP2040's SPI0 bus.  Rename or move?
    logic [WB_ADDR_WIDTH-1:0] spi1_addr;
    logic [   DATA_WIDTH-1:0] spi1_din;     // Peripheral -> SPI1 (WE=0)
    logic [   DATA_WIDTH-1:0] spi1_dout;    // SPI1 -> Peripheral (WE=1)
    logic                     spi1_we;
    logic                     spi1_cycle;
    logic                     spi1_strobe;
    logic                     spi1_stall;
    logic                     spi1_ack;

    spi1_controller spi1 (
        .wb_clock_i(sys_clock_i),
        .wb_addr_o(spi1_addr),
        .wb_data_i(spi1_din),
        .wb_data_o(spi1_dout),
        .wb_we_o(spi1_we),
        .wb_cycle_o(spi1_cycle),
        .wb_strobe_o(spi1_strobe),
        .wb_stall_i(spi1_stall),
        .wb_ack_i(spi1_ack),

        .spi_cs_ni(spi0_cs_ni),     // SPI CS_N
        .spi_sck_i(spi0_sck_i),     // SPI SCK
        .spi_sd_i (spi0_sd_i),      // SPI MCU TX  -> FPGA RX
        .spi_sd_o (spi0_sd_o),      // SPI FPGA TX -> MCU RX
        .spi_stall_o(spi_stall_o)   // Backpressure to MCU
    );

    // For now, IRQ is never driven by FPGA.
    assign cpu_irq_o   = 0;

    // For now, CPU always drives RWB, which is independent from RAM OE/WE.
    assign cpu_we_o    = 0;
    assign cpu_we_oe   = 0;

    logic cpu_wr_strobe;    // TODO: Rename? This is a 1-cycle pulse at end of write cycle (but ignores cpu_we_i)
    logic clk8_en;
    logic clk16_en;
    logic load_sr1;
    logic load_sr2;
    logic [0:0] grant;
    logic grant_valid;

    timing timing (
        .sys_clock_i(sys_clock_i),
        .cpu_be_o(cpu_be_o),
        .cpu_clock_o(cpu_clock_o),
        .cpu_wr_strobe_o(cpu_wr_strobe),
        .clk8_en_o(clk8_en),
        .clk16_en_o(clk16_en),
        .load_sr1_o(load_sr1),
        .load_sr2_o(load_sr2),
        .grant_o(grant),
        .grant_valid_o(grant_valid)
    );

    //
    // Wishbone <-> RAM Bridge
    //

    logic [DATA_WIDTH-1:0] ram_wb_din;
    logic                  ram_wb_stall;
    logic                  ram_wb_ack;

    logic [RAM_ADDR_WIDTH-1:0] ram_ctl_addr;    // Captured address for read/write cycle
    logic                      ram_ctl_oe;      // OE signal for read cycle
    logic                      ram_ctl_we;      // WE signal for write cycle
    logic [    DATA_WIDTH-1:0] ram_ctl_dout;    // FPGA -> RAM
    logic                      ram_ctl_doe;

    ram ram (
        .wb_clock_i(sys_clock_i),
        .wb_addr_i(wb_addr),
        .wb_data_i(wb_dout),
        .wb_data_o(ram_wb_din),
        .wb_we_i(wb_we),
        .wb_cycle_i(wb_cycle),
        .wb_strobe_i(wb_strobe),
        .wb_stall_o(ram_wb_stall),
        .wb_ack_o(ram_wb_ack),
        .wb_sel_i(ram_wb_sel),

        .ram_oe_o(ram_ctl_oe),
        .ram_we_o(ram_ctl_we),
        .ram_addr_o(ram_ctl_addr),
        .ram_data_i(cpu_data_i),
        .ram_data_o(ram_ctl_dout),
        .ram_data_oe(ram_ctl_doe)
    );

    //
    // Register File
    //

    logic [DATA_WIDTH-1:0] reg_wb_din;
    logic reg_wb_stall;
    logic reg_wb_ack;
    logic video_col_80_mode;
    logic [11:10] video_ram_mask;

    register_file register_file (
        .wb_clock_i(sys_clock_i),
        .wb_addr_i(wb_addr),
        .wb_data_i(wb_dout),
        .wb_data_o(reg_wb_din),
        .wb_we_i(wb_we),
        .wb_cycle_i(wb_cycle),
        .wb_strobe_i(wb_strobe),
        .wb_ack_o(reg_wb_ack),
        .wb_stall_o(reg_wb_stall),
        .wb_sel_i(reg_wb_sel),

        // Status register
        .video_graphic_i(graphic_i),
        .config_crt_i(config_crt_i),
        .config_keyboard_i(config_keyboard_i),

        // CPU control register
        .cpu_ready_o(cpu_ready_o),
        .cpu_reset_o(cpu_reset_o),
        .cpu_nmi_o(cpu_nmi_o),

        // Video control register
        .video_col_80_mode_o(video_col_80_mode),

        .video_ram_mask_o(video_ram_mask)
    );

    //
    // Address Decoding
    //

    logic ram_en;
    logic pia1_en;
    logic pia2_en;
    logic via_en;
    logic sid_en;
    logic io_en;
    logic crtc_en;
    logic is_vram;
    logic is_readonly;
    logic decoded_a15;
    logic decoded_a16;

    address_decoding address_decoding (
        .reset_i(cpu_reset_i),
        .sys_clock_i(sys_clock_i),
        
        .cpu_be_i(cpu_be_o),
        .cpu_wr_strobe_i(cpu_we_i && cpu_wr_strobe),
        .cpu_addr_i(cpu_addr_i),
        .cpu_data_i(cpu_data_i),

        .ram_en_o(ram_en),
        .pia1_en_o(pia1_en),
        .pia2_en_o(pia2_en),
        .via_en_o(via_en),
        .sid_en_o(sid_en),
        .io_en_o(io_en),
        .crtc_en_o(crtc_en),
        .is_vram_o(is_vram),

        .is_readonly_o(is_readonly),
        .decoded_a15_o(decoded_a15),
        .decoded_a16_o(decoded_a16)
    );

    //
    // Video
    //

    logic [WB_ADDR_WIDTH-1:0] video_addr;    // Captured address for read
    logic [   DATA_WIDTH-1:0] video_din;     // Peripheral -> Video (WE=0)
    logic [   DATA_WIDTH-1:0] video_dout;    // Video -> Peripheral (WE=1)
    logic                     video_we;
    logic                     video_cycle;
    logic                     video_strobe;
    logic                     video_stall;
    logic                     video_ack;

    logic [   DATA_WIDTH-1:0] crtc_dout;     // CRTC -> CPU
    logic                     crtc_oe;

    video video (
        // Wishbone controller used to fetch VRAM/VROM data
        .wb_clock_i(sys_clock_i),
        .wb_addr_o(video_addr),
        .wb_data_i(video_din),
        .wb_data_o(video_dout),
        .wb_we_o(video_we),
        .wb_cycle_o(video_cycle),
        .wb_strobe_o(video_strobe),
        .wb_stall_i(video_stall),
        .wb_ack_i(video_ack),

        // TODO: Wishbone peripheral to read back CRTC registers?
        .wb_addr_i(),
        .wb_we_i(),
        .wb_cycle_i(),
        .wb_strobe_i(),
        .wb_stall_o(),
        .wb_ack_o(),

        // Video timing
        .clk8_en_i(clk8_en),                // 8 MHz pixel clock for 40 column mode
        .clk16_en_i(clk16_en),              // 16 MHz pixel clock for 80 column mode

        .config_crt_i(config_crt_i),        // Controls polarity of video signals (0 = 12"/CRTC, 1 = 9"/non-CRTC)

        .cpu_reset_i(cpu_reset_i),
        .crtc_clk_en_i(cpu_wr_strobe),      // 1 MHz clock enable for 'sys_clock_i'
        .crtc_cs_i(crtc_en),                // Asserted by address decoding when 'cpu_addr_i' is in CRTC range
        .crtc_rs_i(cpu_addr_i[0]),          // Register select (0 = write address/read status, 1 = read addressed register)
        .crtc_we_i(cpu_we_i),               // Direction of data transfers (0 = reading from CRTC, 1 = writing to CRTC)
        .crtc_data_i(cpu_data_i),           // CPU -> CRTC
        .crtc_data_o(crtc_dout),            // CRTC -> CPU
        .crtc_data_oe(crtc_oe),             // Asserted when CPU is reading from CRTC

        // Dot Gen
        .load_sr1_i(load_sr1),
        .load_sr2_i(load_sr2),
        .col_80_mode_i(video_col_80_mode),  // 0 = 40 column mode, 1 = 80 column mode
        .graphic_i(graphic_i),
        .h_sync_o(h_sync_o),
        .v_sync_o(v_sync_o),
        .video_o(video_o)
    );

    //
    // Audio
    //

    audio audio(
        .reset_i(cpu_reset_i),
        .sys_clock_i(sys_clock_i),
        .clk1_en_i(load_sr1),
        .cpu_wr_en_i(cpu_wr_strobe && cpu_we_i),
        .sid_en_i(sid_en),
        .addr_i(cpu_addr_i[4:0]),
        .data_i(cpu_data_i),
        .diag_i(diag_i),
        .via_cb2_i(via_cb2_i),
        .audio_o(audio_o),

        // TODO: Read back from SID?
        .data_o()
    );

    //
    // USB Keyboard
    //

    logic [DATA_WIDTH-1:0] kbd_wb_din;
    logic                  kbd_wb_stall;
    logic                  kbd_wb_ack;
    logic [DATA_WIDTH-1:0] kbd_dout;
    logic                  kbd_doe;

    keyboard keyboard (
        .wb_clock_i(sys_clock_i),
        .wb_addr_i(wb_addr),
        .wb_data_i(wb_dout),
        .wb_data_o(kbd_wb_din),
        .wb_we_i(wb_we),
        .wb_cycle_i(wb_cycle),
        .wb_strobe_i(wb_strobe),
        .wb_stall_o(kbd_wb_stall),
        .wb_ack_o(kbd_wb_ack),
        .wb_sel_i(kbd_wb_sel),
        .cpu_be_i(cpu_be_o),
        .cpu_data_i(cpu_data_i),
        .cpu_data_o(kbd_dout),
        .cpu_data_oe(kbd_doe),
        .cpu_we_i(cpu_we_i),
        .pia1_cs_i(pia1_en),
        .pia2_cs_i(pia2_en),
        .via_cs_i(via_en),
        .rs_i(cpu_addr_i[VIA_RS_WIDTH-1:0])
    );

    //
    // Wishbone
    //

    wb_demux #(
        .COUNT(2)
    ) wb_demux (
        .wb_clock_i(sys_clock_i),

        // Wishbone controllers to demux
        .wbc_cycle_i({ spi1_cycle, video_cycle }),
        .wbc_strobe_i({ spi1_strobe, video_strobe }),
        .wbc_addr_i({ spi1_addr, video_addr }),
        .wbc_din_o({ spi1_din, video_din }),
        .wbc_dout_i({ spi1_dout, video_dout }),
        .wbc_we_i({ spi1_we, video_we }),
        .wbc_stall_o({ spi1_stall, video_stall }),
        .wbc_ack_o({ spi1_ack, video_ack }),

        // Wishbone bus
        .wb_addr_o(wb_addr),
        .wb_din_i(wb_din),
        .wb_dout_o(wb_dout),
        .wb_we_o(wb_we),
        .wb_cycle_o(wb_cycle),
        .wb_strobe_o(wb_strobe),
        .wb_stall_i(wb_stall),
        .wb_ack_i(wb_ack),

        // Control signals
        .wbc_grant_i(grant),
        .wbc_grant_valid_i(grant_valid)
    );

    wb_mux #(
        .COUNT(3)
    ) wb_mux (
        .wbp_sel_i({ ram_wb_sel, reg_wb_sel, kbd_wb_sel }),

        // Wishbone Bus
        .wb_din_o(wb_din),
        .wb_stall_o(wb_stall),
        .wb_ack_o(wb_ack),

        // Wishbone peripherals to mux
        .wbp_din_i({ ram_wb_din, reg_wb_din, kbd_wb_din }),
        .wbp_stall_i({ ram_wb_stall, reg_wb_stall, kbd_wb_stall }),
        .wbp_ack_i({ ram_wb_ack, reg_wb_ack, kbd_wb_ack })
    );

    //
    // Bus
    //

    always_comb begin
        if (kbd_doe) begin
            cpu_data_o = kbd_dout;
            cpu_data_oe = !cpu_we_i;
        end else if (ram_ctl_doe) begin
            cpu_data_o = ram_ctl_dout;
            cpu_data_oe = 1;
        end else begin
            cpu_data_o = 'x;
            cpu_data_oe = 0;
        end
    end

    wire cpu_rd_en = cpu_be_o && !cpu_we_i;
    wire cpu_wr_en = cpu_be_o &&  cpu_we_i && cpu_clock_o;

    assign io_oe_o   = io_en   && cpu_be_o && !kbd_doe;
    assign pia1_cs_o = pia1_en && cpu_be_o && !kbd_doe;
    assign pia2_cs_o = pia2_en && cpu_be_o;
    assign via_cs_o  =  via_en && cpu_be_o;

    assign ram_oe_o         = (cpu_rd_en && ram_en) || ram_ctl_oe;
    assign ram_we_o         = (cpu_wr_en && ram_en && !is_readonly) || ram_ctl_we;

    assign cpu_addr_oe      = !cpu_be_o;
    assign cpu_addr_o       = ram_ctl_addr[15:0];

    wire ram_addr_a10_mask = !is_vram | video_ram_mask[10] | video_col_80_mode;
    wire ram_addr_a11_mask = !is_vram | video_ram_mask[11];

    // When the CPU is driving the bus, apply masks to RAM A10/A11 to wrap video memory.
    assign ram_addr_a10_o = cpu_be_o
        ? cpu_addr_i[10] & ram_addr_a10_mask
        : cpu_addr_o[10];

    assign ram_addr_a11_o = cpu_be_o
        ? cpu_addr_i[11] & ram_addr_a11_mask
        : cpu_addr_o[11];

    // When the CPU is driving the bus, the control register at $FFF0 control
    // the memory mapping for the upper 64k expansion.
    assign ram_addr_a15_o = cpu_be_o ? decoded_a15 : cpu_addr_o[15];
    assign ram_addr_a16_o = cpu_be_o ? decoded_a16 : ram_ctl_addr[16];

    // synthesis off
    always_ff @(posedge sys_clock_i or negedge sys_clock_i) begin
        assert(!cpu_be_o || !ram_wb_stall) else $fatal(1, "WB<->RAM bridge must be stalled when CPU is driving bus");
        assert(!cpu_be_o || !ram_ctl_oe)   else $fatal(1, "WB<->RAM bridge must not assert OE when CPU is driving bus");
        assert(!cpu_be_o || !ram_ctl_we)   else $fatal(1, "WB<->RAM bridge must not assert WE when CPU is driving bus");
        assert(!cpu_be_o || !ram_wb_ack)   else $fatal(1, "WB<->RAM bridge must not assert ACK when CPU is driving bus");
        assert(!io_oe_o  ||  cpu_be_o)     else $fatal(1, "IO must not be active unless CPU is driving bus");
        assert(!io_oe_o  || !ram_oe_o)     else $fatal(1, "IO and RAM_OE must not drive bus at same time");
        assert(!io_oe_o  || !ram_we_o)     else $fatal(1, "IO and RAM_WE must not be active at same time");
    end
    // synthesis on
endmodule
