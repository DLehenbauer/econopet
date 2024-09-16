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

module video_crtc(
    input  logic        reset_i,
    input  logic        sys_clock_i,            // FPGA System clock
    input  logic        wr_strobe_i,            // Gates writes to CRTC registers
    input  logic        cclk_en_i,              // Gates system clock to 1 MHz character clock
    input  logic        cs_i,                   // CRTC selected for data transfer (driven by address decoding)
    input  logic        rw_ni,                  // Direction of date transfers (0 = writing to CRTC, 1 = reading from CRTC)

    input  logic        rs_i,                   // Register select (0 = write address/read status, 1 = read addressed register)

    input  logic  [7:0] data_i,                 // Transfer data written from CPU to CRTC when CS asserted and /RW is low
    output logic  [7:0] data_o,                 // Transfer data read by CPU from CRTC when CS asserted and /RW is high
    output logic        data_oe,                // Asserted when CPU is reading from CRTC

    output logic        h_sync_o,               // Horizontal sync
    output logic        v_sync_o,               // Vertical sync
    output logic        de_o,                   // Display enable

    output logic [13:0] ma_o,                   // Refresh RAM address lines
    output logic  [4:0] ra_o                    // Raster address lines
);
    localparam R0_H_TOTAL           = 0,    // [7:0] Total displayed and non-displayed characters, minus one, per horizontal line.
                                            //       The frequency of HSYNC is thus determined by this register.
                
               R1_H_DISPLAYED       = 1,    // [7:0] Number of displayed characters per horizontal line.
                
               R2_H_SYNC_POS        = 2,    // [7:0] Position of the HSYNC on the horizontal line, in terms of the character location number on the line.
                                            //       The position of the HSYNC determines the left-to-right location of the displayed text on the video screen.
                                            //       In this way, the side margins are adjusted.

               R3_SYNC_WIDTH        = 3,    // [3:0] Width of HSYNC in character clock times (0 = HSYNC off)
                                            // [7:4] Width of VSYNC in scan lines (0 = 16 scanlines)

               R4_V_TOTAL           = 4,    // [6:0] Total number of character rows in a frame, minus one. This register, along with R5,
                                            //       determines the overall frame rate, which should be close to the line frequency to
                                            //       ensure flicker-free appearance. If the frame time is adjusted to be longer than the
                                            //       period of the line frequency, then /RES may be used to provide absolute synchronism.

               R5_V_ADJUST          = 5,    // [4:0] Number of additional scan lines needed to complete an entire frame scan and is intended
                                            //       as a fine adjustment for the video frame time.

               R6_V_DISPLAYED       = 6,    // [6:0] Number of displayed character rows in each frame. In this way, the vertical size of the
                                            //       displayed text is determined.
            
               R7_V_SYNC_POS        = 7,    // [6:0] Selects the character row time at which the VSYNC pulse is desired to occur and, thus,
                                            //       is used to position the displayed text in the vertical direction.

               R9_MAX_SCAN_LINE     = 9,    // [4:0] Number of scan lines per character row, including spacing.

               R12_START_ADDR_HI    = 12,   // [5:0] High 6 bits of 14 bit display address (starting address of screen_addr_o[13:8]).
               R13_START_ADDR_LO    = 13;   // [7:0] Low 8 bits of 14 bit display address (starting address of screen_addr_o[7:0]).

    logic [4:0] ar = '0;                    // Address register used to select R0..17
    logic [7:0] r[31:0];                    // Storage for R0..17 (extended to next power of 2)

    // CRTC drives data when the current data transfer is reading from the CRTC
    //
    // TODO:
    //  - Status register POR state should be 'x01xxxxx'
    //  - Vertical retrace status bit should fall to 0 cclk ticks before retrace ends
    //
    // (See http://archive.6502.org/datasheets/rockwell_r6545-1_crtc.pdf, pg. 3)

    assign data_oe = rw_ni && cs_i;
    assign data_o  = rs_i == '0
        ? { 2'b0, v_sync, 5'b0 }                // RS = 0: Read status register
        : r[ar];                                // RS = 1: Read addressed register R0..17 (TODO: Allow this?  Infers dual-port RAM?)

    initial begin
        r[R0_H_TOTAL]           = 8'd63;
        r[R1_H_DISPLAYED]       = 8'd40;
        r[R2_H_SYNC_POS]        = 8'd48;
        r[R3_SYNC_WIDTH]        = 8'h51;
        r[R4_V_TOTAL]           = { 1'd0, 7'd32 };
        r[R5_V_ADJUST]          = { 3'd0, 5'd05 };
        r[R6_V_DISPLAYED]       = { 1'd0, 7'd25 };
        r[R7_V_SYNC_POS]        = { 1'd0, 7'd28 };
        r[R9_MAX_SCAN_LINE]     = { 3'd0, 5'd07 };
        r[R12_START_ADDR_HI]    = 8'h10;
        r[R13_START_ADDR_LO]    = 8'h00;
    end

    always_ff @(posedge sys_clock_i) begin
        if (wr_strobe_i && cs_i && !rw_ni) begin
            if (rs_i == '0) ar <= data_i[4:0];  // RS = 0: Write to address register
            else r[ar] <= data_i;               // RS = 1: Write to currently addressed register (R0..17)
        end
    end

    wire  [7:0] h_total         = r[R0_H_TOTAL];
    wire  [7:0] h_displayed     = r[R1_H_DISPLAYED];
    wire  [7:0] h_sync_pos      = r[R2_H_SYNC_POS];
    wire  [3:0] h_sync_width    = r[R3_SYNC_WIDTH][3:0];
    wire  [4:0] v_sync_width    = r[R3_SYNC_WIDTH][7:4] == 0            // Per Datasheet, when bits 4-7 are all
                                    ? 5'h10                             // "0", VSYNC will be 16 scan lines wide.
                                    : { 1'b0, r[R3_SYNC_WIDTH][7:4] };
    wire  [6:0] v_total         = r[R4_V_TOTAL][6:0];
    wire  [4:0] v_adjust        = r[R5_V_ADJUST][4:0];
    wire  [6:0] v_displayed     = r[R6_V_DISPLAYED][6:0];
    wire  [6:0] v_sync_pos      = r[R7_V_SYNC_POS][6:0];
    wire  [4:0] max_scan_line   = r[R9_MAX_SCAN_LINE][4:0];
    wire [13:0] start_addr      = { r[R12_START_ADDR_HI][5:0], r[R13_START_ADDR_LO] };

    // Horizontal

    logic [7:0] h_total_counter = '0;
    logic [3:0] h_sync_counter  = '0;
    logic       h_display       = 1'b1;
    logic       h_sync          = '0;

    wire last_column = h_total_counter == h_displayed;
    wire line_ending = h_total_counter == h_total;

    always_ff @(posedge sys_clock_i) begin
        if (reset_i) h_total_counter <= '0;
        else if (cclk_en_i) begin
            if (line_ending) h_total_counter <= '0;
            else h_total_counter <= h_total_counter + 1'b1;
        end
    end

    always_ff @(posedge sys_clock_i) begin
        if (reset_i) h_display <= 1'b1;
        else if (last_column) h_display <= '0;
        else if (h_total_counter == '0) h_display <= 1'b1;
    end

    always_ff @(posedge sys_clock_i) begin
        if (reset_i) h_sync <= '0;
        else if (h_sync_counter == h_sync_width) h_sync <= 1'b0;
        else if (h_total_counter == h_sync_pos) h_sync <= 1'b1;
    end

    always_ff @(posedge sys_clock_i) begin
        if (reset_i) h_sync_counter <= '0;
        else if (cclk_en_i) begin            
            if (h_sync) h_sync_counter <= h_sync_counter + 1'b1;
            else h_sync_counter <= '0;
        end
    end

    // Raster address generator

    logic [4:0] line_counter = '0;
    wire last_line  = line_counter == max_scan_line;
    wire row_ending = last_line && line_ending;

    always_ff @(posedge sys_clock_i) begin
        if (reset_i) line_counter <= '0;
        else if (cclk_en_i) begin
            if (frame_start) line_counter <= '0;
            else if (row_ending) line_counter <= '0;
            else if (line_ending) line_counter <= line_counter + 1'b1;
        end
    end

    // Vertical

    logic [6:0] v_total_counter = '0;
    logic [4:0] v_sync_counter  = '0;
    logic       v_display       = 1'b1;
    logic       v_sync          = '0;

    wire last_row = v_total_counter == v_total;

    always_ff @(posedge sys_clock_i) begin
        if (reset_i) v_total_counter <= '0;
        else if (cclk_en_i) begin
            if (frame_start) v_total_counter <= '0;
            else if (row_ending) v_total_counter <= v_total_counter + 1'b1;
        end
    end

    always_ff @(posedge sys_clock_i) begin
        if (reset_i) v_display <= 1'b1;
        else if (cclk_en_i) begin
            if (v_total_counter == v_displayed) v_display <= '0;
            else if (frame_start) v_display <= 1'b1;
        end
    end

    // TODO: 'v_sync_latched' appears to prevent multiple VSYNC pulses if v_total_counter/v_sync_pos
    //       change during the current frame.  Why?
    logic v_sync_latched = '0;

    always_ff @(posedge sys_clock_i) begin
        if (reset_i) begin
            v_sync <= '0;
            v_sync_latched <= '0;
        end else begin
            if (frame_start) v_sync_latched <= '0;
            if (v_sync_counter == v_sync_width) v_sync <= 1'b0;
            else if (v_total_counter == v_sync_pos && !v_sync_latched) begin
                v_sync_latched <= 1'b1;
                v_sync <= 1'b1;
            end
        end
    end

    always_ff @(posedge sys_clock_i) begin
        if (reset_i) v_sync_counter <= '0;
        else if (cclk_en_i) begin
            if (line_ending) begin
                if (v_sync) v_sync_counter <= v_sync_counter + 1'b1;
                else v_sync_counter <= '0;
            end
        end
    end

    // Frame

    logic [4:0] adjust_counter = '0;
    wire adjusting     = frame_state_d == ADJUSTING;
    wire adjust_ending = line_ending && adjust_counter == v_adjust;

    always_ff @(posedge sys_clock_i) begin
        if (reset_i) adjust_counter <= '0;
        else if (cclk_en_i) begin
            if (adjust_ending) adjust_counter <= '0;
            else if (adjusting && line_ending) adjust_counter <= adjust_counter + 1'b1;
        end
    end

    localparam
        NORMAL         = 3'b000,
        FRAME_ENDING   = 3'b001,
        ADJUST_PENDING = 3'b011,
        ADJUSTING      = 3'b101;
    
    logic [2:0] frame_state_d, frame_state_q = NORMAL;
    
    wire frame_ending = frame_state_q[0];
    wire frame_start = frame_ending && adjust_ending;

    always_comb begin
        // Do not move into 'default' case.
        frame_state_d = frame_state_q;
        
        unique case (frame_state_q)
            NORMAL: if (last_row && last_line) frame_state_d = FRAME_ENDING;

            FRAME_ENDING: begin
                if (v_adjust != '0) frame_state_d = ADJUST_PENDING;
                else if (line_ending) frame_state_d = NORMAL;
            end

            ADJUST_PENDING: if (line_ending) frame_state_d = ADJUSTING;
            ADJUSTING: if (adjust_ending) frame_state_d = NORMAL;
            default: begin
                // synthesis off
                $fatal(1, "Illegal 'frame_state_q' value: %0d", frame_state_q);
                // synthesis on
            end
        endcase
    end

    always_ff @(posedge sys_clock_i) begin
        if (reset_i) frame_state_q <= NORMAL;
        else if (cclk_en_i)frame_state_q <= frame_state_d;
    end

    // Linear address generator

    logic [13:0] row_addr = '0;

    always_ff @(posedge sys_clock_i) begin
        if (reset_i) row_addr <= '0;
        else if (cclk_en_i) begin
            if (frame_start) row_addr <= start_addr;
            else if (last_line && last_column) row_addr <= ma_o;
        end
    end

    always_ff @(posedge sys_clock_i) begin
        if (reset_i) ma_o <= '0;
        else if (cclk_en_i) begin
            if (frame_start) ma_o <= start_addr;
            else if (line_ending) ma_o <= row_addr;
            else ma_o <= ma_o + 1'b1;
        end
    end

    assign h_sync_o = h_sync;
    assign v_sync_o = v_sync; 
    assign de_o     = h_display && v_display;
    assign ra_o     = line_counter;
endmodule
