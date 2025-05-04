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

#include "menu_config.h"

#include "../config/config.h"
#include "../driver.h"
#include "../term.h"
#include "../global.h"
#include "window.h"

typedef struct context2_s {
    unsigned int skip_count;
} context2_t;

void on_enter_config_callback(void* user_data) {
    context2_t* const ctx = (context2_t*) user_data;
    ctx->skip_count--;
}

void on_action_load_callback(void* user_data, const char* filename, uint32_t address) {
    context2_t* const ctx = (context2_t*) user_data;
    if (ctx->skip_count != 0xffffffff) {
        return;
    }

    snprintf(temp_buffer, sizeof(temp_buffer), "/roms/%s", filename);
    printf("0x%05X: %s\n", address, temp_buffer);

    FILE *file = fopen(temp_buffer, "rb"); // Open file in binary read mode
    if (!file) {
        perror("Failed to open file");
        goto cleanup;
    }

    // Read remaining bytes to the destination address.
    size_t bytes_read;

    while ((bytes_read = fread(temp_buffer, 1, sizeof(temp_buffer), file)) > 0) {
        spi_write(address, temp_buffer, bytes_read);
        address += bytes_read;

        // Check for read errors (other than EOF)
        if (bytes_read < sizeof(temp_buffer) && ferror(file)) {
            perror("Error reading file");
            goto cleanup;
        }
    }

cleanup:
    if (file) {
        fclose(file);
    }
}

void load_config(int selected_config) {
    // Load the selected config
    // This is a placeholder for the actual loading logic
    printf("Loading config: %d\n", selected_config);

    context2_t ctx = {
        .skip_count = selected_config,
    };

    config_sink_t sink = {
        .user_data = &ctx,
        .on_enter_config = on_enter_config_callback,
        .on_exit_config = NULL,
        .on_action_load = on_action_load_callback,
    };
    
    parse_config_file("/config.yaml", &sink);
}

typedef struct context_s {
    window_t* window;
    unsigned int config_count;
} context_t;

void on_config_callback(void* user_data, const char* name) {
    context_t* const ctx = (context_t*) user_data;
    window_puts(ctx->window, window_xy(ctx->window, 0, ctx->config_count++), name);
}

void menu_config_show(uint8_t* const buffer, const unsigned int cols, const unsigned int rows) {
    window_t window = window_create(buffer, cols, rows);

    context_t context = {
        .window = &window,
        .config_count = 0,
    };

    config_sink_t sink = {
        .user_data = &context,
        .on_enter_config = NULL,
        .on_exit_config = on_config_callback,
        .on_action_load = NULL,
    };

    term_begin(&window);

    parse_config_file("/config.yaml", &sink);

    unsigned int selected_config = 0;
    window_reverse(&window, window_xy(&window, 0, selected_config), 40);

    while (true) {
        term_display(&window);

        int ch = EOF;
        do {
            ch = term_getch();
        } while (ch == EOF);

        switch (ch) {
            case KEY_UP: {
                if (selected_config > 0) {
                    window_reverse(&window, window_xy(&window, 0, selected_config), 40);
                    selected_config--;
                    window_reverse(&window, window_xy(&window, 0, selected_config), 40);
                }
                break;
            }
            case KEY_DOWN: {
                if (selected_config < context.config_count - 1) {
                    window_reverse(&window, window_xy(&window, 0, selected_config), 40);
                    selected_config++;
                    window_reverse(&window, window_xy(&window, 0, selected_config), 40);
                }
                break;
            }
            case '\r':
            case '\n': {
                return load_config(selected_config);
            }
            default:
                break;
        }
    }
    
    term_end();
}
