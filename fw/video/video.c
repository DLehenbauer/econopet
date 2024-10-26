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
#include "font.h"

#define FRAME_WIDTH 720
#define FRAME_HEIGHT (480 / DVI_VERTICAL_REPEAT)
#define DVI_TIMING dvi_timing_720x480p_60hz

struct dvi_inst dvi0;
struct semaphore dvi_start_sem;

VideoMode video_mode = VIDEO_MODE_40_COLUMNS;
uint8_t video_char_buffer[VIDEO_CHAR_BUFFER_BYTE_SIZE] = { 0 };

// CRTC registers
const uint8_t r1_h_displayed = 40;

const uint8_t char_pixel_width  = 8;
//const uint8_t chars_start_x     = (FRAME_WIDTH - (char_pixel_width * chars_displayed_x)) >> 1;
const uint8_t char_pixel_height = 8;
const uint8_t chars_displayed_y = 25;
const uint8_t video_start_y     = (FRAME_HEIGHT - char_pixel_height * chars_displayed_y) >> 1;
const uint8_t video_height      = chars_displayed_y * char_pixel_height;

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

static inline bool __not_in_flash_func(video_is_80_col)() {
	return video_mode == VIDEO_MODE_80_COLUMNS;
}

static uint8_t __attribute__((aligned(4))) scanline[FRAME_WIDTH / 8] = { 0 };

static inline void __not_in_flash_func(prepare_scanline)(const uint8_t* chars, uint16_t y) {
	static uint8_t chars_displayed_x = r1_h_displayed;

	bool is_80_col = video_is_80_col();
    y -= video_start_y;

    if (y >= video_height) {
        memset(scanline, 0, sizeof(scanline));

		chars_displayed_x = r1_h_displayed;
		if (is_80_col) {
			chars_displayed_x <<= 1;
		}
	} else if (is_80_col) {
		const uint16_t cy = y / char_pixel_height * chars_displayed_x;
		
		for (uint16_t cx = 0; cx < chars_displayed_x;) {
			uint c = chars[cy + cx];
			
			uint8_t p8 = rom_chars_8800[(c & 0x7f) * char_pixel_height + (y % char_pixel_height)];
			if (c & 0x80) {
				p8 ^= 0xff;
			}
			
			p8 = flip_x(p8);

			scanline[cx++] = p8;
		}
	} else {
		const uint16_t cy = y / char_pixel_height * chars_displayed_x;
		
		for (uint16_t ci = 0, cx = 0; ci < chars_displayed_x; ci++) {
			uint c = chars[cy + ci];
			
			uint8_t p8 = rom_chars_8800[(c & 0x7f) * char_pixel_height + (y % char_pixel_height)];

			if (c & 0x80) {
				p8 ^= 0xff;
			}

			p8 = flip_x(p8);
			uint16_t p16 = stretch_x(p8);

			scanline[cx++] = p16;
			scanline[cx++] = p16 >> 8;
		}
	}

	uint32_t *tmdsbuf;
	queue_remove_blocking(&dvi0.q_tmds_free, &tmdsbuf);
	tmds_encode_1bpp((const uint32_t*) scanline, tmdsbuf, FRAME_WIDTH);
	queue_add_blocking(&dvi0.q_tmds_valid, &tmdsbuf);
}

void __not_in_flash_func(core1_scanline_callback)() {
	static uint16_t y = 1;
	prepare_scanline(video_char_buffer, y);
	y = (y + 1) % FRAME_HEIGHT;
}

void core1_main() {
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
