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

#pragma once

#include <stdbool.h>
#include <stddef.h>

/**
 * Console - Low-level terminal I/O primitives
 * 
 * Provides basic terminal output and line-editing input for the CLI.
 */

// Maximum command line length
#define CONSOLE_LINE_MAX 80

/**
 * Print a string to the console (no newline).
 */
void console_puts(const char* str);

/**
 * Print the CLI prompt.
 */
void console_prompt(void);

/**
 * Process a single character of input for line editing.
 * 
 * Handles:
 * - Printable characters: echo and append to buffer
 * - Backspace (0x7F or 0x08): delete last character
 * - Enter (0x0D or 0x0A): return true (line complete)
 * - Ctrl+C (0x03): clear line, return true with empty buffer
 * 
 * @param ch The character to process
 * @param line_buf Buffer to accumulate the line
 * @param line_len Pointer to current line length (updated by this function)
 * @param max_len Maximum buffer size
 * @return true if line is complete (Enter or Ctrl+C pressed), false otherwise
 */
bool console_process_char(int ch, char* line_buf, size_t* line_len, size_t max_len);

/**
 * Print a newline to the console.
 */
void console_newline(void);
