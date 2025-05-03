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
#include "../term.h"
#include "window.h"

typedef struct context2_s {
    unsigned int skip_count;
} context2_t;

void on_enter_config_callback(void* user_data) {
    context2_t* const ctx = (context2_t*) user_data;
    ctx->skip_count--;
}

void on_action_load_callback(void* user_data, const char* file, uint32_t address) {
    context2_t* const ctx = (context2_t*) user_data;
    if (ctx->skip_count == 0) {
        // Load the file at the specified address
        // This is a placeholder for the actual loading logic
        printf("Loading file: %s at address: %5lx\n", file, address);
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
    
    parse_config_file("data/config.yaml", &sink);
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

    parse_config_file("data/config.yaml", &sink);

    unsigned int selected_config = 0;
    window_reverse(&window, window_xy(&window, 0, selected_config), 40);

    term_begin();


    while (true) {
        term_display(buffer, cols, rows);

        switch (term_getch()) {
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
            case '\n': {
                return load_config(selected_config);
            }
            default:
                break;
        }
    }
    
    term_end();
}
