// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

module superpet_mc6809_tb;
    logic [7:0] data_i;
    logic [7:0] data_o;
    logic [15:0] addr;
    logic rnw;
    logic e = 1'b0;
    logic q = 1'b0;
    logic irq_n = 1'b1;
    logic mmu_firq_n = 1'b1;
    logic nmi_n = 1'b1;
    logic reset_n = 1'b0;
    logic sync;
    logic [111:0] regdata;

    logic [7:0] mem [0:16'hFFFF];

    assign data_i = mem[addr];

    superpet_mc6809 dut (
        .data_i(data_i),
        .data_o(data_o),
        .addr_o(addr),
        .rnw_o(rnw),
        .e_i(e),
        .q_i(q),
        .irq_ni(irq_n),
        .mmu_firq_ni(mmu_firq_n),
        .nmi_ni(nmi_n),
        .busy_o(),
        .lic_o(),
        .halt_ni(1'b1),
        .reset_ni(reset_n),
        .dmabreq_ni(1'b1),
        .regdata_o(regdata),
        .sync_o(sync)
    );

    initial begin
        forever begin
            #10 q = 1'b1;
            #10 e = 1'b1;
            #10 q = 1'b0;
            #10 e = 1'b0;
        end
    end

    task static initialize_memory;
        for (int i = 0; i < 65536; i++) mem[i] = 8'h12;

        mem[16'hFFFE] = 8'h10;
        mem[16'hFFFF] = 8'h00;
        mem[16'hFFF6] = 8'h20;
        mem[16'hFFF7] = 8'h00;
        mem[16'hFFF8] = 8'h30;
        mem[16'hFFF9] = 8'h00;

        mem[16'h2000] = 8'h20;
        mem[16'h2001] = 8'hFE;
        mem[16'h3000] = 8'h20;
        mem[16'h3001] = 8'hFE;
    endtask

    task static reset_core;
        reset_n = 1'b0;
        irq_n = 1'b1;
        mmu_firq_n = 1'b1;
        repeat (4) @(negedge e);
        reset_n = 1'b1;
    endtask

    task static wait_for_sync;
        int cycles;
        cycles = 0;
        while (!sync && cycles < 200) begin
            @(negedge e);
            cycles++;
        end
        if (!sync) $fatal(1, "[%0t] timed out waiting for SYNC", $time);
    endtask

    task static wait_for_addr(input logic [15:0] expected_i);
        int cycles;
        cycles = 0;
        while (addr != expected_i && cycles < 200) begin
            @(negedge e);
            cycles++;
        end
        if (addr != expected_i)
            $fatal(1, "[%0t] timed out waiting for address $%04x", $time, expected_i);
    endtask

    task static assert_no_vector(input logic [15:0] vector_i);
        for (int i = 0; i < 32; i++) begin
            @(negedge e);
            if (addr == vector_i || addr == vector_i + 1'b1)
                $fatal(1, "[%0t] unexpected vector fetch at $%04x", $time, addr);
        end
    endtask

    task static run;
        initialize_memory;

        // Reset leaves FIRQ masked. A request immediately after falling Q
        // must wake SYNC without reaching the vector.
        mem[16'h1000] = 8'h13;
        mem[16'h1001] = 8'h20;
        mem[16'h1002] = 8'hFE;
        reset_core;
        wait_for_sync;
        `assert_equal(dut.ba, 1'b1);
        `assert_equal(dut.bs, 1'b0);
        `assert_equal(dut.avma, 1'b0);
        @(negedge q);
        #1 mmu_firq_n = 1'b0;
        wait (dut.avma == 1'b1);
        `assert_equal(dut.ba, 1'b1);
        `assert_equal(dut.bs, 1'b0);
        `assert_equal(dut.avma, 1'b1);
        `assert_equal(sync, 1'b1);
        wait (sync == 1'b0);
        mmu_firq_n = 1'b1;
        assert_no_vector(16'hFFF6);
        wait_for_addr(16'h1001);

        // The same masked wake must work when asserted just before falling Q.
        reset_core;
        wait_for_sync;
        @(posedge q);
        #19 mmu_firq_n = 1'b0;
        wait (sync == 1'b0);
        mmu_firq_n = 1'b1;
        assert_no_vector(16'hFFF6);
        wait_for_addr(16'h1001);

        // With FIRQ unmasked, the MMU request remains visible until the core
        // leaves SYNC acknowledge. The real BA/BS-derived request then
        // releases before the vector fetch.
        mem[16'h1000] = 8'h1C;
        mem[16'h1001] = 8'hBF;
        mem[16'h1002] = 8'h13;
        mem[16'h1003] = 8'h20;
        mem[16'h1004] = 8'hFE;
        reset_core;
        wait_for_sync;
        mmu_firq_n = 1'b0;
        wait (sync == 1'b0);
        mmu_firq_n = 1'b1;
        wait_for_addr(16'hFFF6);
        wait_for_addr(16'h2000);

        // IRQ follows the MC6809 rule independently of the MMU workaround:
        // masked IRQ wakes without vectoring.
        mem[16'h1000] = 8'h13;
        mem[16'h1001] = 8'h20;
        mem[16'h1002] = 8'hFE;
        reset_core;
        wait_for_sync;
        irq_n = 1'b0;
        wait (sync == 1'b0);
        irq_n = 1'b1;
        assert_no_vector(16'hFFF8);
        wait_for_addr(16'h1001);

        // Unmasked IRQ wakes SYNC and vectors.
        mem[16'h1000] = 8'h1C;
        mem[16'h1001] = 8'hEF;
        mem[16'h1002] = 8'h13;
        mem[16'h1003] = 8'h20;
        mem[16'h1004] = 8'hFE;
        reset_core;
        wait_for_sync;
        irq_n = 1'b0;
        wait_for_addr(16'hFFF8);
        irq_n = 1'b1;
        wait_for_addr(16'h3000);

        // A masked MMU FIRQ and an unmasked IRQ may arrive together. The
        // MMU request wakes SYNC without vectoring, while IRQ is serviced.
        mem[16'h1000] = 8'h1C;
        mem[16'h1001] = 8'hEF;
        mem[16'h1002] = 8'h13;
        mem[16'h1003] = 8'h20;
        mem[16'h1004] = 8'hFE;
        reset_core;
        wait_for_sync;
        mmu_firq_n = 1'b0;
        irq_n = 1'b0;
        wait (sync == 1'b0);
        mmu_firq_n = 1'b1;
        wait_for_addr(16'hFFF8);
        irq_n = 1'b1;
        wait_for_addr(16'h3000);

        // With both masks clear, FIRQ has priority over IRQ.
        mem[16'h1000] = 8'h1C;
        mem[16'h1001] = 8'hAF;
        mem[16'h1002] = 8'h13;
        mem[16'h1003] = 8'h20;
        mem[16'h1004] = 8'hFE;
        reset_core;
        wait_for_sync;
        mmu_firq_n = 1'b0;
        irq_n = 1'b0;
        wait (sync == 1'b0);
        mmu_firq_n = 1'b1;
        wait_for_addr(16'hFFF6);
        irq_n = 1'b1;
        wait_for_addr(16'h2000);

        // An active request during reset must not survive as an artificial
        // wrapper pulse after reset is released.
        mem[16'h1000] = 8'h13;
        reset_n = 1'b0;
        mmu_firq_n = 1'b0;
        repeat (4) @(negedge e);
        mmu_firq_n = 1'b1;
        reset_n = 1'b1;
        wait_for_sync;
        `assert_equal(sync, 1'b1);
    endtask

    `TB_INIT
endmodule
