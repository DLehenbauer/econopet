// Adapted from the PicoDVI 'colour_terminal' sample:
// https://github.com/Wren6991/PicoDVI/blob/ca941baf37e3f04738b8e641896d85feb3430385/software/apps/colour_terminal/tmds_encode_font_2bpp.S

#include "hardware/regs/addressmap.h"
#include "hardware/regs/sio.h"

.syntax unified
.cpu cortex-m0plus
.thumb

// Using the following:
//
// - A font stored as a 1bpp bitmap, with character data in PET layout:
//   all 8 scanlines of char 0, then char 1, etc.
//
// - A character buffer (1 byte per character)
//
// - A color buffer (1 byte per character): high nibble = background palette
//   index (0-15), low nibble = foreground palette index (0-15)
//
// - A 16-color palette, pre-expanded into palette_table by set_palette()
//
// Generate encoded TMDS buffers using a LUT with a 10 bit index:
// - bits 3:0 = 4 font pixels (1bpp)
// - bits 9:4 = 3-bit background + 3-bit foreground (from palette lookup)
// Each LUT entry is 4 TMDS symbols (2 words), giving an 8KB table.
//
// Two variants are provided:
// - tmds_encode_font_8px_palette_1lane: 8px wide characters (single color plane)
// - tmds_encode_font_16px_palette_1lane: 16px wide characters (single color plane, 2x horizontal stretch)

// Offsets suitable for ldr/str (must be <= 0x7c):
#define ACCUM0_OFFS     (SIO_INTERP0_ACCUM0_OFFSET     - SIO_INTERP0_ACCUM0_OFFSET)
#define ACCUM1_OFFS     (SIO_INTERP0_ACCUM1_OFFSET     - SIO_INTERP0_ACCUM0_OFFSET)
#define ACCUM1_ADD_OFFS (SIO_INTERP0_ACCUM1_ADD_OFFSET - SIO_INTERP0_ACCUM0_OFFSET)
#define PEEK0_OFFS      (SIO_INTERP0_PEEK_LANE0_OFFSET - SIO_INTERP0_ACCUM0_OFFSET)
#define PEEK1_OFFS      (SIO_INTERP0_PEEK_LANE1_OFFSET - SIO_INTERP0_ACCUM0_OFFSET)
#define PEEK2_OFFS      (SIO_INTERP0_PEEK_FULL_OFFSET  - SIO_INTERP0_ACCUM0_OFFSET)
#define INTERP1         (SIO_INTERP1_ACCUM0_OFFSET     - SIO_INTERP0_ACCUM0_OFFSET)

// Register usage:
// r0 contains character buffer pointer
// r1 contains color buffer pointer  
// r2 contains output buffer pointer
// r3 is scratch
// r4-r7 are for scratch + pixels
// r8 contains font_base + clamped_scanline (scanline clamped to 0-7)
// r9 contains the TMDS LUT base
// r10 contains palette_table base + plane offset (0, 1, or 2)
// r11 contains font mask: 0xFF if scanline <= 7, 0x00 if scanline > 7
// ip (r12) contains the end pointer

// ============================================================================
// Function-generating macro
// ============================================================================

// Generate a TMDS encode function
// name: function name (symbol)
// wide: 0 for normal (8px chars), 1 for wide (16px chars with 2x horizontal stretch)
//
// Function signature:
//   void name(const uint8_t *charbuf, const uint8_t *colorbuf,
//             uint32_t *tmdsbuf, uint n_pix,
//             const uint8_t *font_base, uint scanline, uint plane, uint32_t invert)
// Stack args at [sp+36]=font_base, [sp+40]=scanline, [sp+44]=plane, [sp+48]=invert
// Note: invert should be 0x00 for normal, 0xFF to swap fg/bg
.macro define_encode_func name wide
.global \name
.type \name,%function
.thumb_func
\name:
    // Prologue: save registers
    push {r4-r7, lr}
    mov r4, r8
    mov r5, r9
    mov r6, r10
    mov r7, r11
    push {r4-r7}

    // Calculate end pointer: tmdsbuf + n_pix * 2 bytes (each pixel = 2 bytes of TMDS)
    lsls r3, #1
    add r3, r2
    mov ip, r3

    // Load font_base into r8, adding clamped scanline offset
    // This allows r8 to be used directly as base for font lookups
    ldr r7, [sp, #36]                                                 // font_base
    ldr r6, [sp, #40]                                                 // scanline
    
    // Clamp scanline to 0-7 range for font address calculation
    // clamped = (scanline > 7) ? 7 : scanline
    movs r4, #7
    cmp r6, r4
    bls 3f                                                            // branch if scanline <= 7
    movs r6, #7                                                       // clamp to 7
3:
    adds r7, r6                                                       // r7 = font_base + clamped_scanline
    mov r8, r7
    
    // Compute font mask: 0xFF if scanline <= 7, 0x00 otherwise
    // When scanline > 7, ANDing font bytes with 0 gives all background pixels
    ldr r7, [sp, #40]                                                 // reload original scanline
    movs r6, #7
    subs r6, r7                                                       // r6 = 7 - scanline (negative if scanline > 7)
    asrs r6, r6, #31                                                  // r6 = 0xFFFFFFFF if scanline > 7, 0 otherwise
    mvns r6, r6                                                       // r6 = 0x00000000 if scanline > 7, 0xFFFFFFFF otherwise
    mov r11, r6                                                       // r11 = font mask
    
    // Load TMDS LUT base into r9
    ldr r7, =tmds_table
    mov r9, r7
    
    // Load palette_table + plane into r10
    ldr r7, [sp, #44]                                                 // plane (0, 1, or 2)
    ldr r6, =palette_table
    adds r7, r6
    mov r10, r7

    // Main loop
.align 2
1:
    cmp r2, ip
    bhs 2f
    .if \wide
        // Wide mode: 2 characters per iteration (16px each = 32px total)
        do_char 0, 0, 1
        do_char 1, 1, 1
        adds r0, #2                                                   // advance char buffer
        adds r1, #2                                                   // advance color buffer
    .else
        // Normal mode: 4 characters per iteration (8px each = 32px total)
        do_char 0, 0, 0
        do_char 1, 1, 0
        do_char 2, 2, 0
        do_char 3, 3, 0
        adds r0, #4                                                   // advance char buffer
        adds r1, #4                                                   // advance color buffer
    .endif
    b 1b
2:
    // Epilogue: restore registers and return
    pop {r4-r7}
    mov r8, r4
    mov r9, r5
    mov r10, r6
    mov r11, r7
    pop {r4-r7, pc}
.endm

// ============================================================================
// Bit-doubling lookup table (for wide mode)
// ============================================================================

// 2 bits -> 4 bits: 00->0000, 01->0011, 10->1100, 11->1111
.section .scratch_x.bit_double_lut, "a"
.align 2
bit_double_lut:
    .byte 0x00  // 00 -> 0000
    .byte 0x03  // 01 -> 0011
    .byte 0x0c  // 10 -> 1100
    .byte 0x0f  // 11 -> 1111

.section .scratch_x.tmds_encode_font_palette, "ax"

// ============================================================================
// Unified character processing macro
// ============================================================================

// Emit 4 doubled pixels from 2 font bits (wide mode helper)
// r4 = font byte (preserved), r5 = LUT base + color offset (preserved)
// bit_shift = bit position of the 2-bit pair to extract (6, 4, 2, or 0)
// Clobbers r3, r6, r7
.macro emit_doubled_4pix bit_shift
    // Extract 2 bits at position bit_shift using shifts (M0+ has no ands with immediate)
    lsls r6, r4, #(24 + (6 - \bit_shift))       // shift target bits to bit 31:30
    lsrs r6, r6, #30                            // shift down to bits 1:0
    ldr r7, =bit_double_lut
    ldrb r6, [r7, r6]                           // r6 = doubled 4-bit pattern
    lsls r6, #3                                 // scale for 8-byte LUT entries
    add r6, r5                                  // r6 = LUT address
    ldmia r6, {r6, r7}
    stmia r2!, {r6, r7}
.endm

// Process one character and emit TMDS pixels
// charbuf_offs: offset from r0 to load character
// colorbuf_offs: offset from r1 to load color byte
// wide: 0 for normal (8px output), 1 for wide (16px output with 2x stretch)
.macro do_char charbuf_offs colorbuf_offs wide
    // Get 8 font bits for this character (font is in PET layout: char_code * 8 + scanline)
    // PET quirk: bit 7 of char code inverts the scanline, bits [6:0] are font index
    // Note: r8 already contains font_base + clamped_scanline, r11 contains font mask
    ldrb r4, [r0, #\charbuf_offs]               // r4 = character code
    lsrs r7, r4, #7                             // r7 = 1 if bit 7 set, else 0
    negs r7, r7                                 // r7 = 0xFFFFFFFF if bit 7 set, else 0
    movs r3, #0x7f
    ands r4, r3                                 // r4 = bits [6:0] (font index)
    lsls r4, #3                                 // r4 = font_index * 8
    add r4, r8                                  // r4 += font_base + clamped_scanline
    ldrb r4, [r4]                               // r4 = 8 font bits
    eors r4, r7                                 // invert if bit 7 was set (XOR with 0xFFFFFFFF or 0x00)
    mov r6, r11
    ands r4, r6                                 // r4 &= font_mask (0 if scanline > 7, forces background)
    ldr r6, [sp, #48]                           // r6 = invert (0x00 or 0xFF)
    eors r4, r6                                 // r4 ^= invert (swap fg/bg if invert is 0xFF)
    uxtb r4, r4                                 // mask to 8 bits (clear upper 24 bits after XOR)

    // Get color byte and look up in palette_table
    // palette_table index = color_byte * 3 + plane (r10 already has palette_table + plane)
    ldrb r3, [r1, #\colorbuf_offs]              // r3 = color byte
    lsls r5, r3, #1                             // r5 = color * 2
    adds r5, r3                                 // r5 = color * 3
    add r5, r10                                 // r5 = &palette_table[color*3 + plane]
    ldrb r3, [r5]                               // r3 = packed fg+bg for this plane
    
    // Build LUT address: LUT is indexed by (palette << 7) | (font_nibble << 3)
    // Each of 64 palette values (6-bit: 3-bit bg + 3-bit fg) has 16 font nibble entries, each 8 bytes
    lsls r5, r3, #7                             // palette offset (bits 10:7)
    add r5, r9                                  // r5 = LUT base + palette offset

    // Emit pixels: normal mode outputs 8px, wide mode outputs 16px
    .if \wide
        // Wide mode: 4 lookups, each producing 4 doubled pixels (16 total)
        emit_doubled_4pix 6                     // bits 7-6 -> 4 pixels
        emit_doubled_4pix 4                     // bits 5-4 -> 4 pixels
        emit_doubled_4pix 2                     // bits 3-2 -> 4 pixels
        emit_doubled_4pix 0                     // bits 1-0 -> 4 pixels
    .else
        // Normal mode: 2 lookups, each producing 4 pixels (8 total)
        // Split font byte into nibbles (MSB nibble is leftmost 4 pixels)
        lsrs r6, r4, #4                         // r6 = upper nibble (bits 7:4)
        movs r7, #0x0f
        ands r7, r4                             // r7 = lower nibble (bits 3:0)
        
        // Scale nibbles for 8-byte LUT entries and add LUT base
        lsls r6, #3
        lsls r7, #3
        add r6, r5                              // r6 = LUT addr for upper nibble
        add r7, r5                              // r7 = LUT addr for lower nibble

        // Look up and write out 8 TMDS symbols (4 from each nibble)
        ldmia r6!, {r3, r4}
        ldmia r7!, {r5, r6}
        stmia r2!, {r3, r4, r5, r6}
    .endif
.endm

// ============================================================================
// Function definitions
// ============================================================================

// Generate both functions using the parameterized macro
define_encode_func tmds_encode_font_8px_palette_1lane 0
define_encode_func tmds_encode_font_16px_palette_1lane 1

// ============================================================================
// TMDS Lookup Table (External)
// ============================================================================
// The tmds_table is pre-generated by generate_tmds_table.py
