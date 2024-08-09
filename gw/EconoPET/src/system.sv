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

module system (
    // Wishbone B4 peripheral
    // (See https://cdn.opencores.org/downloads/wbspec_b4.pdf)
    input  logic                     wb_clock_i,
    input  logic [WB_ADDR_WIDTH-1:0] wb_addr_i,
    input  logic [   DATA_WIDTH-1:0] wb_data_i,
    output logic [   DATA_WIDTH-1:0] wb_data_o,
    input  logic                     wb_we_i,
    input  logic                     wb_cycle_i,
    input  logic                     wb_strobe_i,
    output logic                     wb_stall_o,
    output logic                     wb_ack_o,

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

    // IO
    output logic io_oe_o,
    output logic pia1_cs_o,
    output logic pia2_cs_o,
    output logic via_cs_o
);
    // For now, outgoing CPU control signals are constant.
    assign cpu_ready_o = 1;
    assign cpu_reset_o = 0;
    assign cpu_irq_o   = 0;
    assign cpu_nmi_o   = 0;

    // For now, CPU always drives RWB, which is not connected to RAM OE/WE.
    assign cpu_we_o    = 0;
    assign cpu_we_oe   = 0;

    logic cpu_grant;
    logic wb_grant;

    timing timing (
        .clock_i(wb_clock_i),
        .cpu_grant_o(cpu_grant),
        .wb_grant_o(wb_grant)
    );

    cpu cpu (
        .sys_clock_i(wb_clock_i),
        .cpu_grant_i(cpu_grant),
        .cpu_be_o(cpu_be_o),
        .cpu_clock_o(cpu_clock_o)
    );

    wire ram_ctl_cycle  = wb_cycle_i;
    wire ram_ctl_strobe = wb_grant & wb_strobe_i;
    
    logic ram_ctl_stall;
    assign wb_stall_o   = ~wb_grant | ram_ctl_stall;

    logic wb_ram_oe;
    logic wb_ram_we;
    logic [RAM_ADDR_WIDTH-1:0] wb_ram_addr;

    ram ram (
        .wb_clock_i(wb_clock_i),
        .wb_addr_i(wb_addr_i[16:0]),
        .wb_data_i(wb_data_i),
        .wb_data_o(wb_data_o),
        .wb_we_i(wb_we_i),

        .wb_cycle_i(ram_ctl_cycle),
        .wb_strobe_i(ram_ctl_strobe),
        .wb_stall_o(ram_ctl_stall),
        .wb_ack_o(wb_ack_o),

        .ram_oe_o(wb_ram_oe),
        .ram_we_o(wb_ram_we),
        .ram_addr_o(wb_ram_addr),
        .ram_data_i(cpu_data_i),
        .ram_data_o(cpu_data_o),
        .ram_data_oe(cpu_data_oe)
    );

    //
    // Address Decoding
    //

    logic ram_en;
    logic pia1_en;
    logic pia2_en;
    logic via_en;
    logic io_en;

    address_decoding address_decoding(
        .addr_i(cpu_addr_i),
        .ram_en_o(ram_en),
        .pia1_en_o(pia1_en),
        .pia2_en_o(pia2_en),
        .via_en_o(via_en),
        .io_en_o(io_en)
    );

    wire cpu_rd_strobe = cpu_be_o && !cpu_we_i;
    wire cpu_wr_strobe = cpu_be_o &&  cpu_we_i && cpu_clock_o;

    assign io_oe_o   = io_en   && cpu_be_o;
    assign pia1_cs_o = pia1_en && cpu_be_o;
    assign pia2_cs_o = pia2_en && cpu_be_o;
    assign via_cs_o  =  via_en && cpu_be_o;

    assign ram_oe_o         = (cpu_rd_strobe && ram_en) || wb_ram_oe;
    assign ram_we_o         = (cpu_wr_strobe && ram_en) || wb_ram_we;

    assign cpu_addr_oe      = !cpu_be_o;
    assign cpu_addr_o       = wb_ram_addr[15:0];

    assign ram_addr_a10_o   = cpu_be_o ? cpu_addr_i[10] : cpu_addr_o[10];
    assign ram_addr_a11_o   = cpu_be_o ? cpu_addr_i[11] : cpu_addr_o[11];
    assign ram_addr_a15_o   = cpu_be_o ? cpu_addr_i[15] : cpu_addr_o[15];
    assign ram_addr_a16_o   = cpu_be_o ? 1'b0 : wb_ram_addr[16];

    // synthesis off
    always_ff @(posedge wb_clock_i or negedge wb_clock_i) begin
        assert(!cpu_be_o || !ram_ctl_stall) else $fatal(1, "cpu_ram_oe and ram_ctl_stall cannot be asserted simultaneously");
        assert(!cpu_be_o || !wb_ram_oe) else $fatal(1, "cpu_ram_oe and wb_ram_oe cannot be asserted simultaneously");
        assert(!io_oe_o  || !wb_ram_oe) else $fatal(1, "io_oe_o and wb_ram_oe cannot be asserted simultaneously");
    end
    // synthesis on
endmodule
