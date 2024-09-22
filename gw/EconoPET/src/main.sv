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

    // Video
    input  logic config_crt_i,  // Controls polarity of video signals (0 = 12"/CRTC, 1 = 9"/non-CRTC)
    input  logic graphic_i,     // VIA CA2 pin 39: Character ROM A10 (0 = graphics, 1 = text)
    output logic v_sync_o,
    output logic h_sync_o,
    output logic video_o,

    // SPI1 bus
    input  logic spi1_cs_ni,  // (CS)  Chip Select (active low)
    input  logic spi1_sck_i,  // (SCK) Serial Clock
    input  logic spi1_sd_i,   // (SDI) Serial Data In (MCU -> FPGA)
    output logic spi1_sd_o,   // (SDO) Serial Data Out (FPGA -> MCU)
    output logic spi_stall_o
);
    logic [WB_ADDR_WIDTH-1:0] wb_addr;
    logic [   DATA_WIDTH-1:0] wb_dout;
    logic [   DATA_WIDTH-1:0] wb_din;
    logic                     wb_we;
    logic                     wb_cycle;
    logic                     wb_strobe;
    logic                     wb_stall;
    logic                     wb_ack;

    //
    // SPI <-> Wishbone Bridge
    //

    logic [WB_ADDR_WIDTH-1:0] spi1_addr;
    logic [   DATA_WIDTH-1:0] spi1_dout;
    logic                     spi1_we;
    logic                     spi1_cycle;
    logic                     spi1_strobe;
    logic                     spi1_stall;

    spi1_controller spi1 (
        .wb_clock_i(sys_clock_i),
        .wb_addr_o(spi1_addr),
        .wb_data_o(spi1_dout),
        .wb_data_i(wb_dout),
        .wb_we_o(spi1_we),
        .wb_cycle_o(spi1_cycle),
        .wb_strobe_o(spi1_strobe),
        .wb_stall_i(spi1_stall),
        .wb_ack_i(wb_ack),

        .spi_cs_ni(spi1_cs_ni),     // SPI CS_N
        .spi_sck_i(spi1_sck_i),     // SPI SCK
        .spi_sd_i (spi1_sd_i),      // SPI MCU TX  -> FPGA RX
        .spi_sd_o (spi1_sd_o),      // SPI FPGA TX -> MCU RX
        .spi_stall_o(spi_stall_o)   // Backpressure to MCU
    );

    // For now, outgoing CPU control signals are constant.
    assign cpu_irq_o   = 0;
    assign cpu_nmi_o   = 0;

    // For now, CPU always drives RWB, which is independent from RAM OE/WE.
    assign cpu_we_o    = 0;
    assign cpu_we_oe   = 0;

    logic clk1_en;
    logic clk8_en;
    logic clk16_en;

    timing timing (
        .clock_i(sys_clock_i),
        .clk1_en_o(clk1_en),
        .clk8_en_o(clk8_en),
        .clk16_en_o(clk16_en)
    );

    logic cpu_strobe;
    logic cpu_grant_en;

    cpu cpu (
        .sys_clock_i(sys_clock_i),
        .cpu_grant_i(cpu_grant_en),
        .cpu_be_o(cpu_be_o),
        .cpu_clock_o(cpu_clock_o),
        .cpu_strobe_o(cpu_strobe)
    );

    //
    // Wishbone <-> RAM Bridge
    //

    logic [DATA_WIDTH-1:0] ram_wb_dout;
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
        .wb_data_i(wb_din),
        .wb_data_o(ram_wb_dout),
        .wb_we_i(wb_we),
        .wb_cycle_i(wb_cycle),
        .wb_strobe_i(wb_strobe),
        .wb_stall_o(ram_wb_stall),
        .wb_ack_o(ram_wb_ack),

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

    logic [DATA_WIDTH-1:0] reg_wb_dout;
    logic reg_wb_stall;
    logic reg_wb_ack;

    register_file register_file (
        .wb_clock_i(sys_clock_i),
        .wb_addr_i(wb_addr),
        .wb_data_i(wb_din),
        .wb_data_o(reg_wb_dout),
        .wb_we_i(wb_we),
        .wb_cycle_i(wb_cycle),
        .wb_strobe_i(wb_strobe),
        .wb_ack_o(reg_wb_ack),
        .wb_stall_o(reg_wb_stall),

        .cpu_ready_o(cpu_ready_o),
        .cpu_reset_o(cpu_reset_o)
    );

    //
    // Address Decoding
    //

    logic ram_en;
    logic pia1_en;
    logic pia2_en;
    logic via_en;
    logic io_en;
    logic crtc_en;

    address_decoding address_decoding (
        .cpu_be_i(cpu_be_o),
        .addr_i(cpu_addr_i),

        .ram_en_o(ram_en),
        .pia1_en_o(pia1_en),
        .pia2_en_o(pia2_en),
        .via_en_o(via_en),
        .io_en_o(io_en),
        .crtc_en_o(crtc_en),

        // Not yet used
        .sid_en_o(),
        .magic_en_o(),
        .is_mirrored_o(),
        .is_readonly_o()
    );

    //
    // Video
    //

    logic [WB_ADDR_WIDTH-1:0] video_addr;    // Captured address for read
    logic [   DATA_WIDTH-1:0] video_dout;    // Video -> FPGA
    logic                     video_we;
    logic                     video_cycle;
    logic                     video_strobe;
    logic                     video_stall;

    logic [   DATA_WIDTH-1:0] crtc_dout;     // CRTC -> CPU
    logic                     crtc_oe;
    logic                     col_80_mode_i = 1'b1;     // TODO: Use register file

    video video (
        // Video timing
        .clk1_en_i(clk1_en),                // 1 MHz character clock enable
        .clk8_en_i(clk8_en),                // 8 MHz pixel clock for 40 column mode
        .clk16_en_i(clk16_en),              // 16 MHz pixel clock for 80 column mode

        .config_crt_i(config_crt_i),        // Controls polarity of video signals (0 = 12"/CRTC, 1 = 9"/non-CRTC)

        // Wishbone controller used to fetch VRAM/VROM data
        .wb_clock_i(sys_clock_i),
        .wb_addr_o(video_addr),
        .wb_data_i(wb_dout),
        .wb_data_o(video_dout),
        .wb_we_o(video_we),
        .wb_cycle_o(video_cycle),
        .wb_strobe_o(video_strobe),
        .wb_stall_i(video_stall),
        .wb_ack_i(wb_ack),

        .cpu_reset_i(cpu_reset_i),
        .crtc_clk_en_i(cpu_strobe),         // 1 MHz clock enable for 'sys_clock_i'
        .crtc_cs_i(crtc_en),                // Asserted by address decoding when 'cpu_addr_i' is in CRTC range
        .crtc_rs_i(cpu_addr_i[0]),          // Register select (0 = write address/read status, 1 = read addressed register)
        .crtc_we_i(cpu_we_i),               // Direction of data transfers (0 = reading from CRTC, 1 = writing to CRTC)
        .crtc_data_i(cpu_data_i),           // CPU -> CRTC
        .crtc_data_o(crtc_dout),            // CRTC -> CPU
        .crtc_data_oe(crtc_oe),             // Asserted when CPU is reading from CRTC
        .col_80_mode_i(col_80_mode_i),      // 0 = 40 column mode, 1 = 80 column mode
        .graphic_i(graphic_i),
        .h_sync_o(h_sync_o),
        .v_sync_o(v_sync_o),
        .video_o(video_o)
    );

    //
    // USB Keyboard
    //

    logic                  kbd_wb_stall;
    logic                  kbd_wb_ack;
    logic [DATA_WIDTH-1:0] kbd_dout;
    logic                  kbd_doe;

    keyboard keyboard (
        .wb_clock_i(sys_clock_i),
        .wb_addr_i(wb_addr),
        .wb_data_i(wb_din),
        .wb_data_o(),           // We do not currently support reading back from the keyboard
        .wb_we_i(wb_we),
        .wb_cycle_i(wb_cycle),
        .wb_strobe_i(wb_strobe),
        .wb_stall_o(kbd_wb_stall),
        .wb_ack_o(kbd_wb_ack),
        .pia1_rs_i(cpu_addr_i[1:0]),
        .pia1_cs_i(pia1_en),
        .cpu_valid_strobe_i(cpu_valid_strobe),
        .cpu_done_strobe_i(cpu_done_strobe),
        .cpu_data_i(cpu_data_i),
        .cpu_data_o(kbd_dout),
        .cpu_data_oe(kbd_doe),
        .cpu_we_i(cpu_we_i)
    );

    //
    // Wishbone
    //

    assign wb_dout  = ram_wb_dout;
    assign wb_stall = ram_wb_stall | reg_wb_stall | kbd_wb_stall;
    assign wb_ack   = ram_wb_ack | reg_wb_ack | kbd_wb_ack;

    arbiter arbiter (
        .wb_clock_i(sys_clock_i),
        .wb_addr_o(wb_addr),
        .wb_data_o(wb_din),
        .wb_we_o(wb_we),
        .wb_cycle_o(wb_cycle),
        .wb_strobe_o(wb_strobe),
        .wb_stall_i(wb_stall),
        .wb_ack_i(wb_ack),

        .spi1_addr_i(spi1_addr),
        .spi1_data_i(spi1_dout),
        .spi1_we_i(spi1_we),
        .spi1_cycle_i(spi1_cycle),
        .spi1_strobe_i(spi1_strobe),
        .spi1_stall_o(spi1_stall),

        .video_addr_i(video_addr),
        .video_data_i(wb_din),
        .video_we_i(video_we),
        .video_cycle_i(video_cycle),
        .video_strobe_i(video_strobe),
        .video_stall_o(video_stall),

        .clk8_en_i(clk8_en),
        .cpu_grant_en_o(cpu_grant_en)
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
    assign ram_we_o         = (cpu_wr_en && ram_en) || ram_ctl_we;

    assign cpu_addr_oe      = !cpu_be_o;
    assign cpu_addr_o       = ram_ctl_addr[15:0];

    assign ram_addr_a10_o   = cpu_be_o ? cpu_addr_i[10] : cpu_addr_o[10];
    assign ram_addr_a11_o   = cpu_be_o ? cpu_addr_i[11] : cpu_addr_o[11];
    assign ram_addr_a15_o   = cpu_be_o ? cpu_addr_i[15] : cpu_addr_o[15];
    assign ram_addr_a16_o   = cpu_be_o ? 1'b0 : ram_ctl_addr[16];

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
