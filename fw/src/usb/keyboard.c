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

#include "pch.h"
#include "keyboard.h"

#include "diag/log/log.h"
#include "driver.h"
#include "fatal.h"
#include "keyscan.h"
#include "keystate.h"
#include "usb.h"

uint8_t usb_key_matrix[KEY_COL_COUNT] = {
    /* 0 */ 0xff,
    /* 1 */ 0xff,
    /* 2 */ 0xff,
    /* 3 */ 0xff,
    /* 4 */ 0xff,
    /* 5 */ 0xff,
    /* 6 */ 0xff,
    /* 7 */ 0xff,
    /* 8 */ 0xff,
    /* 9 */ 0xff,
};

uint8_t pet_key_matrix[KEY_COL_COUNT] = {
    /* 0 */ 0xff,
    /* 1 */ 0xff,
    /* 2 */ 0xff,
    /* 3 */ 0xff,
    /* 4 */ 0xff,
    /* 5 */ 0xff,
    /* 6 */ 0xff,
    /* 7 */ 0xff,
    /* 8 */ 0xff,
    /* 9 */ 0xff,
};

// If true, use symbolic keyboard mapping: USB key events are mapped to PET keys
// based on the symbol printed on the USB keyboard.
//
// If false, use physical keyboard mapping: USB key events are mapped to PET keys
// based on the physical position of the key. This mode is useful for software or games
// that expect the original PET keyboard layout, where the key's position is more
// important than its label.
static bool use_symbolic_keymap = true;

// If true, the USB keyboard model is swapped (graphics <-> business) with
// respect to the PET keyboard.
static bool swap_keyboard_model = false;

// State of the USB keyboard caps lock.
static bool caps_lock_enabled = false;

// The currently active USB keyboard model (graphics or business).
static pet_keyboard_model_t active_keyboard_model = pet_keyboard_model_graphics;
static usb_keymap_kind_t active_keymap_kind = usb_keymap_kind_symbolic;
static const usb_keymap_entry_t* s_keymap = system_state.usb_keymap_data[pet_keyboard_model_graphics][usb_keymap_kind_symbolic];

typedef struct {
    uint8_t key;            // USB HID keycode
    uint8_t row;            // PET keyboard row
    uint8_t col;            // PET keyboard column
    uint8_t modifiers;      // USB HID modifier flags (e.g. shift)
    bool pressed;           // true = key pressed, false = key released
} KeyEvent;

#define KEY_BUFFER_LOG2_CAPACITY 4
#define KEY_BUFFER_CAPACITY (1 << KEY_BUFFER_LOG2_CAPACITY)
#define KEY_BUFFER_CAPACITY_MASK (KEY_BUFFER_CAPACITY - 1)

static KeyEvent key_buffer[KEY_BUFFER_CAPACITY];
static uint8_t key_buffer_next_write_index = 0;
static uint8_t key_buffer_next_read_index = 0;

// Note that the theoretical maximum number of keyboards is actually 127.
#define MAX_KEYBOARDS 8

typedef struct {
    uint8_t dev_addr;
    uint8_t instance;
    bool attached;
} keyboard_info_t;

static keyboard_info_t attached_keyboards[MAX_KEYBOARDS] = { 0 };

static void enqueue_key_event(uint8_t keycode, uint8_t row, uint8_t col, uint8_t modifiers, bool pressed) {
    uint8_t new_write_index = (key_buffer_next_write_index + 1) & KEY_BUFFER_CAPACITY_MASK;
    if (new_write_index != key_buffer_next_read_index) {
        KeyEvent keyEvent = {
            .key = keycode,
            .row = row,
            .col = col,
            .modifiers = modifiers,
            .pressed = pressed,
        };
        key_buffer[key_buffer_next_write_index] = keyEvent;
        key_buffer_next_write_index = new_write_index;
    }
}

static bool peek_key_event(KeyEvent* keyEvent) {
    *keyEvent = key_buffer[key_buffer_next_read_index];
    return key_buffer_next_write_index != key_buffer_next_read_index;
}

static void dequeue_key_event() {
    assert(key_buffer_next_write_index != key_buffer_next_read_index);
    key_buffer_next_read_index = (key_buffer_next_read_index + 1) & KEY_BUFFER_CAPACITY_MASK;
}

void usb_keyboard_enqueue_key_up(uint8_t keycode, uint8_t modifiers) {
    KeyStateFlags keystate = keystate_reset(keycode);

    if (!(keystate & KEYSTATE_PRESSED_FLAG)) {
        log_debug("USB: Key up %d=(not found)", keycode);
        return;
    }

    usb_keymap_entry_t keyInfo = s_keymap[
        (keystate & KEYSTATE_SHIFTED_FLAG)
            ? keycode | 0x0100      // s_keymap[0x0000..0x00ff] = shifted keymap
            : keycode];             // s_keymap[0x0100..0x01ff] = unshifted keymap

    const uint8_t row = keyInfo.row;
    const uint8_t col = keyInfo.col;

    enqueue_key_event(keycode, row, col, modifiers, /* pressed: */ false);
}

void usb_keyboard_enqueue_key_down(uint8_t keycode, uint8_t modifiers) {
    bool is_shifted = (modifiers & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) != 0;

    usb_keymap_entry_t keyInfo = s_keymap[
        is_shifted
            ? keycode | 0x0100      // s_keymap[0x0000..0x00ff] = shifted keymap
            : keycode];             // s_keymap[0x0100..0x01ff] = unshifted keymap

    const uint8_t row = keyInfo.row;
    const uint8_t col = keyInfo.col;

    if (keyInfo.shift) { 
        modifiers |= KEYBOARD_MODIFIER_LEFTSHIFT;
    }

    if (keyInfo.unshift) {
        modifiers &= ~(KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
    }

    enqueue_key_event(keycode, row, col, modifiers, /* pressed: */ true);

    keystate_set(keycode, KEYSTATE_PRESSED_FLAG | (is_shifted ? KEYSTATE_SHIFTED_FLAG : 0));
}

static void key_down(uint8_t keycode, uint8_t row, uint8_t col) {
    if (row > 7) {
        log_debug("USB: Key down: %d=(undefined)", keycode);
        return;
    }

    uint8_t rowMask = 1 << row;

    if (usb_key_matrix[col] & rowMask) {
        usb_key_matrix[col] &= ~rowMask;
        log_debug("USB: Key down: %d=(%d,%d)", keycode, row, col);
    }
}

static void key_up(uint8_t keycode, uint8_t row, uint8_t col) {
    if (row > 7) {
        log_debug("USB: Key up: %d=(undefined)", keycode);
        return;
    }

    uint8_t rowMask = 1 << row;

    if (usb_key_matrix[col] & ~rowMask) {
        usb_key_matrix[col] |= rowMask;
        log_debug("USB: Key up: %d=(%d,%d)", keycode, row, col);
    }
}

static const char* const usb_modifier_names[] = {
    "CONTROL_LEFT",
    "SHIFT_LEFT",
    "ALT_LEFT",
    "GUI_LEFT",
    "CONTROL_RIGHT",
    "SHIFT_RIGHT",
    "ALT_RIGHT",
    "GUI_RIGHT",
};
    
static void modifier_down(uint8_t modifier_offset) {
    log_debug("USB: Modifier down: %s", usb_modifier_names[modifier_offset]);
    uint8_t keycode = HID_KEY_CONTROL_LEFT + modifier_offset;
    usb_keymap_entry_t keyInfo = s_keymap[keycode];
    key_down(keycode, keyInfo.row, keyInfo.col);
}

static void modifier_up(uint8_t modifier_offset) {
    log_debug("USB: Modifier up: %s", usb_modifier_names[modifier_offset]);
    uint8_t keycode = HID_KEY_CONTROL_LEFT + modifier_offset;
    usb_keymap_entry_t keyInfo = s_keymap[keycode];
    key_up(keycode, keyInfo.row, keyInfo.col);
}

static void sync_leds(uint8_t dev_addr) {
    static uint8_t led_report = 0;

    led_report = 0;

    if (caps_lock_enabled) {
        led_report |= KEYBOARD_LED_CAPSLOCK;
    }
    if (active_keyboard_model == pet_keyboard_model_graphics) {
        led_report |= KEYBOARD_LED_NUMLOCK;
    }
    if (active_keymap_kind == usb_keymap_kind_symbolic) {
        led_report |= KEYBOARD_LED_SCROLLLOCK;
    }

    tuh_hid_set_report(dev_addr, 0, 0, HID_REPORT_TYPE_OUTPUT, &led_report, sizeof(led_report));
}

void usb_keyboard_added(uint8_t dev_addr, uint8_t instance) {
    for (int i = 0; i < MAX_KEYBOARDS; i++) {
        if (!attached_keyboards[i].attached) {
            // Add new keyboard to our list of attached keyboards.
            attached_keyboards[i].dev_addr = dev_addr;
            attached_keyboards[i].instance = instance;
            attached_keyboards[i].attached = true;

            // Initialize LEDs for the newly attached keyboard.
            sync_leds(dev_addr);

            return;
        }
    }

    fatal("Max supported USB keyboards is %d.", MAX_KEYBOARDS);
}

void usb_keyboard_removed(uint8_t dev_addr, uint8_t instance) {
    for (int i = 0; i < MAX_KEYBOARDS; i++) {
        if (attached_keyboards[i].attached
            && attached_keyboards[i].dev_addr == dev_addr
            && attached_keyboards[i].instance == instance
        ) {
            // Remove keyboard from our list of attached keyboards.
            attached_keyboards[i].attached = false;
            return;
        }
    }

    fatal("USB (addr=%d, instance=%d) not found on removal.", dev_addr, instance);
}

static void usb_keyboard_sync(system_state_t* system_state) {
    // Get the PET keyboard model (graphics or business) as determined by the config
    // DIP switch on the board.
    const pet_keyboard_model_t pet_model = system_state->pet_keyboard_model;

    // Determine the active USB keyboard model (graphics or business), taking into
    // account the 'usb_swap_keyboard_model' setting.
    active_keyboard_model = swap_keyboard_model
        ? pet_model == pet_keyboard_model_graphics      // Swap models
            ? pet_keyboard_model_business
            : pet_keyboard_model_graphics
        : pet_model;                                    // No swap

    // Get the USB keymap kind (symbolic or positional).
    active_keymap_kind = use_symbolic_keymap
        ? usb_keymap_kind_symbolic
        : usb_keymap_kind_positional;

    // Update the current s_keymap pointer to point to the currently active model/kind.
    s_keymap = system_state->usb_keymap_data[active_keyboard_model][active_keymap_kind];

    // Synchronize the keyboard LEDs to match the currently active state.
    for (int i = 0; i < MAX_KEYBOARDS; i++) {
        if (attached_keyboards[i].attached) {
            sync_leds(attached_keyboards[i].dev_addr);
        }
    }
}

void usb_keyboard_reset(system_state_t* system_state) {
    use_symbolic_keymap = true;
    swap_keyboard_model = false;
    caps_lock_enabled = false;

    usb_keyboard_sync(system_state);
}

static uint8_t get_modifiers(KeyEvent keyEvent) {
    uint8_t modifiers = keyEvent.modifiers;

    if (caps_lock_enabled) {
        modifiers |= KEYBOARD_MODIFIER_LEFTSHIFT;
    }

    return modifiers;
}

// Generic lock toggle function for shift lock, num lock, and scroll lock
static void toggle_lock(const KeyEvent* const keyEvent, bool* lock_flag, const char* lock_name) {
    if (keyEvent->pressed) {
        *lock_flag = !(*lock_flag);
        log_debug("USB: %s %s", lock_name, *lock_flag ? "enabled" : "disabled");
        usb_keyboard_sync(&system_state);
    }
}

void dispatch_key_events() {
    static uint8_t active_modifiers = 0;

    KeyEvent keyEvent;

    if (!peek_key_event(&keyEvent)) {
        return;
    }

    {
        // Synchronize modifier keys in the PET keyboard matrix with the USB HID keyboard state.
        uint8_t new_modifiers = get_modifiers(keyEvent);

        if (new_modifiers != active_modifiers) {
            uint8_t old_modifiers = active_modifiers;
            active_modifiers = new_modifiers;

            for (uint8_t i = 0; i < 8; i++) {
                uint8_t new_mod = new_modifiers & 0x01;
                uint8_t old_mod = old_modifiers & 0x01;

                if (new_mod) {
                    if (!old_mod) {
                        modifier_down(i);
                    }
                } else if (old_mod) {
                    modifier_up(i);
                }

                new_modifiers >>= 1;
                old_modifiers >>= 1;
            }
        }
    }

    do {
        switch (keyEvent.key) {
            case HID_KEY_CAPS_LOCK:
                toggle_lock(&keyEvent, &caps_lock_enabled, "Caps Lock");
                break;
            case HID_KEY_NUM_LOCK:
                toggle_lock(&keyEvent, &swap_keyboard_model, "Num Lock");
                break;
            case HID_KEY_SCROLL_LOCK:
                toggle_lock(&keyEvent, &use_symbolic_keymap, "Scroll Lock");
                break;
            default:
                if (keyEvent.pressed) {
                    key_down(keyEvent.key, keyEvent.row, keyEvent.col);
                } else {
                    key_up(keyEvent.key, keyEvent.row, keyEvent.col);
                }
                break;
        }

        dequeue_key_event();

        // Continue updating the PET keyboard matrix as long as there are more events queued
        // and the modifiers are consistent.
    } while (peek_key_event(&keyEvent) && (get_modifiers(keyEvent) == active_modifiers));
}

static bool find_key_in_report(hid_keyboard_report_t const* report, uint8_t keycode) {
    for (uint8_t i = 0; i < sizeof(report->keycode); i++) {
        if (report->keycode[i] == keycode) {
            return true;
        }
    }

    return false;
}

void process_kbd_report(uint8_t dev_addr, hid_keyboard_report_t const* report) {
    (void) dev_addr;

    static hid_keyboard_report_t prev_report = {
        .modifier = 0,
        .reserved = 0,
        .keycode = { 0, 0, 0, 0, 0, 0 },
    };

    const uint8_t modifiers = report->modifier;

    // For each previously pressed key, check if the key still exists in the
    // new report.  If not, the key has been released.
    for (uint8_t i = 0; i < sizeof(prev_report.keycode); i++) {
        const uint8_t keycode = prev_report.keycode[i];
        if (keycode && !find_key_in_report(report, keycode)) {
            usb_keyboard_enqueue_key_up(keycode, modifiers);
        }
    }

    // For each key in the new report, check if the key exists in the new report.
    // If not, the key has been pressed.
    for (uint8_t i = 0; i < sizeof(report->keycode); i++) {
        const uint8_t keycode = report->keycode[i];
        if (keycode && !find_key_in_report(&prev_report, keycode)) {
            usb_keyboard_enqueue_key_down(keycode, modifiers);
        }
    }

    prev_report = *report;
}

int keyboard_getch() {
    // If the USB keyboard matrix has pressed keys it takes precedence.  Otherwise, we fall
    // back to the PET keyboard matrix.  This is the same logic used by the FPGA when determining
    // when to intercept the reads from $E812.
    bool use_usb_keys = false;
    
    for (uint8_t i = 0; i < KEY_COL_COUNT; i++) {
        use_usb_keys |= (usb_key_matrix[i] != 0xff);
    }

    return keyscan_getch(use_usb_keys
        ? usb_key_matrix
        : pet_key_matrix);
}
