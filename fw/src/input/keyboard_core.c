// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "input/keyboard_core.h"

#include <limits.h>

#include "diag/log/log.h"
#include "fatal.h"

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

// All injected producers share the configured symbolic or positional HID-to-PET map.
static bool use_symbolic_keymap = true;

// If true, the USB keyboard model is swapped (graphics <-> business) with
// respect to the PET keyboard.
static bool swap_keyboard_model = false;

// Caps Lock applies to the combined injected keyboard.
static bool caps_lock_enabled = false;

// The currently active USB keyboard model (graphics or business).
static pet_keyboard_model_t active_keyboard_model = pet_keyboard_model_graphics;
static usb_keymap_kind_t active_keymap_kind = usb_keymap_kind_symbolic;
static const usb_keymap_entry_t* s_keymap = system_state.usb_keymap_data[pet_keyboard_model_graphics][usb_keymap_kind_symbolic];

typedef struct {
    uint8_t key;            // Shared HID key vocabulary, independent of report transport.
    uint8_t row;            // PET keyboard row
    uint8_t col;            // PET keyboard column
    uint8_t modifiers;      // Shared HID modifier flags (e.g. shift).
    bool pressed;           // true = key pressed, false = key released
    bool modifier_only;     // Update PET modifiers without changing a key.
} KeyEvent;

#define KEY_BUFFER_LOG2_CAPACITY 4
#define KEY_BUFFER_CAPACITY (1 << KEY_BUFFER_LOG2_CAPACITY)
#define KEY_BUFFER_CAPACITY_MASK (KEY_BUFFER_CAPACITY - 1)
#define KEYCODE_COUNT (UINT8_MAX + 1u)

typedef enum {
    keymap_override_none,
    keymap_override_shift,
    keymap_override_unshift,
} keymap_override_t;

static KeyEvent key_buffer[KEY_BUFFER_CAPACITY];
static uint8_t key_buffer_next_write_index = 0;
static uint8_t key_buffer_next_read_index = 0;
// Tracks raw HID modifiers; key events may carry separate keymap overrides.
static uint8_t queued_raw_modifiers;
static keymap_override_t keymap_overrides[KEYCODE_COUNT];
static uint8_t active_keymap_override_keys[KEYCODE_COUNT];
static uint16_t active_keymap_override_count;
static bool pending_led_sync;

static uint8_t active_modifiers;
static uint16_t matrix_owners[KEY_COL_COUNT][KEY_ROW_COUNT];
static usb_keymap_entry_t held_mapping[KEYCODE_COUNT];

// Keeps the most recently pressed held-key override active until that key is released.
static void set_keymap_override(uint8_t keycode, keymap_override_t override) {
    for (size_t i = 0; i < active_keymap_override_count; ++i) {
        if (active_keymap_override_keys[i] == keycode) {
            for (size_t j = i + 1; j < active_keymap_override_count; ++j)
                active_keymap_override_keys[j - 1] = active_keymap_override_keys[j];
            --active_keymap_override_count;
            break;
        }
    }

    keymap_overrides[keycode] = override;
    if (override != keymap_override_none)
        active_keymap_override_keys[active_keymap_override_count++] = keycode;
}

// Applies the latest held-key override to raw HID modifiers.
static uint8_t apply_keymap_override(uint8_t modifiers) {
    if (active_keymap_override_count == 0)
        return modifiers;

    const uint8_t keycode = active_keymap_override_keys[active_keymap_override_count - 1];
    if (keymap_overrides[keycode] == keymap_override_shift)
        modifiers |= KEYBOARD_MODIFIER_LEFTSHIFT;
    else if (keymap_overrides[keycode] == keymap_override_unshift)
        modifiers &= ~(KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
    return modifiers;
}

// Eight USB interfaces plus the independently paced terminal producer.
#define MAX_KEYBOARD_SOURCES 9

typedef struct {
    input_source_t source;
    bool used;
    uint8_t injected_modifiers;
    uint8_t injected_keys[KEYCODE_COUNT / CHAR_BIT];
} keyboard_source_t;

static keyboard_source_t keyboard_sources[MAX_KEYBOARD_SOURCES];

// Find runtime ownership without depending on a producer's report representation.
static keyboard_source_t* find_keyboard_source(input_source_t source) {
    for (int i = 0; i < MAX_KEYBOARD_SOURCES; i++) {
        keyboard_source_t* const keyboard = &keyboard_sources[i];
        if (keyboard->used && keyboard->source.device == source.device
            && keyboard->source.instance == source.instance)
            return keyboard;
    }

    return NULL;
}

// Allocate independent runtime ownership for a decoded input producer.
static keyboard_source_t* get_keyboard_source(input_source_t source) {
    keyboard_source_t* keyboard = find_keyboard_source(source);
    if (keyboard)
        return keyboard;

    for (int i = 0; i < MAX_KEYBOARD_SOURCES; i++) {
        keyboard = &keyboard_sources[i];
        if (!keyboard->used) {
            *keyboard = (keyboard_source_t) {
                .source = source,
                .used = true,
            };
            return keyboard;
        }
    }

    fatal("Max supported keyboard sources is %d.", MAX_KEYBOARD_SOURCES);
}

// Dispatches queued events when needed instead of dropping a key transition.
static void ensure_key_buffer_space(void) {
    while (((key_buffer_next_write_index + 1) & KEY_BUFFER_CAPACITY_MASK) == key_buffer_next_read_index) {
        if (keyboard_core_dispatch())
            pending_led_sync = true;
    }
}

/** Queue a mapped press or saved-location release for modifier-group dispatch. */
static void enqueue_key_event(uint8_t keycode, uint8_t row, uint8_t col, uint8_t modifiers, bool pressed) {
    ensure_key_buffer_space();
    key_buffer[key_buffer_next_write_index] = (KeyEvent) {
        .key = keycode,
        .row = row,
        .col = col,
        .modifiers = modifiers,
        .pressed = pressed,
        .modifier_only = false,
    };
    key_buffer_next_write_index = (key_buffer_next_write_index + 1) & KEY_BUFFER_CAPACITY_MASK;
}

// Queue a modifier transition even when a producer has no key edges.
static void enqueue_modifier_event(uint8_t modifiers) {
    ensure_key_buffer_space();
    key_buffer[key_buffer_next_write_index] = (KeyEvent) {
        .modifiers = apply_keymap_override(modifiers),
        .modifier_only = true,
    };
    key_buffer_next_write_index = (key_buffer_next_write_index + 1) & KEY_BUFFER_CAPACITY_MASK;
    queued_raw_modifiers = modifiers;
}

// Combine modifiers held across all runtime input producers.
static uint8_t combined_modifiers(void) {
    uint8_t modifiers = 0;
    for (int i = 0; i < MAX_KEYBOARD_SOURCES; ++i) {
        if (keyboard_sources[i].used)
            modifiers |= keyboard_sources[i].injected_modifiers;
    }
    return modifiers;
}

// Queue a modifier-only update when the combined raw state changes.
static void update_queued_modifiers(uint8_t modifiers) {
    if (modifiers != queued_raw_modifiers)
        enqueue_modifier_event(modifiers);
}

/** Inspect the next mapped event without consuming it. */
static bool peek_key_event(KeyEvent* keyEvent) {
    *keyEvent = key_buffer[key_buffer_next_read_index];
    return key_buffer_next_write_index != key_buffer_next_read_index;
}

/** Consume the current event after its matrix effects have been applied. */
static void dequeue_key_event(void) {
    assert(key_buffer_next_write_index != key_buffer_next_read_index);
    key_buffer_next_read_index = (key_buffer_next_read_index + 1) & KEY_BUFFER_CAPACITY_MASK;
}

/** Release the original mapped location after the last producer relinquishes a key. */
static void enqueue_key_up(uint8_t keycode, uint8_t modifiers) {
    const usb_keymap_entry_t keyInfo = held_mapping[keycode];

    const uint8_t row = keyInfo.row;
    const uint8_t col = keyInfo.col;

    set_keymap_override(keycode, keymap_override_none);
    enqueue_key_event(keycode, row, col, apply_keymap_override(modifiers), /* pressed: */ false);
}

/** Save the first owner's effective mapping and queue its press. */
static void enqueue_key_down(uint8_t keycode, uint8_t modifiers) {
    bool is_shifted = (modifiers & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) != 0;

    const usb_keymap_entry_t keyInfo = s_keymap[
        is_shifted
            ? keycode | 0x0100      // s_keymap[0x0000..0x00ff] = shifted keymap
            : keycode];             // s_keymap[0x0100..0x01ff] = unshifted keymap

    const uint8_t row = keyInfo.row;
    const uint8_t col = keyInfo.col;
    held_mapping[keycode] = keyInfo;

    set_keymap_override(keycode, keyInfo.unshift
        ? keymap_override_unshift
        : keyInfo.shift ? keymap_override_shift : keymap_override_none);

    enqueue_key_event(keycode, row, col, apply_keymap_override(modifiers), /* pressed: */ true);
}

/** Press a mapped cell only when its first logical key becomes active. */
static void key_down(uint8_t keycode, uint8_t row, uint8_t col) {
    if (row >= KEY_ROW_COUNT || col >= KEY_COL_COUNT) {
        log_debug("Keyboard: Key down: %d=(undefined)", keycode);
        return;
    }

    uint8_t rowMask = 1 << row;

    if (matrix_owners[col][row]++ == 0) {
        usb_key_matrix[col] &= ~rowMask;
        log_debug("Keyboard: Key down: %d=(%d,%d)", keycode, row, col);
    }
}

/** Release a mapped cell only when its final logical key becomes inactive. */
static void key_up(uint8_t keycode, uint8_t row, uint8_t col) {
    if (row >= KEY_ROW_COUNT || col >= KEY_COL_COUNT) {
        log_debug("Keyboard: Key up: %d=(undefined)", keycode);
        return;
    }

    uint8_t rowMask = 1 << row;

    assert(matrix_owners[col][row] != 0);
    if (--matrix_owners[col][row] == 0) {
        usb_key_matrix[col] |= rowMask;
        log_debug("Keyboard: Key up: %d=(%d,%d)", keycode, row, col);
    }
}

static const char* const modifier_names[] = {
    "CONTROL_LEFT",
    "SHIFT_LEFT",
    "ALT_LEFT",
    "GUI_LEFT",
    "CONTROL_RIGHT",
    "SHIFT_RIGHT",
    "ALT_RIGHT",
    "GUI_RIGHT",
};

/** Save a modifier's mapping when its combined state first becomes active. */
static void modifier_down(uint8_t modifier_offset) {
    log_debug("Keyboard: Modifier down: %s", modifier_names[modifier_offset]);
    uint8_t keycode = HID_KEY_CONTROL_LEFT + modifier_offset;
    usb_keymap_entry_t keyInfo = s_keymap[keycode];
    held_mapping[keycode] = keyInfo;
    key_down(keycode, keyInfo.row, keyInfo.col);
}

/** Release a modifier using its saved mapping, even after mapping settings change. */
static void modifier_up(uint8_t modifier_offset) {
    log_debug("Keyboard: Modifier up: %s", modifier_names[modifier_offset]);
    uint8_t keycode = HID_KEY_CONTROL_LEFT + modifier_offset;
    const usb_keymap_entry_t keyInfo = held_mapping[keycode];
    key_up(keycode, keyInfo.row, keyInfo.col);
}

static bool key_owned_elsewhere(const keyboard_source_t* source, uint8_t keycode);

// Tracks keys routed to the PET matrix independently of menu-only key holds.
static bool key_owned(const keyboard_source_t* keyboard, uint8_t keycode) {
    return (keyboard->injected_keys[keycode / CHAR_BIT] & (1u << (keycode % CHAR_BIT))) != 0;
}

// Updates ownership for one physical key press or release.
static void set_key_owned(keyboard_source_t* keyboard, uint8_t keycode, bool owned) {
    uint8_t* const byte = &keyboard->injected_keys[keycode / CHAR_BIT];
    const uint8_t mask = 1u << (keycode % CHAR_BIT);
    if (owned)
        *byte |= mask;
    else
        *byte &= (uint8_t) ~mask;
}

void keyboard_core_cancel(input_source_t source) {
    keyboard_source_t* const keyboard = find_keyboard_source(source);
    if (keyboard == NULL)
        return;
    keyboard->injected_modifiers = 0;
    const uint8_t modifiers = combined_modifiers();

    for (unsigned int keycode = 1; keycode < KEYCODE_COUNT; ++keycode) {
        if (key_owned(keyboard, keycode)) {
            set_key_owned(keyboard, keycode, false);
            if (!key_owned_elsewhere(keyboard, keycode))
                enqueue_key_up(keycode, modifiers);
        }
    }

    // Apply held-key overrides before exposing a raw modifier-only transition.
    update_queued_modifiers(modifiers);
    *keyboard = (keyboard_source_t) { 0 };
}

/** Select the configured PET map after model or mapping-kind changes. */
static void keyboard_core_sync(system_state_t* system_state) {
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
}

void keyboard_core_reset(system_state_t* system_state) {
    use_symbolic_keymap = true;
    swap_keyboard_model = false;
    caps_lock_enabled = false;
    pending_led_sync = true;
    keyboard_core_sync(system_state);
}

bool keyboard_core_caps_lock(void) { return caps_lock_enabled; }
pet_keyboard_model_t keyboard_core_model(void) { return active_keyboard_model; }
usb_keymap_kind_t keyboard_core_keymap_kind(void) { return active_keymap_kind; }

/** Apply Caps Lock to a queued event's arbitrated modifiers. */
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
        log_debug("Keyboard: %s %s", lock_name, *lock_flag ? "enabled" : "disabled");
        keyboard_core_sync(&system_state);
    }
}

bool keyboard_core_dispatch(void) {
    KeyEvent keyEvent;

    if (!peek_key_event(&keyEvent)) {
        const bool changed = pending_led_sync;
        pending_led_sync = false;
        return changed;
    }
    bool keymap_changed = pending_led_sync;
    pending_led_sync = false;

    {
        // Synchronize matrix modifiers with the combined source-owned state.
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
        if (!keyEvent.modifier_only) {
            switch (keyEvent.key) {
                case HID_KEY_CAPS_LOCK:
                    toggle_lock(&keyEvent, &caps_lock_enabled, "Caps Lock");
                    keymap_changed |= keyEvent.pressed;
                    break;
                case HID_KEY_NUM_LOCK:
                    toggle_lock(&keyEvent, &swap_keyboard_model, "Num Lock");
                    keymap_changed |= keyEvent.pressed;
                    break;
                case HID_KEY_SCROLL_LOCK:
                    toggle_lock(&keyEvent, &use_symbolic_keymap, "Scroll Lock");
                    keymap_changed |= keyEvent.pressed;
                    break;
                default:
                    if (keyEvent.pressed) {
                        key_down(keyEvent.key, keyEvent.row, keyEvent.col);
                    } else {
                        key_up(keyEvent.key, keyEvent.row, keyEvent.col);
                    }
                    break;
            }
        }

        dequeue_key_event();

        // Continue updating the PET keyboard matrix as long as there are more events queued
        // and the modifiers are consistent.
    } while (peek_key_event(&keyEvent) && (get_modifiers(keyEvent) == active_modifiers));
    return keymap_changed;
}

// Retain a shared key until its last producer releases it.
static bool key_owned_elsewhere(const keyboard_source_t* source, uint8_t keycode) {
    for (int i = 0; i < MAX_KEYBOARD_SOURCES; ++i) {
        const keyboard_source_t* const keyboard = &keyboard_sources[i];
        if (keyboard != source && keyboard->used
            && key_owned(keyboard, keycode))
            return true;
    }
    return false;
}

/** Update raw ownership before applying the batch's keymap overrides. */
static void update_source_modifiers(input_source_t source, uint8_t pressed, uint8_t released) {
    keyboard_source_t* keyboard = find_keyboard_source(source);
    if (keyboard == NULL && pressed == 0)
        return;
    if (keyboard == NULL)
        keyboard = get_keyboard_source(source);
    keyboard->injected_modifiers = (keyboard->injected_modifiers & (uint8_t)~released) | pressed;
}

/** Apply a single already-ordered edge under independent producer ownership. */
static void update_source_key(input_source_t source, uint8_t keycode, bool pressed) {
    keyboard_source_t* keyboard = find_keyboard_source(source);
    if (!pressed && (keyboard == NULL || !key_owned(keyboard, keycode)))
        return;
    if (keyboard == NULL)
        keyboard = get_keyboard_source(source);
    const uint8_t modifiers = combined_modifiers();
    if (pressed) {
        if (key_owned(keyboard, keycode))
            return;
        if (!key_owned_elsewhere(keyboard, keycode))
            enqueue_key_down(keycode, modifiers);
        set_key_owned(keyboard, keycode, true);
    } else {
        set_key_owned(keyboard, keycode, false);
        if (!key_owned_elsewhere(keyboard, keycode))
            enqueue_key_up(keycode, modifiers);
    }
}

void keyboard_core_submit(const keyboard_batch_t* batch) {
    vet(batch != NULL && (batch->edge_count == 0 || batch->edges != NULL),
        "invalid keyboard batch");
    update_source_modifiers(batch->source, batch->modifiers_pressed, batch->modifiers_released);

    // Release saved mappings before establishing this batch's new press overrides.
    for (size_t i = 0; i < batch->edge_count; ++i)
        if (!batch->edges[i].pressed)
            update_source_key(batch->source, batch->edges[i].keycode, false);
    for (size_t i = 0; i < batch->edge_count; ++i)
        if (batch->edges[i].pressed)
            update_source_key(batch->source, batch->edges[i].keycode, true);

    // Mapping overrides must precede any raw modifier-only transition.
    update_queued_modifiers(combined_modifiers());
}

void keyboard_core_cancel_all(void) {
    // Raw physical holds stay in the adapters, so cancellation never replays them.
    key_buffer_next_read_index = key_buffer_next_write_index = 0;
    active_modifiers = queued_raw_modifiers = 0;
    memset(matrix_owners, 0, sizeof(matrix_owners));
    memset(usb_key_matrix, 0xff, sizeof(usb_key_matrix));
    memset(keymap_overrides, 0, sizeof(keymap_overrides));
    active_keymap_override_count = 0;
    memset(keyboard_sources, 0, sizeof(keyboard_sources));
}
