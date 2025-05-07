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

#include "../src/menu/window.h"
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

void on_action_load(const char* filename, uint32_t address) {
    // This is a placeholder for the actual callback implementation
    printf("Action load: %s @ %05lx\n", filename, address);
}

void config_test() {
    const window_t window = window_create(buffer, COLS, ROWS);
    
    const setup_sink_t setup_sink = {
        .on_action_load = on_action_load,
    };

    enable_raw_mode();
    menu_config_show(&window, &setup_sink);
    disable_raw_mode();
}
