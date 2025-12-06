#ifndef _TMDS_ENCODE_FONT_2BPP_H
#define _TMDS_ENCODE_FONT_2BPP_H

#include "pico/types.h"

// Set separate 16-color palettes for foreground and background.
// Must be called before encoding.
// fg_palette: 16 bytes for foreground colors, each in RGB332 format (bits 7:5=R, 4:2=G, 1:0=B)
// bg_palette: 16 bytes for background colors, each in RGB332 format (bits 7:5=R, 4:2=G, 1:0=B)
// Note: fg_palette and bg_palette can point to the same array for a shared palette.
void set_palette(const uint8_t *fg_palette, const uint8_t *bg_palette);

// Render characters using an 8px-wide font with 16-color palette.
// This function encodes one color plane (R, G, or B) per call.
//
// charbuf: pointer to the row of characters (8 bits each) for the current
// scanline (byte-aligned)
//
// colourbuf: pointer to color attribute bytes, one per character.
// Each byte: high nibble (bits 7:4) = background palette index (0-15)
//            low nibble (bits 3:0) = foreground palette index (0-15)
//
// tmdsbuf: pointer to output buffer for TMDS symbols
//
// n_pix: number of pixels to encode (must be multiple of 8)
//
// font_base: pointer to the base of the font ROM (byte-aligned). The font is
// expected to be in Commodore PET layout: all 8 scanlines of character 0,
// followed by all 8 scanlines of character 1, etc.
//
// scanline: the scanline within each character (0-7) to render.
//
// plane: color plane to encode (0=R, 1=G, 2=B)

void tmds_encode_font_2bpp(const uint8_t *charbuf, const uint8_t *colourbuf,
    uint32_t *tmdsbuf, uint n_pix, const uint8_t *font_base, uint scanline, uint plane,
    uint32_t invert);

// Wide variant: Render characters using an 8px-wide font stretched to 16px
// wide by doubling each pixel horizontally. This reads half as many characters
// from charbuf and colourbuf for the same n_pix output.
//
// Parameters are the same as tmds_encode_font_2bpp, but:
// - charbuf: pointer to half as many characters (n_pix / 16 characters)
// - colourbuf: pointer to half as many colour entries
// - n_pix: total output pixels (must be multiple of 16)

void tmds_encode_font_2bpp_wide(const uint8_t *charbuf, const uint8_t *colourbuf,
    uint32_t *tmdsbuf, uint n_pix, const uint8_t *font_base, uint scanline, uint plane,
    uint32_t invert);

#endif
