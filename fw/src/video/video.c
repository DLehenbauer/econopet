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

#define CHAR_PIXEL_WIDTH 8

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
                                        //       Graphics Mode: 7 → 8 scan lines/row (full character)
                                        //           Text Mode: 8 → 9 scan lines/row (full character + 1 blank line)

#define CRTC_R10_CURSOR_START_LINE  10  // [6:0] Cursor blink mode and starting scan line [Not implemented]
                                        //
                                        //       [6:5] Cursor blink mode with respect to field rate.
                                        //             (00 = on, 01 = off, 10 = 1/16 rate, 11 = 1/32 rate)
                                        //
                                        //       [4:0] Starting scan line

#define CRTC_R11_CURSOR_END_LINE    11  // [4:0] Ending scan line of cursor [Not implemented]

#define CRTC_R12_START_ADDR_HI      12  // [5:0] High 6 bits of 14 bit display address (starting address of screen_addr_o[13:8]).

#define CRTC_R13_START_ADDR_LO      13  // [7:0] Low 8 bits of 14 bit display address (starting address of screen_addr_o[7:0]).

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
    [CRTC_R12_START_ADDR_HI]     = 0x00, // Display start high
    [CRTC_R13_START_ADDR_LO]     = 0x00, // Display start low
    // Remaining registers unimplemented -> 0
};

static const uint8_t r1_h_displayed   = 40;	// Horizontal displayed characters
static const uint8_t r6_v_displayed   = 25;	// Vertical displayed characters
static const uint8_t r9_max_scan_line = 8;	// Vertical scan lines per character

static inline uint16_t __not_in_flash_func(flip_x)(uint8_t x) {
	// https://graphics.stanford.edu/~seander/bithacks.html#ReverseByteWith32Bits
	return ((x * 0x0802LU & 0x22110LU) | (x * 0x8020LU & 0x88440LU)) * 0x10101LU >> 16;
}

static inline uint16_t __not_in_flash_func(stretch_x)(uint16_t x) {
    x = (x | (x << 4)) & 0x0F0F;
    x = (x | (x << 2)) & 0x3333;
    x = (x | (x << 1)) & 0x5555;
    return x | (x << 1);
}

static uint8_t __attribute__((aligned(4))) scanline[FRAME_WIDTH / 8] = { 0 };

static inline void __not_in_flash_func(prepare_scanline)(uint16_t y) {
	static uint8_t chars_displayed_x = r1_h_displayed;
	static uint8_t cx_start = 0;
	static uint8_t y_start = 0;
	static uint8_t y_visible = 0;

    // TODO: Copy into SRAM and precalculate flip/stretch? (PERF)
    static const uint8_t* p_char_rom = rom_chars_8800;

    y -= y_start;

    if (y >= y_visible) {
		// Blank scan line.  Because scan lines are recycled, this also clears the unused overscan
		// area on the left/right of the visible region.
        memset(scanline, 0, sizeof(scanline));

		// Use blank scan lines to reload/recompute CRTC-dependent values.

		y_visible = r6_v_displayed * r9_max_scan_line;	// Total visible scan lines
		y_start   = (FRAME_HEIGHT - y_visible) >> 1;	// Top margin in scan lines

		chars_displayed_x = (r1_h_displayed << 1);		// Horizontal displayed characters (2x for 80-column mode)
		cx_start = (FRAME_WIDTH - (CHAR_PIXEL_WIDTH * chars_displayed_x)) >> 4;	// Left margin in characters

		// If in 40 column mode, divide the chars_displayed_x by 2.  We do not, however adjust, cx_start
		if (system_state.pet_display_columns == pet_display_columns_40) {
			chars_displayed_x >>= 1;
		}

        // Select graphics/text character ROM
		p_char_rom = video_graphics
			? p_video_font_400
			: p_video_font_000;
	} else {
		uint src = y / r9_max_scan_line * chars_displayed_x;	// Next offset to read from 'video_char_buffer'
		uint dest = cx_start;									// Next byte offset to write in 'scanline'
		uint remaining = chars_displayed_x;						// Remaining number of characters to display in row

		const uint ra = y % r9_max_scan_line;

		if (system_state.pet_display_columns == pet_display_columns_80) {
			while (remaining--) {
				uint c = video_char_buffer[src++];
				uint8_t p8 = p_char_rom[(c & 0x7f) * r9_max_scan_line + ra];
				
				if (c & 0x80) {
					p8 ^= 0xff;
				}
				
				p8 = flip_x(p8);

				scanline[dest++] = p8;
			}
		} else {
			while (remaining--) {
				uint c = video_char_buffer[src++];
				uint8_t p8 = p_char_rom[(c & 0x7f) * r9_max_scan_line + ra];

				if (c & 0x80) {
					p8 ^= 0xff;
				}

				p8 = flip_x(p8);
				uint16_t p16 = stretch_x(p8);

				scanline[dest++] = p16;
				scanline[dest++] = p16 >> 8;
			}
		}
	}

	uint32_t *tmdsbuf;
	queue_remove_blocking(&dvi0.q_tmds_free, &tmdsbuf);
	tmds_encode_1bpp((const uint32_t*) scanline, tmdsbuf, FRAME_WIDTH);
	queue_add_blocking(&dvi0.q_tmds_valid, &tmdsbuf);
}

static void __not_in_flash_func(core1_scanline_callback)() {
	static uint y = 1;
	prepare_scanline(y);
	y = (y + 1) % FRAME_HEIGHT;
}

static void core1_main() {
	dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
	sem_acquire_blocking(&dvi_start_sem);
	dvi_start(&dvi0);

	while (1) 
		__wfi();
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

	prepare_scanline(0);

	sem_init(&dvi_start_sem, /* initial_permits: */ 0, /* max_permits: */ 1);
	hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);
	multicore_launch_core1(core1_main);
	sem_release(&dvi_start_sem);
}
