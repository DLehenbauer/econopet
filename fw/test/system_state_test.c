// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "system_state_test.h"

#include "hardware_contract.h"
#include "sd/sd.h"
#include "system_state.h"

// Check independent wire levels and enum encodings.
START_TEST(test_configuration_pin_levels) {
    ck_assert_uint_eq(ECONOPET_CONFIG_CRT_CRTC, 0u);
    ck_assert_uint_eq(ECONOPET_CONFIG_CRT_FIXED, 1u);
    ck_assert_uint_eq(ECONOPET_CONFIG_KEYBOARD_BUSINESS, 0u);
    ck_assert_uint_eq(ECONOPET_CONFIG_KEYBOARD_GRAPHICS, 1u);
    ck_assert_int_eq(pet_video_type_crtc, ECONOPET_CONFIG_CRT_CRTC);
    ck_assert_int_eq(pet_video_type_fixed, ECONOPET_CONFIG_CRT_FIXED);
    ck_assert_int_eq(pet_keyboard_model_business, ECONOPET_CONFIG_KEYBOARD_BUSINESS);
    ck_assert_int_eq(pet_keyboard_model_graphics, ECONOPET_CONFIG_KEYBOARD_GRAPHICS);
}
END_TEST

// Preserve the pre-alignment startup models rather than relying on enum zero values.
START_TEST(test_default_models) {
    ck_assert_int_eq(system_state.pet_keyboard_model, pet_keyboard_model_graphics);
    ck_assert_int_eq(system_state.pet_video_type, pet_video_type_fixed);
}
END_TEST

// Load the shipped keymap directly and check independent PET matrix coordinates.
START_TEST(test_packaged_keymap_model_order) {
    const char* const root = getenv("ECONOPET_TEST_SDCARD_ROOT");
    ck_assert_ptr_nonnull(root);
    char path[SD_PATH_MAX];
    const int length = snprintf(path, sizeof(path), "%s/ukm/us.bin", root);
    ck_assert_int_ge(length, 0);
    ck_assert_uint_lt((size_t)length, sizeof(path));

    // Reject partial or extended files before interpreting the packed firmware array.
    FILE* const file = fopen(path, "rb");
    ck_assert_ptr_nonnull(file);
    system_state_t state = { 0 };
    ck_assert_uint_eq(fread(state.usb_keymap_data, 1, sizeof(state.usb_keymap_data), file),
        sizeof(state.usb_keymap_data));
    ck_assert_int_eq(fgetc(file), EOF);
    ck_assert_int_eq(ferror(file), 0);
    ck_assert_int_eq(fclose(file), 0);

    // These keys distinguish both keyboard models and symbolic/positional tables.
    const size_t hid_backspace = 0x2a;
    const size_t hid_number_one = 0x1e;
    const struct {
        pet_keyboard_model_t model;
        usb_keymap_kind_t kind;
        size_t hid;
        unsigned int row;
        unsigned int col;
    } cases[] = {
        { pet_keyboard_model_business, usb_keymap_kind_symbolic, hid_backspace, 7, 4 },
        { pet_keyboard_model_business, usb_keymap_kind_positional, hid_backspace, 5, 1 },
        { pet_keyboard_model_graphics, usb_keymap_kind_symbolic, hid_number_one, 6, 6 },
        { pet_keyboard_model_graphics, usb_keymap_kind_positional, hid_number_one, 0, 0 },
    };
    for (size_t i = 0; i < count_of(cases); ++i) {
        const usb_keymap_entry_t entry =
            state.usb_keymap_data[cases[i].model][cases[i].kind][cases[i].hid];
        ck_assert_uint_eq(entry.row, cases[i].row);
        ck_assert_uint_eq(entry.col, cases[i].col);
        ck_assert_uint_eq(entry.reserved, 0);
        ck_assert_uint_eq(entry.unshift, 0);
        ck_assert_uint_eq(entry.shift, 0);
    }
}
END_TEST

// Register hardware-to-firmware model mapping coverage with the host Check runner.
Suite* system_state_suite(void) {
    Suite* const suite = suite_create("system_state");
    TCase* const model = tcase_create("configuration");
    tcase_add_test(model, test_configuration_pin_levels);
    tcase_add_test(model, test_default_models);
    tcase_add_test(model, test_packaged_keymap_model_order);
    suite_add_tcase(suite, model);
    return suite;
}
