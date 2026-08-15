// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

`include "./sim/tb.svh"

// Unit test for the soft 6551 ACIA (acia6551.sv): register access, TX frame
// shape, RX reception via external loopback, status flags, and IRQ behavior
// at the Waterloo-typical settings (9600 8N1 and 2400 7E1).
module acia6551_tb;
    logic sys_clock;
    clock_gen #(SYS_CLOCK_MHZ) clock_gen (.clock_o(sys_clock));
    initial clock_gen.start;

    logic be = 0, data_strobe = 0, we = 0;
    logic [15:0] addr = '0;
    logic [7:0] din = '0;
    logic [7:0] dout;
    logic doe;
    logic txd, rxd, rts_n, irq;

    // External loopback normally; a test can take over the line to model
    // the PC side directly.
    logic drive_rx = 1'b0;
    logic rx_line = 1'b1;
    assign rxd = drive_rx ? rx_line : txd;

    // Send one 1200-baud 8N1 frame on the line (833333ns per bit).
    task automatic send_rx_frame(input logic [7:0] b);
        rx_line <= 1'b0;  #833333;
        for (int i = 0; i < 8; i++) begin
            rx_line <= b[i]; #833333;
        end
        rx_line <= 1'b1;  #833333;
    endtask

    task automatic wait_irq_high;
        int guard;
        guard = 0;
        while (!irq) begin
            guard++;
            if (guard > 500000) $fatal(1, "irq never latched");
            @(posedge sys_clock);
        end
    endtask

    acia6551 dut (
        .sys_clock_i(sys_clock),
        .reset_i(1'b0),
        .cpu_be_i(be),
        .cpu_data_strobe_i(data_strobe),
        .cpu_addr_i(addr),
        .cpu_data_i(din),
        .cpu_data_o(dout),
        .cpu_data_oe(doe),
        .cpu_we_i(we),
        .enable_i(1'b1),
        .txd_o(txd),
        .rxd_i(rxd),
        .rts_n_o(rts_n),
        .cts_n_i(1'b0),
        .dtr_n_o(),
        .dsr_n_i(1'b0),
        .dcd_n_i(1'b0),
        .irq_o(irq)
    );

    task automatic cpu_write(input logic [15:0] a, input logic [7:0] v);
        @(posedge sys_clock);
        addr <= a; din <= v; we <= 1; be <= 1;
        repeat (3) @(posedge sys_clock);
        data_strobe <= 1;
        @(posedge sys_clock);
        data_strobe <= 0;
        @(posedge sys_clock);
        be <= 0; we <= 0;
        repeat (2) @(posedge sys_clock);
    endtask

    task automatic cpu_read(input logic [15:0] a, output logic [7:0] v);
        @(posedge sys_clock);
        addr <= a; we <= 0; be <= 1;
        repeat (3) @(posedge sys_clock);
        data_strobe <= 1;
        @(posedge sys_clock);
        // Sample at the strobe edge -- the value the DUT itself keyed its
        // read side-effects on (the real 6809 captures at this instant too).
        v = doe ? dout : 8'hFF;
        data_strobe <= 0;
        @(posedge sys_clock);
        be <= 0;
        repeat (2) @(posedge sys_clock);
    endtask

    // Poll status until (value & mask) == want; returns the observing read's
    // full status value (that read also clears the IRQ flag, so callers must
    // check bit 7 here, not on a later read).
    task automatic wait_status(input logic [7:0] mask, input logic [7:0] want,
                               output logic [7:0] st);
        int guard;
        guard = 0;
        cpu_read(16'hEFF1, st);
        while ((st & mask) != want) begin
            guard++;
            if (guard > 200000) $fatal(1, "status timeout (mask=%02x want=%02x last=%02x)", mask, want, st);
            cpu_read(16'hEFF1, st);
        end
    endtask

    task automatic xfer_byte(input logic [7:0] b, output logic [7:0] got,
                             output logic [7:0] st);
        cpu_write(16'hEFF0, b);              // TX data
        wait_status(8'h08, 8'h08, st);       // RX full (loopback)
        cpu_read(16'hEFF0, got);
    endtask

    task static run;
        logic [7:0] v, got, st;

        $display("[%t] BEGIN ACIA test", $time);

        // 9600 8N1: control = stop:0 wl:00 clk:1 baud:1110
        cpu_write(16'hEFF3, 8'b0001_1110);
        // command: DTR on, RX IRQ enabled, RTS low no TX IRQ, no parity
        cpu_write(16'hEFF2, 8'b0000_1001);
        cpu_read(16'hEFF3, v); `assert_equal(v, 8'b0001_1110);
        cpu_read(16'hEFF2, v); `assert_equal(v, 8'b0000_1001);
        `assert_equal(rts_n, 1'b0);

        cpu_read(16'hEFF1, v);
        `assert_equal(v[4], 1'b1);           // TX empty
        `assert_equal(v[3], 1'b0);           // RX empty
        // The TPUG bridge refuses to transmit unless (status & $60)==0:
        // bits 6:5 are ~DCD/~DSR levels and must read 'ready' when strapped.
        `assert_equal(v[6], 1'b0);
        `assert_equal(v[5], 1'b0);

        // Level IRQ: status shows bit7 while RDRF set & rx-int enabled; it
        // clears when the DATA register is read (not by the status read).
        xfer_byte(8'hA5, got, st); `assert_equal(got, 8'hA5);
        // (xfer_byte already read the data at EFF0, draining RDRF)
        cpu_read(16'hEFF1, v);
        `assert_equal(v[7], 1'b0);           // pin dropped after data read
        `assert_equal(v[0], 1'b0);           // no parity error
        `assert_equal(v[1], 1'b0);           // no framing error

        xfer_byte(8'h00, got, st); `assert_equal(got, 8'h00);
        xfer_byte(8'hFF, got, st); `assert_equal(got, 8'hFF);
        $display("[%t]   9600 8N1 loopback ok", $time);

        // 2400 7E1 (Waterloo SETUP default rate, even parity)
        cpu_write(16'hEFF3, 8'b0011_1010);   // wl=7, baud=2400
        cpu_write(16'hEFF2, 8'b0110_1001);   // parity even, enabled; DTR; RX IRQ
        xfer_byte(8'h41, got, st); `assert_equal(got, 8'h41);
        xfer_byte(8'h7F, got, st); `assert_equal(got, 8'h7F);
        cpu_read(16'hEFF1, v);
        `assert_equal(v[0], 1'b0);           // parity verified clean
        $display("[%t]   2400 7E1 loopback ok", $time);

        // Bridge transmit flow (TPUG Super-OS/9): queue-side enables the TX
        // interrupt while the transmit register is already empty and sleeps
        // -- the real 6551's IRQ is a LEVEL and must assert immediately.
        cpu_write(16'hEFF3, 8'b0001_1000);   // 1200 8N1, as the driver programs
        cpu_write(16'hEFF2, 8'b0000_1001);   // cmd $09: RTS low, TX IRQ off
        `assert_equal(irq, 1'b0);
        cpu_write(16'hEFF2, 8'b0000_0101);   // cmd $05: TX IRQ ENABLED
        repeat (4) @(posedge sys_clock);
        `assert_equal(irq, 1'b1);            // asserted with no edge involved
        cpu_read(16'hEFF1, v);
        `assert_equal(v[7], 1'b1);           // status bit7 mirrors the level
        cpu_read(16'hEFF1, v);               // status read does NOT clear a level
        `assert_equal(irq, 1'b1);            // still asserted (TDRE & tx-en)
        cpu_write(16'hEFF0, 8'h55);          // service writes the character...
        repeat (2) @(posedge sys_clock);
        `assert_equal(irq, 1'b0);            // ...TDRE=0 drops the pin
        cpu_write(16'hEFF2, 8'b0000_1001);   // (service also disables TX IRQ)
        wait_status(8'h08, 8'h08, st);       // loopback returns the char
        cpu_read(16'hEFF0, got);
        `assert_equal(got, 8'h55);
        $display("[%t]   bridge TX-IRQ level flow ok", $time);

        // Bridge receive ritual (TPUG service): a received frame latches
        // the IRQ; the 60Hz poll reads STATUS (clearing the latch), sees
        // bit7 in the returned byte, and only then reads DATA. A second
        // frame must re-latch identically. rxd is driven directly here
        // (loopback disabled) to model the PC side.
        cpu_write(16'hEFF3, 8'b0001_1000);   // 1200 8N1
        cpu_write(16'hEFF2, 8'b0000_1001);   // cmd $09: RX IRQ enabled
        cpu_read(16'hEFF1, v);               // clear any stale latch
        drive_rx <= 1'b1;
        send_rx_frame(8'h0D);                // a real CR at 1200
        wait_irq_high;
        cpu_read(16'hEFF1, v);               // the poll's status read
        `assert_equal(v[7], 1'b1);           // saw the interrupt (level)...
        `assert_equal(v[3], 1'b1);           // ...with RDRF
        `assert_equal(irq, 1'b1);            // level: still asserted after status read
        cpu_read(16'hEFF0, got);             // the service's data read
        `assert_equal(got, 8'h0D);
        repeat (2) @(posedge sys_clock);
        `assert_equal(irq, 1'b0);            // data read cleared RDRF -> pin drops
        cpu_read(16'hEFF1, v);
        `assert_equal(v[3], 1'b0);           // RDRF cleared
        send_rx_frame(8'h41);                // second frame re-latches
        wait_irq_high;
        cpu_read(16'hEFF1, v);
        `assert_equal(v[7], 1'b1);
        cpu_read(16'hEFF0, got);
        `assert_equal(got, 8'h41);
        drive_rx <= 1'b0;
        $display("[%t]   bridge RX poll ritual ok", $time);

        // Programmed reset: command bits 4:0 -> 00010, parity bits kept
        cpu_write(16'hEFF2, 8'b0110_1001);   // restore even-parity command
        cpu_write(16'hEFF1, 8'h00);
        cpu_read(16'hEFF2, v);
        `assert_equal(v, 8'b0110_0010);

        $display("[%t] END ACIA test", $time);
    endtask

    `TB_INIT
endmodule
