// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "hud_trigger.h"

#include "hud.h"
#include "ieee/ieee_drive.h"
#include "system_state.h"
#include "usb/keyboard.h"
#include "usb/keyscan.h"

// The overlay's disk-status line paints on the PET's bottom row (25-row screen).
#define HUD_STATUS_ROW 24
#define HUD_STATUS_COL 0

// How long the overlay auto-appears after a disk swap when not pinned on.
#define HUD_FLASH_US (3 * 1000 * 1000)

// Debounce for the PET-keyboard chords (matches the MENU button).
#define CHORD_DEBOUNCE_US 50000

// Overlay display state:
//   persistent_on : pinned always-on (toggled by a SHIFT hold or USB F7)
//   flash_until_us: transient window after a disk swap (shows even when off)
static bool     persistent_on = false;
static uint64_t flash_until_us = 0;

// Bumped on every mount; the overlay flashes when it changes.
static unsigned last_mount_gen = 0;
static bool     mount_gen_valid = false;

// Pin the overlay on/off. Shared by the SHIFT-hold trigger and the USB hotkey.
void hud_trigger_toggle(void) {
    persistent_on = !persistent_on;
}

// F7 pins the overlay on/off. Returns true when consumed.
bool hud_trigger_hotkey(uint8_t hid_keycode) {
    if (hid_keycode == 0x40) {   // F7
        hud_trigger_toggle();
        return true;
    }
    return false;
}

// PET-keyboard controls. The keyboard wires to the real PIA, so nothing can
// be hidden from running software; every trigger is built from keys that
// leak harmlessly:
//   hold SHIFT alone ~2.5s -> pin the overlay on/off (shift alone types nothing)
//   SHIFT + 0-3            -> cycle a drive image, only while the overlay is
//                             pinned (so normal typing can never trigger it)
static bool business_kbd(void);

// True when no key other than the shift cells is pressed anywhere in the
// snooped matrix.
static bool is_matrix_idle_except_shift(void) {
    for (unsigned int col = 0; col < 10; col++) {
        uint8_t v = pet_key_matrix[col];
        if (business_kbd()) {
            if (col == 6) v |= (1u << 6) | (1u << 0);   // business L/R shift
        } else {
            if (col == 8) v |= (1u << 0) | (1u << 5);   // graphics L/R shift
        }
        if (v != 0xFF) return false;
    }
    return true;
}

// The two keyboard layouts assign the same matrix cells to different keys
// (the business shift cell is the graphics keypad-1, for example), so the
// cell maps must follow the keyboard DIP -- a cross-model union chords on
// its own shift keys.
static bool business_kbd(void) {
    return system_state.pet_keyboard_model == pet_keyboard_model_business;
}

static bool shift_down(void) {
    if (business_kbd()) {
        return is_key_down(pet_key_matrix, PET_KEY_LSHIFT_B)
            || is_key_down(pet_key_matrix, PET_KEY_RSHIFT_B);
    }
    return is_key_down(pet_key_matrix, PET_KEY_LSHIFT_N)
        || is_key_down(pet_key_matrix, PET_KEY_RSHIFT_N);
}

static bool digit_down(unsigned int n) {
    static const key_event_t n_cells[4] = {
        PET_KEY_0_N, PET_KEY_1_N, PET_KEY_2_N, PET_KEY_3_N,
    };
    static const key_event_t b_cells[4][2] = {
        { PET_KEY_0_B, PET_KEY_0K_B },
        { PET_KEY_1_B, PET_KEY_1K_B },
        { PET_KEY_2_B, PET_KEY_2K_B },
        { PET_KEY_3_B, PET_KEY_3K_B },
    };
    if (business_kbd()) {
        return is_key_down(pet_key_matrix, b_cells[n][0])
            || is_key_down(pet_key_matrix, b_cells[n][1]);
    }
    return is_key_down(pet_key_matrix, n_cells[n]);
}

// Debounced rising-edge detector for a chord. 'state' holds the per-chord
// debounce statics; returns true once when the chord becomes stably held.
typedef struct {
    uint64_t last_change_us;
    bool     raw_prev;
    bool     stable;
} chord_state_t;

static bool chord_rising(chord_state_t* s, bool raw) {
    const uint64_t now = time_us_64();
    if (raw != s->raw_prev) {
        s->raw_prev = raw;
        s->last_change_us = now;
    } else if ((now - s->last_change_us) >= CHORD_DEBOUNCE_US && s->stable != raw) {
        s->stable = raw;
        return s->stable;
    }
    return false;
}

void hud_trigger_task(void) {
    static chord_state_t cycle0_chord;
    static chord_state_t cycle1_chord;
    static chord_state_t cycle2_chord;
    static chord_state_t cycle3_chord;

    const uint64_t now = time_us_64();

    // Auto-flash the overlay whenever a disk image changes (any source).
    const unsigned gen = ieee_drive_mount_generation();
    if (!mount_gen_valid) {
        last_mount_gen = gen;
        mount_gen_valid = true;
    } else if (gen != last_mount_gen) {
        last_mount_gen = gen;
        flash_until_us = now + HUD_FLASH_US;
    }

    // Holding SHIFT alone for a beat pins the overlay on/off. Arming needs a
    // rising shift edge (a latched shift lock never fires) and any other key
    // disarms, so shifted typing never triggers it.
    static uint64_t shift_hold_start = 0;
    static bool shift_prev = false, shift_armed = false, shift_fired = false;
    const bool shift_now = shift_down();
    if (shift_now && !shift_prev) {
        shift_armed = true;
        shift_fired = false;
        shift_hold_start = now;
    } else if (!shift_now) {
        shift_armed = false;
    }
    shift_prev = shift_now;
    // Other keys (e.g. a chorded digit) pause the timer rather than re-arming
    // it, so cycling never toggles the overlay by accident.
    if (!is_matrix_idle_except_shift()) {
        shift_hold_start = now;
    }
    if (shift_armed && !shift_fired && (now - shift_hold_start) >= 2500000) {
        shift_fired = true;
        hud_trigger_toggle();
    }
    // While the overlay is pinned, SHIFT + 0-3 cycle the drive images (the
    // swap auto-flashes via the mount-generation check above).
    if (persistent_on) {
        if (chord_rising(&cycle0_chord, (shift_down() && digit_down(0)))) {
            ieee_drive_cycle(0);
        }
        if (chord_rising(&cycle1_chord, (shift_down() && digit_down(1)))) {
            ieee_drive_cycle(1);
        }
        if (chord_rising(&cycle2_chord, (shift_down() && digit_down(2)))) {
            ieee_drive_cycle(2);
        }
        if (chord_rising(&cycle3_chord, (shift_down() && digit_down(3)))) {
            ieee_drive_cycle(3);
        }
    }

    // Shown while pinned on, or during the post-swap flash window.
    const bool shown = persistent_on || (int64_t)(flash_until_us - now) > 0;

    // Repaint only when the text changes; pushing the buffer over SPI every
    // pass would compete with the IEEE FIFO refill.
    static char last_line[84] = "";
    static bool line_valid = false;

    if (shown) {
        char line[84];
        ieee_drive_hud_status(line, sizeof(line));
        if (!line_valid || strcmp(line, last_line) != 0) {
            hud_show_text(HUD_STATUS_ROW, HUD_STATUS_COL, line);
            snprintf(last_line, sizeof(last_line), "%s", line);
            line_valid = true;
        }
    } else {
        hud_hide();
        line_valid = false;  // force a repaint next time it's shown
    }
}
