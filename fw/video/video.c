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

#define FRAME_WIDTH 720
#define FRAME_HEIGHT (480 / DVI_VERTICAL_REPEAT)
#define DVI_TIMING dvi_timing_720x480p_60hz

#define CHAR_PIXEL_WIDTH 8

struct dvi_inst dvi0;
struct semaphore dvi_start_sem;

bool video_graphics = false;
bool video_is_80_col = false;
uint8_t video_char_buffer[VIDEO_CHAR_BUFFER_BYTE_SIZE] = { 0 };

// CRTC registers
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

static inline void __not_in_flash_func(prepare_scanline)(const uint8_t* chars, uint16_t y) {
	static uint8_t chars_displayed_x = r1_h_displayed;
	static uint8_t cx_start = 0;
	static uint8_t y_start = 0;
	static uint8_t y_visible = 0;

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
		if (!video_is_80_col) {
			chars_displayed_x >>= 1;
		}
	} else {
		// Select graphics/text character ROM
		const uint8_t* const p_char_rom = video_graphics
			? p_video_font_400
			: p_video_font_000;

		// 'cy' is the memory offset of first character in row
		const uint16_t cy = y / r9_max_scan_line * chars_displayed_x;

		// 'cx' is the character position in the scanline to write to.
		uint16_t cx = cx_start;

		if (video_is_80_col) {
			for (uint16_t ci = 0; ci < chars_displayed_x; ci++) {
				uint c = chars[cy + ci];
				uint8_t p8 = p_char_rom[(c & 0x7f) * r9_max_scan_line + (y % r9_max_scan_line)];
				
				if (c & 0x80) {
					p8 ^= 0xff;
				}
				
				p8 = flip_x(p8);

				scanline[cx++] = p8;
			}
		} else {
			for (uint16_t ci = 0; ci < chars_displayed_x; ci++) {
				uint c = chars[cy + ci];
				uint8_t p8 = p_char_rom[(c & 0x7f) * r9_max_scan_line + (y % r9_max_scan_line)];

				if (c & 0x80) {
					p8 ^= 0xff;
				}

				p8 = flip_x(p8);
				uint16_t p16 = stretch_x(p8);

				scanline[cx++] = p16;
				scanline[cx++] = p16 >> 8;
			}
		}
	}

	uint32_t *tmdsbuf;
	queue_remove_blocking(&dvi0.q_tmds_free, &tmdsbuf);
	tmds_encode_1bpp((const uint32_t*) scanline, tmdsbuf, FRAME_WIDTH);
	queue_add_blocking(&dvi0.q_tmds_valid, &tmdsbuf);
}

static void __not_in_flash_func(core1_scanline_callback)() {
	static uint16_t y = 1;
	prepare_scanline(video_char_buffer, y);
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

	prepare_scanline(video_char_buffer, 0);

	sem_init(&dvi_start_sem, 0, 1);
	hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);
	multicore_launch_core1(core1_main);
	sem_release(&dvi_start_sem);
}
