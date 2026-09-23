// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

module superpet_mc6809 (
    input  logic [  7:0] data_i,
    output logic [  7:0] data_o,
    output logic [ 15:0] addr_o,
    output logic         rnw_o,
    input  logic         e_i,
    input  logic         q_i,
    input  logic         irq_ni,
    input  logic         mmu_firq_ni,
    input  logic         nmi_ni,
    output logic         busy_o,
    output logic         lic_o,
    input  logic         halt_ni,
    input  logic         reset_ni,
    input  logic         dmabreq_ni,
    output logic [111:0] regdata_o,
    output logic         sync_o
);
    logic bs;
    logic ba;
    logic avma;

    localparam int REGDATA_CC_F_BIT = 86;

    // The physical MMU's combinational FIRQ ends with the SYNC handshake.
    // mc6809i keeps BA asserted for an extra internal exit state and pipelines
    // interrupt inputs. When FIRQ is masked, present the MMU request for one
    // falling-Q sample so it wakes SYNC without remaining pending at the next
    // fetch. An unmasked request passes through so the core takes the interrupt
    // as real silicon would.
    logic mmu_firq_sampled = 1'b0;
    always_ff @(negedge q_i or negedge reset_ni) begin
        if (!reset_ni)
            mmu_firq_sampled <= 1'b0;
        else if (mmu_firq_ni)
            mmu_firq_sampled <= 1'b0;
        else if (regdata_o[REGDATA_CC_F_BIT])
            mmu_firq_sampled <= 1'b1;
    end

    wire mmu_firq_shaped_n = mmu_firq_ni
                           || (mmu_firq_sampled && regdata_o[REGDATA_CC_F_BIT]);

    assign sync_o = ba && !bs;

    mc6809i core (
        .D(data_i),
        .DOut(data_o),
        .ADDR(addr_o),
        .RnW(rnw_o),
        .E(e_i),
        .Q(q_i),
        .BS(bs),
        .BA(ba),
        .nIRQ(irq_ni),
        .nFIRQ(mmu_firq_shaped_n),
        .nNMI(nmi_ni),
        .AVMA(avma),
        .BUSY(busy_o),
        .LIC(lic_o),
        .nHALT(halt_ni),
        .nRESET(reset_ni),
        .nDMABREQ(dmabreq_ni),
        .RegData(regdata_o)
    );
endmodule
