// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>

typedef struct uart_inst uart_inst_t;
extern uart_inst_t* const uart0;
#define uart_default uart0

// Match the SDK byte-acquisition boundary used by the UART adapter.
bool uart_is_readable(uart_inst_t* uart);
char uart_getc(uart_inst_t* uart);
