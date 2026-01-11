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

#include "crtc.h"
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
    // Display geometry, recalculated during blank scanlines
    static dvi_display_geometry_t geo = {
        .chars_per_row = 40,
        .rows = 25,
        .scanlines_per_row = 8,
        .vram_start = 0x000,
        .vram_mask = 0x3ff,
        .invert_mask = 0x00,
        .visible_scanlines = 200,
        .top_margin = 20,
        .double_width = true,
        .left_margin_words = 0,
        .content_words = 0,
        .right_margin_words = 0,
    };

    // TODO: Copy character into SRAM and precalculate flip/stretch? (PERF)
    static const uint8_t* p_char_rom = rom_chars_e800;

    uint8_t* const video_char_buffer = system_state.video_char_buffer;
    uint8_t* const colorbuf = video_char_buffer + 0x800;

    // For convenience, remap local `y` so that `y == 0` is the first visible scan line.
    // Because `y` is unsigned, the top blank area wraps around to a large integer.
    y -= geo.top_margin;

    if (y >= geo.visible_scanlines) {
        // Blank scan line - use blank scan lines to reload/recompute CRTC-dependent values.
        crtc_calculate_geometry(
            system_state.pet_crtc_registers,
            system_state.pet_display_columns,
            FRAME_WIDTH,
            FRAME_HEIGHT,
            FONT_WIDTH,
            DVI_SYMBOLS_PER_WORD,
            &geo
        );

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
        const uint row_offset = geo.vram_start + y / geo.scanlines_per_row * geo.chars_per_row;
        const uint row_start = row_offset & geo.vram_mask;

        const uint ra = y % geo.scanlines_per_row;

        uint32_t *tmdsbuf;
        queue_remove_blocking(&dvi0.q_tmds_free, &tmdsbuf);

        // Copy left/right blank margins for all lanes
        copy_blank_margins(tmdsbuf, geo.left_margin_words, geo.right_margin_words);

        // Check if the row wraps around the video buffer boundary.
        // The buffer size is (vram_mask + 1), so we wrap if row_start + chars_per_row > vram_mask + 1.
        const uint buffer_size = geo.vram_mask + 1;
        const uint first_chars = MIN(buffer_size - row_start, geo.chars_per_row);

        // First part: from row_start (may be all characters if no wrap)
        if (geo.double_width) {
            tmds_encode_font_16px_palette(
                &video_char_buffer[row_start],
                &colorbuf[row_start],
                &tmdsbuf[geo.left_margin_words],
                first_chars,
                p_char_rom,
                ra,
                geo.invert_mask
            );
        } else {
            tmds_encode_font_8px_palette(
                &video_char_buffer[row_start],
                &colorbuf[row_start],
                &tmdsbuf[geo.left_margin_words],
                first_chars,
                p_char_rom,
                ra,
                geo.invert_mask
            );
        }

        // Second part: wrap-around from start of buffer (if needed)
        if (first_chars < geo.chars_per_row) {
            const uint second_chars = geo.chars_per_row - first_chars;
            uint first_words = first_chars * FONT_WIDTH / DVI_SYMBOLS_PER_WORD;

            if (geo.double_width) {
                first_words *= 2;   // 40-column mode: each character is 16 pixels wide
                tmds_encode_font_16px_palette(
                    &video_char_buffer[0],
                    &colorbuf[0],
                    &tmdsbuf[geo.left_margin_words + first_words],
                    second_chars,
                    p_char_rom,
                    ra,
                    geo.invert_mask
                );
            } else {
                tmds_encode_font_8px_palette(
                    &video_char_buffer[0],
                    &colorbuf[0],
                    &tmdsbuf[geo.left_margin_words + first_words],
                    second_chars,
                    p_char_rom,
                    ra,
                    geo.invert_mask
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
