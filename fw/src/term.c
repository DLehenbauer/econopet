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

// This table maps from PET character ROM offsets to the closest VT-100 supported equivalents.
static const char* const term_chars_lower[] = {
    //            0        1     2    3    4    5    6    7    8    9    A    B     C    D    E    F
    /* 00 */         "@", "a",  "b", "c", "d", "e", "f", "g", "h", "i", "j", "k",  "l",         "m",         "n", "o",
    /* 10 */         "p", "q",  "r", "s", "t", "u", "v", "w", "x", "y", "z", "[", "\\",         "]",         "^", "<",
    /* 20 */         " ", "!", "\"", "#", "$", "%", "&", "'", "(", ")", "*", "+",  ",",         "-",         ".", "/",
    /* 30 */         "0", "1",  "2", "3", "4", "5", "6", "7", "8", "9", ":", ";",  "<",         "=",         ">", "?",
    /* 40 */ "\e(0q\e(B", "A",  "B", "C", "D", "E", "F", "G", "H", "I", "J", "K",  "L",         "M",         "N", "O",
    /* 50 */         "P", "Q",  "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "_",  "_", "\e(0x\e(B",         "_", "_",
    /* 60 */         "_", "_",  "_", "_", "_", "_", "_", "_", "_", "_", "_", "_",  "_", "\e(0m\e(B", "\e(0k\e(B", "_",
    /* 70 */ "\e(0l\e(B", "_",  "_", "_", "_", "_", "_", "_", "_", "_", "_", "_",  "_", "\e(0j\e(B",         "_", "_"
};

static const char* const home = "\e[H";
static const char* const reverse_off = "\e[m";
static const char* const reverse_on = "\e[7m";
static const char* const cursor_off = "\e[?25l";
static const char* const cursor_on = "\e[?25h";
static const char* const enter_alternate = "\e[?1049h";
static const char* const exit_alternate = "\e[?1049l";
static const char* const clear_screen = "\e[2J\e[H";
static const char* const echo_off = "\e[12l";
static const char* const echo_on = "\e[12h";

void term_begin(const window_t* const window) {
    fputs(enter_alternate, stdout);
    fputs(echo_off, stdout);
    fputs(cursor_off, stdout);
    fputs(reverse_off, stdout);
    fputs(clear_screen, stdout);
    fflush(stdout);

    window_fill(window, CH_SPACE);
}

void term_display(const window_t* const window) {
    bool reverse = false;

    fputs(home, stdout);

    const unsigned int cols = window->width;
    const unsigned int rows = window->height;
    uint8_t* char_buffer = window->start;

    for (unsigned int r = 0; r < rows; r++) {
        for (unsigned int c = 0; c < cols; c++) {
            const char ch = *char_buffer++;

            if (reverse != ((ch & 0x80) != 0)) {
                reverse = !reverse;
                fputs(reverse ? reverse_on : reverse_off, stdout);
            }

            fputs(term_chars_lower[ch & 0x7F], stdout);
        }
        fputs("\r\n", stdout);
    }
}

void term_end() {
    fputs(reverse_off, stdout);
    fputs(cursor_on, stdout);
    fputs(echo_on, stdout);
    fputs(exit_alternate, stdout);
    fflush(stdout);
}

// Define a structure for mapping escape sequences to key constants
typedef struct {
    const char* sequence;
    int key;
} escape_sequence_t;

// Map escape sequences to key constants. (ESC [ has already been match.)
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

int term_getch() {
    int ch = term_input_char();
    if (ch != '\e') {
        return ch;
    }

    ch = getchar();
    if (ch != '[') {
        return ch;
    }

    char buffer[10];
    size_t bytes_read = 0;

    do {
        ch = getchar();
        buffer[bytes_read++] = ch;
    } while (bytes_read < sizeof(buffer) && !is_sequence_terminator(ch));

    for (const escape_sequence_t* entry = &escape_sequences[0]; entry->sequence != NULL; entry++) {
        if (strncmp(buffer, entry->sequence, bytes_read) == 0) {
            return entry->key;
        }
    }

    return EOF;
}
