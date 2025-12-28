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

#include "console.h"
#include <stdio.h>

// Control characters
#define CTRL_C      0x03
#define BACKSPACE   0x08
#define TAB         0x09
#define LF          0x0A
#define CR          0x0D
#define DEL         0x7F

// ANSI escape sequences
static const char* const term_erase_char = "\b \b";

void console_puts(const char* str) {
    fputs(str, stdout);
    fflush(stdout);
}

void console_prompt(void) {
    console_puts("econopet> ");
}

void console_newline(void) {
    console_puts("\r\n");
}

bool console_process_char(int ch, char* line_buf, size_t* line_len, size_t max_len) {
    // Handle Ctrl+C - cancel current line
    if (ch == CTRL_C) {
        console_puts("^C");
        console_newline();
        *line_len = 0;
        line_buf[0] = '\0';
        return true;
    }

    // Handle Enter (CR or LF)
    if (ch == CR || ch == LF) {
        console_newline();
        line_buf[*line_len] = '\0';
        return true;
    }

    // Handle Backspace or DEL
    if (ch == BACKSPACE || ch == DEL) {
        if (*line_len > 0) {
            (*line_len)--;
            console_puts(term_erase_char);
        }
        return false;
    }

    // Handle printable characters
    if (ch >= 0x20 && ch < 0x7F) {
        if (*line_len < max_len - 1) {
            line_buf[*line_len] = (char)ch;
            (*line_len)++;
            // Echo the character
            char echo[2] = { (char)ch, '\0' };
            console_puts(echo);
        }
        return false;
    }

    // Ignore other control characters
    return false;
}
