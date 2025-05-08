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

#include "config/config.h"
#include "driver.h"
#include "term.h"
#include "global.h"
#include "window.h"

typedef struct setup_context_s {
    const setup_sink_t* const setup_sink;
    unsigned int skip_count;
} setup_context_t;

void on_enter_config_callback(void* user_data) {
    setup_context_t* const ctx = (setup_context_t*) user_data;
    ctx->skip_count--;
}

void on_action_load_callback(void* user_data, const char* filename, uint32_t address) {
    setup_context_t* const ctx = (setup_context_t*) user_data;
    if (ctx->skip_count != 0xffffffff) {
        return;
    }

    if (ctx->setup_sink->on_action_load != NULL) {
        ctx->setup_sink->on_action_load(filename, address);
    }
}

void load_config(const setup_sink_t* const setup_sink, int selected_config) {
    // Load the selected config
    // This is a placeholder for the actual loading logic
    printf("Loading config: %d\n", selected_config);

    setup_context_t ctx = {
        .setup_sink = setup_sink,
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
    const window_t* const window;
    unsigned int config_count;
} context_t;

void on_config_callback(void* user_data, const char* name) {
    context_t* const ctx = (context_t*) user_data;
    window_puts(ctx->window, window_xy(ctx->window, 0, ctx->config_count++), name);
}

void menu_config_show(const window_t* const window, const setup_sink_t* const setup_sink) {
    context_t context = {
        .window = window,
        .config_count = 0,
    };

    config_sink_t sink = {
        .user_data = &context,
        .on_enter_config = NULL,
        .on_exit_config = on_config_callback,
        .on_action_load = NULL,
        .window = window,
    };

    term_begin(window);

    parse_config_file("/config.yaml", &sink);

    unsigned int selected_config = 0;
    window_reverse(window, window_xy(window, 0, selected_config), 40);

    while (true) {
        term_display(window);

        int ch = EOF;
        do {
            ch = term_getch();
        } while (ch == EOF);

        switch (ch) {
            case KEY_UP: {
                if (selected_config > 0) {
                    window_reverse(window, window_xy(window, 0, selected_config), 40);
                    selected_config--;
                    window_reverse(window, window_xy(window, 0, selected_config), 40);
                }
                break;
            }
            case KEY_DOWN: {
                if (selected_config < context.config_count - 1) {
                    window_reverse(window, window_xy(window, 0, selected_config), 40);
                    selected_config++;
                    window_reverse(window, window_xy(window, 0, selected_config), 40);
                }
                break;
            }
            case '\r':
            case '\n': {
                return load_config(setup_sink, selected_config);
            }
            default:
                break;
        }
    }
    
    term_end();
}
