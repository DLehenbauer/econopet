// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "input/input_uart.h"

#include "hardware/uart.h"

#include "diag/log/log.h"
#include "input/input.h"
#include "input/input_routing.h"

#define UART_ESCAPE 0x1b
#define UART_ESCAPE_TIMEOUT_US 10000u
#define UART_ESCAPE_CAPACITY 10u

/** An incomplete escape remains adapter-owned until a suffix or timeout arrives. */
typedef enum {
    uart_decode_plain,
    uart_decode_escape,
    uart_decode_csi,
} uart_decode_state_t;

/** Exact CSI suffixes supported by the terminal bindings. */
typedef struct {
    const char* suffix;
    int key;
} uart_escape_t;

static const uart_escape_t escapes[] = {
    {"A", KEY_UP}, {"B", KEY_DOWN}, {"C", KEY_RIGHT}, {"D", KEY_LEFT},
    {"F", KEY_END}, {"H", KEY_HOME}, {"5~", KEY_PGUP}, {"6~", KEY_PGDN},
};

static input_uart_filter_t uart_filter;
static uart_decode_state_t decode_state;
static uint64_t last_byte_time;
static char suffix[UART_ESCAPE_CAPACITY + 1u];
static size_t suffix_length;

bool input_uart_accept(input_uart_filter_t* filter, int key) {
    const bool accept = !(filter->cr_pending && key == '\n');
    filter->cr_pending = key == '\r';
    return accept;
}

void input_uart_init(void) {
    uart_filter = (input_uart_filter_t){0};
    decode_state = uart_decode_plain;
    suffix_length = 0;
}

void input_uart_process(int key) {
    if (!input_uart_accept(&uart_filter, key)) {
        return;
    }

    input_routing_terminal(key);
}

/** Retire a timed-out partial CSI, or deliver a lone Escape without blocking other inputs. */
static void expire_escape(void) {
    if (decode_state == uart_decode_plain || time_us_64() - last_byte_time < UART_ESCAPE_TIMEOUT_US)
        return;
    const bool lone_escape = decode_state == uart_decode_escape;
    decode_state = uart_decode_plain;
    if (lone_escape)
        input_uart_process(UART_ESCAPE);
    else
        log_debug("UART escape sequence incomplete");
}

/** Advance one byte, preserving the existing non-CSI Escape-prefix behavior. */
static void decode_byte(uint8_t byte) {
    last_byte_time = time_us_64();
    // A new prefix replaces an incomplete CSI even when its bytes were already buffered.
    if (byte == UART_ESCAPE && decode_state == uart_decode_csi) {
        decode_state = uart_decode_escape;
        return;
    }
    switch (decode_state) {
        case uart_decode_plain:
            if (byte == UART_ESCAPE)
                decode_state = uart_decode_escape;
            else
                input_uart_process(byte);
            break;
        case uart_decode_escape:
            if (byte == '[') {
                decode_state = uart_decode_csi;
                suffix_length = 0;
            } else {
                decode_state = uart_decode_plain;
                input_uart_process(byte);
            }
            break;
        case uart_decode_csi:
            suffix[suffix_length++] = (char)byte;
            if ((byte >= 64 && byte <= 126) || byte <= 31 || suffix_length == UART_ESCAPE_CAPACITY) {
                decode_state = uart_decode_plain;
                suffix[suffix_length] = '\0';
                for (size_t i = 0; i < count_of(escapes); ++i) {
                    if (strcmp(suffix, escapes[i].suffix) == 0) {
                        input_uart_process(escapes[i].key);
                        return;
                    }
                }
                log_debug("UART escape sequence unsupported");
            }
            break;
    }
}

void input_uart_task(void) {
    // Received suffixes take precedence over an idle timeout delayed by other servicing.
    while (uart_is_readable(uart_default)) {
        decode_byte((uint8_t)uart_getc(uart_default));
    }
    expire_escape();
}
