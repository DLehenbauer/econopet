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
#include "display/char_encoding.h"

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

            fputs(vrom_to_term(ch), stdout);
        }
        fputs("\r\n", stdout);
    }

    term_present();
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
