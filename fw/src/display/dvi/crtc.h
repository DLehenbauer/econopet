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

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "system_state.h"

/**
 * Display geometry calculated from CRTC registers.
 * 
 * This structure holds all the derived values needed to render the display,
 * computed from the raw CRTC register values and system configuration.
 */
typedef struct dvi_display_geometry_s {
    // Character grid dimensions
    uint chars_per_row;         // Characters per row (adjusted for display mode)
    uint rows;                  // Number of character rows
    uint scanlines_per_row;     // Scanlines per character row
    
    // Video RAM addressing
    uint vram_start;            // Start offset in video buffer
    uint vram_mask;             // Address wrap mask
    
    // Display inversion
    uint8_t invert_mask;        // 0x00 = normal, 0xFF = inverted
    
    // Visible area positioning (in scanlines)
    uint visible_scanlines;     // Total visible scanlines
    uint top_margin;            // Scanlines before visible area starts
    
    // Display mode
    bool double_width;          // true = 40-col (16px chars), false = 80-col (8px chars)
    
    // TMDS buffer layout (in 32-bit words per lane)
    uint left_margin_words;     // Blank words at start of each lane
    uint content_words;         // Content words per lane
    uint right_margin_words;    // Blank words at end of each lane
} dvi_display_geometry_t;

/**
 * Calculate display geometry from CRTC registers.
 * 
 * Interprets the CRTC register values to compute all the derived geometry
 * needed for rendering the display. This function performs the same role
 * as the CRTC chip in the original PET hardware.
 * 
 * @param crtc          Array of CRTC register values
 * @param columns       Display column mode (40 or 80)
 * @param frame_width   Frame width in pixels
 * @param frame_height  Frame height in scanlines
 * @param font_width    Font character width in pixels (typically 8)
 * @param symbols_per_word  TMDS symbols per 32-bit word (typically 2)
 * @param out           Output geometry structure
 */
static inline void crtc_calculate_geometry(
    const uint8_t crtc[CRTC_REG_COUNT],
    pet_display_columns_t columns,
    uint frame_width,
    uint frame_height,
    uint font_width,
    uint symbols_per_word,
    dvi_display_geometry_t* out
) {
    const uint words_per_lane = frame_width / symbols_per_word;

    // Extract values from CRTC registers
    uint h_displayed   = crtc[CRTC_R1_H_DISPLAYED];                     // R1[7:0]: Horizontal displayed characters

    // Clamp h_displayed to prevent unsigned underflow when computing margins.
    // This guards against garbage CRTC register values during initialization.
    const uint max_h_displayed = frame_width / 16;
    if (h_displayed > max_h_displayed) {
        h_displayed = max_h_displayed;
    }

    const uint v_displayed   = crtc[CRTC_R6_V_DISPLAYED] & 0x7F;        // R6[6:0]: Vertical displayed character rows
    uint lines_per_row = (crtc[CRTC_R9_MAX_SCAN_LINE] & 0x1F) + 1;      // R9[4:0]: Scan lines per character row (plus one)

    // Clamp lines_per_row to fit v_displayed rows within frame_height, ensuring at least
    // one blank scanline per frame to reload CRTC values. Guards against division by zero.
    if (v_displayed > 0) {
        uint max_lines = frame_height / v_displayed;
        if (lines_per_row > max_lines) {
            lines_per_row = max_lines;
        }
    }

    uint display_start = ((crtc[CRTC_R12_START_ADDR_HI] & 0x3f) << 8)   // R12[5:0]: High 6 bits of display start address
                         | crtc[CRTC_R13_START_ADDR_LO];                // R13[7:0]: Low 8 bits of display start address

    // ma[12] = invert video (1 = normal, 0 = inverted)
    out->invert_mask = display_start & (1 << 12) ? 0x00 : 0xff;

    const uint y_visible = v_displayed * lines_per_row;   // Total visible scan lines
    const uint y_start   = (frame_height - y_visible) / 2;   // Top margin in scan lines

    // Compute left margin based on horizontal displayed characters. Note that h_displayed
    // has not yet been adjusted for 80-column mode, so is 1/2 the final value assuming 8 pixel
    // characters. h_displayed is already clamped to max_h_displayed, so this cannot underflow.
    const uint x_start = max_h_displayed - h_displayed;   // Left margin in 8-pixel units

    // Precompute TMDS word offsets for margins and content for the frame
    const uint left_margin_words = x_start * font_width / symbols_per_word;
    const uint content_pixels = h_displayed * font_width * 2;           // Always * 2 because h_displayed not yet adjusted for 80-columns
    const uint content_words = content_pixels / symbols_per_word;       // Always word aligned
    const uint right_margin_start = left_margin_words + content_words;
    const uint right_margin_words = words_per_lane - right_margin_start;

    const bool is_80_col = (columns == pet_display_columns_80);
    const uint display_mask = is_80_col ? 0x7ff : 0x3ff;

    if (is_80_col) {
        h_displayed <<= 1;              // Double horizontal displayed characters for 80-column mode
        display_start <<= 1;            // Double start address for 80-column mode
    }

    display_start &= display_mask;

    // Fill output structure
    out->chars_per_row = h_displayed;
    out->rows = v_displayed;
    out->scanlines_per_row = lines_per_row;
    out->vram_start = display_start;
    out->vram_mask = display_mask;
    out->visible_scanlines = y_visible;
    out->top_margin = y_start;
    out->double_width = !is_80_col;
    out->left_margin_words = left_margin_words;
    out->content_words = content_words;
    out->right_margin_words = right_margin_words;
}