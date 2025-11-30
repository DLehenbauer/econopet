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

#include "video.h"
#include "pet.h"
#include "roms/roms.h"
#include "system_state.h"

#define FRAME_WIDTH 720
#define FRAME_HEIGHT (480 / DVI_VERTICAL_REPEAT)
#define DVI_TIMING dvi_timing_720x480p_60hz

#define FONT_WIDTH 8
#define FONT_HEIGHT 8

struct dvi_inst dvi0;
struct semaphore dvi_start_sem;

bool video_graphics = false;
uint8_t video_char_buffer[VIDEO_CHAR_BUFFER_BYTE_SIZE] = { 0 };

// ---------------------------------------------------------------------------
// CRTC (6545) registers
// http://archive.6502.org/datasheets/rockwell_r6545-1_crtc.pdf
// ---------------------------------------------------------------------------

#define CRTC_R0_H_TOTAL             0   // [7:0] Total displayed and non-displayed characters, minus one, per horizontal line.
                                        //       The frequency of HSYNC is thus determined by this register.
                
#define CRTC_R1_H_DISPLAYED         1   // [7:0] Number of displayed characters per horizontal line.
                
#define CRTC_R2_H_SYNC_POS          2   // [7:0] Position of the HSYNC on the horizontal line, in terms of the character location number on the line.
                                        //       The position of the HSYNC determines the left-to-right location of the displayed text on the video screen.
                                        //       In this way, the side margins are adjusted.

#define CRTC_R3_SYNC_WIDTH          3   // [3:0] Width of HSYNC in character clock times (0 = HSYNC off)
                                        // [7:4] Width of VSYNC in scan lines (0 = 16 scanlines)

#define CRTC_R4_V_TOTAL             4   // [6:0] Total number of character rows in a frame, minus one. This register, along with R5,
                                        //       determines the overall frame rate, which should be close to the line frequency to
                                        //       ensure flicker-free appearance. If the frame time is adjusted to be longer than the
                                        //       period of the line frequency, then /RES may be used to provide absolute synchronism.

#define CRTC_R5_V_ADJUST            5   // [4:0] Number of additional scan lines needed to complete an entire frame scan and is intended
                                        //       as a fine adjustment for the video frame time.

#define CRTC_R6_V_DISPLAYED         6   // [6:0] Number of displayed character rows in each frame. In this way, the vertical size of the
                                        //       displayed text is determined.
            
#define CRTC_R7_V_SYNC_POS          7   // [6:0] Selects the character row time at which the VSYNC pulse is desired to occur and, thus,
                                        //       is used to position the displayed text in the vertical direction.

#define CRTC_R8_MODE_CONTROL        8   // [7:0] Selects operating mode [Not implemented]
                                        //
                                        //       [0] Must be 0
                                        //       [1] Not used
                                        //       [2] RAD: Refresh RAM addressing Mode (0 = straight binary, 1 = row/col)
                                        //       [3] Must be 0
                                        //       [4] DES: Display Enable Skew (0 = no delay, 1 = delay Display Enable one char at a time)
                                        //       [5] CSK: Cursor Skew (0 = no delay, 1 = delay Cursor one char at a time)
                                        //       [6] Not used
                                        //       [7] Not used

#define CRTC_R9_MAX_SCAN_LINE       9   // [4:0] Number of scan lines per character row, minus one, including spacing.
                                        //
                                        //       Graphics Mode: 7 -> 8 scan lines per row (full character)
                                        //           Text Mode: 9 -> 10 scan lines per row (full character + 2 blank lines)

#define CRTC_R10_CURSOR_START_LINE  10  // [6:0] Cursor blink mode and starting scan line [Not implemented]
                                        //
                                        //       [6:5] Cursor blink mode with respect to field rate.
                                        //             (00 = on, 01 = off, 10 = 1/16 rate, 11 = 1/32 rate)
                                        //
                                        //       [4:0] Starting scan line

#define CRTC_R11_CURSOR_END_LINE    11  // [4:0] Ending scan line of cursor [Not implemented]

#define CRTC_R12_START_ADDR_HI      12  // [5:0] High 6 bits of 14 bit display address (starting offset in video_char_buffer).
                                        //
                                        //       Note that on the PET, bits MA[13:12] do not address video RAM and instead are
                                        //       used for special functions:
                                        //
                                        //         [5] TA13 selects an alternative character rom (0 = normal, 1 = international)
                                        //         [4] TA12 inverts the video signal (0 = inverted, 1 = normal)

#define CRTC_R13_START_ADDR_LO      13  // [7:0] Low 8 bits of 14 bit display address (starting offset in video_char_buffer).

// Initialize CRTC registers with default values for standard PET display
uint8_t pet_crtc_registers[CRTC_REG_COUNT] = {
    [CRTC_R0_H_TOTAL]            = 0x31, // Horizontal total (minus one)
    [CRTC_R1_H_DISPLAYED]        = 0x28, // Displayed (40 chars)
    [CRTC_R2_H_SYNC_POS]         = 0x29, // HSYNC position
    [CRTC_R3_SYNC_WIDTH]         = 0x0F, // Sync widths
    [CRTC_R4_V_TOTAL]            = 0x28, // Vertical total (minus one)
    [CRTC_R5_V_ADJUST]           = 0x05, // Vertical adjust
    [CRTC_R6_V_DISPLAYED]        = 0x19, // Vertical displayed (25 rows)
    [CRTC_R7_V_SYNC_POS]         = 0x21, // VSYNC position
    [CRTC_R8_MODE_CONTROL]       = 0x00, // Mode control (unused)
    [CRTC_R9_MAX_SCAN_LINE]      = 0x07, // Num scan lines per row (minus one)
    [CRTC_R12_START_ADDR_HI]     = 0x10, // Display start high (ma[13]=0: no option ROM, ma[12]=1: normal video)
    [CRTC_R13_START_ADDR_LO]     = 0x00, // Display start low
    // Remaining registers unimplemented -> 0
};

static inline uint16_t __not_in_flash_func(stretch_x)(uint16_t x) {
    x = (x | (x << 4)) & 0x0F0F;
    x = (x | (x << 2)) & 0x3333;
    x = (x | (x << 1)) & 0x5555;
    return x | (x << 1);
}

static uint8_t __attribute__((aligned(4))) scanline[FRAME_WIDTH / FONT_WIDTH] = { 0 };

// ---------------------------------------------------------------------------
// TMDS encoding wrappers
//
// These inline functions wrap tmds_encode_1bpp with an API that matches the
// eventual target functions: tmds_encode_font_2bpp and tmds_encode_font_2bpp_wide.
//
// The color-related parameters (colourbuf, plane) are currently unused/ignored.
// When migrating to the real tmds_encode_font_2bpp functions, replace these
// wrapper calls with direct calls to the assembly implementations.
// ---------------------------------------------------------------------------

/**
 * Encode characters to TMDS for 80-column mode (8px wide characters).
 *
 * This wrapper renders character glyphs to a pixel buffer and calls
 * tmds_encode_1bpp. It matches the signature of tmds_encode_font_2bpp
 * from PicoDVI's colour_terminal app.
 *
 * @param charbuf       Character buffer (video RAM)
 * @param colourbuf     Color buffer (unused - pass NULL)
 * @param tmdsbuf       Output TMDS buffer
 * @param n_chars       Number of characters to encode
 * @param font_base     Font bitmap base (8 bytes per character)
 * @param scanline_idx  Scanline index within character row (0-7, >7 = blank)
 * @param plane         Color plane 0-2 (unused - pass 0)
 * @param invert        0x00 normal, 0xFF to invert video
 */
static inline void __not_in_flash_func(tmds_encode_font_2bpp_inline)(
    const uint8_t *charbuf,
    const uint8_t *colourbuf,
    uint32_t *tmdsbuf,
    uint n_chars,
    const uint8_t *font_base,
    uint scanline_idx,
    uint plane,
    uint32_t invert
) {
    (void)colourbuf;  // Unused for now
    (void)plane;      // Unused for now

    const uint8_t invert_mask = (uint8_t)invert;
    uint8_t *p_dest = scanline;

    for (uint i = 0; i < n_chars; i++) {
        uint8_t p8;

        if (scanline_idx >= FONT_HEIGHT) {
            // Outside character height: blank line (respects invert)
            p8 = invert_mask;
        } else {
            const uint8_t c = charbuf[i];
            p8 = font_base[(c & 0x7f) * FONT_HEIGHT + scanline_idx];
            p8 ^= invert_mask;
            if (c & 0x80) {
                p8 ^= 0xff;  // Invert if bit 7 set in character code
            }
        }

        *p_dest++ = p8;
    }

    tmds_encode_1bpp((const uint32_t *)scanline, tmdsbuf, n_chars * FONT_WIDTH);
}

/**
 * Encode characters to TMDS for 40-column mode (16px wide characters, 2x stretch).
 *
 * This wrapper renders character glyphs with 2x horizontal stretch to a pixel
 * buffer and calls tmds_encode_1bpp. It matches the signature of
 * tmds_encode_font_2bpp_wide from PicoDVI's colour_terminal app.
 *
 * @param charbuf       Character buffer (video RAM)
 * @param colourbuf     Color buffer (unused - pass NULL)
 * @param tmdsbuf       Output TMDS buffer
 * @param n_chars       Number of characters to encode
 * @param font_base     Font bitmap base (8 bytes per character)
 * @param scanline_idx  Scanline index within character row (0-7, >7 = blank)
 * @param plane         Color plane 0-2 (unused - pass 0)
 * @param invert        0x00 normal, 0xFF to invert video
 */
static inline void __not_in_flash_func(tmds_encode_font_2bpp_wide_inline)(
    const uint8_t *charbuf,
    const uint8_t *colourbuf,
    uint32_t *tmdsbuf,
    uint n_chars,
    const uint8_t *font_base,
    uint scanline_idx,
    uint plane,
    uint32_t invert
) {
    (void)colourbuf;  // Unused for now
    (void)plane;      // Unused for now

    const uint8_t invert_mask = (uint8_t)invert;
    uint8_t *p_dest = scanline;

    for (uint i = 0; i < n_chars; i++) {
        uint8_t p8;

        if (scanline_idx >= FONT_HEIGHT) {
            // Outside character height: blank line (respects invert)
            p8 = invert_mask;
        } else {
            const uint8_t c = charbuf[i];
            p8 = font_base[(c & 0x7f) * FONT_HEIGHT + scanline_idx];
            p8 ^= invert_mask;
            if (c & 0x80) {
                p8 ^= 0xff;  // Invert if bit 7 set in character code
            }
        }

        // Stretch each pixel horizontally by 2x
        uint16_t p16 = stretch_x(p8);
        *p_dest++ = p16 >> 8;
        *p_dest++ = (uint8_t)p16;
    }

    tmds_encode_1bpp((const uint32_t *)scanline, tmdsbuf, n_chars * FONT_WIDTH * 2);
}

// Precalculated TMDS-encoded blank scanline. This buffer is dequeued from
// dvi0.q_tmds_free during video_init() and kept permanently. For blank scanlines
// (y >= y_visible), we memcpy from this buffer instead of re-encoding each time.
static uint32_t* blank_tmdsbuf;

static inline void __not_in_flash_func(prepare_scanline)(uint16_t y) {
    static uint h_displayed   = 40;	        // Horizontal displayed characters
    static uint v_displayed   = 25;	        // Vertical displayed characters
    static uint lines_per_row = 8;	        // Vertical scan lines per character
    static uint display_start = 0x1000;     // Start address in video_char_buffer (ma[13:12] are special)
    static uint display_mask  = 0x3ff;      // Mask for display address wrapping
    static uint8_t invert_mask = 0x00;      // ma[12] = invert video (1 = normal, 0 = inverted)

	static uint x_start = 0;
	static uint y_start = 0;
	static uint y_visible = 0;
    static bool is_80_col = false;
    static uint left_margin_words = 0;
    static uint content_pixels = 0;
    static uint content_words = 0;
    static uint right_margin_start = 0;
    static uint right_margin_words = 0;

    // TODO: Copy character into SRAM and precalculate flip/stretch? (PERF)
    static const uint8_t* p_char_rom = rom_chars_8800;

    // For convenience, remap local `y` so that `y == 0` is the first visible scan line.
    // Because `y` is unsigned, the top blank area wraps around to a large integer.
    y -= y_start;

    if (y >= y_visible) {
        // Blank scan line.  Because scan lines are recycled, this also clears the unused overscan
        // area on the left/right of the visible region.
        memset(scanline, 0, sizeof(scanline));

		// Use blank scan lines to reload/recompute CRTC-dependent values.
        h_displayed   = pet_crtc_registers[CRTC_R1_H_DISPLAYED];	                // R1[7:0]: Horizontal displayed characters
        v_displayed   = pet_crtc_registers[CRTC_R6_V_DISPLAYED] & 0x7F;	            // R6[6:0]: Vertical displayed character rows
        lines_per_row = (pet_crtc_registers[CRTC_R9_MAX_SCAN_LINE] & 0x1F) + 1;     // R9[4:0]: Scan lines per character row (plus one)

        display_start = ((pet_crtc_registers[CRTC_R12_START_ADDR_HI] & 0x3f) << 8)  // R12[5:0]: High 6 bits of display start address
                        | pet_crtc_registers[CRTC_R13_START_ADDR_LO];               // R13[7:0]: Low 8 bits of display start address

        invert_mask = display_start & (1 << 12) ? 0x00 : 0xff;                      // ma[12] = invert video (1 = normal, 0 = inverted)

        // Clamp `lines_per_row` to fit `v_displayed` rows within `FRAME_HEIGHT`, ensuring at least
        // one blank scanline per frame to reload CRTC values. Guards against division by zero.
        if (v_displayed > 0) {
            lines_per_row = MIN(lines_per_row, FRAME_HEIGHT / v_displayed);
        }

		y_visible = v_displayed * lines_per_row;	    // Total visible scan lines
		y_start   = (FRAME_HEIGHT - y_visible) / 2;	    // Top margin in scan lines

        // Compute left margin based on horizontal displayed characters. Note that `h_displayed`
        // has not yet been adjusted for 80-column mode, so is 1/2 the final value assuming 8 pixel
        // characters.
		x_start = ((FRAME_WIDTH / 16) - h_displayed);   // Left margin in bytes (8 pixels)

        // Precompute TMDS word offsets for margins and content for the frame
        left_margin_words = x_start * FONT_WIDTH / DVI_SYMBOLS_PER_WORD;
        content_pixels = h_displayed * FONT_WIDTH * 2;          // Always * 2 because h_displayed not yet adjusted for 80-columns
        content_words = content_pixels / DVI_SYMBOLS_PER_WORD;  // Always word aligned: division by DVI_SYMBOLS_PER_WORD cancels * 2 above.
        right_margin_start = left_margin_words + content_words;
        right_margin_words = (FRAME_WIDTH / DVI_SYMBOLS_PER_WORD) - right_margin_start;

        is_80_col = (system_state.pet_display_columns == pet_display_columns_80);
        if (is_80_col) {
            h_displayed <<= 1;              // Double horizontal displayed characters for 80-column mode
            display_start <<= 1;            // Double start address for 80-column mode
            display_mask = 0x7ff;           // 80 columns machine has 2Kb video RAM
        } else {
            display_mask = 0x3ff;           // 40 columns machine has 1Kb video RAM
        }

        display_start &= display_mask;

        // Select graphics/text character ROM
		p_char_rom = video_graphics
			? p_video_font_400
			: p_video_font_000;

        uint32_t *tmdsbuf;
        queue_remove_blocking(&dvi0.q_tmds_free, &tmdsbuf);
        memcpy(tmdsbuf, blank_tmdsbuf, (FRAME_WIDTH / DVI_SYMBOLS_PER_WORD) * sizeof(uint32_t));
        dvi_validate_tmds_buffer(tmdsbuf);
        queue_add_blocking(&dvi0.q_tmds_valid, &tmdsbuf);
    } else {
        // Calculate start offset in video_char_buffer for this row.
        // Note: display_start may have bits above display_mask (ma[13:12] are special),
        // so we mask only when computing the actual buffer index.
        const uint row_offset = display_start + y / lines_per_row * h_displayed;
        const uint row_start = row_offset & display_mask;

        const uint ra = y % lines_per_row;

        uint32_t *tmdsbuf;
        queue_remove_blocking(&dvi0.q_tmds_free, &tmdsbuf);

        // Copy left margin from precalculated blank TMDS buffer
        memcpy(tmdsbuf, blank_tmdsbuf, left_margin_words * sizeof(uint32_t));

        // Check if the row wraps around the video buffer boundary.
        // The buffer size is (display_mask + 1), so we wrap if row_start + h_displayed > display_mask + 1.
        const uint buffer_size = display_mask + 1;
        const uint first_chars = MIN(buffer_size - row_start, h_displayed);

        // First part: from row_start (may be all characters if no wrap)
        if (is_80_col) {
            tmds_encode_font_2bpp_inline(
                &video_char_buffer[row_start],
                NULL,                           // colourbuf (unused)
                &tmdsbuf[left_margin_words],
                first_chars,
                p_char_rom,
                ra,
                0,                              // plane (unused)
                invert_mask
            );
        } else {
            tmds_encode_font_2bpp_wide_inline(
                &video_char_buffer[row_start],
                NULL,                           // colourbuf (unused)
                &tmdsbuf[left_margin_words],
                first_chars,
                p_char_rom,
                ra,
                0,                              // plane (unused)
                invert_mask
            );
        }

        // Second part: wrap-around from start of buffer (if needed)
        if (first_chars < h_displayed) {
            const uint second_chars = h_displayed - first_chars;
            uint first_words = first_chars * FONT_WIDTH / DVI_SYMBOLS_PER_WORD;

            if (is_80_col) {
                tmds_encode_font_2bpp_inline(
                    &video_char_buffer[0],
                    NULL,
                    &tmdsbuf[left_margin_words + first_words],
                    second_chars,
                    p_char_rom,
                    ra,
                    0,
                    invert_mask
                );
            } else {
                first_words *= 2;   // 40-column mode: each character is 16 pixels wide
                tmds_encode_font_2bpp_wide_inline(
                    &video_char_buffer[0],
                    NULL,
                    &tmdsbuf[left_margin_words + first_words],
                    second_chars,
                    p_char_rom,
                    ra,
                    0,
                    invert_mask
                );
            }
        }

        // Copy right margin from precalculated blank TMDS buffer
        memcpy(&tmdsbuf[right_margin_start], &blank_tmdsbuf[right_margin_start], right_margin_words * sizeof(uint32_t));

        dvi_validate_tmds_buffer(tmdsbuf);
        queue_add_blocking(&dvi0.q_tmds_valid, &tmdsbuf);
    }
}

static void __not_in_flash_func(core1_scanline_callback)() {
    static uint y = 0;
    prepare_scanline(y);
    y = (y + 1) % FRAME_HEIGHT;
}

static void core1_main() {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    sem_acquire_blocking(&dvi_start_sem);
    dvi_start(&dvi0);

    while (1) {
        __wfi();
    }
    __builtin_unreachable();
}

void video_init() {
    const uint32_t f_clk_sys = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS);
    const int32_t delta = f_clk_sys - DVI_TIMING.bit_clk_khz;
    if (!(-1 <= delta && delta <= 1)) {
        panic("FAIL: Incorrect clk_sys frequency. Expected %d +/-1 kHz, but got %d kHz.", DVI_TIMING.bit_clk_khz, f_clk_sys);
    }

    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = micromod_cfg;
    dvi0.scanline_callback = core1_scanline_callback;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    // Dequeue one TMDS buffer from the free queue to use as our precalculated blank scanline.
    // This buffer is kept permanently and never returned to the queue.
    queue_remove_blocking(&dvi0.q_tmds_free, &blank_tmdsbuf);
    
    // Precalculate TMDS-encoded blank scanline (all zeros).
    // This is used for blank scanlines (y >= y_visible) instead of re-encoding each time.
    memset(scanline, 0, sizeof(scanline));
    tmds_encode_1bpp((const uint32_t*) scanline, blank_tmdsbuf, FRAME_WIDTH);
    dvi_validate_tmds_buffer(blank_tmdsbuf);

    // Prepare initial scanline before starting DVI on core 1.
    dvi0.scanline_callback();

    sem_init(&dvi_start_sem, /* initial_permits: */ 0, /* max_permits: */ 1);
    hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);
    multicore_launch_core1(core1_main);
    sem_release(&dvi_start_sem);
}
