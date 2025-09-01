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

import common_pkg::*;

module spi1_controller (
    // Wishbone B4 pipelined controller
    // (See: https://cdn.opencores.org/downloads/wbspec_b4.pdf)
    input  logic wb_clock_i,                      // Bus clock
    output logic [WB_ADDR_WIDTH-1:0] wbc_addr_o,   // Address of pending read/write (valid when 'cycle_o' asserted)
    output logic [   DATA_WIDTH-1:0] wbc_data_o,   // Data received from MCU to write (valid when 'cycle_o' asserted)
    input  logic [   DATA_WIDTH-1:0] wbc_data_i,   // Data to transmit to MCU (captured on 'wb_clock_i' when 'wbc_ack_i' asserted)
    output logic wbc_we_o,                         // Direction of bus transfer (0 = reading, 1 = writing)
    output logic wbc_cycle_o,                      // Requests a bus cycle from the arbiter
    output logic wbc_strobe_o,                     // Signals next request ('addr_o', 'data_o', and 'wbc_we_o' are valid).
    input  logic wbc_stall_i,                      // Signals that peripheral is not ready to accept request
    input  logic wbc_ack_i,                        // Signals termination of cycle ('data_i' valid)

    // SPI
    input  logic spi_cs_ni,  // (CS)  Chip Select (active low)
    input  logic spi_sck_i,  // (SCK) Serial Clock
    input  logic spi_sd_i,   // (SDI) Serial Data In (MCU -> FPGA)
    output logic spi_sd_o,   // (SDO) Serial Data Out (FPGA -> MCU)

    output logic spi_stall_o        // Flow control: When asserted, MCU should wait to send further commands.
);
    //
    // SCK clock domain signals
    //

    logic spi_strobe;  // Asserted on rising SCK edge when incoming 'spi_data_rx' is valid.
                       // Outgoing 'spi_data_tx' is captured on the following negative SCK edge.

    logic [   DATA_WIDTH-1:0] spi_data_rx;  // Byte received from SPI (see also 'spi_strobe').
    logic [   DATA_WIDTH-1:0] spi_data_tx;  // Next byte to transmit to SPI (see also 'spi_strobe').

    spi spi (
        .spi_cs_ni(spi_cs_ni),
        .spi_sck_i(spi_sck_i),
        .spi_sd_i(spi_sd_i),
        .spi_sd_o(spi_sd_o),
        .data_i(spi_data_tx),
        .data_o(spi_data_rx),
        .strobe_o(spi_strobe)
    );

    // CDC from SCK to 'wb_clock_i'

    logic spi_start_pulse;
    logic spi_reset_pulse;

    sync2_edge_detect sync_cs_n (   // Cross from CS_N to 'wb_clock_i' domain
        .clock_i(wb_clock_i),
        .data_i (spi_cs_ni),
        .data_o (),                 // Unused: Only need edge detection
        .ne_o   (spi_start_pulse),
        .pe_o   (spi_reset_pulse)
    );

    logic spi_strobe_pulse;

    sync2_edge_detect sync_valid (  // Cross from SCK to 'wb_clock_i' domain
        .clock_i(wb_clock_i),
        .data_i (spi_strobe),
        .data_o (),                 // Unused: Only need positive edge detection
        .pe_o   (spi_strobe_pulse),
        .ne_o   ()                  // Unused: Only need positive edge detection
    );

    // State encoding for our FSM:
    //
    //  D = data    (processing a write command, awaiting byte to write)
    //  A = address (processing a random access command, awaiting address bytes)
    //  V = valid   (a command has been received, request cycle from bus arbiter)
    //
    //                               VAD
    localparam bit [2:0] READ_CMD         = 3'b000,
                         READ_DATA_ARG    = 3'b001,
                         READ_ADDR_HI_ARG = 3'b010,
                         READ_ADDR_LO_ARG = 3'b011,
                         VALID            = 3'b100;

    logic [2:0] spi_state = READ_CMD;  // Current state of FSM
    wire spi_valid = spi_state[2];

    always_ff @(posedge wb_clock_i) begin
        if (spi_reset_pulse) begin
            // Reset the FSM when the MCU deasserts 'spi_cs_ni'.
            spi_state <= READ_CMD;
        end else if (spi_strobe_pulse) begin
            unique case (spi_state)
                READ_CMD: begin
                    wbc_we_o  <= spi_data_rx[7];  // Bit 7: Transfer direction (0 = reading, 1 = writing)

                    if (spi_data_rx[6:5] == 2'b10) begin
                        // If the incomming CMD reads target address as an argument, capture A16 from rx[0] now.
                        wbc_addr_o <= {spi_data_rx[WB_ADDR_WIDTH-16-1:0], 16'hxxxx};
                        spi_state <= READ_ADDR_HI_ARG;
                    end else begin
                        // Otherwise increment/decrement the previous address.
                        wbc_addr_o <= wbc_addr_o + {{(WB_ADDR_WIDTH - 1){spi_data_rx[6]}}, spi_data_rx[5]};
                        spi_state <= spi_data_rx[7]
                            ? READ_DATA_ARG
                            : VALID;
                    end
                end

                READ_ADDR_HI_ARG: begin
                    wbc_addr_o[15:8] <= spi_data_rx;
                    spi_state       <= READ_ADDR_LO_ARG;
                end

                READ_ADDR_LO_ARG: begin
                    wbc_addr_o[7:0] <= spi_data_rx;
                    spi_state      <= wbc_we_o
                        ? READ_DATA_ARG
                        : VALID;
                end

                READ_DATA_ARG: begin
                    wbc_data_o <= spi_data_rx;
                    spi_state <= VALID;
                end

                VALID: begin
                    // Remain in the valid state until negative CS_N edge resets the FSM.
                    spi_state <= VALID;
                end

                default: begin
                    // synthesis off
                    $fatal(1, "Invalid state in SPI1 controller: %b", spi_state);
                    // synthesis on
                end
            endcase
        end
    end

    // Wishbone controller state machine ('wb_clock_i' domain)

    initial begin
        wbc_strobe_o = '0;
    end

    // MCU/FPGA handshake works as follows:
    // - MCU waits for FPGA to deassert 'spi_stall_o'
    // - MCU asserts 'spi_cs_ni' (this resets our FSM)
    // - MCU transmits bytes (advances FSM to VALID state -> cmd_valid_pe)
    // - MCU waits for FPGA to assert READY (ack_i -> spi_ready_o)
    // - MCU deasserts CS_N (no effect)

    // State encoding for our FSM:
    //
    //  S = stall   (receiving or processing command)
    //  C = cycle   (requesting bus cycle)
    //
    //                               CS
    localparam bit [1:0] READY          = 2'b00,  // 'spi_cs_ni' deasserted
                         RECEIVING_CMD  = 2'b01,  // 'spi_cs_ni' asserted
                         PROCESSING_CMD = 2'b11;  // Received 'spi_data_o' is valid

    logic [1:0] wbc_state = READY;
    assign spi_stall_o = wbc_state[0];
    assign wbc_cycle_o  = wbc_state[1];

    always_ff @(posedge wb_clock_i) begin
        if (spi_reset_pulse) begin
            // Reset the FSM when the MCU deasserts 'spi_cs_ni'.
            wbc_state <= READY;  // Deassert 'wbc_cycle_o' and 'spi_stall_o'
            wbc_strobe_o <= '0;
        end else begin
            unique case (wbc_state)
                READY: begin
                    wbc_strobe_o <= 0;

                    if (spi_start_pulse) begin
                        wbc_state <= RECEIVING_CMD;
                    end
                end
                RECEIVING_CMD: begin
                    if (spi_valid) begin
                        wbc_strobe_o <= 1'b1;
                        wbc_state <= PROCESSING_CMD;
                    end
                end
                PROCESSING_CMD: begin
                    // Continue asserting wbc_strobe_o while the bus is stalled.
                    if (!wbc_stall_i) wbc_strobe_o <= '0;

                    // The '!wbc_strobe_o' prevents the SPI1 controller for accepting ACKs
                    // before it's request has been accepted.
                    if (!wbc_strobe_o && wbc_ack_i) begin
                        spi_data_tx <= wbc_data_i;
                        wbc_state    <= READY;
                    end
                end
                default: begin
                    // synthesis off
                    $fatal(1, "Invalid state in SPI1 controller: %b", wbc_state);
                    // synthesis on
                end
            endcase
        end
    end
endmodule
