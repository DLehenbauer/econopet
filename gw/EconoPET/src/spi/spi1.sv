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
    parameter DATA_WIDTH = 8,
    parameter ADDR_WIDTH = 17
) (
    input  logic                    clk_i,      // Bus clock
    
    output logic [ADDR_WIDTH-1:0]   wb_addr_o,  // Address of pending read/write (valid when 'cycle_o' asserted)
    output logic [DATA_WIDTH-1:0]   wb_data_o,  // Data received from MCU to write (valid when 'cycle_o' asserted)
    input  logic [DATA_WIDTH-1:0]   wb_data_i,  // Data to transmit to MCU (captured on 'wb_clk_i' when 'wb_ack_i' asserted)
    output logic                    wb_we_o,    // Direction of bus transfer (0 = reading, 1 = writing)
    output logic                    wb_cycle_o, // Requests a bus cycle from the arbiter
    input  logic                    wb_ack_i,   // Signals termination of cycle ('data_i' valid)

    // SPI
    input  logic spi_cs_ni,         // (CS)  Chip Select (active low)
    input  logic spi_sck_i,         // (SCK) Serial Clock
    input  logic spi_sd_i,          // (SDI) Serial Data In (MCU -> FPGA)
    output logic spi_sd_o,          // (SDO) Serial Data Out (FPGA -> MCU)
    
    output logic spi_stall_o        // Flow control: Signal to SPI controller (MCU) that previous command is in progress.
);
    logic [DATA_WIDTH - 1:0] spi_data_i;    // Next byte to transmit
    logic [DATA_WIDTH - 1:0] spi_data_o;    // Last byte recieved (captured on cycle_o)
    logic                    spi_cycle;     // Full byte received (capture 'spi_data_o' and load next 'spi_data_i')

    spi spi(
        .spi_cs_ni(spi_cs_ni),
        .spi_sck_i(spi_sck_i),
        .spi_sd_i(spi_sd_i),
        .spi_sd_o(spi_sd_o),
        .data_i(spi_data_i),
        .data_o(spi_data_o),
        .cycle_o(spi_cycle)
    );

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

    logic [2:0] state = READ_CMD;   // Current state of FSM

    // Asserted if current CMD reads the target address, in which case FSM will
    // transition through READ_ADDR_*_ARG.
    logic cmd_rd_a;

    always_ff @(negedge spi_cs_ni or posedge spi_sck_i) begin
        if (!spi_sck_i) begin
            // Deasserting CS_N asynchronously resets the FSM.
            state <= READ_CMD;
        end else begin
            // 'spi_cycle' indicates the FPGA has received a full byte from the MCU.  We decode it synchronously
            // on the positive SCK edge before the next incoming SPI bit shifts 'spi_data_o'.
            if (spi_cycle) begin
                unique case (state)
                    READ_CMD: begin
                        wb_we_o  <= spi_data_o[7];  // Bit 7: Transfer direction (0 = reading, 1 = writing)
                        cmd_rd_a <= spi_data_o[6];  // Bit 6: Update 'wb_addr_o' (0 = increment, 1 = set)

                        if (spi_data_o[6]) begin
                            // If the incomming CMD reads target address as an argument, capture A16 from rx[0] now.
                            wb_addr_o <= { spi_data_o[0], 16'hxxxx };
                        end else begin
                            // Otherwise increment the previous address.
                            wb_addr_o <= wb_addr_o + 1'b1;
                        end

                        unique casez(spi_data_o)
                            8'b1???????: state <= READ_DATA_ARG;
                            8'b01??????: state <= READ_ADDR_HI_ARG;
                            default:     state <= VALID;
                        endcase
                    end

                    READ_DATA_ARG: begin
                        wb_data_o <= spi_data_o;
                        state <= cmd_rd_a
                            ? READ_ADDR_HI_ARG
                            : VALID;
                    end

                    READ_ADDR_HI_ARG: begin
                        wb_addr_o[15:8] <= spi_data_o;
                        state           <= READ_ADDR_LO_ARG;
                    end

                    READ_ADDR_LO_ARG: begin
                        wb_addr_o[7:0] <= spi_data_o;
                        state          <= VALID;
                    end

                    VALID: begin
                        // Remain in the valid state until negative CS_N edge resets the FSM.
                        state <= VALID;
                    end
                endcase
            end
        end
    end
    
    logic spi_selected_pulse;
    logic spi_deselected_pulse;

    sync2_edge_detect sync_cs_n(    // Cross from CS_N to 'clk_i' domain
        .clk_i(clk_i),
        .data_i(spi_cs_ni),
        .ne_o(spi_selected_pulse),
        .pe_o(spi_deselected_pulse)
    );

    logic spi_cmd_pulse;

    sync2_edge_detect sync_valid(   // Cross from SCK to 'clk_i' domain
        .clk_i(clk_i),
        .data_i(state[2]),          // 'state[2]' bit indicates 'state == VALID'.
        .pe_o(spi_cmd_pulse)
    );

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
    localparam READY            = 2'b00,
               RECEIVING_CMD    = 2'b01,
               PROCESSING_CMD   = 2'b11;

    logic [1:0] state2 = READY;
    assign spi_stall_o = state2[0];
    assign wb_cycle_o  = state2[1];

    always_ff @(posedge clk_i) begin
        if (spi_deselected_pulse) begin
            state2 <= READY;
        end else begin
            unique casez(state2)
                READY:          if (spi_selected_pulse) state2 <= RECEIVING_CMD;
                RECEIVING_CMD:  if (spi_cmd_pulse)      state2 <= PROCESSING_CMD;
                PROCESSING_CMD: if (wb_ack_i) begin
                    spi_data_i <= wb_data_i;
                    state2 <= READY;
                end
            endcase
        end
    end
endmodule
