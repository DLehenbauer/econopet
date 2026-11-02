// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

import common_pkg::*;

// Performs an MCU-requested access to a physical PIA or VIA during a halted
// CPU bus slot. The low seven Wishbone address bits are the fixed $E8xx offset.
// Unmapped offsets acknowledge with $FF without acquiring the external bus.
module io_controller (
    input  logic                      wb_clock_i,
    input  logic [ WB_ADDR_WIDTH-1:0] wbp_addr_i,
    input  logic [    DATA_WIDTH-1:0] wbp_data_i,
    output logic [    DATA_WIDTH-1:0] wbp_data_o,
    input  logic                      wbp_we_i,
    input  logic                      wbp_cycle_i,
    input  logic                      wbp_strobe_i,
    output logic                      wbp_stall_o,
    output logic                      wbp_ack_o,
    input  logic                      wbp_sel_i,

    input  logic                      cpu_ready_i,
    input  logic                      cpu_slot_be_i,
    input  logic                      cpu_data_strobe_i,
    input  logic                      cpu_hold_strobe_i,
    input  logic [    DATA_WIDTH-1:0] cpu_data_i,

    output logic                      bus_reserve_o,
    output logic                      bus_own_o,
    output logic [CPU_ADDR_WIDTH-1:0] bus_addr_o,
    output logic [    DATA_WIDTH-1:0] bus_data_o,
    output logic                      bus_data_oe_o,
    output logic                      bus_we_o,
    output logic                      io_oe_o,
    output logic                      pia1_cs_o,
    output logic                      pia2_cs_o,
    output logic                      via_cs_o
);
    typedef enum logic [2:0] {
        IDLE      = 3'b000,
        WAIT_SLOT = 3'b010,
        ACTIVE    = 3'b011,
        ACK       = 3'b100
    } state_t;

    typedef enum logic [1:0] {
        IO_NONE,
        IO_PIA1,
        IO_PIA2,
        IO_VIA
    } io_device_t;

    state_t state_q = IDLE;
    // Avoid state decoding on the high-fanout CPU bus-enable path.
    logic bus_reserve_q = 1'b0;
    logic [6:0] addr_q;             // [6:4] Device select, [3:0] Address within device range
    logic [DATA_WIDTH-1:0] data_q;
    logic we_q;

    // ACK must remain asserted until SPI regains a Wishbone arbitration slot
    // and observes it. External-bus ownership ends before entering ACK.
    assign wbp_stall_o = state_q != IDLE && state_q != ACK;
    assign wbp_ack_o = state_q == ACK;
    assign bus_reserve_o = bus_reserve_q;
    assign bus_own_o = state_q == ACTIVE;
    assign bus_addr_o = {8'he8, 1'b0, addr_q};
    assign bus_data_o = data_q;
    assign bus_data_oe_o = bus_own_o && we_q;
    assign bus_we_o = we_q;
    assign io_oe_o = bus_own_o;

    // Decode the incoming request while idle and its latched address thereafter.
    wire [2:0] device_addr = state_q == IDLE
        ? wbp_addr_i[6:4]
        : addr_q[6:4];

    io_device_t decoded_device;

    always_comb begin
        decoded_device = IO_NONE;
        unique casez (device_addr)
            'b001:   decoded_device = IO_PIA1;   // PIA1  : E810-E81F
            'b01?:   decoded_device = IO_PIA2;   // PIA2  : E820-E83F
            'b1??:   decoded_device = IO_VIA;    // VIA   : E840-E87F
            default: decoded_device = IO_NONE;   // Unmap : E800-E80F
        endcase
    end

    assign pia1_cs_o = bus_own_o && decoded_device == IO_PIA1;
    assign pia2_cs_o = bus_own_o && decoded_device == IO_PIA2;
    assign via_cs_o  = bus_own_o && decoded_device == IO_VIA;

    always_ff @(posedge wb_clock_i) begin
        unique case (state_q)
            IDLE: begin
                if (wbp_sel_i && wbp_cycle_i && wbp_strobe_i) begin
                    // synthesis off
                    assert (!cpu_ready_i) else $fatal(1, "Unsafe I/O request");
                    // synthesis on

                    if (cpu_ready_i || decoded_device == IO_NONE) begin
                        // Unmapped or unsafe requests acknowledge without
                        // driving the external bus.
                        wbp_data_o <= 8'hff;
                        state_q <= ACK;
                    end else begin
                        addr_q <= wbp_addr_i[6:0];
                        data_q <= wbp_data_i;
                        we_q <= wbp_we_i;
                        bus_reserve_q <= 1'b1;
                        state_q <= WAIT_SLOT;
                    end
                end
            end

            WAIT_SLOT: begin
                if (cpu_slot_be_i) state_q <= ACTIVE;
            end

            ACTIVE: begin
                if (cpu_data_strobe_i && !we_q) begin
                    wbp_data_o <= cpu_data_i;
                end

                // This strobe is observed one system clock after PHI2 falls,
                // preserving the external devices' post-PHI2 hold interval.
                if (cpu_hold_strobe_i) begin
                    bus_reserve_q <= 1'b0;
                    state_q <= ACK;
                end
            end

            ACK: begin
                if (!wbp_cycle_i) state_q <= IDLE;
            end

            default: begin
                bus_reserve_q <= 1'b0;
                state_q <= IDLE;
            end
        endcase
    end
endmodule
