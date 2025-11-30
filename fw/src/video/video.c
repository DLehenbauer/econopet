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

static inline uint8_t __not_in_flash_func(get_char_scanline)(
    const uint src,                                     // Current source offset in video_char_buffer
    const uint8_t* const p_char_rom,                    // Start of character set bitmap data
    const uint ra,                                      // Row address within character (0 to FONT_HEIGHT-1)
    const uint8_t invert_mask                           // Display invert mask
) {
    if (ra >= FONT_HEIGHT) {
        return invert_mask;                             // Outside character height: blank line
    }

	const uint c = video_char_buffer[src];              // Read character from video RAM
    uint8_t p8 = p_char_rom[                            // Fetch character bitmap row
        (c & 0x7f) * FONT_HEIGHT + ra];

    p8 ^= invert_mask;                                  // Apply display invert mask

	if (c & 0x80) {                                     // If bit 7 of character is set
		p8 ^= 0xff;                                     // Invert character bitmap
	}
	
    return p8;
}

static uint8_t __attribute__((aligned(4))) scanline[FRAME_WIDTH / FONT_WIDTH] = { 0 };

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

        is_80_col = (system_state.pet_display_columns == pet_display_columns_80);
        if (is_80_col) {
            h_displayed <<= 1;          // Double horizontal displayed characters for 80-column mode
            display_mask = 0x7ff;       // 80 columns machine has 2Kb video RAM
        } else {
            display_mask = 0x3ff;       // 40 columns machine has 1Kb video RAM
        }

        // Select graphics/text character ROM
		p_char_rom = video_graphics
			? p_video_font_400
			: p_video_font_000;
	} else {
		uint src = display_start + y / lines_per_row * h_displayed;     // Next offset to read from `video_char_buffer`
		uint8_t* p_dest = &scanline[x_start];					        // Next byte to write in `scanline`
		uint remaining = h_displayed;						            // Remaining number of characters to display in `scanline`

		const uint ra = y % lines_per_row;

		if (is_80_col) {
			while (remaining--) {
                src &= display_mask;
				uint8_t p8 = get_char_scanline(src++, p_char_rom, ra, invert_mask);
				*p_dest++ = p8;
			}
		} else {
			while (remaining--) {
                src &= display_mask;
				uint8_t p8 = get_char_scanline(src++, p_char_rom, ra, invert_mask);

				uint16_t p16 = stretch_x(p8);   // Stretch character horizontally for 40-column mode
				*p_dest++ = p16 >> 8;
				*p_dest++ = p16;
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

    prepare_scanline(0);

    sem_init(&dvi_start_sem, /* initial_permits: */ 0, /* max_permits: */ 1);
    hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);
    multicore_launch_core1(core1_main);
    sem_release(&dvi_start_sem);
}
