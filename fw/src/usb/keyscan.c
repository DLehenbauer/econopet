// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "keyscan.h"

#include "input/input_controls.h"
#include "input/input_routing.h"
#include "input/keyboard_core.h"

// Reserve matrix position 7/F as the no-event sentinel.  KeyEx extensions
// should avoid mapping this key.
const key_event_t key_event_none = PET_KEY_EVENT(false, 0x7, 0xF);

static uint8_t previous[ECONOPET_KBD_COL_COUNT] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};
static unsigned int r, c;

void keyscan_reset(const uint8_t* matrix) {
    memcpy(previous, matrix, sizeof(previous));
    r = c = 0;
}

static void next_rc(unsigned int* row, unsigned int* col) {
    unsigned int r = *row;
    unsigned int c = *col;

    r = (r + 1) % ECONOPET_KBD_ROW_COUNT;
    if (r == 0) {
        c = (c + 1) % ECONOPET_KBD_COL_COUNT;
    }

    *row = r;
    *col = c;
}

// This function scans the given PET keyboard matrix and returns the next PET key event
// or key_event_none if no key is pressed.
key_event_t next_key_event(const uint8_t* matrix) {
    unsigned int start_r = r;
    unsigned int start_c = c;

    key_event_t event = key_event_none;

    do {
        assert(r < ECONOPET_KBD_ROW_COUNT);
        assert(c < ECONOPET_KBD_COL_COUNT);

        // Check if the bit is set in the current (row, column) position.
        const uint8_t mask = 1 << r;

        if ((matrix[c] & mask) == 0 && (previous[c] & mask) != 0) {
            // Key is pressed and was previously released
            event = PET_KEY_EVENT(/* pressed: */ true, r, c);
            previous[c] &= ~mask;
        } else if ((matrix[c] & mask) != 0 && (previous[c] & mask) == 0) {
            // Key is released and was previously pressed
            event = PET_KEY_EVENT(/* pressed: */ false, r, c);
            previous[c] |= mask;
        }
        
        next_rc(&r, &c);
    } while (event == key_event_none && (r != start_r || c != start_c));

    return event;
}

bool is_key_down(const uint8_t matrix[ECONOPET_KBD_COL_COUNT], key_event_t event) {
    assert(event != key_event_none);
 
    const unsigned int row = PET_KEY_ROW(event);
    const unsigned int col = PET_KEY_COL(event);
    const uint8_t mask = 1 << row;
    
    return (matrix[col] & mask) == 0;
}

int keyscan_is_shifted(const uint8_t matrix[ECONOPET_KBD_COL_COUNT]) {
    const bool business = system_state.pet_keyboard_model == pet_keyboard_model_business;
    return is_key_down(matrix, business ? PET_KEY_LSHIFT_B : PET_KEY_LSHIFT_N)
        || is_key_down(matrix, business ? PET_KEY_RSHIFT_B : PET_KEY_RSHIFT_N);
}

tui_action_t keyscan_navigation(const uint8_t matrix[ECONOPET_KBD_COL_COUNT]) {
    static const key_event_t pet_keys[] = {
        // Graphics Keyboard
        PET_KEY_DOWN_N,
        PET_KEY_RIGHT_N,
        PET_KEY_RETURN_N,
        PET_KEY_RUNSTOP_N,
        PET_KEY_DELETE_N,

        // Business Keyboard
        PET_KEY_DOWN_B,
        PET_KEY_RIGHT_B,
        PET_KEY_RETURN_B,
        PET_KEY_RUNSTOP_B,
        PET_KEY_DELETE_B,
    };
    
    static const tui_action_t navigation_unshifted[] = {
        // Graphics Keyboard
        /*    PET_KEY_DOWN_N: */ tui_action_down,
        /*   PET_KEY_RIGHT_N: */ tui_action_page_down,
        /*  PET_KEY_RETURN_N: */ tui_action_activate,
        /* PET_KEY_RUNSTOP_N: */ tui_action_back,
        /*  PET_KEY_DELETE_N: */ tui_action_back,

        // Business Keyboard
        /*    PET_KEY_DOWN_B: */ tui_action_down,
        /*   PET_KEY_RIGHT_B: */ tui_action_page_down,
        /*  PET_KEY_RETURN_B: */ tui_action_activate,
        /* PET_KEY_RUNSTOP_B: */ tui_action_back,
        /*  PET_KEY_DELETE_B: */ tui_action_back,
    };
    
    static const tui_action_t navigation_shifted[] = {
        // Graphics Keyboard
        /*    PET_KEY_DOWN_N: */ tui_action_up,
        /*   PET_KEY_RIGHT_N: */ tui_action_page_up,
        /*  PET_KEY_RETURN_N: */ tui_action_activate,
        /* PET_KEY_RUNSTOP_N: */ tui_action_back,
        /*  PET_KEY_DELETE_N: */ tui_action_back,

        // Business Keyboard
        /*    PET_KEY_DOWN_B: */ tui_action_up,
        /*   PET_KEY_RIGHT_B: */ tui_action_page_up,
        /*  PET_KEY_RETURN_B: */ tui_action_activate,
        /* PET_KEY_RUNSTOP_B: */ tui_action_back,
        /*  PET_KEY_DELETE_B: */ tui_action_back,
    };
    
    static_assert((count_of(pet_keys) == count_of(navigation_unshifted))
        && (count_of(pet_keys) == count_of(navigation_shifted)),
        "Key event arrays must be the same size");
    
    while (true) {
        key_event_t event = next_key_event(matrix);
        if (event == key_event_none) {
            return tui_action_none;
        }

        // Decode only the configured layout, not unrelated keys at the other layout's positions.
        const size_t keys_per_model = count_of(pet_keys) / 2;
        const size_t first = system_state.pet_keyboard_model == pet_keyboard_model_business
            ? keys_per_model : 0;
        for (size_t i = first; i < first + keys_per_model; i++) {
            if (event == pet_keys[i]) {
                return keyscan_is_shifted(matrix) ? navigation_shifted[i] : navigation_unshifted[i];
            }
        }
    }

    return tui_action_none;
}

static input_hold_t menu_hold;
static bool injected_shifts_blocked;

/** Reject snooped injected Shifts until the physical snapshot observes both released. */
static bool physical_chord(bool* other_key) {
    const bool business = system_state.pet_keyboard_model == pet_keyboard_model_business;
    const key_event_t left = business ? PET_KEY_LSHIFT_B : PET_KEY_LSHIFT_N;
    const key_event_t right = business ? PET_KEY_RSHIFT_B : PET_KEY_RSHIFT_N;
    const bool pet_left = is_key_down(pet_key_matrix, left);
    const bool pet_right = is_key_down(pet_key_matrix, right);
    if (is_key_down(usb_key_matrix, left) || is_key_down(usb_key_matrix, right))
        injected_shifts_blocked = true;
    else if (!pet_left && !pet_right)
        injected_shifts_blocked = false;
    *other_key = false;
    for (unsigned int col = 0; col < ECONOPET_KBD_COL_COUNT; ++col) {
        uint8_t allowed = 0;
        if (col == PET_KEY_COL(left)) allowed |= 1u << PET_KEY_ROW(left);
        if (col == PET_KEY_COL(right)) allowed |= 1u << PET_KEY_ROW(right);
        *other_key |= (uint8_t)(pet_key_matrix[col] | allowed) != UINT8_MAX;
    }
    return !injected_shifts_blocked && pet_left && pet_right;
}

void keyscan_poll_gestures(void) {
    bool other_key;
    const bool both = physical_chord(&other_key);
    input_chord_update(INPUT_SOURCE_PET, &menu_hold, both, other_key);
    if (input_hold_ready(&menu_hold)) {
        input_routing_gesture(INPUT_SOURCE_PET, input_gesture_dual_shift);
        input_hold_consume(&menu_hold);
    }
}

void keyscan_consume_gestures(void) {
    bool other_key;
    input_hold_update(&menu_hold, physical_chord(&other_key));
    input_hold_consume(&menu_hold);
}

void keyscan_sample(void) {
    tui_action_t action;
    while ((action = keyscan_navigation(pet_key_matrix)) != tui_action_none)
        (void)input_routing_submit(INPUT_SOURCE_PET, action);
}
