/* SPDX-License-Identifier: CC0-1.0 */
/* https://github.com/dlehenbauer/econopet */

/*
 * colortest.c -- Draw all 16 ColourPET RGBI foreground and background colors.
 *
 * Write screen and color RAM directly, without editor-ROM output.
 */

#include <conio.h>
#include <peekpoke.h>

#define SCREEN_BASE 0x8000u
#define COLOR_BASE 0x8800u
#define SCREEN_WIDTH 40
#define SCREEN_HEIGHT 25
#define COLUMN_WIDTH 19
#define BACKGROUND_COLUMN 21
#define FIRST_COLOR_ROW 4
#define COLOR_COUNT 16
#define WHITE 15
#define BLACK 0
#define SCREEN_SPACE 32
#define VIA_PCR_ADDRESS 0xE84Cu
#define VIA_PCR_UPPER_MASK 0xF0
#define VIA_PCR_GRAPHICS 0x0C

static const char* const names[COLOR_COUNT] = {
    "black", "medium gray", "blue", "light blue",
    "green", "light green", "dark cyan", "light cyan",
    "red", "light red", "dark magenta", "magenta",
    "dark yellow", "yellow", "light gray", "white"
};

static const unsigned char contrast[COLOR_COUNT] = {
    WHITE, WHITE, WHITE, WHITE, WHITE, BLACK, WHITE, BLACK,
    WHITE, BLACK, WHITE, BLACK, WHITE, BLACK, BLACK, BLACK
};

static unsigned int screen = SCREEN_BASE;
static unsigned int color = COLOR_BASE;
static unsigned char row;
static unsigned char column;
static unsigned char index;
static unsigned char foreground;
static unsigned char background;
static unsigned char character;
static unsigned char attribute;
static const char* text;
static char label[COLUMN_WIDTH + 1];
static unsigned char length;

/* Build a numbered color label for both chart columns. */
static void make_label(void)
{
    label[0] = index < 10 ? ' ' : '1';
    label[1] = '0' + index % 10;
    label[2] = ' ';
    length = 3;
    text = names[index];
    while (*text) {
        label[length++] = *text++;
    }
    label[length] = '\0';
}

/* Paint a complete row, including blank cells and their attributes. */
static void paint_row(void)
{
    for (column = 0; column < SCREEN_WIDTH; ++column) {
        if (row >= FIRST_COLOR_ROW
            && row < FIRST_COLOR_ROW + COLOR_COUNT
            && column == BACKGROUND_COLUMN) {
            text = label;
        }
        character = SCREEN_SPACE;
        if (*text) {
            character = *text++;
        }
        if (character >= 64 && character <= 95) {
            character -= 64;
        }
        attribute = WHITE;
        if (column < COLUMN_WIDTH) {
            attribute = foreground;
        } else if (column >= BACKGROUND_COLUMN) {
            attribute = background;
        }
        POKE(screen, character);
        POKE(color, attribute);
        ++screen;
        ++color;
    }
}

/* Draw the chart once and retain it until a key is pressed. */
int main(void)
{
    /* Override CC65's lowercase font without calling the editor ROM. */
    POKE(VIA_PCR_ADDRESS,
         (PEEK(VIA_PCR_ADDRESS) & VIA_PCR_UPPER_MASK) | VIA_PCR_GRAPHICS);

    /* Paint top to bottom so the title provides immediate feedback. */
    for (row = 0; row < SCREEN_HEIGHT; ++row) {
        text = "";
        foreground = WHITE;
        background = WHITE;
        switch (row) {
            case 0: text = "colourpet rgbi color test"; break;
            case 2: text = "foreground           background"; break;
            case 21: text = "direct screen and color ram pokes"; break;
            case 22: text = "fg: low nibble / bg: high nibble"; break;
            case 24: text = "[press any key to exit]"; break;
        }
        if (row >= FIRST_COLOR_ROW
            && row < FIRST_COLOR_ROW + COLOR_COUNT) {
            index = row - FIRST_COLOR_ROW;
            make_label();
            text = label;
            foreground = index == BLACK ? WHITE << 4 : index;
            background = (index << 4) | contrast[index];
        }
        paint_row();
    }

    /* Discard pending keystrokes before waiting for the user. */
    while (kbhit()) {
        cgetc();
    }
    while (!kbhit()) {
    }
    cgetc();
    return 0;
}
