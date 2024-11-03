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

#include "keyboard.h"

typedef struct __attribute__((packed)) {
    // First byte contains row/col packed as nibbles
    unsigned int row: 4;        // Rows   : 0-7 (F = undefined key mapping)
    unsigned int col: 4;        // Column : 0-9

    // Second byte contains flags
    unsigned int reserved: 6;
    unsigned int deshift: 1;    // 0 = Normal, 1 = Implicitly remove shift
    unsigned int shift: 1;      // 0 = Normal, 1 = Implicitly add shift
} KeyInfo;

typedef struct {
    uint8_t key;
    bool pressed;
} KeyEvent;

#define KEY_BUFFER_LOG2_CAPACITY 4
#define KEY_BUFFER_CAPACITY (1 << KEY_BUFFER_LOG2_CAPACITY)
#define KEY_BUFFER_CAPACITY_MASK (KEY_BUFFER_CAPACITY - 1)

static KeyEvent key_buffer[KEY_BUFFER_CAPACITY];
static uint8_t key_buffer_head = 0;
static uint8_t key_buffer_tail = 0;

static void enqueue_key_event(KeyEvent keyEvent) {
    uint8_t newHead = (key_buffer_head + 1) & KEY_BUFFER_CAPACITY_MASK;
    if (newHead != key_buffer_tail) {
        key_buffer[key_buffer_head] = keyEvent;
        key_buffer_head = newHead;
    }
}

static void enqueue_key_up(uint8_t keycode) {
    KeyEvent keyEvent = {
        .key = keycode,
        .pressed = false,
    };
    enqueue_key_event(keyEvent);
}

static void enqueue_key_down(uint8_t keycode) {
    KeyEvent keyEvent = {
        .key = keycode,
        .pressed = true,
    };
    enqueue_key_event(keyEvent);
}

static bool dequeue_key_event(KeyEvent* keyEvent) {
    if (key_buffer_head == key_buffer_tail) {
        return false;
    } else {
        *keyEvent = key_buffer[key_buffer_tail];
        key_buffer_tail = (key_buffer_tail + 1) & KEY_BUFFER_CAPACITY_MASK;
        return true;
    }
}

static const uint8_t keymap_raw[] = {
    #include "../keymaps/grus_pos.h"
};

static const KeyInfo* s_keymap = (KeyInfo*)((void*)keymap_raw);

uint8_t key_matrix[KEY_COL_COUNT] = {
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

static bool find_key_in_report(hid_keyboard_report_t const* report, uint8_t keycode) {
    for (uint8_t i = 0; i < 6; i++) {
        if (report->keycode[i] == keycode) {
            return true;
        }
    }

    return false;
}

void key_down(uint8_t keycode) {
    KeyInfo const key = s_keymap[keycode];
    uint8_t row = key.row;
    uint8_t rowMask = 1 << row;

    if (!rowMask) {
        printf("USB: Key down %d=(undefined)\n", keycode);
        return;
    }

    uint8_t col = key.col;

    if (key_matrix[col] & rowMask) {
        key_matrix[col] &= ~rowMask;
        printf("USB: Key down: %d=(%d,%d)\n", keycode, ffs(rowMask) - 1, col);
    }
}

void key_up(uint8_t keycode) {
    KeyInfo const key = s_keymap[keycode];
    uint8_t row = key.row;
    uint8_t rowMask = 1 << row;

    if (!rowMask) {
        printf("USB: Key up %d=(undefined)\n", keycode);
        return;
    }

    uint8_t col = key.col;

    if (key_matrix[col] & ~rowMask) {
        key_matrix[col] |= rowMask;
        printf("USB: Key up: %d=(%d,%d)\n", keycode, ffs(rowMask) - 1, col);
    }
}

void dispatch_key_events() {
    KeyEvent keyEvent;
    while (dequeue_key_event(&keyEvent)) {
        if (keyEvent.pressed) {
            key_down(keyEvent.key);
        } else {
            key_up(keyEvent.key);
        }
    }
}

void process_kbd_report(hid_keyboard_report_t const* report) {
    static hid_keyboard_report_t prev_report = {0, 0, {0}};

    uint8_t current_modifiers = report->modifier;
    uint8_t previous_modifiers = prev_report.modifier;

    for (uint8_t i = 0; i < 8; i++) {
        uint8_t current_modifier = current_modifiers & 0x01;
        uint8_t previous_modifier = previous_modifiers & 0x01;

        if (current_modifier) {
            if (!previous_modifier) {
                enqueue_key_down(HID_KEY_CONTROL_LEFT + i);
            }
        } else if (previous_modifier) {
            enqueue_key_up(HID_KEY_CONTROL_LEFT + i);
        }

        current_modifiers >>= 1;
        previous_modifiers >>= 1;
    }

    for (uint8_t i = 0; i < 6; i++) {
        uint8_t keycode = prev_report.keycode[i];
        if (keycode && !find_key_in_report(report, keycode)) {
            enqueue_key_up(keycode);
        }
    }

    for (uint8_t i = 0; i < 6; i++) {
        uint8_t keycode = report->keycode[i];
        if (keycode && !find_key_in_report(&prev_report, keycode)) {
            enqueue_key_down(keycode);
        }
    }

    prev_report = *report;
}
