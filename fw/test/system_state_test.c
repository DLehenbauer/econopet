// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "system_state_test.h"

#include "hardware_contract.h"
#include "system_state.h"

// Check independent wire levels and every display/keyboard configuration pair.
START_TEST(test_configuration_pin_levels) {
    ck_assert_uint_eq(ECONOPET_CONFIG_CRT_CRTC, 0u);
    ck_assert_uint_eq(ECONOPET_CONFIG_CRT_FIXED, 1u);
    ck_assert_uint_eq(ECONOPET_CONFIG_KEYBOARD_BUSINESS, 0u);
    ck_assert_uint_eq(ECONOPET_CONFIG_KEYBOARD_GRAPHICS, 1u);

    system_state_t state = {
        .pet_display_columns = pet_display_columns_80,
        .video_ram_mask = pet_video_ram_mask_colourpet,
    };
    const bool levels[] = { false, true };
    for (size_t crt = 0; crt < sizeof(levels) / sizeof(levels[0]); ++crt) {
        for (size_t keyboard = 0; keyboard < sizeof(levels) / sizeof(levels[0]); ++keyboard) {
            system_state_set_config_pins(&state, levels[crt], levels[keyboard]);
            ck_assert_int_eq(state.pet_video_type,
                levels[crt] ? pet_video_type_fixed : pet_video_type_crtc);
            ck_assert_int_eq(state.pet_keyboard_model,
                levels[keyboard] ? pet_keyboard_model_graphics : pet_keyboard_model_business);
            ck_assert_int_eq(state.pet_display_columns, pet_display_columns_80);
            ck_assert_uint_eq(state.video_ram_mask, pet_video_ram_mask_colourpet);
        }
    }
}
END_TEST

// Register hardware-to-firmware model mapping coverage with the host Check runner.
Suite* system_state_suite(void) {
    Suite* const suite = suite_create("system_state");
    TCase* const model = tcase_create("configuration");
    tcase_add_test(model, test_configuration_pin_levels);
    suite_add_tcase(suite, model);
    return suite;
}
