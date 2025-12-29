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
#include "input.h"

#include "diag/log/log.h"
#include "driver.h"
#include "hw.h"
#include "system_state.h"
#include "term_inject.h"
#include "ui/cli.h"
#include "usb/keyboard.h"
#include "usb/usb.h"

// Ring buffer for firmware input queue
#define INPUT_BUFFER_LOG2_CAPACITY 4
#define INPUT_BUFFER_CAPACITY (1 << INPUT_BUFFER_LOG2_CAPACITY)
#define INPUT_BUFFER_CAPACITY_MASK (INPUT_BUFFER_CAPACITY - 1)

static int input_buffer[INPUT_BUFFER_CAPACITY];
static uint8_t input_buffer_head = 0;
static uint8_t input_buffer_tail = 0;

static void enqueue_key(int keycode) {
    uint8_t next_head = (input_buffer_head + 1) & INPUT_BUFFER_CAPACITY_MASK;
    if (next_head != input_buffer_tail) {
        input_buffer[input_buffer_head] = keycode;
        input_buffer_head = next_head;
    }
}

static int dequeue_key(void) {
    if (input_buffer_head == input_buffer_tail) {
        return EOF;
    }
    int keycode = input_buffer[input_buffer_tail];
    input_buffer_tail = (input_buffer_tail + 1) & INPUT_BUFFER_CAPACITY_MASK;
    return keycode;
}

// Read state of MENU button
static bool read_button_debounced(void) {
    static uint64_t last_change_time = 0;
    static bool raw_state = true;       // Last raw button state (initial: pressed for capacitor charging)
    static bool debounced_state = true; // Debounced button state
    
    const uint64_t DEBOUNCE_TIME_US = 50000;  // 50ms debounce time

    bool current_raw = !gpio_get(MENU_BTN_GP);  // Active low button
    uint64_t current_time = time_us_64();

    // Check if raw button state has changed, record the new state and time.
    if (current_raw != raw_state) {
        raw_state = current_raw;
        last_change_time = current_time;

        // Return previous debounced state during transition
        return debounced_state;
    }

    // Check if we're still in debounce period
    if ((current_time - last_change_time) < DEBOUNCE_TIME_US) {
        // Still debouncing, return previous debounced state
        return debounced_state;
    }

    // Button state is stable, update debounced state
    debounced_state = current_raw;
    return debounced_state;
}

// Returns KEY_BTN_SHORT, KEY_BTN_LONG, or EOF for no action
static int get_button_action(void) {
    // Track button press/release events and timing for short/long press detection
    static bool is_pressed = true;     // Initial state matches debounced initial state
    static bool was_handled = true;
    static uint64_t press_start;
    
    const uint64_t LONG_PRESS_TIME_US = 500000; // 500ms for long press

    bool was_pressed = is_pressed;
    is_pressed = read_button_debounced();
    
    uint64_t current_time = time_us_64();
    int action = EOF;

    if (!is_pressed) {
        // Button is not currently pressed.
        if (was_pressed && !was_handled) {
            // Button has just been released and did not previously exceed the
            // long-press threshold.
            action = KEY_BTN_SHORT;
            log_debug("MENU button: short press");
        }
    } else {
        // The button is currently pressed.
        if (!was_pressed) {
            // Button has just been pressed (after debounce).  Record the start time.
            press_start = current_time;
            was_handled = false;
        } else if (!was_handled && (current_time - press_start > LONG_PRESS_TIME_US)) {
            // Button has been held for more than 500ms.  Consider this a long press.
            was_handled = true;
            action = KEY_BTN_LONG;
            log_debug("MENU button: long press");
        }
    }

    return action;
}

// Map escape sequences to key constants. (ESC [ has already been matched.)
typedef struct {
    const char* sequence;
    int key;
} escape_sequence_t;

static const escape_sequence_t escape_sequences[] = {
    { "A",  KEY_UP },
    { "B",  KEY_DOWN },
    { "C",  KEY_RIGHT },
    { "D",  KEY_LEFT },
    { "F",  KEY_END },
    { "H",  KEY_HOME },
    { "5~", KEY_PGUP },
    { "6~", KEY_PGDN },
    { NULL, EOF }
};

static bool is_sequence_terminator(int ch) {
    return (64 <= ch && ch <= 126)
        || (0 <= ch && ch <= 31);
}

// Timeout for escape sequence completion (microseconds)
// VT100 escape sequences should complete within a few milliseconds at any baud rate
#define ESCAPE_SEQUENCE_TIMEOUT_US 10000

// Wait for UART to have data available, with timeout
static bool uart_wait_readable(uint64_t timeout_us) {
    uint64_t start = time_us_64();
    while (!uart_is_readable(uart_default)) {
        if (time_us_64() - start > timeout_us) {
            return false;
        }
        tight_loop_contents();
    }
    return true;
}

// Parse escape sequences from UART input
// Returns the keycode, or EOF if incomplete/no match
static int parse_uart_escape_sequence(void) {
    char buffer[10];
    size_t bytes_read = 0;
    int ch;

    do {
        if (!uart_wait_readable(ESCAPE_SEQUENCE_TIMEOUT_US)) {
            return EOF;  // Incomplete sequence (timeout)
        }
        ch = uart_getc(uart_default);
        buffer[bytes_read++] = ch;
    } while (bytes_read < sizeof(buffer) && !is_sequence_terminator(ch));

    for (const escape_sequence_t* entry = &escape_sequences[0]; entry->sequence != NULL; entry++) {
        if (strncmp(buffer, entry->sequence, bytes_read) == 0) {
            return entry->key;
        }
    }

    return EOF;  // Unknown sequence
}

// Read a single keycode from UART, handling escape sequences
static int uart_getch(void) {
    if (!uart_is_readable(uart_default)) {
        return EOF;
    }

    int ch = uart_getc(uart_default);
    if (ch != '\e') {
        return ch;
    }

    // Check for CSI (ESC [)
    // Wait briefly for follow-up characters since escape sequences may span
    // multiple UART frames
    if (!uart_wait_readable(ESCAPE_SEQUENCE_TIMEOUT_US)) {
        return ch;  // Just ESC (timeout)
    }

    int next = uart_getc(uart_default);
    if (next != '[') {
        return next;  // Not a CSI sequence, return second char
    }

    return parse_uart_escape_sequence();
}

void input_init(void) {
    gpio_init(MENU_BTN_GP);
    gpio_pull_up(MENU_BTN_GP);
    gpio_set_dir(MENU_BTN_GP, GPIO_IN);
}

void input_task(void) {
    // 1. Handle UART input based on term_input_dest
    if (system_state.term_input_dest != term_input_ignore) {
        int ch = uart_getch();
        while (ch != EOF) {
            if (system_state.term_input_dest == term_input_to_pet) {
                // Remote mode: check for Ctrl+C to exit
                if (ch == 0x03) {
                    cli_exit_remote();
                } else {
                    // Inject as PET keystroke
                    term_inject_char(ch);
                }
            } else if (system_state.term_input_dest == term_input_to_firmware) {
                // Firmware mode: CLI or menu depending on term_mode
                if (system_state.term_mode == term_mode_cli) {
                    // CLI mode: route to CLI processor directly
                    cli_process_char(ch);
                } else {
                    // Menu/other mode: queue for firmware to read via input_getch()
                    enqueue_key(ch);
                }
            }
            ch = uart_getch();
        }
    }
    
    // 2. Process pending terminal keystroke injections
    term_inject_task();
    
    // 3. Handle USB keyboard events
    tuh_task();
    hid_app_task();
    dispatch_key_events();
    
    // 4. Sync USB keyboard state to FPGA
    sync_state();
    
    // 5. Handle button based on mode (always goes to firmware).
    int button_action = get_button_action();
    if (button_action != EOF) {
        enqueue_key(button_action);
    }
}

int input_getch(void) {
    // First check keyboard input (PET and USB)
    int keycode = keyboard_getch();
    if (keycode != EOF) {
        return keycode;
    }
    
    // Then check the firmware input queue (UART + button events)
    return dequeue_key();
}
