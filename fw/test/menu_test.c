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

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include "options.h"

static uint8_t display[80 * 25] = { 0 };

const char* home = "\e[H";
const char* reverse_off = "\e[m";
const char* reverse_on = "\e[7m";
const char* cursor_off = "\e[?25l";
const char* cursor_on = "\e[?25h";

const uint8_t columns = 80;
const uint8_t rows = 25;

// This table maps from ASCII to the character ROM offsets.
static const uint8_t ascii_to_vrom[] = {
    /*              0               1               2               3               4               5               6               7               8               9               A               B               C               D               E               F  */
    /* 0 | NUL */ 0x00, /* SOH */ 0x01, /* STX */ 0x02, /* ETX */ 0x03, /* EOT */ 0x04, /* ENQ */ 0x05, /* ACK */ 0x06, /* BEL */ 0x07, /*  BS */ 0x08, /*  HT */ 0x09, /*  LF */ 0x0A, /*  VT */ 0x0B, /*  FF */ 0x0C, /*  CR */ 0x0D, /*  SO */ 0x0E, /*  SI */ 0x0F,
    /* 1 | DLE */ 0x10, /* DC1 */ 0x11, /* DC2 */ 0x12, /* DC3 */ 0x13, /* DC4 */ 0x14, /* NAK */ 0x15, /* SYN */ 0x16, /* ETB */ 0x17, /* CAN */ 0x18, /*  EM */ 0x19, /* SUB */ 0x1A, /* ESC */ 0x1B, /*  FS */ 0x1C, /*  GS */ 0x1D, /*  RS */ 0x1E, /*  US */ 0x1F,
    /* 2 |  SP */ 0x20, /*   ! */ 0x21, /*   " */ 0x22, /*   # */ 0x23, /*   $ */ 0x24, /*   % */ 0x25, /*   & */ 0x26, /*   ' */ 0x27, /*   ( */ 0x28, /*   ) */ 0x29, /*   * */ 0x2A, /*   + */ 0x2B, /*   , */ 0x2C, /*   - */ 0x2D, /*   . */ 0x2E, /*   / */ 0x2F,
    /* 3 |   0 */ 0x30, /*   1 */ 0x31, /*   2 */ 0x32, /*   3 */ 0x33, /*   4 */ 0x34, /*   5 */ 0x35, /*   6 */ 0x36, /*   7 */ 0x37, /*   8 */ 0x38, /*   9 */ 0x39, /*   : */ 0x3A, /*   ; */ 0x3B, /*   < */ 0x3C, /*   = */ 0x3D, /*   > */ 0x3E, /*   ? */ 0x3F,
    /* 4 |   @ */ 0x00, /*   A */ 0x41, /*   B */ 0x42, /*   C */ 0x43, /*   D */ 0x44, /*   E */ 0x45, /*   F */ 0x46, /*   G */ 0x47, /*   H */ 0x48, /*   I */ 0x49, /*   J */ 0x4A, /*   K */ 0x4B, /*   L */ 0x4C, /*   M */ 0x4D, /*   N */ 0x4E, /*   O */ 0x4F,
    /* 5 |   P */ 0x50, /*   Q */ 0x51, /*   R */ 0x52, /*   S */ 0x53, /*   T */ 0x54, /*   U */ 0x55, /*   V */ 0x56, /*   W */ 0x57, /*   X */ 0x58, /*   Y */ 0x59, /*   Z */ 0x5A, /*   [ */ 0x1B, /*   \ */ 0x1C, /*   ] */ 0x1D, /*   ^ */ 0x1E, /*   _ */ 0x64,
    /* 6 |   ` */ 0x27, /*   a */ 0x01, /*   b */ 0x02, /*   c */ 0x03, /*   d */ 0x04, /*   e */ 0x05, /*   f */ 0x06, /*   g */ 0x07, /*   h */ 0x08, /*   i */ 0x09, /*   j */ 0x0A, /*   k */ 0x0B, /*   l */ 0x0C, /*   m */ 0x0D, /*   n */ 0x0E, /*   o */ 0x0F,
    /* 7 |   p */ 0x10, /*   q */ 0x11, /*   r */ 0x12, /*   s */ 0x13, /*   t */ 0x14, /*   u */ 0x15, /*   v */ 0x16, /*   w */ 0x17, /*   x */ 0x18, /*   y */ 0x19, /*   z */ 0x1A, /*   { */ 0x6B, /*   | */ 0x5B, /*   } */ 0x73, /*   ~ */ 0x71, /* DEL */ 0x7F,
};

// This table maps from PET character ROM offsets to the closest VT-100 supported equivalents.
static const char* term_chars_lower[] = {
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

void clear_screen() {
    fputs("\e[2J\e[H", stdout);
}

// Function to read a single key press
char getch() {
    struct termios oldt, newt;
    char ch;

    // Get the current terminal settings
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;

    // Disable canonical mode and echo
    newt.c_lflag &= ~(ICANON | ECHO);

    // Set the new terminal settings
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    // Read a single character
    ch = getchar();

    // Restore the old terminal settings
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);

    return ch;
}

static inline uint8_t* get_screen_addr(uint8_t x, uint8_t y) {
    return &display[y * columns + x];
}

static inline void check_screen_addr(uint8_t* pAddr) {
    assert(pAddr >= display);
    assert(pAddr < display + sizeof(display));
}

static inline void _fill_line(uint8_t x, uint8_t y, uint8_t width, uint8_t c) {
    uint8_t *pOut = get_screen_addr(x, y);

    while (width--) {
        *pOut++ = c;
    }
}

void fill_line(uint8_t x, uint8_t y, uint8_t width, uint8_t c) {
    assert(x + width <= columns);
    assert(y < rows);

    _fill_line(x, y, width, c);
}

void fill_line_3(uint8_t x, uint8_t y, uint8_t width, uint8_t left, uint8_t middle, uint8_t right) {
    // Subtract the left and right characters from the width
    width -= 2;

    assert(x + width <= columns);
    assert(y < rows);

    uint8_t *pOut = get_screen_addr(x, y);
    *pOut++ = left;
    while (width--) {
        *pOut++ = middle;
    }
    *pOut++ = right;

    check_screen_addr(pOut);
}

void fill_rect(uint8_t x, uint8_t y, uint8_t width, uint8_t height, uint8_t c) {
    assert(x + width <= columns);
    assert(y + height <= rows);

    while (height--) {
        _fill_line(x, y++, width, c);
    }
}

uint8_t* _draw_text(uint8_t *pOut, const char* str) {
    uint8_t *pCh = (uint8_t*) str;

    while (*pCh != 0) {
        *pOut++ = ascii_to_vrom[*(pCh++)];
    }

    return pOut;
}

uint8_t* _draw_text_reversed(uint8_t *pOut, const char* str) {
    uint8_t *pCh = (uint8_t*) str;

    while (*pCh != 0) {
        *pOut++ = (ascii_to_vrom[*(pCh++)] | 0x80);
    }

    return pOut;
}

uint8_t* draw_text(uint8_t x, uint8_t y, const char* str) {
    assert(x < columns);
    assert(y < rows);

    uint8_t *pOut = get_screen_addr(x, y);
    uint8_t *pCh = (uint8_t*) str;

    while (*pCh != 0) {
        *pOut++ = ascii_to_vrom[*(pCh++)];
        check_screen_addr(pOut);
    }

    return pOut;
}

void draw_group_frame(uint8_t x, uint8_t y, uint8_t width, uint8_t height, const char* title) {
    assert(x + width <= columns);
    assert(y + height <= rows);

    const uint8_t ty = y;

    fill_line_3(x, y, width, 0x70, 0x40, 0x6E);

    y++;
    for (uint8_t i = 1; i < height - 1; i++, y++) {
        uint8_t *pOut = get_screen_addr(x, y);
        *pOut = 0x5D; // left
        pOut += (width - 1);
        *pOut = 0x5D; // right
    }

    fill_line_3(x, y++, width, 0x6D, 0x40, 0x7D);
    draw_text(x + 2, ty, title);
}

void recordOptionLayout(uint8_t x, uint8_t y, uint8_t w, Option* pOption, uint8_t value, uint8_t* pOptionIndex) {
    Layout layout = {
        .pOption = pOption,
        .value = value,
        .x = x,
        .y = y,
        .w = w
    };

    layouts[(*pOptionIndex)++] = layout;
}

int draw_choice(uint8_t x, uint8_t y, uint8_t w, Option* pOption, uint8_t* pOptionIndex) {
    const OptionSelect *pSelect = &pOption->select;

    for (int i = 0; i < pSelect->count; i++, y++) {
        uint8_t *pOut = get_screen_addr(x, y);
        pOut = _draw_text(pOut, " (");
        pOut = _draw_text(pOut, pSelect->selected == i ? "*" : " ");
        pOut = _draw_text(pOut, ") ");
        pOut = _draw_text(pOut, pSelect->values[i]);

        recordOptionLayout(x, y, w, pOption, i, pOptionIndex);
    }
    return y;
}

int draw_button(uint8_t x, uint8_t y, uint8_t w, Option* pOption, uint8_t* pOptionIndex) {
    uint8_t *pOut = get_screen_addr(x, y);
    pOut = _draw_text(pOut, "[ ");
    pOut = _draw_text(pOut, pOption->action.label);
    pOut = _draw_text(pOut, " ]");
    recordOptionLayout(x, y, w, pOption, 0, pOptionIndex);
    return y;
}

int draw_option(uint8_t x, uint8_t y, uint8_t w, Option* option, uint8_t* pOptionIndex) {
    switch (option->kind) {
        case OptionKind_Choice: return draw_choice(x, y, w, option, pOptionIndex);
        case OptionKind_Spacer: return y + 1;
        default: assert(false);
    }
}

int draw_group(uint8_t x, uint8_t y, uint8_t w, Group* group, uint8_t* pOptionIndex) {
    uint8_t y_start = y;
    Option* pOption = &group->options[0];
    
    y++;
    for (size_t i = 0; pOption->kind != OptionKind_End; i++) {
        y = draw_option(x + 1, y, w - 2, pOption++, pOptionIndex);
    }
    y++;

    draw_group_frame(x, y_start, w, y - y_start, group->name);
    
    return y;
}

int cursor = 0;

void draw_cursor() {
    Layout layout = layouts[cursor];

    assert(layout.pOption != NULL);

    uint8_t* pSrc = get_screen_addr(layout.x, layout.y);
    for (int i = layout.w; i != 0; i--, pSrc++) {
        *pSrc |= 0x80;
    }
}

void undraw_cursor() {
    Layout layout = layouts[cursor];

    assert(layout.pOption != NULL);

    uint8_t* pSrc = get_screen_addr(layout.x, layout.y);
    for (int i = layout.w; i != 0; i--, pSrc++) {
        *pSrc &= 0x7F;
    }
}

void onSelect(Option* pOption) {
    // Choosing 80 column video forces Basic 4.0
    if (pOption == &groups[Group_Video].options[Option_Video_Columns] &&
        pOption->select.selected != Option_Video_Columns_40) {
        groups[Group_Basic].options[Option_Basic_Version].select.selected = Option_Basic_Version_4;
    }

    // Choosing 50 Hz video forces Basic 4.0
    if (pOption == &groups[Group_Video].options[Option_Video_VSync] &&
        pOption->select.selected != Option_Video_VSync_60Hz) {
        groups[Group_Basic].options[Option_Basic_Version].select.selected = Option_Basic_Version_4;
    }

    if (pOption == &groups[Group_Basic].options[Option_Basic_Version_2] ||
        pOption == &groups[Group_Basic].options[Option_Basic_Version_3]) {
        groups[Group_Video].options[Option_Video_Columns].select.selected = Option_Video_Columns_40;
        groups[Group_Video].options[Option_Video_VSync].select.selected   = Option_Video_VSync_60Hz;
    }
}

void select() {
    Layout* layout = &layouts[cursor];
    Option* pOption = layout->pOption;
    assert(pOption != NULL);
    switch (pOption->kind) {
        case OptionKind_Choice:
            pOption->select.selected = layout->value;
            onSelect(pOption);
            break;
    }
}

void display_config_menu() {
    uint8_t y = 0;
    uint8_t optionIndex = 0;

    // Draw Basic group frame:
    y = draw_group(0,   y, 30, &groups[0], &optionIndex);
    y = draw_group(0, ++y, 30, &groups[1], &optionIndex);
    y = draw_button(0, ++y, 9, &groups[Group_Button].options[Option_Button_Reset], &optionIndex);
 
    draw_cursor();
}

void term_display(const uint8_t* pSrc, uint8_t cols, uint8_t rows) {
    fputs(home, stdout);
    fputs(reverse_off, stdout);
    fputs(cursor_off, stdout);

    bool reverse = false;

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            char ch = *pSrc++;
            if (reverse != ((ch & 0x80) != 0)) {
                reverse = !reverse;
                fputs(reverse ? reverse_on : reverse_off, stdout);
            }
            fputs(term_chars_lower[ch & 0x7F], stdout);
        }
        putchar('\n');
    }
}

void cursor_down() {
    undraw_cursor();
    
    cursor++;
    if (layouts[cursor].pOption == NULL) {
        cursor = 0;
    }
}

void menu() {
    while (true) {
        display_config_menu(display);
        term_display(display, 80, 25);
        const char ch = getch();
        switch (ch) {
            case '\n':
                select();
                break;
            default:
                cursor_down();
                break;
        }
    }

    fputs(cursor_on, stdout);   // restore cursor
}
