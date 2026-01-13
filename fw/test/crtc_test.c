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

#include <string.h>
#include "crtc_test.h"
#include "display/dvi/crtc.h"

// Frame dimensions matching dvi.c
#define FRAME_WIDTH 720
#define FRAME_HEIGHT 240  // 480 / DVI_VERTICAL_REPEAT (2)
#define FONT_WIDTH 8
#define SYMBOLS_PER_WORD 2
#define WORDS_PER_LANE (FRAME_WIDTH / SYMBOLS_PER_WORD)

// Helper to initialize CRTC registers with typical PET values
static void init_crtc_40col(uint8_t crtc[CRTC_REG_COUNT]) {
    memset(crtc, 0, CRTC_REG_COUNT);
    crtc[CRTC_R1_H_DISPLAYED] = 40;         // 40 characters displayed
    crtc[CRTC_R6_V_DISPLAYED] = 25;         // 25 rows
    crtc[CRTC_R9_MAX_SCAN_LINE] = 7;        // 8 scanlines per row (7 + 1)
    crtc[CRTC_R12_START_ADDR_HI] = 0x10;    // Start at $1000 (bit 12 set = normal video)
    crtc[CRTC_R13_START_ADDR_LO] = 0x00;
}

static void init_crtc_80col(uint8_t crtc[CRTC_REG_COUNT]) {
    memset(crtc, 0, CRTC_REG_COUNT);
    crtc[CRTC_R1_H_DISPLAYED] = 40;         // 40 in register, doubled for 80-col mode
    crtc[CRTC_R6_V_DISPLAYED] = 25;         // 25 rows
    crtc[CRTC_R9_MAX_SCAN_LINE] = 7;        // 8 scanlines per row (7 + 1)
    crtc[CRTC_R12_START_ADDR_HI] = 0x10;    // Start at $1000 (bit 12 set = normal video)
    crtc[CRTC_R13_START_ADDR_LO] = 0x00;
}

// Test: Basic 40-column mode geometry
START_TEST(test_40col_basic_geometry) {
    uint8_t crtc[CRTC_REG_COUNT];
    init_crtc_40col(crtc);
    
    dvi_display_geometry_t geo;
    crtc_calculate_geometry(crtc, pet_display_columns_40, FRAME_WIDTH, FRAME_HEIGHT,
                            FONT_WIDTH, SYMBOLS_PER_WORD, &geo);
    
    ck_assert_uint_eq(geo.chars_per_row, 40);
    ck_assert_uint_eq(geo.rows, 25);
    ck_assert_uint_eq(geo.scanlines_per_row, 8);
    ck_assert_uint_eq(geo.visible_scanlines, 200);  // 25 * 8
    ck_assert(geo.double_width);  // 40-col mode uses double-width characters
}
END_TEST

// Test: Basic 80-column mode geometry
START_TEST(test_80col_basic_geometry) {
    uint8_t crtc[CRTC_REG_COUNT];
    init_crtc_80col(crtc);
    
    dvi_display_geometry_t geo;
    crtc_calculate_geometry(crtc, pet_display_columns_80, FRAME_WIDTH, FRAME_HEIGHT,
                            FONT_WIDTH, SYMBOLS_PER_WORD, &geo);
    
    ck_assert_uint_eq(geo.chars_per_row, 80);  // Doubled from register value
    ck_assert_uint_eq(geo.rows, 25);
    ck_assert_uint_eq(geo.scanlines_per_row, 8);
    ck_assert_uint_eq(geo.visible_scanlines, 200);  // 25 * 8
    ck_assert(!geo.double_width);  // 80-col mode uses normal-width characters
}
END_TEST

// Test: Video RAM masking for 40-column mode
START_TEST(test_40col_vram_mask) {
    uint8_t crtc[CRTC_REG_COUNT];
    init_crtc_40col(crtc);
    
    dvi_display_geometry_t geo;
    crtc_calculate_geometry(crtc, pet_display_columns_40, FRAME_WIDTH, FRAME_HEIGHT,
                            FONT_WIDTH, SYMBOLS_PER_WORD, &geo);
    
    ck_assert_uint_eq(geo.vram_mask, 0x3ff);  // 1KB video RAM
}
END_TEST

// Test: Video RAM masking for 80-column mode
START_TEST(test_80col_vram_mask) {
    uint8_t crtc[CRTC_REG_COUNT];
    init_crtc_80col(crtc);
    
    dvi_display_geometry_t geo;
    crtc_calculate_geometry(crtc, pet_display_columns_80, FRAME_WIDTH, FRAME_HEIGHT,
                            FONT_WIDTH, SYMBOLS_PER_WORD, &geo);
    
    ck_assert_uint_eq(geo.vram_mask, 0x7ff);  // 2KB video RAM
}
END_TEST

// Test: Display start address for 40-column mode
START_TEST(test_40col_vram_start) {
    uint8_t crtc[CRTC_REG_COUNT];
    init_crtc_40col(crtc);
    crtc[CRTC_R12_START_ADDR_HI] = 0x10;  // $1000
    crtc[CRTC_R13_START_ADDR_LO] = 0x80;  // + $80 = $1080
    
    dvi_display_geometry_t geo;
    crtc_calculate_geometry(crtc, pet_display_columns_40, FRAME_WIDTH, FRAME_HEIGHT,
                            FONT_WIDTH, SYMBOLS_PER_WORD, &geo);
    
    // Start address is masked to 10 bits for 40-col
    ck_assert_uint_eq(geo.vram_start, 0x080);  // $1080 & $3ff = $080
}
END_TEST

// Test: Display start address for 80-column mode (doubled)
START_TEST(test_80col_vram_start) {
    uint8_t crtc[CRTC_REG_COUNT];
    init_crtc_80col(crtc);
    crtc[CRTC_R12_START_ADDR_HI] = 0x10;  // $1000
    crtc[CRTC_R13_START_ADDR_LO] = 0x40;  // + $40 = $1040
    
    dvi_display_geometry_t geo;
    crtc_calculate_geometry(crtc, pet_display_columns_80, FRAME_WIDTH, FRAME_HEIGHT,
                            FONT_WIDTH, SYMBOLS_PER_WORD, &geo);
    
    // Start address is doubled then masked to 11 bits for 80-col
    // $1040 << 1 = $2080, & $7ff = $080
    ck_assert_uint_eq(geo.vram_start, 0x080);
}
END_TEST

// Test: Video inversion when bit 12 is clear
START_TEST(test_video_inverted) {
    uint8_t crtc[CRTC_REG_COUNT];
    init_crtc_40col(crtc);
    crtc[CRTC_R12_START_ADDR_HI] = 0x00;  // Bit 12 clear = inverted
    
    dvi_display_geometry_t geo;
    crtc_calculate_geometry(crtc, pet_display_columns_40, FRAME_WIDTH, FRAME_HEIGHT,
                            FONT_WIDTH, SYMBOLS_PER_WORD, &geo);
    
    ck_assert_uint_eq(geo.invert_mask, 0xff);
}
END_TEST

// Test: Video normal when bit 12 is set
START_TEST(test_video_normal) {
    uint8_t crtc[CRTC_REG_COUNT];
    init_crtc_40col(crtc);
    crtc[CRTC_R12_START_ADDR_HI] = 0x10;  // Bit 12 set = normal
    
    dvi_display_geometry_t geo;
    crtc_calculate_geometry(crtc, pet_display_columns_40, FRAME_WIDTH, FRAME_HEIGHT,
                            FONT_WIDTH, SYMBOLS_PER_WORD, &geo);
    
    ck_assert_uint_eq(geo.invert_mask, 0x00);
}
END_TEST

// Test: Top margin calculation (centered display)
START_TEST(test_top_margin_centered) {
    uint8_t crtc[CRTC_REG_COUNT];
    init_crtc_40col(crtc);
    
    dvi_display_geometry_t geo;
    crtc_calculate_geometry(crtc, pet_display_columns_40, FRAME_WIDTH, FRAME_HEIGHT,
                            FONT_WIDTH, SYMBOLS_PER_WORD, &geo);
    
    // 240 scanlines total, 200 visible (25 rows * 8 scanlines)
    // Top margin = (240 - 200) / 2 = 20
    ck_assert_uint_eq(geo.top_margin, 20);
}
END_TEST

// Test: TMDS margin calculations for 40-column mode
START_TEST(test_40col_tmds_margins) {
    uint8_t crtc[CRTC_REG_COUNT];
    init_crtc_40col(crtc);
    
    dvi_display_geometry_t geo;
    crtc_calculate_geometry(crtc, pet_display_columns_40, FRAME_WIDTH, FRAME_HEIGHT,
                            FONT_WIDTH, SYMBOLS_PER_WORD, &geo);
    
    // 720 pixels wide, 40 chars * 16 pixels = 640 content pixels
    // Left margin = (720/16 - 40) = 45 - 40 = 5 (in 8-pixel units)
    // Left margin words = 5 * 8 / 2 = 20
    ck_assert_uint_eq(geo.left_margin_words, 20);
    
    // Content = 40 * 8 * 2 = 640 pixels = 320 words
    ck_assert_uint_eq(geo.content_words, 320);
    
    // Right margin = 360 - 20 - 320 = 20 words
    ck_assert_uint_eq(geo.right_margin_words, 20);
    
    // Total should equal words per lane
    ck_assert_uint_eq(geo.left_margin_words + geo.content_words + geo.right_margin_words,
                      WORDS_PER_LANE);
}
END_TEST

// Test: TMDS margin calculations for 80-column mode
START_TEST(test_80col_tmds_margins) {
    uint8_t crtc[CRTC_REG_COUNT];
    init_crtc_80col(crtc);
    
    dvi_display_geometry_t geo;
    crtc_calculate_geometry(crtc, pet_display_columns_80, FRAME_WIDTH, FRAME_HEIGHT,
                            FONT_WIDTH, SYMBOLS_PER_WORD, &geo);
    
    // Same TMDS layout as 40-col (margins computed before mode adjustment)
    ck_assert_uint_eq(geo.left_margin_words, 20);
    ck_assert_uint_eq(geo.content_words, 320);
    ck_assert_uint_eq(geo.right_margin_words, 20);
}
END_TEST

// Test: Scanlines per row clamping to prevent overflow
START_TEST(test_scanlines_clamped) {
    uint8_t crtc[CRTC_REG_COUNT];
    init_crtc_40col(crtc);
    crtc[CRTC_R9_MAX_SCAN_LINE] = 31;  // Would give 32 scanlines per row
    crtc[CRTC_R6_V_DISPLAYED] = 25;    // 25 * 32 = 800, exceeds 240
    
    dvi_display_geometry_t geo;
    crtc_calculate_geometry(crtc, pet_display_columns_40, FRAME_WIDTH, FRAME_HEIGHT,
                            FONT_WIDTH, SYMBOLS_PER_WORD, &geo);
    
    // Should be clamped to 240 / 25 = 9 scanlines per row
    ck_assert_uint_le(geo.scanlines_per_row, FRAME_HEIGHT / 25);
    ck_assert_uint_le(geo.visible_scanlines, FRAME_HEIGHT);
}
END_TEST

// Test: Fewer displayed rows
START_TEST(test_fewer_rows) {
    uint8_t crtc[CRTC_REG_COUNT];
    init_crtc_40col(crtc);
    crtc[CRTC_R6_V_DISPLAYED] = 20;  // Only 20 rows
    
    dvi_display_geometry_t geo;
    crtc_calculate_geometry(crtc, pet_display_columns_40, FRAME_WIDTH, FRAME_HEIGHT,
                            FONT_WIDTH, SYMBOLS_PER_WORD, &geo);
    
    ck_assert_uint_eq(geo.rows, 20);
    ck_assert_uint_eq(geo.visible_scanlines, 160);  // 20 * 8
    ck_assert_uint_eq(geo.top_margin, 40);  // (240 - 160) / 2
}
END_TEST

// Test: Fewer displayed columns
START_TEST(test_fewer_columns) {
    uint8_t crtc[CRTC_REG_COUNT];
    init_crtc_40col(crtc);
    crtc[CRTC_R1_H_DISPLAYED] = 32;  // Only 32 columns
    
    dvi_display_geometry_t geo;
    crtc_calculate_geometry(crtc, pet_display_columns_40, FRAME_WIDTH, FRAME_HEIGHT,
                            FONT_WIDTH, SYMBOLS_PER_WORD, &geo);
    
    ck_assert_uint_eq(geo.chars_per_row, 32);
    
    // Left margin = (720/16 - 32) = 45 - 32 = 13 (in 8-pixel units)
    // Left margin words = 13 * 8 / 2 = 52
    ck_assert_uint_eq(geo.left_margin_words, 52);
}
END_TEST

// Test: Zero rows handled gracefully
START_TEST(test_zero_rows) {
    uint8_t crtc[CRTC_REG_COUNT];
    init_crtc_40col(crtc);
    crtc[CRTC_R6_V_DISPLAYED] = 0;  // Edge case: no rows
    
    dvi_display_geometry_t geo;
    crtc_calculate_geometry(crtc, pet_display_columns_40, FRAME_WIDTH, FRAME_HEIGHT,
                            FONT_WIDTH, SYMBOLS_PER_WORD, &geo);
    
    // Should not crash, rows = 0
    ck_assert_uint_eq(geo.rows, 0);
    ck_assert_uint_eq(geo.visible_scanlines, 0);
}
END_TEST

// Test: h_displayed larger than frame width causes clamping (not underflow)
START_TEST(test_h_displayed_overflow) {
    uint8_t crtc[CRTC_REG_COUNT];
    init_crtc_40col(crtc);
    crtc[CRTC_R1_H_DISPLAYED] = 113;  // 0x71 - larger than 720/16 = 45
    
    dvi_display_geometry_t geo;
    crtc_calculate_geometry(crtc, pet_display_columns_40, FRAME_WIDTH, FRAME_HEIGHT,
                            FONT_WIDTH, SYMBOLS_PER_WORD, &geo);
    
    // h_displayed should be clamped to max (45 for 40-col mode)
    ck_assert_uint_eq(geo.chars_per_row, 45);
    
    // Left margin should be 0 (no space left)
    ck_assert_uint_eq(geo.left_margin_words, 0);
    
    // Total margin + content should not exceed words per lane
    ck_assert_uint_le(geo.left_margin_words + geo.content_words + geo.right_margin_words,
                      WORDS_PER_LANE);
}
END_TEST

// Test: Garbage CRTC register values that previously caused hard fault
// These are actual values observed during initialization before CRTC is programmed
START_TEST(test_garbage_crtc_values) {
    uint8_t crtc[CRTC_REG_COUNT];
    
    // The 8032 burnin test initializes the CRTC with these garbage values
    // when run on a 40-column PET.  My guess is that `8032 test.prg` hard
    // codes the address of the ROM's CRTC initialization tables, which is
    // different on 40 vs. 80 column edit ROMs.
    crtc[CRTC_R0_H_TOTAL] = 0x4C;
    crtc[CRTC_R1_H_DISPLAYED] = 0x71;       // 113 chars - way too large!
    crtc[CRTC_R2_H_SYNC_POS] = 0xE0;
    crtc[CRTC_R3_SYNC_WIDTH] = 0x3D;
    crtc[CRTC_R4_V_TOTAL] = 0x2E;
    crtc[CRTC_R5_V_ADJUST] = 0x10;
    crtc[CRTC_R6_V_DISPLAYED] = 0x03;       // Only 3 rows
    crtc[CRTC_R7_V_SYNC_POS] = 0x3C;
    crtc[CRTC_R8_MODE_CONTROL] = 0x20;
    crtc[CRTC_R9_MAX_SCAN_LINE] = 0x5B;     // Would be 28 scanlines per row after masking
    crtc[CRTC_R10_CURSOR_START_LINE] = 0x12;
    crtc[CRTC_R11_CURSOR_END_LINE] = 0x2D;
    crtc[CRTC_R12_START_ADDR_HI] = 0x30;
    crtc[CRTC_R13_START_ADDR_LO] = 0x00;
    
    dvi_display_geometry_t geo;
    // This should NOT crash or cause undefined behavior
    crtc_calculate_geometry(crtc, pet_display_columns_40, FRAME_WIDTH, FRAME_HEIGHT,
                            FONT_WIDTH, SYMBOLS_PER_WORD, &geo);
    
    // Verify reasonable clamped values
    ck_assert_uint_le(geo.chars_per_row, FRAME_WIDTH / 16);  // Clamped to max
    ck_assert_uint_le(geo.visible_scanlines, FRAME_HEIGHT);
    
    // Verify margin calculations are sane (no underflow)
    ck_assert_uint_le(geo.left_margin_words, WORDS_PER_LANE);
    ck_assert_uint_le(geo.content_words, WORDS_PER_LANE);
    ck_assert_uint_le(geo.right_margin_words, WORDS_PER_LANE);
    
    // Total should equal words per lane
    ck_assert_uint_eq(geo.left_margin_words + geo.content_words + geo.right_margin_words,
                      WORDS_PER_LANE);
}
END_TEST

Suite *crtc_suite(void) {
    Suite *s;
    TCase *tc_40col;
    TCase *tc_80col;
    TCase *tc_invert;
    TCase *tc_margins;
    TCase *tc_edge;

    s = suite_create("crtc");

    // 40-column mode tests
    tc_40col = tcase_create("40-column");
    tcase_add_test(tc_40col, test_40col_basic_geometry);
    tcase_add_test(tc_40col, test_40col_vram_mask);
    tcase_add_test(tc_40col, test_40col_vram_start);
    tcase_add_test(tc_40col, test_40col_tmds_margins);
    suite_add_tcase(s, tc_40col);

    // 80-column mode tests
    tc_80col = tcase_create("80-column");
    tcase_add_test(tc_80col, test_80col_basic_geometry);
    tcase_add_test(tc_80col, test_80col_vram_mask);
    tcase_add_test(tc_80col, test_80col_vram_start);
    tcase_add_test(tc_80col, test_80col_tmds_margins);
    suite_add_tcase(s, tc_80col);

    // Video inversion tests
    tc_invert = tcase_create("inversion");
    tcase_add_test(tc_invert, test_video_inverted);
    tcase_add_test(tc_invert, test_video_normal);
    suite_add_tcase(s, tc_invert);

    // Margin and centering tests
    tc_margins = tcase_create("margins");
    tcase_add_test(tc_margins, test_top_margin_centered);
    tcase_add_test(tc_margins, test_fewer_rows);
    tcase_add_test(tc_margins, test_fewer_columns);
    suite_add_tcase(s, tc_margins);

    // Edge case tests
    tc_edge = tcase_create("edge_cases");
    tcase_add_test(tc_edge, test_scanlines_clamped);
    tcase_add_test(tc_edge, test_zero_rows);
    tcase_add_test(tc_edge, test_h_displayed_overflow);
    tcase_add_test(tc_edge, test_garbage_crtc_values);
    suite_add_tcase(s, tc_edge);

    return s;
}