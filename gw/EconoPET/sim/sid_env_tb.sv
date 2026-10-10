// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

module sid_env_tb;
    localparam logic [2:0] STAGE_ATT = 3'b001;
    localparam logic [2:0] STAGE_DEC_SUS = 3'b010;
    localparam logic [2:0] STAGE_REL = 3'b100;
    localparam logic [4:0] SUSTAIN_RELEASE_ADDR = 5'h06;

    logic clock = 1'b0;
    logic reset = 1'b0;
    logic gate = 1'b0;
    logic [3:0] sustain = '0;
    wire [7:0] envelope;

    sid_env_imp impl (
        .clk(clock),
        .clkEn(1'b0),
        .iRst(reset),
        .iGate(gate),
        .iAtt(4'h0),
        .iDec(4'h0),
        .iSus(sustain),
        .iRel(4'h0),
        .oOut(envelope)
    );

    logic write_en = 1'b0;
    logic [7:0] write_data = '0;
    wire [7:0] written_envelope;

    sid_env written (
        .clk(clock),
        .clkEn(1'b0),
        .iRst(reset),
        .iWE(write_en),
        .iAddr(SUSTAIN_RELEASE_ADDR),
        .iData(write_data),
        .oOut(written_envelope)
    );

    // Retain the original envelope update as an independent next-state oracle.
    function automatic logic [7:0] expected_envelope(
        input logic [7:0] value,
        input logic [2:0] stage,
        input logic [3:0] sustain_value,
        input logic [3:0] control
    );
        expected_envelope = value;
        if (control[3]) begin
            expected_envelope = 8'haa;
        end else begin
            case (1'b1)
                stage[0]:
                    if (control[0] && value != 8'hff && control[1])
                        expected_envelope = value + 8'd1;
                stage[1]:
                    if (control[0] && control[2] && value != {sustain_value, sustain_value})
                        expected_envelope = value == 0 ? 8'h00 : value - 8'd1;
                stage[2]:
                    if (!control[0] && control[2])
                        expected_envelope = value == 0 ? 8'h00 : value - 8'd1;
                default: ;
            endcase
        end
    endfunction

    // Check every envelope, sustain, stage and update-control combination.
    task exhaustive_next_state;
        logic [7:0] expected;
        logic [2:0] expected_stage;
        $dumpoff;
        for (int value = 0; value < 256; value++) begin
            for (int level = 0; level < 16; level++) begin
                for (int stage = 0; stage < 8; stage++) begin
                    for (int control = 0; control < 16; control++) begin
                        impl.env = 8'(value);
                        impl.stage = 3'(stage);
                        impl.cntRst = 1'(control >> 1);
                        impl.divRst = 1'(control >> 2);
                        gate = 1'(control);
                        reset = 1'(control >> 3);
                        sustain = 4'(level);
                        expected = expected_envelope(8'(value), 3'(stage), 4'(level), 4'(control));
                        expected_stage = 3'(stage);
                        if (reset) begin
                            expected_stage = STAGE_REL;
                        end else begin
                            case (1'b1)
                                expected_stage[0]: begin
                                    if (!gate) expected_stage = STAGE_REL;
                                    else if (value == 255) expected_stage = STAGE_DEC_SUS;
                                end
                                expected_stage[1]:
                                    if (!gate) expected_stage = STAGE_REL;
                                expected_stage[2]:
                                    if (gate) expected_stage = STAGE_ATT;
                                default: expected_stage = STAGE_REL;
                            endcase
                        end
                        #1 clock = 1'b1;
                        #1;
                        `assert_equal(envelope, expected)
                        `assert_equal(impl.stage, expected_stage)
                        clock = 1'b0;
                    end
                end
            end
        end
        $dumpon;
    endtask

    // A falling-edge sustain write must affect the immediately following update.
    task sustain_write_visibility;
        logic [7:0] expected;
        reset = 1'b0;
        for (int value = 0; value < 256; value++) begin
            for (int level = 0; level < 16; level++) begin
                written.impl.env = 8'(value);
                written.impl.stage = STAGE_DEC_SUS;
                written.regGate = 1'b1;
                written.regSus = ~4'(level);
                write_data = {4'(level), 4'h0};
                write_en = 1'b1;
                #1 clock = 1'b1;
                #1;
                written.impl.env = 8'(value);
                clock = 1'b0;
                #1;
                write_en = 1'b0;
                written.impl.divRst = 1'b1;
                expected = expected_envelope(8'(value), STAGE_DEC_SUS, 4'(level), 4'b0101);
                #1 clock = 1'b1;
                #1;
                `assert_equal(written.regSus, 4'(level))
                `assert_equal(written_envelope, expected)
                clock = 1'b0;
            end
        end
    endtask

    // Verify next-state equivalence and the physical write-to-update boundary.
    task run;
        #1;
        exhaustive_next_state;
        sustain_write_visibility;
    endtask

    `TB_INIT
endmodule
