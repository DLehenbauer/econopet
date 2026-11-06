// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>

typedef struct {
    bool menu_cr_pending;
} input_uart_filter_t;

// Filters the LF of a UART CRLF pair whose CR was delivered to the menu.
bool input_uart_accept(input_uart_filter_t* filter, int key, bool menu);
