// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>

typedef struct {
    bool cr_pending;
} input_uart_filter_t;

// Filters the LF of a UART CRLF pair across CLI, remote and menu transitions.
bool input_uart_accept(input_uart_filter_t* filter, int key);

// Reset the terminal stream's escape decoder and CRLF state.
void input_uart_init(void);

// Acquire available UART bytes and advance incremental decoding without waiting.
void input_uart_task(void);

// Normalize decoded UART input and submit it to the router for destination selection.
void input_uart_process(int key);
