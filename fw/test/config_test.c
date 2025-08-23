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

#include "system_state.h"
#include "menu/window.h"
#include "menu/menu_config.h"

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

void on_action_load(void* context, const char* filename, uint32_t address) {
    (void) context;

    // This is a placeholder for the actual callback implementation
    printf("Action load: %s @ %04x\n", filename, address);
}

void on_action_patch(void* context, uint32_t address, const binary_t* binary) {
    (void) context;

    printf("Action patch: %04x, length: %zu\n", address, binary->size);
}

void on_action_copy(void* context, uint32_t source, uint32_t destination, uint32_t length) {
    (void) context;

    printf("Action copy: %04x -> %04x, length: %u\n", source, destination, length);
}

void on_action_set_options(void* context, options_t* options) {
    (void) context;

    // This is a placeholder for the actual callback implementation
    printf("Action set options: columns = %u\n", options->columns);
}

void on_action_set_keymap(void* context, const char* filename) {
    (void) context;

    // This is a placeholder for the actual callback implementation
    printf("Action set keymap: %s\n", filename);
}

void on_action_fix_checksum(void* context, uint32_t start_addr, uint32_t end_addr, uint32_t fix_addr, uint32_t checksum) {
    (void) context;

    // This is a placeholder for the actual callback implementation
    printf("Action fix checksum: start=0x%04x, end=0x%04x, fix=0x%04x, checksum=0x%02x\n", 
           start_addr, end_addr, fix_addr, checksum);
}

void config_test() {
    const window_t window = window_create(buffer, COLS, ROWS);

    system_state_t system_state = {
        .pet_keyboard_model = pet_keyboard_model_business,
        .pet_video_type = pet_video_type_crtc,
    };

    const setup_sink_t setup_sink = {
        .context = NULL,
        .on_load = on_action_load,
        .on_copy = on_action_copy,
        .on_patch = on_action_patch,
        .on_set_options = on_action_set_options,
        .on_set_keymap = on_action_set_keymap,
        .on_fix_checksum = on_action_fix_checksum,
        .system_state = &system_state,
    };

    enable_raw_mode();
    menu_config_show(&window, &setup_sink);
    disable_raw_mode();
}
