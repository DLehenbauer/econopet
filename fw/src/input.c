// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "input.h"

#include "diag/log/log.h"
#include "driver.h"
#include "hw.h"
#include "input_controls.h"
#include "input_uart.h"
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
static input_uart_filter_t uart_filter;

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
            const bool menu = system_state.term_input_dest == term_input_to_firmware
                && system_state.term_mode != term_mode_cli;
            if (!input_uart_accept(&uart_filter, ch, menu)) {
                ch = uart_getch();
                continue;
            }
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
    
    // 5. Sample button state.
    input_button_update(!gpio_get(MENU_BTN_GP));
}

int input_getch(void) {
    // Menu button and SHIFT-SHIFT escape sequence
    const int control = input_controls_getch();
    if (control != EOF) {
        return control;
    }

    // Keyboard input (PET and USB)
    int keycode = keyboard_getch();
    if (keycode != EOF) {
        return keycode;
    }
    
    // UART input queue
    return dequeue_key();
}
