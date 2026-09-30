// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

import common_pkg::*;

module memory_control (
    input  logic                      reset_i,

    input  logic                      sys_clock_i,
    // Address is valid; capture this transaction's motherboard eligibility.
    input  logic                      cpu_addr_strobe_i,
    input  logic                      cpu_wr_strobe_i,
    input  logic [CPU_ADDR_WIDTH-1:0] cpu_addr_i,
    input  logic [    DATA_WIDTH-1:0] cpu_data_i,

    // 1 on a SuperPET machine (soft 6809, or a 6502 with the machine-type
    // bit). Gates the SuperPET MMU ($EFFC/$EFF8 latches, flat mode); off, a
    // 6502 sees a stock PET/8096 map. The 8096 $FFF0 banking is not gated (a
    // stock PET-8096 feature available to either CPU).
    input  logic                      superpet_en_i,

    output logic bank_en_o,
    output logic bank_a15_o,
    output logic bank_ro_o,

    // SuperPET latches -- see the block comment inside. sync_i is the 6809's
    // BA=1/BS=0 SYNC acknowledge decoded by the Super-OS/9 MMU.
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
    //
    //   $EFFC-$EFFD  bank select pair (the loader's STD $EFFC works because
    //                D's low byte -- the bank -- lands on $EFFD last):
    //                  [3:0] bank at $9000-$9FFF
    //                  [5]   SYNC/FIRQ disable   (Super-OS/9 MMU only) *SEE NOTE*
    //                  [6]   flat all-RAM mode   (Super-OS/9 MMU only) *SEE NOTE*
    //                  [7]   0 = write-protect the system latch
    //   $EFF8-$EFFB  system latch, writable only while unlocked:
    //                  [1]   0 = write-protect the expansion RAM
    //                  ([0] CPU select and [3] diag are not modeled)
    //   $EFFE-$EFFF  ROM/RAM select for $9xxx -- always RAM with the 6809
    //                active on real hardware; not modeled.
    //
    // The MMU schematic and netlist decode SYNC as BA=1/BS=0. Unless disabled
    // by bit 5, that combinational trigger asserts FIRQ and clears this entire
    // $EFFC latch. The motherboard system latch at $EFF8 is separate: clearing
    // $EFFC re-protects it through bit 7 but does not change its RAM-WP state.
    //
    // NOTE: Naberezny's prose table appears to reverse bits 5 and 6. Gate-level
    //       netlist tracing and TPUG TEST.OS9's LDB #$40 / STB $EFFC establish
    //       that D5 ($20) disables the SYNC-triggered latch clear and /FIRQ, while
    //       D6 ($40) enables the flat 64KB OS-9 mapping.
    localparam int U3_SYNCDIS_BIT = 5,
                   U3_FLAT_BIT   = 6,
                   U3_SYSWE_BIT  = 7;

    // U3 is the MMU's replacement for the motherboard $EFFC latch. The real
    // 74LS273 captures all eight data bits and clears them together.
    logic [DATA_WIDTH-1:0] superpet_u3_q = '0;

    // EconoPET has no input for the SuperPET's physical RAM R/W selector.
    // Start writable, matching VICE and the switch setting required by TPUG
    // TEST.BANKS, while retaining programmable $EFF8 behavior.
    logic superpet_ramwp_q = 1'b0;

    wire superpet_sync_dis = superpet_u3_q[U3_SYNCDIS_BIT];
    wire superpet_flat     = superpet_u3_q[U3_FLAT_BIT];
    wire superpet_ctrlwp   = !superpet_u3_q[U3_SYSWE_BIT];

    wire superpet_mmu_trigger = superpet_en_i && sync_i && !superpet_sync_dis;

    // U1 isolates the motherboard control decoders while flat. Capture that
    // fact with the address so a write uses the mapping at which its bus cycle
    // began, even if the write itself changes U3.
    logic motherboard_decode_q = 1'b1;
    wire motherboard_wr_strobe = cpu_wr_strobe_i && motherboard_decode_q;

    always_ff @(posedge sys_clock_i) begin
        if (reset_i) begin
            mem_ctl <= MEM_CTL_INIT;
            superpet_u3_q <= '0;
            motherboard_decode_q <= 1'b1;
        end else begin
            if (cpu_addr_strobe_i)
                motherboard_decode_q <= !superpet_flat;

            if (superpet_mmu_trigger) begin
                superpet_u3_q <= '0;
            end else if (motherboard_wr_strobe && cpu_addr_i == 16'hFFF0) begin
                // 8096 expansion banking -- a stock PET-8096 feature,
                // available to either CPU (not gated by superpet_en_i).
                mem_ctl <= cpu_data_i;
            end else if (superpet_en_i && motherboard_wr_strobe
                         && (cpu_addr_i & 16'hFFFE) == 16'hEFFC) begin
                superpet_u3_q <= cpu_data_i;
            end else if (superpet_en_i && motherboard_wr_strobe
                         && (cpu_addr_i & 16'hFFFC) == 16'hEFF8) begin
                if (!superpet_ctrlwp) superpet_ramwp_q <= !cpu_data_i[1];
            end
        end
    end

    assign superpet_flat_o   = superpet_flat;
    assign superpet_firq_n_o = !superpet_mmu_trigger;
    assign superpet_ramwp_o  = superpet_ramwp_q;
    assign superpet_bank_o   = superpet_u3_q[3:0];

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
    // Address is valid; capture physical decode and address.
    input  logic                      cpu_addr_strobe_i,
    input  logic                      cpu_wr_strobe_i,
    input  logic [CPU_ADDR_WIDTH-1:0] cpu_addr_i,
    input  logic [    DATA_WIDTH-1:0] cpu_data_i,

    // 1 on a SuperPET machine (see memory_control); off, a 6502 sees a
    // stock PET/8096 map.
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
        .cpu_addr_strobe_i(cpu_addr_strobe_i),
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

    logic [NUM_BITS-1:0] select_d;

    always_comb begin
        if (superpet_flat) begin
            // Super-OS/9 flat mode: the entire 64K is expansion RAM.
            select_d = RAM;
        end else if (bank_en) begin
            select_d = bank_ro ? ROM : RAM;
        end else begin
            priority casez (cpu_addr_i)
                // PET memory map
                CPU_ADDR_WIDTH'('b0???_????_????_????): select_d = RAM;    // RAM  : 0000-7FFF
                CPU_ADDR_WIDTH'('b1000_1111_????_????): select_d = SID;    // SID  : 8F00-8FFF (takes precedence over VRAM)
                // verilator lint_off CASEOVERLAP
                CPU_ADDR_WIDTH'('b1000_????_????_????): select_d = VRAM;   // VRAM : 8000-8FFF (intentionally overlaps with SID)
                // verilator lint_on CASEOVERLAP
                // SuperPET: $9000-$9FFF is bank-switched RAM, one of 16 4KB banks
                // selected via $EFFC (bank bits spliced into cpu_addr_o in main.sv).
                // With a 6502 selected it stays option ROM, as stock.
                CPU_ADDR_WIDTH'('b1001_????_????_????): select_d = superpet_en_i ? RAM : ROM;
                CPU_ADDR_WIDTH'('b1110_1000_0000_????): select_d = UNMAPPED; //      : E800-E80F (Unmapped)
                CPU_ADDR_WIDTH'('b1110_1000_0001_????): select_d = PIA1;     // PIA1 : E810-E81F
                CPU_ADDR_WIDTH'('b1110_1000_001?_????): select_d = PIA2;     // PIA2 : E820-E83F
                CPU_ADDR_WIDTH'('b1110_1000_01??_????): select_d = VIA;      // VIA  : E840-E87F
                CPU_ADDR_WIDTH'('b1110_1000_1???_????): select_d = CRTC;     // CRTC : E880-E8FF
                // SuperPET I/O: 6702 ($EFE0), 6551 ($EFF0), latches ($EFF8/$EFFC).
                // Unmapped so the peripherals can override open_bus in
                // main.sv; stock ROM on a stock machine.
                CPU_ADDR_WIDTH'('b1110_1111_1???_????): select_d = superpet_en_i ? UNMAPPED : ROM;
                default:                                select_d = ROM;      // ROM  : A000-E7FF, E900-EF7F, F000-FFFF
            endcase
        end
    end

    // Capture one physical mapping when the CPU address becomes valid. U3 and
    // $FFF0 may then change during the write without moving the active SRAM
    // address or changing its device select.
    wire superpet_9k_sel_d = superpet_en_i && !superpet_flat
                           && cpu_addr_i[15:12] == 4'b1001;

    logic [16:12] decoded_addr_d;
    logic superpet_wp_d;

    always_comb begin
        decoded_addr_d[12] = (SUPERPET_FULL_BANK && superpet_9k_sel_d)
                           ? superpet_bank[0] : cpu_addr_i[12];
        decoded_addr_d[13] = (SUPERPET_FULL_BANK && superpet_9k_sel_d)
                           ? superpet_bank[1] : cpu_addr_i[13];
        decoded_addr_d[14] = (SUPERPET_FULL_BANK && superpet_9k_sel_d)
                           ? superpet_bank[2] : cpu_addr_i[14];
        decoded_addr_d[15] = superpet_flat ? cpu_addr_i[15]
                           : superpet_9k_sel_d ? superpet_bank[3]
                           : bank_en ? bank_a15
                           : cpu_addr_i[15];
        decoded_addr_d[16] = superpet_flat || bank_en || superpet_9k_sel_d;
        superpet_wp_d = superpet_ramwp && superpet_9k_sel_d;
    end

    logic [NUM_BITS-1:0] select_q = NONE;
    logic [16:12] decoded_addr_q = '0;
    logic superpet_wp_q = 1'b0;

    always_ff @(posedge sys_clock_i) begin
        if (!cpu_be_i) begin
            select_q <= NONE;
            superpet_wp_q <= 1'b0;
        end else if (cpu_addr_strobe_i) begin
            select_q <= select_d;
            decoded_addr_q <= decoded_addr_d;
            superpet_wp_q <= superpet_wp_d;
        end
    end

    assign ram_en_o         = select_q[RAM_EN_BIT];
    assign is_readonly_o    = select_q[IS_READONLY_BIT];
    assign is_vram_o        = select_q[IS_VRAM_BIT];

    assign sid_en_o         = select_q[SID_EN_BIT];
    assign io_en_o          = select_q[IO_EN_BIT];
    assign pia1_en_o        = select_q[PIA1_EN_BIT];
    assign pia2_en_o        = select_q[PIA2_EN_BIT];
    assign via_en_o         = select_q[VIA_EN_BIT];
    assign crtc_en_o        = select_q[CRTC_EN_BIT];
    assign unmapped_o       = select_q[UNMAPPED_BIT];

    assign superpet_wp_o = superpet_wp_q;
    assign decoded_a12_o = decoded_addr_q[12];
    assign decoded_a13_o = decoded_addr_q[13];
    assign decoded_a14_o = decoded_addr_q[14];
    assign decoded_a15_o = decoded_addr_q[15];
    assign decoded_a16_o = decoded_addr_q[16];
endmodule
