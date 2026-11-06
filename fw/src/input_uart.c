// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "input_uart.h"

bool input_uart_accept(input_uart_filter_t* filter, int key, bool menu) {
    const bool accept = !(filter->menu_cr_pending && key == '\n');
    filter->menu_cr_pending = menu && key == '\r';
    return accept;
}
