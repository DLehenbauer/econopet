# PicoDVI library experiments

# 80 columns w/palette
Hacking the 'mandel-full' example, even with vertical doubling, consumes nearly 100% of core to display a static scanline.

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pll.h"
#include "hardware/sync.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/ssi.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/sem.h"
#include "pico/stdlib.h"
#include "tmds_encode.h"
#include "dvi.h"
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"
#include "font.h"
#include "mandelbrot.h"

#define FRAME_WIDTH 640
#define FRAME_HEIGHT 240
#define VREG_VSEL VREG_VOLTAGE_1_10
#define DVI_TIMING dvi_timing_640x480p_60hz

#define OFFSET_X 20
#define OFFSET_Y 20
#define CHAR_COLS 40
#define CHAR_ROWS 25
#define FONT_CHAR_WIDTH 8
#define FONT_CHAR_HEIGHT 8

#define PALETTE_BITS 4
#define PALETTE_SIZE (1 << PALETTE_BITS)
uint32_t palette[PALETTE_SIZE];

uint32_t tmds_palette[PALETTE_SIZE * 6];

struct dvi_inst dvi0;
struct semaphore dvi_start_sem;

static uint8_t __attribute__((aligned(4))) scanline[FRAME_WIDTH] = {0};

static inline void __not_in_flash_func(prepare_scanline)(const char *chars, int16_t y) {
    y -= OFFSET_Y;

    if (y < 0 || y >= 200) {
        memset(scanline, 0x00, sizeof(scanline));
    } else {
        uint16_t x = OFFSET_X;

        for (uint8_t col = 0; col < CHAR_COLS; col++) {
            char ch = chars[col + y / FONT_CHAR_HEIGHT * CHAR_COLS];
            
            bool reverse = ch & 0x80;
            ch &= 0x7f;

            uint8_t p8 = rom_chars_8800[ch * FONT_CHAR_HEIGHT + (y % FONT_CHAR_HEIGHT)];
            if (reverse) {
                p8 ^= 0xff;
            }

            const uint16_t fg = 0x07e4;
            const uint16_t bg = 0x0000;

            scanline[x++] = (p8 & 0x80) ? fg : bg;
            scanline[x++] = (p8 & 0x40) ? fg : bg;
            scanline[x++] = (p8 & 0x20) ? fg : bg;
            scanline[x++] = (p8 & 0x10) ? fg : bg;
            scanline[x++] = (p8 & 0x08) ? fg : bg;
            scanline[x++] = (p8 & 0x04) ? fg : bg;
            scanline[x++] = (p8 & 0x02) ? fg : bg;
            scanline[x++] = (p8 & 0x01) ? fg : bg;
    	}
    }

    uint32_t *tmdsbuf;
	  queue_remove_blocking(&dvi0.q_tmds_free, &tmdsbuf);
    tmds_encode_palette_data((const uint32_t*)scanline, tmds_palette, tmdsbuf, FRAME_WIDTH, PALETTE_BITS);
    queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf);
}

void __not_in_flash("core1_main") core1_main() {
  dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
  sem_acquire_blocking(&dvi_start_sem);
  dvi_start(&dvi0);

  uint y = 0;
  while (1) {
	  prepare_scanline("\0\1\2\3", y);
	  y = (y + 1) % FRAME_HEIGHT;
  }
  __builtin_unreachable();
}

int __not_in_flash("main") main() {
  vreg_set_voltage(VREG_VSEL);
  sleep_ms(10);
  set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);

  setup_default_uart();

  palette[0]  = 0x000000; // black
  palette[1]  = 0x0000AA; // blue
  palette[2]  = 0x00AA00; // green
  palette[3]  = 0x00AAAA; // cyan
  palette[4]  = 0xAA0000; // red
  palette[5]  = 0xAA00AA; // magenta
  palette[6]  = 0xAA5500; // brown
  palette[7]  = 0xAAAAAA; // light gray
  palette[8]  = 0x555555; // dark gray
  palette[9]  = 0x5555FF; // light blue
  palette[10] = 0x55FF55; // light green
  palette[11] = 0x55FFFF; // light cyan
  palette[12] = 0xFF5555; // light red
  palette[13] = 0xFF55FF; // light magenta
  palette[14] = 0xFFFF55; // yellow
  palette[15] = 0xFFFFFF; // white
  tmds_setup_palette24_symbols(palette, tmds_palette, PALETTE_SIZE);

  printf("Configuring DVI\n");

  dvi0.timing = &DVI_TIMING;
  dvi0.ser_cfg = DVI_DEFAULT_SERIAL_CONFIG;
  dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

  for (int i = 0; i < 16; i++) {
    scanline[i] = i;
  }

  printf("Core 1 start\n");
  sem_init(&dvi_start_sem, 0, 1);
  hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);
  multicore_launch_core1(core1_main);
  sem_release(&dvi_start_sem);

  while (1) {
    __wfe();
  }

  __builtin_unreachable();
}
```

# 80 Column w/16 colors
Hacking the 'colour_terminal' example produces 80 colums with CGA colors, but has some downsides:

1. Font data needs to be swizzled to be scanline oriented.
2. There is blue/yellow garbage at 720x480 (no overscan).
3. "Stretching" the font for 40 columns would require two chars per column.

```c
#include "../pch.h"
#include "dvi.h"
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"
#include "tmds_encode_font_2bpp.h"

#include "font.h"
#define FONT_CHAR_WIDTH 8
#define FONT_CHAR_HEIGHT 8
#define FONT_N_CHARS 128
#define FONT_FIRST_ASCII 32

static uint8_t __attribute__((aligned(4), section(".data" ".rom_data"))) font_8x8[2048] = { 0 };

// 720x480p @ 60 Hz (270 MHz)
// Required by CEA for EDTV/HDTV displays.
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

#define FRAME_WIDTH 640
#define FRAME_HEIGHT 240
#define VREG_VSEL VREG_VOLTAGE_1_20
#define DVI_TIMING dvi_timing_640x480p_60hz

struct dvi_inst dvi0;

#define CHAR_COLS (FRAME_WIDTH / FONT_CHAR_WIDTH)
#define CHAR_ROWS (FRAME_HEIGHT / FONT_CHAR_HEIGHT)

char charbuf[CHAR_ROWS * CHAR_COLS];
#define COLOUR_PLANE_SIZE_WORDS (CHAR_ROWS * CHAR_COLS * 4 / 32)
uint32_t colourbuf[3 * COLOUR_PLANE_SIZE_WORDS];

static inline void set_char(uint x, uint y, char c) {
	if (x >= CHAR_COLS || y >= CHAR_ROWS)
		return;
	charbuf[x + y * CHAR_COLS] = c;
}

// Pixel format RGB222
static inline void set_colour(uint x, uint y, uint8_t fg, uint8_t bg) {
	if (x >= CHAR_COLS || y >= CHAR_ROWS)
		return;
	uint char_index = x + y * CHAR_COLS;
	uint bit_index = char_index % 8 * 4;
	uint word_index = char_index / 8;
	for (int plane = 0; plane < 3; ++plane) {
		uint32_t fg_bg_combined = (fg & 0x3) | (bg << 2 & 0xc);
		colourbuf[word_index] = (colourbuf[word_index] & ~(0xfu << bit_index)) | (fg_bg_combined << bit_index);
		fg >>= 2;
		bg >>= 2;
		word_index += COLOUR_PLANE_SIZE_WORDS;
	}
}

void core1_main() {
	dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
	dvi_start(&dvi0);
	while (true) {
		for (uint y = 0; y < FRAME_HEIGHT; ++y) {
			uint32_t *tmdsbuf;
			queue_remove_blocking(&dvi0.q_tmds_free, &tmdsbuf);
			for (int plane = 0; plane < 3; ++plane) {
				tmds_encode_font_2bpp(
					(const uint8_t*)&charbuf[y / FONT_CHAR_HEIGHT * CHAR_COLS],
					&colourbuf[y / FONT_CHAR_HEIGHT * (COLOUR_PLANE_SIZE_WORDS / CHAR_ROWS) + plane * COLOUR_PLANE_SIZE_WORDS],
					tmdsbuf + plane * (FRAME_WIDTH / DVI_SYMBOLS_PER_WORD),
					FRAME_WIDTH,
					(const uint8_t*)&font_8x8[y % FONT_CHAR_HEIGHT * FONT_N_CHARS] - FONT_FIRST_ASCII
				);
			}
			queue_add_blocking(&dvi0.q_tmds_valid, &tmdsbuf);
		}
	}
}

void video_init() {
    // Ensure system clock frequency matches expected bit clock.
    uint32_t f_clk_sys = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS);
    int32_t delta      = f_clk_sys - DVI_TIMING.bit_clk_khz;
    if (!(-1 <= delta && delta <= 1)) {
        panic("FAIL: Incorrect clk_sys frequency.  Expected %d +/-1 kHz, but got %d kHz.", DVI_TIMING.bit_clk_khz, f_clk_sys);
    }

	for (int y = 0; y < FONT_CHAR_HEIGHT; y++) {
		for (int ch = 0; ch < 128; ch++) {
			int i = 128 * y + ch;
			int j = ch * FONT_CHAR_HEIGHT + y;

			uint8_t v = rom_chars_8800[j];

			v = ((v >> 1) & 0x55) | ((v & 0x55) << 1);
			v = ((v >> 2) & 0x33) | ((v & 0x33) << 2);
			v = ((v >> 4) & 0x0F) | ((v & 0x0F) << 4);

			font_8x8[i] = v;
		}
	}

	dvi0.timing = &DVI_TIMING;
	dvi0.ser_cfg = micromod_cfg;
	dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

	for (uint y = 0; y < CHAR_ROWS; ++y) {
		for (uint x = 0; x < CHAR_COLS; ++x) {
			set_char(x, y, (x + y * CHAR_COLS) % FONT_N_CHARS + FONT_FIRST_ASCII);
			set_colour(x, y, /* fg: */ 0b00111111, /* bg: */ 0b00000000);
		}
	}

	hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);
	multicore_launch_core1(core1_main);
}
```