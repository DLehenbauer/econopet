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

#include <termios.h>
#include <unistd.h>

#include "../src/menu/menu_config.h"

#define COLS 40
#define ROWS 25
#define BUFFER_SIZE (COLS * ROWS)
static uint8_t buffer[BUFFER_SIZE] = {0};

void enable_raw_mode() {
    struct termios t;
    tcgetattr(STDIN_FILENO, &t);
    t.c_lflag &= ~(ICANON | ECHO); // Disable canonical mode and echo
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

void disable_raw_mode() {
    struct termios t;
    tcgetattr(STDIN_FILENO, &t);
    t.c_lflag |= (ICANON | ECHO); // Re-enable canonical mode and echo
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

int term_input_char() {
    return getchar();
}

void config_test() {
    enable_raw_mode();
    menu_config_show(buffer, COLS, ROWS);
    disable_raw_mode();
}
