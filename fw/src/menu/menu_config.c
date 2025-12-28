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
#include "diag/log/log.h"
#include "diag/mem.h"
#include "display/display.h"
#include "display/window.h"
#include "driver.h"
#include "global.h"
#include "input.h"

void load_config(const setup_sink_t* const setup_sink, int selected_config) {
    // Load the selected config
    log_info("Loading config: %d", selected_config);

    // Suspend the CPU while we're loading the config.
    set_cpu(/* ready: */ false, /* reset: */ false, /* nmi: */ false);

    // In later PET/CBM models, reading from an unmapped address holds the previous byte
    // transferred on the data bus.  This has the effect of making it appear that unmapped
    // regions are filled with the the high byte of the corresponding memory address:
    //
    //      .m 90f0 9108
    //      .:  90f0  90 90 90 90 90 90 90 90
    //      .:  90f8  90 90 90 90 90 90 90 90
    //      .:  9100  91 91 91 91 91 91 91 91
    //      .:  9108  91 91 91 91 91 91 91 91
    //
    // In the EconoPET, all "unmapped" memory regions fall through to RAM (or soft-ROM),
    // so we approximate this effect by prefilling $9000-$FFFF with the high byte of the
    // address.  The loaded config will overwrite the populated ROM regions.
    for (uint16_t a = 0x90; a <= 0xff; a++) {
        spi_fill(a << 8, a, 0x100);
    }

    config_sink_t sink = {
        .context = NULL,
        .setup = setup_sink,
        .on_enter_config = NULL,
        .on_exit_config = NULL,
    };
    
    parse_config_file("/config.yaml", &sink, selected_config);
}

typedef struct context_s {
    const window_t* const window;
    unsigned int config_count;
} context_t;

void on_config_callback(void* context, const char* name) {
    context_t* const ctx = (context_t*) context;
    window_puts(ctx->window, window_xy(ctx->window, 0, ctx->config_count++), name);
}

void menu_config_show(const window_t* const window, const setup_sink_t* const setup_sink) {
    context_t context = {
        .window = window,
        .config_count = 0,
    };

    config_sink_t sink = {
        .context = &context,
        .setup = NULL,
        .on_enter_config = NULL,
        .on_exit_config = on_config_callback,
    };

    // Fill window with spaces
    window_fill(window, 0x20);

    parse_config_file("/config.yaml", &sink, -1);

    unsigned int selected_config = 0;
    window_reverse(window, window_xy(window, 0, selected_config), 40);

    while (true) {
        // Sync display (writes buffer to PET RAM and terminal)
        display_task();

        int ch = EOF;
        do {
            input_task();
            ch = input_getch();
        } while (ch == EOF);

        switch (ch) {
            case KEY_UP: {
                window_reverse(window, window_xy(window, 0, selected_config), 40);
                selected_config = (selected_config + context.config_count - 1) % context.config_count;
                window_reverse(window, window_xy(window, 0, selected_config), 40);
                break;
            }
            case KEY_DOWN: 
            case KEY_BTN_SHORT: {
                window_reverse(window, window_xy(window, 0, selected_config), 40);
                selected_config = (selected_config + 1) % context.config_count;
                window_reverse(window, window_xy(window, 0, selected_config), 40);
                break;
            }
            case '\r':
            case '\n':
            case KEY_BTN_LONG: {
                return load_config(setup_sink, selected_config);
            }
            case 'T':
            case 't': {
                set_cpu(/*ready: */ false, /* reset:*/ false, /* nmi: */ false);
                test_ram();
            }

            default:
                break;
        }
    }
}
