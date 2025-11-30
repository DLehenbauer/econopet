#include "tmds_encode_font_2bpp.h"
#include <stdbool.h>
#include <stdint.h>

// Expanded palette table: 256 entries × 3 bytes
// Index = color byte (hi nibble = bg, lo nibble = fg)
// Each entry contains packed 4-bit values for the TMDS LUT:
//   { R_packed, G_packed, B_packed }
// where each byte = (bg_2bit << 2) | fg_2bit
// This format matches what the TMDS LUT expects for its upper index bits.
__attribute__((section(".scratch_x.palette_table"), aligned(4)))
uint8_t palette_table[256 * 3];

// TMDS lookup table: 4 backgrounds × 4 foregrounds × 16 pixel patterns × 2 words
// Total: 256 entries × 8 bytes = 2KB
__attribute__((section(".scratch_x.palettised_1bpp_tables"), aligned(4)))
uint32_t palettised_1bpp_tables[256 * 2];

// ============================================================================
// TMDS Table Generation
// ============================================================================
// Direct translation of "Figure 3-5. T.M.D.S. Encode Algorithm"
// on page 29 of DVI 1.0 spec.

/**
 * Count the number of 1 bits in x.
 */
static int popcount(uint32_t x) {
    int n = 0;
    while (x) {
        n++;
        x = x & (x - 1);
    }
    return n;
}

/**
 * Equivalent to N1(q) - N0(q) in the DVI spec.
 */
static int byteimbalance(uint32_t x) {
    return 2 * popcount(x) - 8;
}

/**
 * TMDS Encoder state
 */
typedef struct {
    int imbalance;
} TMDSEncode;

/* Control symbols lookup */
static const uint32_t ctrl_syms[4] = {
    0b1101010100,  /* 0b00 */
    0b0010101011,  /* 0b01 */
    0b0101010100,  /* 0b10 */
    0b1010101011   /* 0b11 */
};

static void tmds_init(TMDSEncode *enc) {
    enc->imbalance = 0;
}

static uint32_t tmds_encode_sym(TMDSEncode *enc, uint32_t d, uint32_t c, bool de) {
    if (!de) {
        enc->imbalance = 0;
        return ctrl_syms[c];
    }

    /* Minimise transitions */
    uint32_t q_m = d & 0x1;
    if (popcount(d) > 4 || (popcount(d) == 4 && !(d & 0x1))) {
        for (int i = 0; i < 7; i++) {
            q_m = q_m | ((~(q_m >> i ^ d >> (i + 1)) & 0x1) << (i + 1));
        }
    } else {
        for (int i = 0; i < 7; i++) {
            q_m = q_m | (((q_m >> i ^ d >> (i + 1)) & 0x1) << (i + 1));
        }
        q_m = q_m | 0x100;
    }

    /* Correct DC balance */
    const uint32_t inversion_mask = 0x2ff;
    uint32_t q_out = 0;

    if (enc->imbalance == 0 || byteimbalance(q_m & 0xff) == 0) {
        q_out = q_m ^ ((q_m & 0x100) ? 0 : inversion_mask);
        if (q_m & 0x100) {
            enc->imbalance += byteimbalance(q_m & 0xff);
        } else {
            enc->imbalance -= byteimbalance(q_m & 0xff);
        }
    } else if ((enc->imbalance > 0) == (byteimbalance(q_m & 0xff) > 0)) {
        q_out = q_m ^ inversion_mask;
        enc->imbalance += ((q_m & 0x100) >> 7) - byteimbalance(q_m & 0xff);
    } else {
        q_out = q_m;
        enc->imbalance += byteimbalance(q_m & 0xff) - ((~q_m & 0x100) >> 7);
    }

    return q_out;
}

/*
 * Palette levels for 2bpp encoding
 * These values are chosen to produce zero DC balance when encoded in pairs
 */
static const uint8_t levels_2bpp_even[4] = {0x05, 0x50, 0xaf, 0xfa};
static const uint8_t levels_2bpp_odd[4] = {0x04, 0x51, 0xae, 0xfb};

/**
 * Get the pixel level for a given background/foreground palette,
 * pixel position x, and pixel run pattern.
 *
 * Note: In the nibble, the leftmost pixel (x=0) is in the MSB (bit 3),
 * and the rightmost pixel (x=3) is in the LSB (bit 0).
 */
static uint8_t level(int bg, int fg, int x, int pix) {
    int index = (pix & (8 >> x)) ? fg : bg;
    return (x & 1) ? levels_2bpp_odd[index] : levels_2bpp_even[index];
}

void init_palettised_1bpp_tables(void) {
    TMDSEncode enc;
    tmds_init(&enc);
    
    uint32_t *table = palettised_1bpp_tables;

    for (int background = 0; background < 4; background++) {
        for (int foreground = 0; foreground < 4; foreground++) {
            for (int pixrun = 0; pixrun < 16; pixrun++) {
                uint32_t sym[4];
                for (int x = 0; x < 4; x++) {
                    sym[x] = tmds_encode_sym(&enc, level(background, foreground, x, pixrun), 0, true);
                }
                // Each entry is 2 words: symbols 0,1 packed and symbols 2,3 packed
                *table++ = (sym[1] << 10) | sym[0];
                *table++ = (sym[3] << 10) | sym[2];
            }
        }
    }
}

// ============================================================================
// Palette Functions
// ============================================================================

void set_palette(const uint8_t *fg_palette, const uint8_t *bg_palette) {
    for (int bg = 0; bg < 16; bg++) {
        for (int fg = 0; fg < 16; fg++) {
            uint8_t color_byte = (bg << 4) | fg;
            uint8_t bg_rgb222 = bg_palette[bg];
            uint8_t fg_rgb222 = fg_palette[fg];
            
            // Extract R, G, B 2-bit values from RGB222 format
            // RGB222: bits 5:4 = R, bits 3:2 = G, bits 1:0 = B
            uint8_t bg_r = (bg_rgb222 >> 4) & 0x03;
            uint8_t bg_g = (bg_rgb222 >> 2) & 0x03;
            uint8_t bg_b = bg_rgb222 & 0x03;
            
            uint8_t fg_r = (fg_rgb222 >> 4) & 0x03;
            uint8_t fg_g = (fg_rgb222 >> 2) & 0x03;
            uint8_t fg_b = fg_rgb222 & 0x03;
            
            // Pack as (bg << 2) | fg for each channel
            // This creates a 4-bit value that indexes into the TMDS LUT
            // The LUT is organized with bg in bits 3:2 and fg in bits 1:0
            // PicoDVI TMDS planes are ordered: 0=Blue, 1=Green, 2=Red
            palette_table[color_byte * 3 + 0] = (bg_b << 2) | fg_b;  // B plane (TMDS lane 0)
            palette_table[color_byte * 3 + 1] = (bg_g << 2) | fg_g;  // G plane (TMDS lane 1)
            palette_table[color_byte * 3 + 2] = (bg_r << 2) | fg_r;  // R plane (TMDS lane 2)
        }
    }
}
