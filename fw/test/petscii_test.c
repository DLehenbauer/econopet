// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "cbm/petscii.h"
#include "petscii_test.h"

// PETSCII -> ASCII (cc65 toascii): unshifted letters $41-$5A fold to lowercase.
START_TEST(test_to_ascii_unshifted_letters) {
    for (uint8_t c = 0x41; c <= 0x5A; c++) {
        ck_assert_int_eq(petscii_to_ascii(c), (char)(c | 0x20));
    }
}
END_TEST

// PETSCII -> ASCII: shifted letters $C1-$DA fold to uppercase. ($C1-$DA is the
// exact range produced by the forward map for A-Z.)
START_TEST(test_to_ascii_shifted_letters) {
    for (uint8_t c = 0xC1; c <= 0xDA; c++) {
        ck_assert_int_eq(petscii_to_ascii(c), (char)(c & 0x7F));
    }
}
END_TEST

// Digits, space, and punctuation pass through unchanged.
START_TEST(test_to_ascii_passthrough) {
    for (uint8_t c = '0'; c <= '9'; c++) {
        ck_assert_int_eq(petscii_to_ascii(c), (char)c);
    }
    ck_assert_int_eq(petscii_to_ascii(' '), ' ');
    ck_assert_int_eq(petscii_to_ascii('.'), '.');
    ck_assert_int_eq(petscii_to_ascii('*'), '*');
}
END_TEST

// ASCII -> PETSCII (cc65 charmap), text mode: A-Z -> shifted $C1-$DA.
START_TEST(test_to_petscii_upper_letters) {
    for (char c = 'A'; c <= 'Z'; c++) {
        ck_assert_uint_eq(ascii_to_petscii((uint8_t)c, false), (uint8_t)(c + 0x80));
    }
}
END_TEST

// ASCII -> PETSCII (cc65 charmap), text mode: a-z -> unshifted $41-$5A.
START_TEST(test_to_petscii_lower_letters) {
    for (char c = 'a'; c <= 'z'; c++) {
        ck_assert_uint_eq(ascii_to_petscii((uint8_t)c, false), (uint8_t)(c - 0x20));
    }
}
END_TEST

// Graphics mode folds letters to lowercase first, so both cases land in the
// unshifted range $41-$5A (shown as uppercase glyphs).
START_TEST(test_to_petscii_graphics_folds_case) {
    for (char c = 'A'; c <= 'Z'; c++) {
        // 'A'($41)..'Z'($5A) map to the same unshifted codes $41..$5A.
        ck_assert_uint_eq(ascii_to_petscii((uint8_t)c, true), (uint8_t)c);
    }
    for (char c = 'a'; c <= 'z'; c++) {
        ck_assert_uint_eq(ascii_to_petscii((uint8_t)c, true), (uint8_t)(c - 0x20));
    }
}
END_TEST

// ASCII -> PETSCII: relocated control codes (swap pairs) and punctuation.
START_TEST(test_to_petscii_remaps) {
    ck_assert_uint_eq(ascii_to_petscii(0x08, false), 0x14);
    ck_assert_uint_eq(ascii_to_petscii(0x14, false), 0x08);
    ck_assert_uint_eq(ascii_to_petscii(0x0A, false), 0x0D);
    ck_assert_uint_eq(ascii_to_petscii(0x0D, false), 0x0A);
    ck_assert_uint_eq(ascii_to_petscii(0x0B, false), 0x11);
    ck_assert_uint_eq(ascii_to_petscii(0x11, false), 0x0B);
    ck_assert_uint_eq(ascii_to_petscii(0x0C, false), 0x93);
    ck_assert_uint_eq(ascii_to_petscii(0x93, false), 0x0C);
    ck_assert_uint_eq(ascii_to_petscii(0x5C, false), 0xBF);
    ck_assert_uint_eq(ascii_to_petscii(0x5F, false), 0xA4);
    ck_assert_uint_eq(ascii_to_petscii(0x60, false), 0xAD);
    ck_assert_uint_eq(ascii_to_petscii(0x7B, false), 0xB3);
    ck_assert_uint_eq(ascii_to_petscii(0x7C, false), 0xDD);
    ck_assert_uint_eq(ascii_to_petscii(0x7D, false), 0xAB);
    ck_assert_uint_eq(ascii_to_petscii(0x7E, false), 0xB1);
    ck_assert_uint_eq(ascii_to_petscii(0x7F, false), 0xDF);
}
END_TEST

// PETSCII -> ASCII: the editing control swaps and relocated punctuation map
// back to their ASCII source codes (the inverse of the forward remaps).
START_TEST(test_to_ascii_remaps) {
    ck_assert_int_eq(petscii_to_ascii(0x14), 0x08);
    ck_assert_int_eq(petscii_to_ascii(0x08), 0x14);
    ck_assert_int_eq(petscii_to_ascii(0x0D), 0x0A);
    ck_assert_int_eq(petscii_to_ascii(0x0A), 0x0D);
    ck_assert_int_eq(petscii_to_ascii(0x11), 0x0B);
    ck_assert_int_eq(petscii_to_ascii(0x0B), 0x11);
    ck_assert_int_eq(petscii_to_ascii(0x93), 0x0C);
    ck_assert_int_eq(petscii_to_ascii(0x0C), (char)0x93);
    ck_assert_int_eq(petscii_to_ascii(0xBF), 0x5C);
    ck_assert_int_eq(petscii_to_ascii(0xA4), 0x5F);
    ck_assert_int_eq(petscii_to_ascii(0xAD), 0x60);
    ck_assert_int_eq(petscii_to_ascii(0xB3), 0x7B);
    ck_assert_int_eq(petscii_to_ascii(0xDD), 0x7C);
    ck_assert_int_eq(petscii_to_ascii(0xAB), 0x7D);
    ck_assert_int_eq(petscii_to_ascii(0xB1), 0x7E);
    ck_assert_int_eq(petscii_to_ascii(0xDF), 0x7F);
}
END_TEST

// petscii_to_ascii is the exact inverse of ascii_to_petscii (fold_case = false):
// every 7-bit ASCII byte survives the round trip unchanged. (The high half is
// not in the forward map's domain, so it is not part of the round trip.)
START_TEST(test_roundtrip_inverse) {
    for (int c = 0x00; c <= 0x7F; c++) {
        uint8_t petscii = ascii_to_petscii((uint8_t)c, false);
        ck_assert_int_eq(petscii_to_ascii(petscii), (char)(uint8_t)c);
    }
}
END_TEST

// ASCII -> PETSCII: digits, space, and other punctuation are identity.
START_TEST(test_to_petscii_passthrough) {
    for (uint8_t c = '0'; c <= '9'; c++) {
        ck_assert_uint_eq(ascii_to_petscii(c, false), c);
    }
    ck_assert_uint_eq(ascii_to_petscii(' ', false), (uint8_t)' ');
    ck_assert_uint_eq(ascii_to_petscii('-', false), (uint8_t)'-');
    ck_assert_uint_eq(ascii_to_petscii('.', false), (uint8_t)'.');
    ck_assert_uint_eq(ascii_to_petscii('*', false), (uint8_t)'*');
}
END_TEST

Suite* petscii_suite(void) {
    Suite* s = suite_create("petscii");
    TCase* tc = tcase_create("convert");

    tcase_add_test(tc, test_to_ascii_unshifted_letters);
    tcase_add_test(tc, test_to_ascii_shifted_letters);
    tcase_add_test(tc, test_to_ascii_passthrough);
    tcase_add_test(tc, test_to_ascii_remaps);
    tcase_add_test(tc, test_roundtrip_inverse);
    tcase_add_test(tc, test_to_petscii_upper_letters);
    tcase_add_test(tc, test_to_petscii_lower_letters);
    tcase_add_test(tc, test_to_petscii_graphics_folds_case);
    tcase_add_test(tc, test_to_petscii_remaps);
    tcase_add_test(tc, test_to_petscii_passthrough);

    suite_add_tcase(s, tc);
    return s;
}
