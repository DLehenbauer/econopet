// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

import common_pkg::*;

module memory_control (
    input  logic                      reset_i,

    input  logic                      sys_clock_i,
    input  logic                      cpu_be_i,
    input  logic                      cpu_wr_strobe_i,
    input  logic [CPU_ADDR_WIDTH-1:0] cpu_addr_i,
    input  logic [    DATA_WIDTH-1:0] cpu_data_i,

    // 1 only when the soft 6809 owns the bus. Gates the SuperPET-specific MMU
    // ($EFFC/$EFF8 latches, flat mode) so a 6502 sees a stock
    // PET/8096 map. The 8096 $FFF0 expansion banking below is not gated (it's a
    // stock PET-8096 feature available to either CPU).
    input  logic                      superpet_en_i,

    output logic bank_en_o,
    output logic bank_a15_o,
    output logic bank_ro_o,

    // SuperPET latches -- see the block comment inside. sync_i is the 6809's
    // SYNC bus state (BA=1, BS=0), the Super-OS/9 MMU's flat-mode exit.
    input  logic       sync_i,
    output logic [3:0] superpet_bank_o,
    output logic       superpet_ramwp_o,
    output logic       superpet_flat_o,
    output logic       superpet_firq_n_o
);
    // RAM expansion is disabled at power on. Only MEM_CTL_ENABLE has to be
    // known; the rest stay X so 4-state sims flag reads before configuration.
`ifdef VERILATOR
    // The two-state model randomizes the whole partially-X vector, including its known bit.
    localparam logic [DATA_WIDTH-1:0] MEM_CTL_INIT = '0;
`else
    localparam logic [DATA_WIDTH-1:0] MEM_CTL_INIT = 8'b0xxx_xxxx;
`endif

    logic [DATA_WIDTH-1:0] mem_ctl = MEM_CTL_INIT;

    // SuperPET latches, faithful to VICE petmem.c store_super_io() and the
    // CommonPET replica netlist:
    //   $EFFC-$EFFD  bank select pair (the loader's STD $EFFC works because
    //                D's low byte -- the bank -- lands on $EFFD last):
    //                  [3:0] bank at $9000-$9FFF
    //                  [5]   FIRQ disable        (Super-OS/9 MMU only)
    //                  [6]   flat all-RAM mode   (Super-OS/9 MMU only)
    //                  [7]   0 = write-protect the system latch
    //   $EFF8-$EFFB  system latch, writable only while unlocked:
    //                  [1]   0 = write-protect the expansion RAM
    //                  ([0] CPU select and [3] diag are not modeled)
    //   $EFFE-$EFFF  ROM/RAM select for $9xxx -- always RAM with the 6809
    //                active on real hardware; not modeled.
    // Super-OS/9 MMU exit: executing SYNC (sync_i) while flat drops back to
    // banked mode (bank 0, latches re-protected) and pulses FIRQ to wake
    // the core, unless FIRQ-disable was set.
    logic [3:0] superpet_bank = 4'b0000;
    logic superpet_ctrlwp = 1'b1;    // system latch write-protected
    logic superpet_ramwp  = 1'b0;    // expansion RAM writable
    logic superpet_flat   = 1'b0;
    logic superpet_firq_dis = 1'b0;
    logic [9:0] firq_timer = '0;

    // FIRQ pulse held ~8 E cycles: long enough for the core to leave SYNC
    // and take the vector, short enough to end before the handler returns.
    // One E cycle = 64 sys_clocks (one 6809 bus cycle per arbiter round).
    localparam logic [9:0] FIRQ_RELOAD = 10'd511;

    always_ff @(posedge sys_clock_i) begin
        if (firq_timer != 0) firq_timer <= firq_timer - 1'b1;

        if (reset_i) begin
            mem_ctl <= MEM_CTL_INIT;
            superpet_bank <= 4'b0000;
            superpet_ctrlwp <= 1'b1;
            superpet_ramwp  <= 1'b0;
            superpet_flat   <= 1'b0;
            superpet_firq_dis <= 1'b0;
            firq_timer <= '0;
        end else if (superpet_en_i && sync_i && superpet_flat) begin
            superpet_flat     <= 1'b0;
            superpet_bank <= 4'b0000;
            superpet_ctrlwp   <= 1'b1;
            if (!superpet_firq_dis) firq_timer <= FIRQ_RELOAD;
            superpet_firq_dis <= 1'b0;
        end else if (cpu_wr_strobe_i && cpu_addr_i == 16'hFFF0) begin
            // 8096 expansion banking -- a stock PET-8096 feature, available to
            // either CPU (not gated by superpet_en_i).
            mem_ctl <= cpu_data_i;
        end else if (superpet_en_i && cpu_wr_strobe_i && (cpu_addr_i & 16'hFFFE) == 16'hEFFC) begin
            superpet_bank <= cpu_data_i[3:0];
            superpet_firq_dis <= cpu_data_i[5];
            superpet_flat     <= cpu_data_i[6];
            superpet_ctrlwp   <= !cpu_data_i[7];
        end else if (superpet_en_i && cpu_wr_strobe_i && (cpu_addr_i & 16'hFFFC) == 16'hEFF8) begin
            if (!superpet_ctrlwp) superpet_ramwp <= !cpu_data_i[1];
        end
    end

    assign superpet_flat_o   = superpet_flat;
    assign superpet_firq_n_o = firq_timer == 0;
    assign superpet_ramwp_o  = superpet_ramwp;
    assign superpet_bank_o   = superpet_bank;

    logic io_peek;      // Asserted when IO peek-through enabled and address is $8000-$8FFF.
    logic screen_peek;  // Asserted when screen peek-through enabled and address is $E800-$EFFF.

    always_comb begin
        io_peek     = '0;
        screen_peek = '0;
        
        priority casez (cpu_addr_i)
            CPU_ADDR_WIDTH'('b1000_????_????_????): begin   // $8000-$8FFF: Screen peek-through
                screen_peek = mem_ctl[MEM_CTL_SCREEN_PEEK];
            end
            CPU_ADDR_WIDTH'('b1110_1???_????_????): begin   // $E810-$EFFF: IO peek-through
                io_peek = mem_ctl[MEM_CTL_IO_PEEK];
            end
            default: ;                                      // No peek-through
        endcase
    end

    wire mem_enabled = mem_ctl[MEM_CTL_ENABLE];

    always_comb begin
        bank_en_o  = '0;
        bank_a15_o = 'x;  // Unused when bank_en is '0
        bank_ro_o  = 'x;

        if (mem_enabled) begin
            unique casez (cpu_addr_i)
                CPU_ADDR_WIDTH'('b10??_????_????_????): begin   // $8000-$BFFF: Lower bank (0/1)
                    bank_en_o  = !screen_peek;
                    bank_a15_o = mem_ctl[MEM_CTL_SELECT_LO];
                    bank_ro_o  = mem_ctl[MEM_CTL_WRITE_PROTECT_LO];
                end
                CPU_ADDR_WIDTH'('b11??_????_????_????): begin   // $C000-$FFFF: Upper bank (2/3)
                    bank_en_o  = !io_peek;
                    bank_a15_o = mem_ctl[MEM_CTL_SELECT_HI];
                    bank_ro_o  = mem_ctl[MEM_CTL_WRITE_PROTECT_HI];
                end
                default: ;                                      // No bank
            endcase
        end
    end
endmodule

module address_decoding #(
    // SRAM A12-A14 have no FPGA pins -- they hang off the shared bus, which
    // a soft core drives, so main.sv can splice the bank bits in. Off for a
    // physical-CPU setup, where only a15/a16 have dedicated FPGA pins.
    parameter bit SUPERPET_FULL_BANK = 1'b1
) (
    input  logic                      reset_i,
    input  logic                      sys_clock_i,

    input  logic                      cpu_be_i,
    input  logic                      cpu_wr_strobe_i,
    input  logic [CPU_ADDR_WIDTH-1:0] cpu_addr_i,
    input  logic [    DATA_WIDTH-1:0] cpu_data_i,

    // 1 only when the soft 6809 owns the bus; gates the SuperPET MMU
    // (see memory_control) so a 6502 sees a stock PET/8096 map.
    input  logic                      superpet_en_i,

    output logic                      ram_en_o,
    output logic                      sid_en_o,
    output logic                      pia1_en_o,
    output logic                      pia2_en_o,
    output logic                      via_en_o,
    output logic                      crtc_en_o,
    output logic                      io_en_o,
    output logic                      unmapped_o,
    output logic                      is_vram_o,
    output logic                      is_readonly_o,

    // a12-a14 reach the SRAM via the FPGA-driven shared bus (main.sv splices
    // them into cpu_addr_o during the CPU window -- see module header);
    // a15/a16 have dedicated FPGA pins. a10/a11 remain the natural
    // 4KB-window offset bits, so the 4-bit bank number occupies a12-a15.
    output logic                      decoded_a12_o,
    output logic                      decoded_a13_o,
    output logic                      decoded_a14_o,
    output logic                      decoded_a15_o,
    output logic                      decoded_a16_o,

    // SuperPET MMU (Super-OS/9): sync_i = 6809 SYNC bus state; flat_o maps
    // the whole 64K to expansion RAM; wp_o blocks banked-window writes;
    // firq_n_o wakes the core out of the flat-mode-exiting SYNC.
    input  logic                      sync_i,
    output logic                      superpet_flat_o,
    output logic                      superpet_wp_o,
    output logic                      superpet_firq_n_o
);
    logic bank_en;
    logic bank_a15;
    logic bank_ro;
    logic [3:0] superpet_bank;
    logic superpet_ramwp;
    logic superpet_flat;

    memory_control memory_control (
        .reset_i(reset_i),
        .sys_clock_i(sys_clock_i),
        .cpu_be_i(cpu_be_i),
        .cpu_wr_strobe_i(cpu_wr_strobe_i),
        .cpu_addr_i(cpu_addr_i),
        .cpu_data_i(cpu_data_i),
        .superpet_en_i(superpet_en_i),

        .bank_en_o(bank_en),
        .bank_a15_o(bank_a15),
        .bank_ro_o(bank_ro),
        .sync_i(sync_i),
        .superpet_bank_o(superpet_bank),
        .superpet_ramwp_o(superpet_ramwp),
        .superpet_flat_o(superpet_flat),
        .superpet_firq_n_o(superpet_firq_n_o)
    );

    assign superpet_flat_o = superpet_flat;

    localparam RAM_EN_BIT       = 0,
               SID_EN_BIT       = 1,
               PIA1_EN_BIT      = 2,
               PIA2_EN_BIT      = 3,
               VIA_EN_BIT       = 4,
               CRTC_EN_BIT      = 5,
               IO_EN_BIT        = 6,
               IS_READONLY_BIT  = 7,
               IS_VRAM_BIT      = 8,
               UNMAPPED_BIT     = 9;

    localparam NUM_BITS         = 10;

    localparam RAM_EN_MASK       = NUM_BITS'(1'b1) << RAM_EN_BIT,
               SID_EN_MASK       = NUM_BITS'(1'b1) << SID_EN_BIT,
               PIA1_EN_MASK      = NUM_BITS'(1'b1) << PIA1_EN_BIT,
               PIA2_EN_MASK      = NUM_BITS'(1'b1) << PIA2_EN_BIT,
               VIA_EN_MASK       = NUM_BITS'(1'b1) << VIA_EN_BIT,
               CRTC_EN_MASK      = NUM_BITS'(1'b1) << CRTC_EN_BIT,
               IO_EN_MASK        = NUM_BITS'(1'b1) << IO_EN_BIT,
               IS_READONLY_MASK  = NUM_BITS'(1'b1) << IS_READONLY_BIT,
               IS_VRAM_MASK      = NUM_BITS'(1'b1) << IS_VRAM_BIT,
               UNMAPPED_MASK     = NUM_BITS'(1'b1) << UNMAPPED_BIT;

    localparam NONE     = NUM_BITS'('0),
               RAM      = RAM_EN_MASK,
               VRAM     = RAM_EN_MASK  | IS_VRAM_MASK,
               SID      = SID_EN_MASK,                 // No IO_EN: SID implemented on FPGA
               ROM      = RAM_EN_MASK  | IS_READONLY_MASK,
               UNMAPPED = UNMAPPED_MASK,               // Open bus: no device responds
               PIA1     = PIA1_EN_MASK | IO_EN_MASK,
               PIA2     = PIA2_EN_MASK | IO_EN_MASK,
               VIA      = VIA_EN_MASK  | IO_EN_MASK,
               CRTC     = CRTC_EN_MASK;                // No IO_EN: CRTC implemented on FPGA

    logic [NUM_BITS-1:0] select = NUM_BITS'('hxxx);

    initial begin
        select = NONE;
    end

    always_ff @(posedge sys_clock_i) begin
        if (!cpu_be_i) begin
            select <= NONE;
        end else begin
            if (superpet_flat) begin
                // Super-OS/9 flat mode: the entire 64K is expansion RAM.
                // No ROM, no I/O, no VRAM overlay (VICE petmem behavior);
                // the video circuit keeps scanning the real VRAM directly.
                select <= RAM;
            end else if (bank_en) begin
                select <= bank_ro
                    ? ROM
                    : RAM;
            end else begin
                priority casez (cpu_addr_i)
                    // PET memory map
                    CPU_ADDR_WIDTH'('b0???_????_????_????): select <= RAM;    // RAM  : 0000-7FFF
                    CPU_ADDR_WIDTH'('b1000_1111_????_????): select <= SID;    // SID  : 8F00-8FFF (takes precedence over VRAM)
                    // verilator lint_off CASEOVERLAP
                    CPU_ADDR_WIDTH'('b1000_????_????_????): select <= VRAM;   // VRAM : 8000-8FFF (intentionally overlaps with SID)
                    // verilator lint_on CASEOVERLAP
                    // SuperPET: $9000-$9FFF is bank-switched RAM, one of 16 4KB banks
                    // selected via $EFFC (bank bits spliced into cpu_addr_o in main.sv).
                    // With a 6502 selected it stays option ROM, as stock.
                    CPU_ADDR_WIDTH'('b1001_????_????_????): select <= superpet_en_i ? RAM : ROM;
                    CPU_ADDR_WIDTH'('b1110_1000_0000_????): select <= UNMAPPED; //      : E800-E80F (Unmapped)
                    CPU_ADDR_WIDTH'('b1110_1000_0001_????): select <= PIA1;     // PIA1 : E810-E81F
                    CPU_ADDR_WIDTH'('b1110_1000_001?_????): select <= PIA2;     // PIA2 : E820-E83F
                    CPU_ADDR_WIDTH'('b1110_1000_01??_????): select <= VIA;      // VIA  : E840-E87F
                    CPU_ADDR_WIDTH'('b1110_1000_1???_????): select <= CRTC;     // CRTC : E880-E8FF
                    // SuperPET I/O: 6702 ($EFE0), 6551 ($EFF0), latches ($EFF8/$EFFC).
                    // Unmapped so the peripherals can override open_bus in main.sv;
                    // stock ROM with a 6502 selected.
                    CPU_ADDR_WIDTH'('b1110_1111_1???_????): select <= superpet_en_i ? UNMAPPED : ROM;
                    default:                                select <= ROM;      // ROM  : A000-E7FF, E900-EF7F, F000-FFFF
                endcase
            end
        end
    end

    assign ram_en_o         = select[RAM_EN_BIT];
    assign is_readonly_o    = select[IS_READONLY_BIT];
    assign is_vram_o        = select[IS_VRAM_BIT];

    assign sid_en_o         = select[SID_EN_BIT];
    assign io_en_o          = select[IO_EN_BIT];
    assign pia1_en_o        = select[PIA1_EN_BIT];
    assign pia2_en_o        = select[PIA2_EN_BIT];
    assign via_en_o         = select[VIA_EN_BIT];
    assign crtc_en_o        = select[CRTC_EN_BIT];
    assign unmapped_o       = select[UNMAPPED_BIT];

    // SuperPET $9000-$9FFF moves to the upper 64K of SRAM (a16=1) so it does
    // not alias the unbanked ROM image. The 4KB bank from the $EFFC register
    // occupies a15-a12; a10/a11 stay the in-bank offset.
    wire superpet_9k_sel = superpet_en_i && !superpet_flat && cpu_addr_i[15:12] == 4'b1001;

    // Expansion-RAM write protect (system latch bit 1) applies to the banked
    // window; in flat mode OS-9 owns the whole map and WP is not applied.
    assign superpet_wp_o = superpet_ramwp && superpet_9k_sel;

    // a12-a14: SUPERPET_FULL_BANK selects the bank register (default) or passes
    // them through -- an escape hatch for a physical-CPU setup, and a no-op here
    // either way since a14:12 is naturally 3'b001.
    assign decoded_a12_o    = (SUPERPET_FULL_BANK && superpet_9k_sel) ? superpet_bank[0] : cpu_addr_i[12];
    assign decoded_a13_o    = (SUPERPET_FULL_BANK && superpet_9k_sel) ? superpet_bank[1] : cpu_addr_i[13];
    assign decoded_a14_o    = (SUPERPET_FULL_BANK && superpet_9k_sel) ? superpet_bank[2] : cpu_addr_i[14];
    assign decoded_a15_o    = superpet_9k_sel ? superpet_bank[3] : (bank_en ? bank_a15 : cpu_addr_i[15]);
    // Flat mode: identity mapping into the upper 64K (a16=1) -- the same
    // physical region the banked window pages through, seen linearly.
    assign decoded_a16_o    = superpet_flat || bank_en || superpet_9k_sel;
endmodule
