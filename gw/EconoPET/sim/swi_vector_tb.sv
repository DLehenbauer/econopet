// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

// Regression for the mc6809i SWI3/SWI2 vector-selection bug (see
// cavnex/mc6809#6): each software interrupt must fetch
// its own vector -- SWI $FFFA, SWI2 $FFF4, SWI3 $FFF2. The original core
// dispatched SWI3 through $FFFA, which crashed Super-OS/9 (its SuperPET
// hardware bridge is SWI3-based) while leaving Waterloo software untouched.
module swi_vector_tb;
    logic clk = 0;
    always #250 clk = ~clk;   // 2MHz E via divider below (period irrelevant)

    // Simple E/Q generation: quadrature from a 2-bit counter.
    logic [1:0] ph = 0;
    always @(posedge clk) ph <= ph + 1;
    wire E = ph[1];
    wire Q = ph[1] ^ ph[0];

    logic [15:0] ADDR;
    logic [7:0] DIn, DOut;
    logic RnW, BS, BA, AVMA, BUSY, LIC;

    // 64K behavioral memory:
    //   $1000: program  SWI ; SWI2 ; SWI3 (reached via handlers)
    //   handlers at $2000/$2100/$2200 record their entry.
    logic [7:0] mem [0:65535];
    integer i;
    initial begin
        for (i = 0; i < 65536; i = i + 1) mem[i] = 8'h12;   // NOP sea
        // reset vector -> $1000
        mem[16'hFFFE] = 8'h10; mem[16'hFFFF] = 8'h00;
        // program: set up a stack first (an uninitialized S wraps the
        // interrupt frames over the vector page), then SWI ; SWI2 ; SWI3
        mem[16'h1000] = 8'h10; mem[16'h1001] = 8'hCE;  // LDS #$0F00
        mem[16'h1002] = 8'h0F; mem[16'h1003] = 8'h00;
        mem[16'h1004] = 8'h3F;                         // SWI
        mem[16'h1005] = 8'h10; mem[16'h1006] = 8'h3F;  // SWI2
        mem[16'h1007] = 8'h11; mem[16'h1008] = 8'h3F;  // SWI3
        mem[16'h1009] = 8'h20; mem[16'h100A] = 8'hFE;  // BRA *
        // vectors
        mem[16'hFFFA] = 8'h20; mem[16'hFFFB] = 8'h00;  // SWI  -> $2000
        mem[16'hFFF4] = 8'h21; mem[16'hFFF5] = 8'h00;  // SWI2 -> $2100
        mem[16'hFFF2] = 8'h22; mem[16'hFFF3] = 8'h00;  // SWI3 -> $2200
        // handlers: RTI
        mem[16'h2000] = 8'h3B;
        mem[16'h2100] = 8'h3B;
        mem[16'h2200] = 8'h3B;
    end

    always @(*) DIn = mem[ADDR];
    always @(negedge E) if (!RnW) mem[ADDR] = DOut;

    mc6809i cpu (
        .D(DIn), .DOut(DOut), .ADDR(ADDR), .RnW(RnW),
        .E(E), .Q(Q),
        .BS(BS), .BA(BA),
        .nIRQ(1'b1), .nFIRQ(1'b1), .nNMI(1'b1),
        .AVMA(AVMA), .BUSY(BUSY), .LIC(LIC),
        .nHALT(1'b1), .nRESET(nRESET), .nDMABREQ(1'b1),
        .RegData()
    );

    logic nRESET = 0;
    initial begin
        repeat (20) @(posedge E);
        nRESET = 1;
    end

    // Record handler entries (opcode fetch at handler address)
    logic hit_swi = 0, hit_swi2 = 0, hit_swi3 = 0;
    always @(posedge E) begin
        if (nRESET && BS == 0 && BA == 0) begin
            if (ADDR == 16'h2000) hit_swi  <= 1;
            if (ADDR == 16'h2100) hit_swi2 <= 1;
            if (ADDR == 16'h2200) hit_swi3 <= 1;
        end
    end

    task static run;
        $display("[%t] BEGIN SWI vector test", $time);
        repeat (600) @(posedge E);
        `assert_equal(hit_swi,  1'b1);
        `assert_equal(hit_swi2, 1'b1);
        `assert_equal(hit_swi3, 1'b1);   // fails on the unpatched core
        $display("[%t] END SWI vector test (SWI/SWI2/SWI3 all correct)", $time);
    endtask

    `TB_INIT
endmodule
