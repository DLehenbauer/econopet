// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "input/keyboard_core.h"

#include <limits.h>

#include "diag/log/log.h"
#include "fatal.h"

uint8_t usb_key_matrix[ECONOPET_KBD_COL_COUNT] = {
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
    /* 10 */ 0xff,
    /* 11 */ 0xff,
    /* 12 */ 0xff,
    /* 13 */ 0xff,
    /* 14 */ 0xff,
    /* 15 */ 0xff,
};

uint8_t pet_key_matrix[ECONOPET_KBD_COL_COUNT] = {
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
    /* 10 */ 0xff,
    /* 11 */ 0xff,
    /* 12 */ 0xff,
    /* 13 */ 0xff,
    /* 14 */ 0xff,
    /* 15 */ 0xff,
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

// Eight USB interfaces plus the independently paced terminal producer.
#define MAX_KEYBOARD_SOURCES 9

typedef enum {
    keymap_override_none,
    keymap_override_shift,
    keymap_override_unshift,
} keymap_override_t;

typedef struct {
    uint8_t source;
    uint8_t key;            // Shared HID key vocabulary, independent of report transport.
    uint8_t source_modifiers[MAX_KEYBOARD_SOURCES];
    bool pressed;           // true = key pressed, false = key released
    bool modifier_only;     // Update PET modifiers without changing a key.
} KeyEvent;

#define KEY_BUFFER_LOG2_CAPACITY 4
#define KEY_BUFFER_CAPACITY (1 << KEY_BUFFER_LOG2_CAPACITY)
#define KEY_BUFFER_CAPACITY_MASK (KEY_BUFFER_CAPACITY - 1)
#define KEYCODE_COUNT (UINT8_MAX + 1u)

static KeyEvent key_buffer[KEY_BUFFER_CAPACITY];
static uint8_t key_buffer_next_write_index = 0;
static uint8_t key_buffer_next_read_index = 0;
static keymap_override_t keymap_overrides[KEYCODE_COUNT];
static uint8_t active_keymap_override_keys[KEYCODE_COUNT];
static uint16_t active_keymap_override_count;
static bool pending_led_sync;

static uint8_t active_modifiers;
static uint16_t matrix_owners[ECONOPET_KBD_COL_COUNT][ECONOPET_KBD_ROW_COUNT];
static usb_keymap_entry_t modifier_mapping[CHAR_BIT];
static usb_keymap_entry_t dispatched_mapping[KEYCODE_COUNT];

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

typedef struct {
    input_source_t source;
    bool used;
    uint8_t injected_modifiers;
    uint8_t injected_keys[KEYCODE_COUNT / CHAR_BIT];
    uint8_t dispatched_modifiers;
    uint8_t dispatched_keys[KEYCODE_COUNT / CHAR_BIT];
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

/** Retain producer identity and raw modifier observations until mapped delivery. */
static void enqueue_key_event(const keyboard_source_t* source, KeyEvent event) {
    ensure_key_buffer_space();
    event.source = (uint8_t)(source - keyboard_sources);
    for (size_t i = 0; i < MAX_KEYBOARD_SOURCES; ++i)
        event.source_modifiers[i] = keyboard_sources[i].injected_modifiers;
    key_buffer[key_buffer_next_write_index] = event;
    key_buffer_next_write_index = (key_buffer_next_write_index + 1) & KEY_BUFFER_CAPACITY_MASK;
}

// Queue a modifier transition even when a producer has no key edges.
static void enqueue_modifier_event(const keyboard_source_t* source) {
    enqueue_key_event(source, (KeyEvent){.modifier_only = true});
}

/** Combine the raw modifier observations retained with one pending edge. */
static uint8_t event_modifiers(const KeyEvent* event) {
    uint8_t modifiers = 0;
    for (size_t i = 0; i < MAX_KEYBOARD_SOURCES; ++i)
        modifiers |= event->source_modifiers[i];
    return modifiers;
}

/** Resolve a new logical press after preceding mapping-lock transitions. */
static usb_keymap_entry_t resolve_key_mapping(const KeyEvent* event) {
    const bool shifted = (event_modifiers(event)
        & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) != 0;
    return s_keymap[shifted ? event->key | KEYCODE_COUNT : event->key];
}

/** Translate a resolved symbolic mapping's forced modifier policy. */
static keymap_override_t mapping_override(usb_keymap_entry_t mapping) {
    return mapping.unshift ? keymap_override_unshift
        : mapping.shift ? keymap_override_shift : keymap_override_none;
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

/** Press a mapped cell only when its first logical key becomes active. */
static void key_down(uint8_t keycode, uint8_t row, uint8_t col) {
    if (row >= ECONOPET_KBD_ROW_COUNT || col >= ECONOPET_KBD_COL_COUNT) {
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
    if (row >= ECONOPET_KBD_ROW_COUNT || col >= ECONOPET_KBD_COL_COUNT) {
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
    modifier_mapping[modifier_offset] = keyInfo;
    key_down(keycode, keyInfo.row, keyInfo.col);
}

/** Release a modifier using its saved mapping, even after mapping settings change. */
static void modifier_up(uint8_t modifier_offset) {
    log_debug("Keyboard: Modifier up: %s", modifier_names[modifier_offset]);
    uint8_t keycode = HID_KEY_CONTROL_LEFT + modifier_offset;
    const usb_keymap_entry_t keyInfo = modifier_mapping[modifier_offset];
    key_up(keycode, keyInfo.row, keyInfo.col);
}

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

/** Observe delivered ownership independently of still-pending producer edges. */
static bool key_dispatched(const keyboard_source_t* source, uint8_t keycode) {
    return (source->dispatched_keys[keycode / CHAR_BIT] & (1u << (keycode % CHAR_BIT))) != 0;
}

/** Retain a delivered logical key until every published producer releases it. */
static bool key_dispatched_elsewhere(const keyboard_source_t* source, uint8_t keycode) {
    for (size_t i = 0; i < MAX_KEYBOARD_SOURCES; ++i)
        if (&keyboard_sources[i] != source && key_dispatched(&keyboard_sources[i], keycode))
            return true;
    return false;
}

/** Apply the selected modifier group using saved modifier mappings on release. */
static void publish_modifiers(uint8_t modifiers) {
    const uint8_t changed = modifiers ^ active_modifiers;
    active_modifiers = modifiers;
    for (uint8_t i = 0; i < CHAR_BIT; ++i) {
        const uint8_t mask = 1u << i;
        if (changed & mask) {
            if (modifiers & mask)
                modifier_down(i);
            else
                modifier_up(i);
        }
    }
}

void keyboard_core_cancel(input_source_t source) {
    keyboard_source_t* const keyboard = find_keyboard_source(source);
    if (keyboard == NULL)
        return;
    const uint8_t source_index = (uint8_t)(keyboard - keyboard_sources);

    // Remove only this producer's pending edges and modifier contributions.
    uint8_t write = key_buffer_next_read_index;
    for (uint8_t read = write; read != key_buffer_next_write_index;
         read = (read + 1) & KEY_BUFFER_CAPACITY_MASK) {
        KeyEvent event = key_buffer[read];
        if (event.source == source_index)
            continue;
        event.source_modifiers[source_index] = 0;
        key_buffer[write] = event;
        write = (write + 1) & KEY_BUFFER_CAPACITY_MASK;
    }
    key_buffer_next_write_index = write;

    // Retire delivered ownership without publishing any cancelled queued press.
    for (unsigned int keycode = 0; keycode < KEYCODE_COUNT; ++keycode) {
        if (key_dispatched(keyboard, keycode) && !key_dispatched_elsewhere(keyboard, keycode)) {
            if (keycode != HID_KEY_CAPS_LOCK && keycode != HID_KEY_NUM_LOCK
                && keycode != HID_KEY_SCROLL_LOCK) {
                const usb_keymap_entry_t mapping = dispatched_mapping[keycode];
                key_up(keycode, mapping.row, mapping.col);
            }
            set_keymap_override(keycode, keymap_override_none);
        }
    }
    *keyboard = (keyboard_source_t) { 0 };
    uint8_t modifiers = 0;
    for (size_t i = 0; i < MAX_KEYBOARD_SOURCES; ++i)
        modifiers |= keyboard_sources[i].dispatched_modifiers;
    modifiers = apply_keymap_override(modifiers);
    if (caps_lock_enabled)
        modifiers |= KEYBOARD_MODIFIER_LEFTSHIFT;
    publish_modifiers(modifiers);
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
    uint8_t modifiers = event_modifiers(&keyEvent);

    // Preview the next edge's override without mutating a group deferred to a later cycle.
    const keyboard_source_t* source = &keyboard_sources[keyEvent.source];
    const bool transition = !keyEvent.modifier_only
        && !key_dispatched_elsewhere(source, keyEvent.key)
        && key_dispatched(source, keyEvent.key) != keyEvent.pressed;
    keymap_override_t override = keymap_override_none;
    for (size_t i = active_keymap_override_count; i > 0; --i) {
        const uint8_t key = active_keymap_override_keys[i - 1];
        if (!transition || key != keyEvent.key) {
            override = keymap_overrides[key];
            break;
        }
    }
    if (transition && keyEvent.pressed) {
        const keymap_override_t next_override = mapping_override(resolve_key_mapping(&keyEvent));
        if (next_override != keymap_override_none)
            override = next_override;
    }
    if (override == keymap_override_shift)
        modifiers |= KEYBOARD_MODIFIER_LEFTSHIFT;
    else if (override == keymap_override_unshift)
        modifiers &= ~(KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);

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

    publish_modifiers(get_modifiers(keyEvent));

    do {
        for (size_t i = 0; i < MAX_KEYBOARD_SOURCES; ++i)
            keyboard_sources[i].dispatched_modifiers = keyEvent.source_modifiers[i];
        if (!keyEvent.modifier_only) {
            keyboard_source_t* source = &keyboard_sources[keyEvent.source];
            const bool owned = key_dispatched(source, keyEvent.key);
            const bool transition = owned != keyEvent.pressed
                && !key_dispatched_elsewhere(source, keyEvent.key);
            uint8_t* const byte = &source->dispatched_keys[keyEvent.key / CHAR_BIT];
            const uint8_t mask = 1u << (keyEvent.key % CHAR_BIT);
            if (keyEvent.pressed)
                *byte |= mask;
            else
                *byte &= (uint8_t)~mask;
            if (transition) {
                const usb_keymap_entry_t mapping = keyEvent.pressed
                    ? resolve_key_mapping(&keyEvent) : dispatched_mapping[keyEvent.key];
                set_keymap_override(keyEvent.key,
                    keyEvent.pressed ? mapping_override(mapping) : keymap_override_none);
                switch (keyEvent.key) {
                    case HID_KEY_CAPS_LOCK:
                        toggle_lock(&keyEvent, &caps_lock_enabled, "Caps Lock");
                        keymap_changed |= keyEvent.pressed;
                        publish_modifiers(get_modifiers(keyEvent));
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
                            dispatched_mapping[keyEvent.key] = mapping;
                            key_down(keyEvent.key, mapping.row, mapping.col);
                        } else {
                            key_up(keyEvent.key, mapping.row, mapping.col);
                        }
                        break;
                }
            }
        }

        dequeue_key_event();

        // Continue updating the PET keyboard matrix as long as there are more events queued
        // and the modifiers are consistent.
    } while (peek_key_event(&keyEvent) && (get_modifiers(keyEvent) == active_modifiers));
    return keymap_changed;
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
    if (key_owned(keyboard, keycode) == pressed)
        return;
    set_key_owned(keyboard, keycode, pressed);
    enqueue_key_event(keyboard, (KeyEvent){.key = keycode, .pressed = pressed});
}

void keyboard_core_submit(const keyboard_batch_t* batch) {
    vet(batch != NULL && (batch->edge_count == 0 || batch->edges != NULL),
        "invalid keyboard batch");
    const keyboard_source_t* const before = find_keyboard_source(batch->source);
    const uint8_t old_modifiers = before != NULL ? before->injected_modifiers : 0;
    update_source_modifiers(batch->source, batch->modifiers_pressed, batch->modifiers_released);

    // Release saved mappings before establishing this batch's new press overrides.
    for (size_t i = 0; i < batch->edge_count; ++i)
        if (!batch->edges[i].pressed)
            update_source_key(batch->source, batch->edges[i].keycode, false);
    for (size_t i = 0; i < batch->edge_count; ++i)
        if (batch->edges[i].pressed)
            update_source_key(batch->source, batch->edges[i].keycode, true);

    // Preserve modifier ownership changes even when another source holds the same raw bit.
    const keyboard_source_t* const source = find_keyboard_source(batch->source);
    if (source != NULL && source->injected_modifiers != old_modifiers)
        enqueue_modifier_event(source);
}

void keyboard_core_cancel_all(void) {
    // Raw physical holds stay in the adapters, so cancellation never replays them.
    key_buffer_next_read_index = key_buffer_next_write_index = 0;
    active_modifiers = 0;
    memset(matrix_owners, 0, sizeof(matrix_owners));
    memset(usb_key_matrix, 0xff, sizeof(usb_key_matrix));
    memset(keymap_overrides, 0, sizeof(keymap_overrides));
    active_keymap_override_count = 0;
    memset(keyboard_sources, 0, sizeof(keyboard_sources));
}
