#include "../pch.h"
#include "font.h"

#define FONT_CHAR_WIDTH 8
#define FONT_CHAR_HEIGHT 8
#define FONT_N_CHARS 95
#define FONT_FIRST_ASCII 32

// CEA Mode 2: 720x480p @ 60 Hz (270 MHz)
// Required for EDTV/HDTV displays.
const struct dvi_timing __not_in_flash_func(dvi_timing_720x480p_60hz) = {
	.h_sync_polarity   = false,
	.h_front_porch     = 16,
	.h_sync_width      = 62,
	.h_back_porch      = 60,
	.h_active_pixels   = 720,

	.v_sync_polarity   = false,
	.v_front_porch     = 9,
	.v_sync_width      = 6,
	.v_back_porch      = 30,
	.v_active_lines    = 480,

	.bit_clk_khz       = 270000
};

#define FRAME_WIDTH 720
#define FRAME_HEIGHT (480 / DVI_VERTICAL_REPEAT)
#define DVI_TIMING dvi_timing_720x480p_60hz

#define LED_PIN 16

struct dvi_inst dvi0;
struct semaphore dvi_start_sem;

#define CHAR_COLS (FRAME_WIDTH / 8)
#define CHAR_ROWS (FRAME_HEIGHT / 8)
char charbuf[CHAR_ROWS * CHAR_COLS];

static inline uint16_t __not_in_flash_func(stretch_x)(uint16_t x) {
    x = (x | (x << 4)) & 0x0F0F;
    x = (x | (x << 2)) & 0x3333;
    x = (x | (x << 1)) & 0x5555;

    return x | (x << 1);
}

static uint8_t __attribute__((aligned(4))) scanline[FRAME_WIDTH / 8] = { 0 };

static inline void __not_in_flash_func(prepare_scanline)(const char* chars, uint16_t y) {
	const uint8_t cy = y / FONT_CHAR_HEIGHT * CHAR_COLS;
	
	for (uint i = 0, x = 0; i < CHAR_COLS; i++) {
		uint c = chars[i + cy];
		
		bool reverse = c & 0x80;
		c &= 0x7f;

		uint8_t p8 = rom_chars_8800[c * FONT_CHAR_HEIGHT + (y % FONT_CHAR_HEIGHT)];

		// https://graphics.stanford.edu/~seander/bithacks.html#ReverseByteWith32Bits
		p8 = ((p8 * 0x0802LU & 0x22110LU) | (p8 * 0x8020LU & 0x88440LU)) * 0x10101LU >> 16;

		if (reverse) {
			p8 ^= 0xff;
		}
		
		scanline[x++] = p8;
	}

	uint32_t *tmdsbuf;
	queue_remove_blocking(&dvi0.q_tmds_free, &tmdsbuf);
	tmds_encode_1bpp((const uint32_t*) scanline, tmdsbuf, FRAME_WIDTH);
	queue_add_blocking(&dvi0.q_tmds_valid, &tmdsbuf);
}

void __not_in_flash_func(core1_scanline_callback)() {
	static uint y = 1;
	prepare_scanline(charbuf, y);
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

	for (int i = 0; i < CHAR_ROWS * CHAR_COLS; i++) {
		charbuf[i] = i;
	}

	prepare_scanline(charbuf, 0);

	sem_init(&dvi_start_sem, 0, 1);
	hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);
	multicore_launch_core1(core1_main);
	sem_release(&dvi_start_sem);
}
