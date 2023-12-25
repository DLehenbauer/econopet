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
 
 module top #(
    parameter DATA_WIDTH = 8,
    parameter WB_ADDR_WIDTH = 20,
    parameter CPU_ADDR_WIDTH = 16,
    parameter RAM_ADDR_WIDTH = 17
)(
    // CPU
    output logic                        cpu_be_o,

    input  logic [CPU_ADDR_WIDTH-1:0]   cpu_addr_i,
    output logic [CPU_ADDR_WIDTH-1:0]   cpu_addr_o,
    output logic [CPU_ADDR_WIDTH-1:0]   cpu_addr_oe,

    input  logic [DATA_WIDTH-1:0]       cpu_data_i,
    output logic [DATA_WIDTH-1:0]       cpu_data_o,
    output logic [DATA_WIDTH-1:0]       cpu_data_oe,

    // RAM
    output logic                        ram_addr_a10_o,
    output logic                        ram_addr_a11_o,
    output logic                        ram_addr_a15_o,
    output logic                        ram_addr_a16_o,
    output logic                        ram_oe_n_o,
    output logic                        ram_we_n_o,

    // IO
    output logic                        io_oe_n_o,
    output logic                        pia1_cs_n_o,
    output logic                        pia2_cs_n_o,
    output logic                        via_cs_n_o,

    // FPGA
    input  logic                        clock_i,           // 64 MHz clock (from PLL)
    output logic                        status_no,         // NSTATUS LED (0 = On, 1 = Off)
    
    // SPI1 bus
    input  logic                        spi1_cs_ni,        // (CS)  Chip Select (active low)
    input  logic                        spi1_sck_i,        // (SCK) Serial Clock
    input  logic                        spi1_sd_i,         // (SDI) Serial Data In (MCU -> FPGA)
    output logic                        spi1_sd_o,         // (SDO) Serial Data Out (FPGA -> MCU)
    output logic                        spi_stall_o,

    // Config
    input  logic                        config_crt_i,      // (0 = 12", 1 = 9")
    input  logic                        config_kbd_i,      // (0 = Business, 1 = Graphics)
    
    // Spare pins
    output logic  [9:0]                 spare_o
);
    // Efinity Interface Designer generates a separate output enable for each bus signal.
    // Create a combined logic signal to control OE for bus_addr_o[15:0].
    logic cpu_addr_merged_oe;

    assign cpu_addr_oe = {
        cpu_addr_merged_oe, cpu_addr_merged_oe, cpu_addr_merged_oe, cpu_addr_merged_oe,
        cpu_addr_merged_oe, cpu_addr_merged_oe, cpu_addr_merged_oe, cpu_addr_merged_oe,
        cpu_addr_merged_oe, cpu_addr_merged_oe, cpu_addr_merged_oe, cpu_addr_merged_oe,
        cpu_addr_merged_oe, cpu_addr_merged_oe, cpu_addr_merged_oe, cpu_addr_merged_oe
    };

    // Efinity Interface Designer generates a separate output enable for each bus signal.
    // Create a combined logic signal to control OE for bus_data_o[7:0].
    logic cpu_data_merged_oe;
    
    assign cpu_data_oe = {
        cpu_data_merged_oe, cpu_data_merged_oe, cpu_data_merged_oe, cpu_data_merged_oe,
        cpu_data_merged_oe, cpu_data_merged_oe, cpu_data_merged_oe, cpu_data_merged_oe
    };

    // Avoid contention
    assign io_oe_n_o    = 1'b1;
    assign pia1_cs_n_o  = 1'b1;
    assign pia2_cs_n_o  = 1'b1;
    assign via_cs_n_o   = 1'b1;

    // For consistency, convert active low signals to active high signals.
    logic ram_oe_o;
    assign ram_oe_n_o = !ram_oe_o;

    logic ram_we_o;
    assign ram_we_n_o = !ram_we_o;

    main main(
        .clock_i(clock_i),
        .status_no(status_no),

        .cpu_be_o(cpu_be_o),

        .cpu_addr_i(cpu_addr_i),
        .cpu_addr_o(cpu_addr_o),
        .cpu_addr_oe(cpu_addr_merged_oe),

        .cpu_data_i(cpu_data_i),
        .cpu_data_o(cpu_data_o),
        .cpu_data_oe(cpu_data_merged_oe),
    
        .ram_addr_o({
            ram_addr_a16_o, ram_addr_a15_o, cpu_addr_o[14:12], ram_addr_a11_o, ram_addr_a10_o, cpu_addr_o[9:0]
        }),
        .ram_oe_o(ram_oe_o),
        .ram_we_o(ram_we_o),

        .spi1_cs_ni(spi1_cs_ni),
        .spi1_sck_i(spi1_sck_i),
        .spi1_sd_i(spi1_sd_i),
        .spi1_sd_o(spi1_sd_o),
        .spi_stall_o(spi_stall_o),
        .spare_o(spare_o)
    );
endmodule
