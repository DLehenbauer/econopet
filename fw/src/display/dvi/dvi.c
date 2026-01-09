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

#include "pch.h"
#include "dvi.h"

#include "pet.h"
#include "roms/roms.h"
#include "system_state.h"
#include "tmds_encode.h"

// Define VIDEO_CORE1_LOOP to use a tight loop in core1_main() like the colour_terminal
// demo, instead of using interrupt-driven scanline callbacks. This may be useful for
// debugging or profiling.
#define VIDEO_CORE1_LOOP

#define FRAME_WIDTH 720
#define FRAME_HEIGHT (480 / DVI_VERTICAL_REPEAT)
#define DVI_TIMING dvi_timing_720x480p_60hz

#define FONT_WIDTH 8
#define FONT_HEIGHT 8

// Number of 32-bit words per TMDS lane
#define WORDS_PER_LANE (FRAME_WIDTH / DVI_SYMBOLS_PER_WORD)

struct dvi_inst dvi0;
struct semaphore dvi_start_sem;

// C128 Palette (16 colors, RRRGGGBB)
static const uint8_t c128_palette[16] = {
    0x00, // 0: Black
    0x49, // 1: Medium Gray
    0x01, // 2: Blue
    0x03, // 3: Light Blue
    0x10, // 4: Green
    0x1C, // 5: Light Green
    0x0D, // 6: Dark Cyan
    0x1F, // 7: Light Cyan
    0x40, // 8: Red
    0xE0, // 9: Light Red
    0x81, // 10: Dark Magenta
    0xE3, // 11: Magenta
    0x6C, // 12: Dark Yellow
    0xDC, // 13: Yellow
    0xB6, // 14: Light Gray
    0xFF  // 15: White
};

// Color buffer: one byte per character position
// High nibble (bits 7:4) = background palette index (0-15)
// Low nibble (bits 3:0) = foreground palette index (0-15)
// Initialized to bright green (10) on black (0) = 0x0A
#define MAX_CHARS_PER_LINE (FRAME_WIDTH / FONT_WIDTH)

// ---------------------------------------------------------------------------
// TMDS encoding wrappers
//
// These inline functions call the optimized assembly encoders from 
// tmds_encode.S for each of the 3 RGB planes.
// ---------------------------------------------------------------------------

/**
 * Encode characters to TMDS for 80-column mode (8px wide characters).
 *
 * Calls the assembly tmds_encode_font_8px_palette_1lane for each RGB plane.
 *
 * @param charbuf       Character buffer (video RAM)
 * @param p_colorbuf    Color buffer (one byte per character)
 * @param tmdsbuf       Output TMDS buffer
 * @param n_chars       Number of characters to encode
 * @param font_base     Font bitmap base (8 bytes per character)
 * @param scanline_idx  Scanline index within character row (0-7, >7 = blank)
 * @param invert        0x00 normal, 0xFF to invert video
 */
static inline void __not_in_flash_func(tmds_encode_font_8px_palette)(
    const uint8_t *charbuf,
    const uint8_t *p_colorbuf,
    uint32_t *tmdsbuf,
    uint n_chars,
    const uint8_t *font_base,
    uint scanline_idx,
    uint32_t invert
) {
    const uint n_pix = n_chars * FONT_WIDTH;

    // Encode all 3 RGB planes
    for (uint plane = 0; plane < N_TMDS_LANES; ++plane) {
        tmds_encode_font_8px_palette_1lane(
            charbuf,
            p_colorbuf,
            &tmdsbuf[plane * WORDS_PER_LANE],
            n_pix,
            font_base,
            scanline_idx,
            plane,
            invert
        );
    }
}

/**
 * Encode characters to TMDS for 40-column mode (16px wide characters, 2x stretch).
 *
 * Calls the assembly tmds_encode_font_16px_palette_1lane for each RGB plane.
 *
 * @param charbuf       Character buffer (video RAM)
 * @param p_colorbuf    Color buffer (one byte per character)
 * @param tmdsbuf       Output TMDS buffer
 * @param n_chars       Number of characters to encode
 * @param font_base     Font bitmap base (8 bytes per character)
 * @param scanline_idx  Scanline index within character row (0-7, >7 = blank)
 * @param invert        0x00 normal, 0xFF to invert video
 */
static inline void __not_in_flash_func(tmds_encode_font_16px_palette)(
    const uint8_t *charbuf,
    const uint8_t *p_colorbuf,
    uint32_t *tmdsbuf,
    uint n_chars,
    const uint8_t *font_base,
    uint scanline_idx,
    uint32_t invert
) {
    const uint n_pix = n_chars * FONT_WIDTH * 2;  // 2x stretch

    // Encode all 3 RGB planes
    for (uint plane = 0; plane < N_TMDS_LANES; ++plane) {
        tmds_encode_font_16px_palette_1lane(
            charbuf,
            p_colorbuf,
            &tmdsbuf[plane * WORDS_PER_LANE],
            n_pix,
            font_base,
            scanline_idx,
            plane,
            invert
        );
    }
}

// Precalculated TMDS-encoded blank scanline. This buffer is dequeued from
// dvi0.q_tmds_free during video_init() and kept permanently. For blank scanlines
// (y >= y_visible), we memcpy from this buffer instead of re-encoding each time.
static uint32_t* blank_tmdsbuf;

// Copy left and right blank margins for all TMDS lanes into target buffer.
// left_margin_words/right_margin_words are counts of 32-bit words per lane.
static inline void copy_blank_margins(uint32_t *tmdsbuf, uint left_margin_words, uint right_margin_words) {
    const uint right_margin_start = WORDS_PER_LANE - right_margin_words;
    const size_t left_bytes = left_margin_words * sizeof(uint32_t);
    const size_t right_bytes = right_margin_words * sizeof(uint32_t);
    uint32_t* dst_lane = tmdsbuf;
    uint32_t* src_lane = blank_tmdsbuf;

    uint remaining = N_TMDS_LANES;
    while (remaining--) {
        memcpy(dst_lane, src_lane, left_bytes);
        memcpy(dst_lane + right_margin_start, src_lane + right_margin_start, right_bytes);
        dst_lane += WORDS_PER_LANE;
        src_lane += WORDS_PER_LANE;
    }
}

static inline void __not_in_flash_func(prepare_scanline)(uint16_t y) {
    static uint h_displayed   = 40;         // Horizontal displayed characters
    static uint v_displayed   = 25;         // Vertical displayed characters
    static uint lines_per_row = 8;          // Vertical scan lines per character
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
    static const uint8_t* p_char_rom = rom_chars_e800;

    uint8_t* const video_char_buffer = system_state.video_char_buffer;
    uint8_t* const colorbuf = video_char_buffer + 0x800;

    // Local pointer to CRTC registers for performance (avoids repeated struct member access)
    const uint8_t* const crtc = system_state.pet_crtc_registers;

    // For convenience, remap local `y` so that `y == 0` is the first visible scan line.
    // Because `y` is unsigned, the top blank area wraps around to a large integer.
    y -= y_start;

    if (y >= y_visible) {
        // Blank scan line - use blank scan lines to reload/recompute CRTC-dependent values.
        h_displayed   = crtc[CRTC_R1_H_DISPLAYED];                      // R1[7:0]: Horizontal displayed characters
        v_displayed   = crtc[CRTC_R6_V_DISPLAYED] & 0x7F;               // R6[6:0]: Vertical displayed character rows
        lines_per_row = (crtc[CRTC_R9_MAX_SCAN_LINE] & 0x1F) + 1;       // R9[4:0]: Scan lines per character row (plus one)

        display_start = ((crtc[CRTC_R12_START_ADDR_HI] & 0x3f) << 8)    // R12[5:0]: High 6 bits of display start address
                        | crtc[CRTC_R13_START_ADDR_LO];                 // R13[7:0]: Low 8 bits of display start address

        invert_mask = display_start & (1 << 12) ? 0x00 : 0xff;          // ma[12] = invert video (1 = normal, 0 = inverted)

        // Clamp `lines_per_row` to fit `v_displayed` rows within `FRAME_HEIGHT`, ensuring at least
        // one blank scanline per frame to reload CRTC values. Guards against division by zero.
        if (v_displayed > 0) {
            lines_per_row = MIN(lines_per_row, FRAME_HEIGHT / v_displayed);
        }

        y_visible = v_displayed * lines_per_row;    // Total visible scan lines
        y_start   = (FRAME_HEIGHT - y_visible) / 2;    // Top margin in scan lines

        // Compute left margin based on horizontal displayed characters. Note that `h_displayed`
        // has not yet been adjusted for 80-column mode, so is 1/2 the final value assuming 8 pixel
        // characters.
        x_start = ((FRAME_WIDTH / 16) - h_displayed);   // Left margin in bytes (8 pixels)

        // Precompute TMDS word offsets for margins and content for the frame
        left_margin_words = x_start * FONT_WIDTH / DVI_SYMBOLS_PER_WORD;
        content_pixels = h_displayed * FONT_WIDTH * 2;          // Always * 2 because h_displayed not yet adjusted for 80-columns
        content_words = content_pixels / DVI_SYMBOLS_PER_WORD;  // Always word aligned: division by DVI_SYMBOLS_PER_WORD cancels * 2 above.
        right_margin_start = left_margin_words + content_words;
        right_margin_words = WORDS_PER_LANE - right_margin_start;

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
        p_char_rom = system_state.video_graphics
            ? p_video_font_400
            : p_video_font_000;

        uint32_t *tmdsbuf;
        queue_remove_blocking(&dvi0.q_tmds_free, &tmdsbuf);
        memcpy(tmdsbuf, blank_tmdsbuf, N_TMDS_LANES * WORDS_PER_LANE * sizeof(uint32_t));
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

        // Copy left/right blank margins for all lanes
        copy_blank_margins(tmdsbuf, left_margin_words, right_margin_words);

        // Check if the row wraps around the video buffer boundary.
        // The buffer size is (display_mask + 1), so we wrap if row_start + h_displayed > display_mask + 1.
        const uint buffer_size = display_mask + 1;
        const uint first_chars = MIN(buffer_size - row_start, h_displayed);

        // First part: from row_start (may be all characters if no wrap)
        if (is_80_col) {
            tmds_encode_font_8px_palette(
                &video_char_buffer[row_start],
                &colorbuf[row_start],
                &tmdsbuf[left_margin_words],
                first_chars,
                p_char_rom,
                ra,
                invert_mask
            );
        } else {
            tmds_encode_font_16px_palette(
                &video_char_buffer[row_start],
                &colorbuf[row_start],
                &tmdsbuf[left_margin_words],
                first_chars,
                p_char_rom,
                ra,
                invert_mask
            );
        }

        // Second part: wrap-around from start of buffer (if needed)
        if (first_chars < h_displayed) {
            const uint second_chars = h_displayed - first_chars;
            uint first_words = first_chars * FONT_WIDTH / DVI_SYMBOLS_PER_WORD;

            if (is_80_col) {
                tmds_encode_font_8px_palette(
                    &video_char_buffer[0],
                    &colorbuf[0],
                    &tmdsbuf[left_margin_words + first_words],
                    second_chars,
                    p_char_rom,
                    ra,
                    invert_mask
                );
            } else {
                first_words *= 2;   // 40-column mode: each character is 16 pixels wide
                tmds_encode_font_16px_palette(
                    &video_char_buffer[0],
                    &colorbuf[0],
                    &tmdsbuf[left_margin_words + first_words],
                    second_chars,
                    p_char_rom,
                    ra,
                    invert_mask
                );
            }
        }

        dvi_validate_tmds_buffer(tmdsbuf);
        queue_add_blocking(&dvi0.q_tmds_valid, &tmdsbuf);
    }
}

#ifdef VIDEO_CORE1_LOOP

// Tight loop mode: core1 iterates through all scanlines per frame in a loop,
// similar to the PicoDVI colour_terminal demo. No interrupt-driven callbacks.
static void __not_in_flash_func(core1_main)() {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    sem_acquire_blocking(&dvi_start_sem);
    dvi_start(&dvi0);

    while (true) {
        for (uint y = 0; y < FRAME_HEIGHT; ++y) {
            prepare_scanline(y);
        }
    }
    __builtin_unreachable();
}

#else

// Interrupt mode: scanlines are prepared via interrupt-driven callback.
static void __not_in_flash_func(core1_scanline_callback)() {
    static uint y = 0;
    prepare_scanline(y);
    y = (y + 1) % FRAME_HEIGHT;
}

static void __not_in_flash_func(core1_main)() {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    sem_acquire_blocking(&dvi_start_sem);
    dvi_start(&dvi0);

    while (1) {
        __wfi();
    }
    __builtin_unreachable();
}

#endif // VIDEO_CORE1_LOOP

void video_init() {
    const uint32_t f_clk_sys = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS);
    const int32_t delta = f_clk_sys - DVI_TIMING.bit_clk_khz;
    if (!(-1 <= delta && delta <= 1)) {
        panic("FAIL: Incorrect clk_sys frequency. Expected %d +/-1 kHz, but got %d kHz.", DVI_TIMING.bit_clk_khz, f_clk_sys);
    }

    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = micromod_cfg;
#ifndef VIDEO_CORE1_LOOP
    dvi0.scanline_callback = core1_scanline_callback;
#endif
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    // Dequeue one TMDS buffer from the free queue to use as our precalculated blank scanline.
    // This buffer is kept permanently and never returned to the queue.
    queue_remove_blocking(&dvi0.q_tmds_free, &blank_tmdsbuf);

    uint8_t* const video_char_buffer = system_state.video_char_buffer;
    uint8_t* const colorbuf = video_char_buffer + 0x800;
    
    // Initialize the palette (using CGA palette for both fg and bg colors)
    set_palette(c128_palette, c128_palette);

    // Precalculate TMDS-encoded blank scanline (all zeros / background color).
    // This is used for blank scanlines (y >= y_visible) instead of re-encoding each time.
    tmds_encode_font_8px_palette(
        video_char_buffer,          // charbuf (content ignored for blank lines)
        colorbuf,                   // colorbuf
        blank_tmdsbuf,
        FRAME_WIDTH / FONT_WIDTH,   // n_chars
        p_video_font_000,           // font_base
        FONT_HEIGHT,                // ra >= font height = blank line (shows background only)
        0x00                        // no invert
    );
    dvi_validate_tmds_buffer(blank_tmdsbuf);

#ifndef VIDEO_CORE1_LOOP
    // Prepare initial scanline before starting DVI on core 1.
    dvi0.scanline_callback();
#endif

    sem_init(&dvi_start_sem, /* initial_permits: */ 0, /* max_permits: */ 1);
    hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);
    multicore_launch_core1(core1_main);
    sem_release(&dvi_start_sem);
}
