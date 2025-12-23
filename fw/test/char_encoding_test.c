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

#include "../src/display/char_encoding.h"
#include "char_encoding_test.h"
#include <stdbool.h>
#include <string.h>

// Test that printable ASCII characters map correctly to their corresponding
// VROM offsets.
START_TEST(test_ascii_to_vrom) {
    // Check '0-9'
    for (uint8_t i = 0; i < 10; i++) {
        ck_assert_uint_eq(ascii_to_vrom('0' + i), 0x30 + i);
    }

    // Check 'a-z' and 'A-Z'
    for (uint8_t i = 0; i < 26; i++) {
        ck_assert_uint_eq(ascii_to_vrom('a' + i), 0x01 + i);
        ck_assert_uint_eq(ascii_to_vrom('A' + i), 0x41 + i);
    }

    // Check ' !"#$%&'()*+,-./'
    for (uint8_t c = 0x20; c <= 0x2F; c++) {
        ck_assert_uint_eq(ascii_to_vrom(c), c);
    }

    // Check ':;<=>?'
    for (uint8_t c = 0x3A; c <= 0x3F; c++) {
        ck_assert_uint_eq(ascii_to_vrom(c), c);
    }

    // Check '[\]^'
    for (uint8_t c = '['; c <= '^'; c++) {
        ck_assert_uint_eq(ascii_to_vrom(c), c - 0x40);
    }

    // Remaining special characters with unique mappings
    ck_assert_uint_eq(ascii_to_vrom('@'), 0x00);    // Disjoint
    ck_assert_uint_eq(ascii_to_vrom('_'), 0x64);    // Substitute PETSCII character gfx
    ck_assert_uint_eq(ascii_to_vrom('`'), 0x27);    // Substitute single quote (')
    ck_assert_uint_eq(ascii_to_vrom('{'), 0x6B);    // Substitute PETSCII character gfx
    ck_assert_uint_eq(ascii_to_vrom('|'), 0x5B);    // Substitute PETSCII character gfx
    ck_assert_uint_eq(ascii_to_vrom('}'), 0x73);    // Substitute PETSCII character gfx
    ck_assert_uint_eq(ascii_to_vrom('~'), 0x71);    // Substitute PETSCII character gfx

    // Boundary conditions
    ck_assert_uint_eq(ascii_to_vrom(128), 0x00);  // Out of range
    ck_assert_uint_eq(ascii_to_vrom(255), 0x00);  // Out of range
} END_TEST

// Test VROM to terminal conversion returns valid strings
START_TEST(test_vrom_to_term_not_null) {
    // All 128 VROM values should return non-null strings
    for (uint8_t i = 0; i < 128; i++) {
        const char* str = vrom_to_term(i);
        ck_assert_ptr_nonnull(str);
        ck_assert_uint_gt(strlen(str), 0);
    }
} END_TEST

// Test VROM to terminal for standard ASCII characters
START_TEST(test_vrom_to_term_standard_chars) {
    // Space
    ck_assert_str_eq(vrom_to_term(0x20), " ");

    // Digits
    ck_assert_str_eq(vrom_to_term(0x30), "0");
    ck_assert_str_eq(vrom_to_term(0x39), "9");

    // Uppercase letters
    ck_assert_str_eq(vrom_to_term(0x41), "A");
    ck_assert_str_eq(vrom_to_term(0x5A), "Z");

    // Lowercase letters (in PET VROM)
    ck_assert_str_eq(vrom_to_term(0x01), "a");
    ck_assert_str_eq(vrom_to_term(0x1A), "z");
} END_TEST

// Test VROM to terminal masks high bit (reverse video)
START_TEST(test_vrom_to_term_high_bit_mask) {
    // High bit should be masked - same output regardless of bit 7
    ck_assert_str_eq(vrom_to_term(0x20), vrom_to_term(0xA0));
    ck_assert_str_eq(vrom_to_term(0x41), vrom_to_term(0xC1));
    ck_assert_str_eq(vrom_to_term(0x01), vrom_to_term(0x81));
} END_TEST

// Test VROM to terminal for line drawing characters (use escape sequences)
START_TEST(test_vrom_to_term_line_drawing) {
    // 0x40 is horizontal line - uses VT-100 line drawing mode
    const char* hl = vrom_to_term(0x40);
    ck_assert_ptr_nonnull(strstr(hl, "\e(0"));  // Enters line drawing mode
    ck_assert_ptr_nonnull(strstr(hl, "\e(B"));  // Exits line drawing mode

    // 0x5D is vertical line
    const char* vl = vrom_to_term(0x5D);
    ck_assert_ptr_nonnull(strstr(vl, "\e(0"));

    // 0x70 is top-left corner
    const char* tl = vrom_to_term(0x70);
    ck_assert_ptr_nonnull(strstr(tl, "\e(0"));
} END_TEST

// Test roundtrip for common ASCII characters
START_TEST(test_roundtrip_ascii) {
    // For simple printable ASCII, converting to VROM then to term should give recognizable output
    const char* term_space = vrom_to_term(ascii_to_vrom(' '));
    ck_assert_str_eq(term_space, " ");

    const char* term_A = vrom_to_term(ascii_to_vrom('A'));
    ck_assert_str_eq(term_A, "A");

    const char* term_0 = vrom_to_term(ascii_to_vrom('0'));
    ck_assert_str_eq(term_0, "0");

    const char* term_a = vrom_to_term(ascii_to_vrom('a'));
    ck_assert_str_eq(term_a, "a");
} END_TEST

Suite* char_encoding_suite(void) {
    Suite* s = suite_create("CharEncoding");

    TCase* ascii_tests = tcase_create("ascii_to_vrom");
    tcase_add_test(ascii_tests, test_ascii_to_vrom);
    suite_add_tcase(s, ascii_tests);

    TCase* vrom_tests = tcase_create("vrom_to_term");
    tcase_add_test(vrom_tests, test_vrom_to_term_not_null);
    tcase_add_test(vrom_tests, test_vrom_to_term_standard_chars);
    tcase_add_test(vrom_tests, test_vrom_to_term_high_bit_mask);
    tcase_add_test(vrom_tests, test_vrom_to_term_line_drawing);
    suite_add_tcase(s, vrom_tests);

    TCase* roundtrip_tests = tcase_create("roundtrip");
    tcase_add_test(roundtrip_tests, test_roundtrip_ascii);
    suite_add_tcase(s, roundtrip_tests);

    return s;
}
