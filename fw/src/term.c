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
static const char* __in_flash(".term") term_chars_lower[] = {
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


static const char* __in_flash(".term") home = "\e[H";
static const char* __in_flash(".term") reverse_off = "\e[m";
static const char* __in_flash(".term") reverse_on = "\e[7m";
static const char* __in_flash(".term") cursor_off = "\e[?25l";
static const char* __in_flash(".term") cursor_on = "\e[?25h";
static const char* __in_flash(".term") enter_alternate = "\e[?1049h";
static const char* __in_flash(".term") exit_alternate = "\e[?1049l";
static const char* __in_flash(".term") clear_screen = "\e[2J\e[H";

static struct termios oldt;

void term_begin() {
    fputs(enter_alternate, stdout);
    fputs(cursor_off, stdout);
    fputs(reverse_off, stdout);
    fputs(clear_screen, stdout);
    fflush(stdout);

    struct termios newt;
    char ch;

    // Get the current terminal settings
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;

    // Disable canonical mode and echo
    newt.c_lflag &= ~(ICANON | ECHO);

    // Set the new terminal settings
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
}

void term_display(const uint8_t* char_buffer, const unsigned int cols, const unsigned int rows) {
    bool reverse = false;

    fputs(home, stdout);

    for (unsigned int r = 0; r < rows; r++) {
        for (unsigned int c = 0; c < cols; c++) {
            const char ch = *char_buffer++;

            if (reverse != ((ch & 0x80) != 0)) {
                reverse = !reverse;
                fputs(reverse ? reverse_on : reverse_off, stdout);
            }

            fputs(term_chars_lower[ch & 0x7F], stdout);
        }
        putchar('\n');
    }
}

void term_end() {
    // Restore the old terminal settings
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);

    fputs(reverse_off, stdout);
    fputs(cursor_on, stdout);
    fputs(exit_alternate, stdout);
    fflush(stdout);
}

// Define a structure for mapping escape sequences to key constants
typedef struct {
    const char* sequence;
    int key;
} escape_sequence_t;

// Static table of escape sequences
static const escape_sequence_t escape_sequences[] = {
    { "[A", KEY_UP },    // Up arrow
    { "[B", KEY_DOWN },  // Down arrow
    { "[C", KEY_RIGHT }, // Right arrow
    { "[D", KEY_LEFT },  // Left arrow
    { "[H", KEY_HOME },  // Home
    { "[F", KEY_END },   // End
    { "[5~", KEY_PGUP }, // Page Up
    { "[6~", KEY_PGDN }, // Page Down
    { NULL, 0 }              // Sentinel to mark the end of the table
};

int term_getch() {
    char buffer[10];
    int bytes_read;

    bytes_read = read(STDIN_FILENO, buffer, 1);
    if (bytes_read <= 0) {
        return -1;
    }

    if (buffer[0] == '\x1B') {
        bytes_read += read(STDIN_FILENO, buffer + 1, sizeof(buffer) - 1);

        for (int i = 0; i < ARRAY_SIZE(escape_sequences); i++) {
            if (strncmp(buffer + 1, escape_sequences[i].sequence, bytes_read - 1) == 0) {
                return escape_sequences[i].key;
            }
        }

        return -1;
    }

    return buffer[0];
}
