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

module spi1_controller #(
    parameter WB_DATA_WIDTH = 8,
    parameter WB_ADDR_WIDTH = 20
) (
    // Wishbone B4 pipelined controller
    // (See: https://cdn.opencores.org/downloads/wbspec_b4.pdf)
    input  logic                     wb_clock_i,  // Bus clock    
    output logic [WB_ADDR_WIDTH-1:0] wb_addr_o,   // Address of pending read/write (valid when 'cycle_o' asserted)
    output logic [WB_DATA_WIDTH-1:0] wb_data_o,   // Data received from MCU to write (valid when 'cycle_o' asserted)
    input  logic [WB_DATA_WIDTH-1:0] wb_data_i,   // Data to transmit to MCU (captured on 'wb_clock_i' when 'wb_ack_i' asserted)
    output logic                     wb_we_o,     // Direction of bus transfer (0 = reading, 1 = writing)
    output logic                     wb_cycle_o,  // Requests a bus cycle from the arbiter
    output logic                     wb_strobe_o, // Signals next request ('addr_o', 'data_o', and 'wb_we_o' are valid).
    input  logic                     wb_ack_i,    // Signals termination of cycle ('data_i' valid)

    // SPI
    input  logic spi_cs_ni,         // (CS)  Chip Select (active low)
    input  logic spi_sck_i,         // (SCK) Serial Clock
    input  logic spi_sd_i,          // (SDI) Serial Data In (MCU -> FPGA)
    output logic spi_sd_o,          // (SDO) Serial Data Out (FPGA -> MCU)
    
    output logic spi_stall_o        // Flow control: When asserted, MCU should wait to send further commands.
);
    //
    // SCK clock domain signals
    //

    logic                     spi_cycle;      // Asserted on rising SCK edge when incoming 'spi_data_rx' is valid.
                                              // Outgoing 'spi_data_tx' is captured on the following negative SCK edge.

    logic [WB_DATA_WIDTH-1:0] spi_data_rx;    // Byte received from SPI (see also 'spi_cycle').
    logic [WB_DATA_WIDTH-1:0] spi_data_tx;    // Next byte to transmit to SPI (see also 'spi_cycle').

    spi spi(
        .spi_cs_ni(spi_cs_ni),
        .spi_sck_i(spi_sck_i),
        .spi_sd_i(spi_sd_i),
        .spi_sd_o(spi_sd_o),
        .data_i(spi_data_tx),
        .data_o(spi_data_rx),
        .cycle_o(spi_cycle)
    );

    //
    // SCK domain state machine
    //

    // State encoding for our FSM:
    //
    //  D = data    (processing a write command, awaiting byte to write)
    //  A = address (processing a random access command, awaiting address bytes)
    //  V = valid   (a command has been received, request cycle from bus arbiter)
    //
    //                               VAD
    localparam READ_CMD         = 3'b000,
               READ_DATA_ARG    = 3'b001,
               READ_ADDR_HI_ARG = 3'b010,
               READ_ADDR_LO_ARG = 3'b011,
               VALID            = 3'b100;

    logic [2:0] spi_state = READ_CMD;   // Current state of FSM

    always_ff @(posedge spi_cs_ni or posedge spi_sck_i) begin
        if (spi_cs_ni) begin
            // Reset the FSM when the MCU deasserts 'spi_cs_ni'.
            spi_state <= READ_CMD;
        end else begin
            // 'spi_cycle' indicates the FPGA has received a full byte from the MCU.  We decode it synchronously
            // on the positive SCK edge before the next incoming SPI bit shifts 'spi_data_rx'.
            if (spi_cycle) begin
                unique case (spi_state)
                    READ_CMD: begin
                        wb_we_o  <= spi_data_rx[7];  // Bit 7: Transfer direction (0 = reading, 1 = writing)

                        if (spi_data_rx[6]) begin
                            // If the incomming CMD reads target address as an argument, capture A16 from rx[0] now.
                            wb_addr_o <= { spi_data_rx[WB_ADDR_WIDTH-16-1:0], 16'hxxxx };
                            spi_state <= READ_ADDR_HI_ARG;
                        end else begin
                            // Otherwise increment the previous address.
                            wb_addr_o <= wb_addr_o + 1'b1;
                            spi_state <= spi_data_rx[7]
                                ? READ_DATA_ARG
                                : VALID;
                        end
                    end

                    READ_ADDR_HI_ARG: begin
                        wb_addr_o[15:8] <= spi_data_rx;
                        spi_state       <= READ_ADDR_LO_ARG;
                    end

                    READ_ADDR_LO_ARG: begin
                        wb_addr_o[7:0] <= spi_data_rx;
                        spi_state      <= wb_we_o
                            ? READ_DATA_ARG
                            : VALID;
                    end

                    READ_DATA_ARG: begin
                        wb_data_o <= spi_data_rx;
                        spi_state <= VALID;
                    end

                    VALID: begin
                        // Remain in the valid state until negative CS_N edge resets the FSM.
                        spi_state <= VALID;
                    end
                endcase
            end
        end
    end
    
    // CDC from SCK to 'wb_clock_i'

    logic spi_selected_pulse;
    logic spi_deselected_pulse;

    sync2_edge_detect sync_cs_n(    // Cross from CS_N to 'wb_clock_i' domain
        .clock_i(wb_clock_i),
        .data_i(spi_cs_ni),
        .ne_o(spi_selected_pulse),
        .pe_o(spi_deselected_pulse)
    );

    logic spi_cmd_pulse;

    sync2_edge_detect sync_valid(   // Cross from SCK to 'wb_clock_i' domain
        .clock_i(wb_clock_i),
        .data_i(spi_state[2]),      // 'spi_state[2]' bit indicates 'state == VALID'.
        .pe_o(spi_cmd_pulse)
    );

    // Wishbone controller state machine ('wb_clock_i' domain)

    initial begin
        wb_strobe_o = '0;
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
    localparam READY            = 2'b00,    // 'spi_cs_ni' deasserted
               RECEIVING_CMD    = 2'b01,    // 'spi_cs_ni' asserted
               PROCESSING_CMD   = 2'b11;    // Received 'spi_data_o' is valid

    logic [1:0] wb_state = READY;
    assign spi_stall_o = wb_state[0];
    assign wb_cycle_o  = wb_state[1];

    always_ff @(posedge wb_clock_i) begin
        if (spi_deselected_pulse) begin
            // Reset the FSM when the MCU deasserts 'spi_cs_ni'.
            wb_state <= READY;  // Deassert 'wb_cycle_o' and 'spi_stall_o' 
            wb_strobe_o <= '0;
        end else begin
            unique casez(wb_state)
                READY: begin
                    if (spi_selected_pulse) begin
                        wb_state <= RECEIVING_CMD;
                    end
                end
                RECEIVING_CMD: begin
                    if (spi_cmd_pulse) begin
                        wb_strobe_o <= 1'b1;
                        wb_state <= PROCESSING_CMD;
                    end
                end
                PROCESSING_CMD: begin
                    wb_strobe_o <= '0;

                    if (wb_ack_i) begin
                        spi_data_tx <= wb_data_i;
                        wb_state    <= READY;
                    end
                end
            endcase
        end
    end
endmodule
