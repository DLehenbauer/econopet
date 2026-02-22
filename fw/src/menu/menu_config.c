// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "menu_config.h"

#include "config/config.h"
#include "diag/log/log.h"
#include "diag/mem.h"
#include "display/display.h"
#include "display/window.h"
#include "driver.h"
#include "fatal.h"
#include "global.h"
#include "input.h"
#include "pet.h"
#include "roms/roms.h"

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
        .on_default = NULL,
    };
    
    parse_config_file("/config.yaml", &sink, selected_config);
}

typedef struct context_s {
    const window_t* const window;
    unsigned int config_count;
    char default_id[41];
    int default_index;
} context_t;

static void on_default_callback(void* context, const char* id) {
    context_t* const ctx = (context_t*) context;
    strncpy(ctx->default_id, id, sizeof(ctx->default_id) - 1);
    ctx->default_id[sizeof(ctx->default_id) - 1] = '\0';
}

static void on_config_callback(void* context, const char* id, const char* name) {
    context_t* const ctx = (context_t*) context;
    window_puts(ctx->window, window_xy(ctx->window, 0, ctx->config_count), name);

    if (ctx->default_id[0] != '\0' && strcmp(id, ctx->default_id) == 0) {
        ctx->default_index = (int) ctx->config_count;
    }

    ctx->config_count++;
}

void menu_config_show(const window_t* const window, const setup_sink_t* const setup_sink, bool is_boot) {
    context_t context = {
        .window = window,
        .config_count = 0,
        .default_id = { 0 },
        .default_index = -1,
    };

    config_sink_t sink = {
        .context = &context,
        .setup = NULL,
        .on_enter_config = NULL,
        .on_exit_config = on_config_callback,
        .on_default = on_default_callback,
    };

    // Fill window with spaces
    window_fill(window, 0x20);

    parse_config_file("/config.yaml", &sink, -1);

    bool has_default = context.default_id[0] != '\0';
    bool default_matched = has_default && context.default_index >= 0;

    vet(!has_default || default_matched,
        "/config.yaml: unknown default '%s'", context.default_id);

    // If the machine was powered on and a default config was specified, automatically
    // boot it without showing the menu.
    if (is_boot && default_matched) {
        log_info("Auto-booting default config: '%s' (index %d)", context.default_id, context.default_index);
        return load_config(setup_sink, context.default_index);
    }

    // Play the happy boot tune via NMI now that we know we are showing the menu.
    pet_nmi();

    // Pre-select the default config (if found) or the first config
    unsigned int selected_config = default_matched
        ? (unsigned int) context.default_index
        : 0;
    
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
