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

#pragma once

#include "../pch.h"

/**
 * Set separate 16-color palettes for foreground and background.
 * 
 * Must be called before encoding.
 * 
 * @param fg_palette 16 bytes for foreground colors, each in RGB332 format (bits 7:5=R, 4:2=G, 1:0=B)
 * @param bg_palette 16 bytes for background colors, each in RGB332 format (bits 7:5=R, 4:2=G, 1:0=B)
 * 
 * Note: fg_palette and bg_palette can point to the same array for a shared palette.
 */
void set_palette(const uint8_t* fg_palette, const uint8_t* bg_palette);

/**
 * Render characters using an 8px-wide font with 16-color palette (single color plane).
 * 
 * This is the low-level encoder that processes one color plane (R, G, or B) per call.
 * Most callers should use the higher-level wrapper that encodes all 3 planes.
 * 
 * @param charbuf Pointer to the row of characters (8 bits each) for the current scanline (byte-aligned)
 * @param colorbuf Pointer to color attribute bytes, one per character.
 *                 Each byte: high nibble (bits 7:4) = background palette index (0-15)
 *                            low nibble (bits 3:0) = foreground palette index (0-15)
 * @param tmdsbuf Pointer to output buffer for TMDS symbols
 * @param n_pix Number of pixels to encode (must be multiple of 8)
 * @param font_base Pointer to the base of the font ROM (byte-aligned). The font is expected to be
 *                  in Commodore PET layout: all 8 scanlines of character 0, followed by all 8
 *                  scanlines of character 1, etc.
 * @param scanline The scanline within each character (0-7) to render
 * @param plane Color plane to encode (0=R, 1=G, 2=B)
 * @param invert Invert mask for character rendering
 */
void tmds_encode_font_8px_palette_1lane(const uint8_t* charbuf, const uint8_t* colorbuf, uint32_t* tmdsbuf, uint n_pix,
                                        const uint8_t* font_base, uint scanline, uint plane, uint32_t invert);

/**
 * Render characters using an 8px-wide font stretched to 16px wide (single color plane).
 * 
 * This is the low-level encoder that processes one color plane (R, G, or B) per call.
 * Most callers should use the higher-level wrapper that encodes all 3 planes.
 * 
 * This wide variant stretches each pixel horizontally by doubling it. This reads half as many
 * characters from charbuf and colorbuf for the same n_pix output.
 * 
 * @param charbuf Pointer to half as many characters (n_pix / 16 characters)
 * @param colorbuf Pointer to half as many color entries
 * @param tmdsbuf Pointer to output buffer for TMDS symbols
 * @param n_pix Total output pixels (must be multiple of 16)
 * @param font_base Pointer to the base of the font ROM (byte-aligned)
 * @param scanline The scanline within each character (0-7) to render
 * @param plane Color plane to encode (0=R, 1=G, 2=B)
 * @param invert Invert mask for character rendering
 */
void tmds_encode_font_16px_palette_1lane(const uint8_t* charbuf, const uint8_t* colorbuf, uint32_t* tmdsbuf, uint n_pix,
                                         const uint8_t* font_base, uint scanline, uint plane, uint32_t invert);
