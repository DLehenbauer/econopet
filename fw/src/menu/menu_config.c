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
#include "diag/mem.h"
#include "driver.h"
#include "term.h"
#include "global.h"
#include "window.h"

typedef struct setup_context_s {
    const setup_sink_t* const original_setup;
    unsigned int skip_count;
} setup_context_t;

static void on_enter_config_callback(void* context) {
    setup_context_t* const ctx = (setup_context_t*) context;
    ctx->skip_count--;
}

static void on_action_load_callback(void* context, const char* filename, uint32_t address) {
    const setup_context_t* const ctx = (setup_context_t*) context;
    if (ctx->skip_count != 0xffffffff) {
        return;
    }

    const setup_sink_t* const setup = ctx->original_setup;
    if (setup->on_load != NULL) {
        setup->on_load(setup->context, filename, address);
    }
}

static void on_action_patch_callback(void* context, uint32_t address, const binary_t* binary) {
    const setup_context_t* const ctx = (setup_context_t*) context;
    if (ctx->skip_count != 0xffffffff) {
        return;
    }

    const setup_sink_t* const setup = ctx->original_setup;
    if (setup->on_patch != NULL) {
        setup->on_patch(setup->context, address, binary);
    }
}

static void on_action_copy_callback(void* context, uint32_t source, uint32_t destination, uint32_t length) {
    const setup_context_t* const ctx = (setup_context_t*) context;
    if (ctx->skip_count != 0xffffffff) {
        return;
    }

    const setup_sink_t* const setup = ctx->original_setup;
    if (setup->on_copy != NULL) {
        setup->on_copy(setup->context, source, destination, length);
    }
}

static void on_action_set_callback(void* context, options_t* options) {
    const setup_context_t* const ctx = (setup_context_t*) context;
    if (ctx->skip_count != 0xffffffff) {
        return;
    }

    const setup_sink_t* const setup = ctx->original_setup;
    if (setup->on_set_options != NULL) {
        setup->on_set_options(setup->context, options);
    }
}

static void on_action_set_keymap_callback(void* context, const char* filename) {
    const setup_context_t* const ctx = (setup_context_t*) context;
    if (ctx->skip_count != 0xffffffff) {
        return;
    }

    const setup_sink_t* const setup = ctx->original_setup;
    if (setup->on_set_keymap != NULL) {
        setup->on_set_keymap(setup->context, filename);
    }
}

static void on_action_fix_checksum_callback(void* context, uint32_t start_addr, uint32_t end_addr, uint32_t fix_addr, uint32_t checksum) {
    const setup_context_t* const ctx = (setup_context_t*) context;
    if (ctx->skip_count != 0xffffffff) {
        return;
    }

    const setup_sink_t* const setup = ctx->original_setup;
    if (setup->on_fix_checksum != NULL) {
        setup->on_fix_checksum(setup->context, start_addr, end_addr, fix_addr, checksum);
    }
}

void load_config(const setup_sink_t* const setup_sink, int selected_config) {
    // Load the selected config
    printf("Loading config: %d\n", selected_config);

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
    // address.  The loaded config will overwrite the populate ROM regions.
    for (uint16_t a = 0x90; a <= 0xff; a++) {
        spi_fill(a << 8, a, 0x100);
    }

    setup_context_t ctx = {
        .original_setup = setup_sink,
        .skip_count = selected_config,
    };

    setup_sink_t intermediate_setup_sink = {
        .context = &ctx,
        .on_load = on_action_load_callback,
        .on_patch = on_action_patch_callback,
        .on_copy = on_action_copy_callback,
        .on_set_options = on_action_set_callback,
        .on_set_keymap = on_action_set_keymap_callback,
        .on_fix_checksum = on_action_fix_checksum_callback,
        .model = setup_sink->model
    };

    config_sink_t sink = {
        .context = &ctx,
        .setup = &intermediate_setup_sink,
        .on_enter_config = on_enter_config_callback,
        .on_exit_config = NULL,
    };
    
    parse_config_file("/config.yaml", &sink);
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

    term_begin(window);
    term_display(window);

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
            case 'T':
            case 't': {
                set_cpu(/*ready: */ false, /* reset:*/ false, /* nmi: */ false);
                test_ram();
            }

            default:
                break;
        }
    }
    
    term_end();
}
