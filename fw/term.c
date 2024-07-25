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

#include "term.h"

static const char __in_flash(".term_chars_lower") term_chars_lower[] = {
    //        0    1    2    3    4    5    6    7    8    9    A    B    C    D    E    F
    /* 00 */ '@', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
    /* 10 */ 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '[', '\\', ']', '^', '<',
    /* 20 */ ' ', '!', '"', '#', '$', '%', '&', '\'', '(', ')', '*', '+', ',', '-', '.', '/',
    /* 30 */ '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', ';', '<', '=', '>', '?',
    /* 40 */ '-', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
    /* 50 */ 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '_', '_', '_', '_', '_',
    /* 60 */ '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_',
    /* 70 */ '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_'
};

static const char* clear = "\e[2J";
static const char* reverse_off = "\e[m";
static const char* reverse_on = "\e[7m";

bool reverse = false;

void term_put_char(uint8_t ch) {
    bool reversed = (ch & 0x80) != 0;
    if (reversed != reverse) {
        printf(reversed ? reverse_on : reverse_off);
        reverse = reversed;
    }
    putchar(term_chars_lower[ch & 0x7F]);
}

void term_display(const uint8_t* const pSrc, uint8_t cols, uint8_t rows) {
    const uint8_t* p = pSrc;

    printf(clear);
    printf(reverse_off);
    reverse = false;

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            term_put_char(*p++);
        }
        printf("\n");
    }
}
