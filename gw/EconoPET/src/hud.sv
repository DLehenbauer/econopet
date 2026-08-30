// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

import common_pkg::*;

// HUD overlay device -- video scan-out substitution.
//
// Firmware writes a run of screen codes into hud_buf; while enabled, the
// fabric substitutes them over the video controller's VRAM reads at scan-out.
// Firmware supplies the 11-bit VRAM offset, so there is no column-mode logic.
module hud (
    input  logic wb_clock_i,

    // Wishbone B4 peripheral (MCU side) -- buffer + control-register writes.
    input  logic [WB_ADDR_WIDTH-1:0] wbp_addr_i,
    input  logic [   DATA_WIDTH-1:0] wbp_data_i,
    output logic [   DATA_WIDTH-1:0] wbp_data_o,
    input  logic                     wbp_we_i,
    input  logic                     wbp_cycle_i,
    input  logic                     wbp_strobe_i,
    output logic                     wbp_stall_o,
    output logic                     wbp_ack_o,
    input  logic                     wbp_sel_i,

    // Video scan-out substitution.
    input  logic [WB_ADDR_WIDTH-1:0] vid_addr_i,    // video controller read address
    output logic                     ov_active_o,   // substitute on this read
    output logic [   DATA_WIDTH-1:0] ov_char_o      // HUD character to substitute
);
    // ------------------------------------------------------------------
    // Control registers + text buffer
    // ------------------------------------------------------------------
    logic                       enable   = 1'b0;
    logic [VRAM_ADDR_WIDTH-1:0] base_off = '0;
    logic [     DATA_WIDTH-1:0] len      = '0;     // 0..255 characters

    // Single write port (peripheral) + single registered read port (scan-out)
    // -> maps to one dual-port BRAM instead of registers.
    logic [DATA_WIDTH-1:0] hud_buf [HUD_BUF_SIZE-1:0];

    // ------------------------------------------------------------------
    // Wishbone peripheral (MCU writes buffer + control regs). Buffer is
    // write-only over this port (firmware never reads it back).
    // ------------------------------------------------------------------
    wire [HUD_ADDR_WIDTH-1:0] p_addr = wbp_addr_i[HUD_ADDR_WIDTH-1:0];
    wire                      p_ctrl = p_addr[8];        // 1 = control regs, 0 = buffer
    wire [7:0]                p_idx  = p_addr[7:0];
    wire                      wb_req = wbp_sel_i && wbp_cycle_i && wbp_strobe_i;

    assign wbp_stall_o = 1'b0;

    always_ff @(posedge wb_clock_i) begin
        wbp_ack_o <= 1'b0;

        if (wb_req) begin
            wbp_ack_o <= 1'b1;
            if (p_ctrl) begin
                unique case (p_idx[3:0])
                    HUD_REG_CTRL: begin
                        wbp_data_o <= {7'b0, enable};
                        if (wbp_we_i) enable <= wbp_data_i[0];
                    end
                    HUD_REG_OFF_LO: begin
                        wbp_data_o <= base_off[7:0];
                        if (wbp_we_i) base_off[7:0] <= wbp_data_i;
                    end
                    HUD_REG_OFF_HI: begin
                        wbp_data_o <= {5'b0, base_off[VRAM_ADDR_WIDTH-1:8]};
                        if (wbp_we_i) base_off[VRAM_ADDR_WIDTH-1:8] <= wbp_data_i[VRAM_ADDR_WIDTH-1-8:0];
                    end
                    HUD_REG_LEN: begin
                        wbp_data_o <= len;
                        if (wbp_we_i) len <= wbp_data_i;
                    end
                    default: wbp_data_o <= 8'h00;
                endcase
            end else begin
                wbp_data_o <= 8'h00;
                if (wbp_we_i) hud_buf[p_idx] <= wbp_data_i;
            end
        end
    end

    // ------------------------------------------------------------------
    // Scan-out substitution.
    //
    // The video read address is wb_vram_addr(offset); decode it and test
    // whether 'offset' falls inside the overlay run [base_off, base_off+len).
    // ov_char_o is a registered BRAM read -- relies on the video controller
    // holding vid_addr_i stable for several cycles.
    // ------------------------------------------------------------------
    wire is_vram = vid_addr_i[WB_ADDR_WIDTH-1:VRAM_ADDR_WIDTH] == WB_VRAM_BASE;
    wire [VRAM_ADDR_WIDTH-1:0] vid_off = vid_addr_i[VRAM_ADDR_WIDTH-1:0];

    // base_off <= vid_off < base_off + len. Compute the high bound in 12 bits
    // so base_off + len can't wrap the 11-bit offset space.
    wire [VRAM_ADDR_WIDTH:0] hi = {1'b0, base_off} + {{(VRAM_ADDR_WIDTH+1-DATA_WIDTH){1'b0}}, len};
    wire in_range = enable && is_vram
                 && (vid_off >= base_off)
                 && ({1'b0, vid_off} < hi);

    // Index within the run (0..len-1; the difference is < 256 when in range).
    wire [7:0] idx = vid_off[7:0] - base_off[7:0];

    logic [DATA_WIDTH-1:0] ov_char_r;
    always_ff @(posedge wb_clock_i) ov_char_r <= hud_buf[idx];

    assign ov_active_o = in_range;
    assign ov_char_o   = ov_char_r;
endmodule
