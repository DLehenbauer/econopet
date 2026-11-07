// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "menu_config.h"

#include "config/config.h"
#include "diag/log/log.h"
#include "diag/mem.h"
#include "display/display.h"
#include "driver.h"
#include "fatal.h"
#include "ieee/ieee_drive.h"
#include "input.h"
#include "input_controls.h"
#include "menu_ui.h"
#include "menu_screen.h"
#include "pet.h"
#include "roms/roms.h"
#include "usb/keyboard.h"

#define MENU_CHARSET_TIMEOUT_US 100000u
// Allow at least one 60 Hz DVI frame for core 1 to select the new font.
#define MENU_CHARSET_SETTLE_US 20000u

typedef struct {
    size_t first, capacity, count;
    char (*names)[CONFIG_TEXT_CAPACITY];
    char default_id[CONFIG_TEXT_CAPACITY];
    int default_index;
} catalog_context_t;

// Saves the default ID for the catalog callbacks to match against configuration
// IDs. A default encountered after any catalog entry is fatal.
static void catalog_default(void* context, const char* id) {
    catalog_context_t* const ctx = context;
    vet(ctx->count == 0, "/config.yaml: default must appear before configs");
    snprintf(ctx->default_id, sizeof(ctx->default_id), "%s", id);
}

// Handles each catalog entry in file order: resolves a matching default ID,
// copies its display name if it falls in the requested page, and advances the
// total count. Catalogs that exceed the menu's integer index range are fatal.
static void catalog_config(void* context, const char* id, const char* name) {
    catalog_context_t* const ctx = context;
    vet(ctx->count < INT_MAX, "too many model configurations");
    if (ctx->default_id[0] != '\0' && strcmp(ctx->default_id, id) == 0)
        ctx->default_index = (int) ctx->count;
    if (ctx->names != NULL && ctx->count >= ctx->first
        && ctx->count - ctx->first < ctx->capacity)
        snprintf(ctx->names[ctx->count - ctx->first], CONFIG_TEXT_CAPACITY, "%s", name);
    ctx->count++;
}

// Reads /config.yaml without executing setup actions. Copies at most capacity
// display names starting at zero-based first, while counting the whole catalog
// and resolving its default in one pass. Pass NULL for names to skip copying.
// Returns the total count and default index (CONFIG_ENUMERATE if absent).
// Empty catalogs, unknown default IDs, and defaults after entries are fatal.
menu_config_catalog_t menu_config_enumerate(size_t first, size_t capacity,
                                           char names[][CONFIG_TEXT_CAPACITY]) {
    catalog_context_t context = {
        .first = first, .capacity = capacity, .names = names,
        .default_index = CONFIG_ENUMERATE
    };
    const config_sink_t sink = {
        .context = &context, .on_default = catalog_default, .on_exit_config = catalog_config
    };
    parse_config_file("/config.yaml", &sink, CONFIG_ENUMERATE);
    vet(context.default_id[0] == '\0' || context.default_index >= 0,
        "/config.yaml: unknown default '%s'", context.default_id);
    vet(context.count > 0, "/config.yaml: no model configurations");
    return (menu_config_catalog_t) {context.count, context.default_index};
}

// Reloads the selected configuration with the PET halted. Prefills $9000-$FFFF
// with each address's high byte to approximate an undriven bus, invokes setup
// callbacks in YAML order, and reads back the character ROM for DVI rendering.
// Applies mounts only for a newly selected configuration. The mount API ejects
// incompatible partners. Skips mounts when restoring the live machine after menu.rom.
// Returns with the CPU halted for the caller's subsequent reset.
void load_config(const setup_sink_t* const setup_sink, int selected_config, bool apply_mounts) {
    log_info("Loading config: %d", selected_config);

    set_cpu(CPU_HALT);
    if (apply_mounts)
        vet(ieee_drive_unmount_all(), "Failed to unmount IEEE disks");

    // In later PET/CBM models, reading from an unmapped address holds the previous byte
    // transferred on the data bus.  This has the effect of making it appear that unmapped
    // regions are filled with the high byte of the corresponding memory address:
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

    // Execute setup actions in YAML order, skipping mounts during restoration.
    const setup_sink_t actions = {
        .context = setup_sink->context,
        .on_load = setup_sink->on_load,
        .on_patch = setup_sink->on_patch,
        .on_copy = setup_sink->on_copy,
        .on_mount = apply_mounts ? setup_sink->on_mount : NULL,
        .on_set_options = setup_sink->on_set_options,
        .on_fix_checksum = setup_sink->on_fix_checksum,
        .system_state = setup_sink->system_state,
    };
    config_sink_t sink = {
        .context = NULL,
        .setup = &actions,
        .on_enter_config = NULL,
        .on_exit_config = NULL,
        .on_default = NULL,
    };

    parse_config_file("/config.yaml", &sink, selected_config);

    // Read back the configured character ROM for local DVI rendering.
    roms_refresh_char_rom();
}

// Services input and drive work from the menu loop and cooperative directory
// scans, allowing busy mount retries to progress.
static void menu_service(void) {
    input_task();
    ieee_drive_task();
}

// Wait for menu.rom to select lowercase before presenting menu characters.
static void menu_wait_for_charset(void) {
    const uint64_t start = time_us_64();
    while ((spi_read_at(ECONOPET_WB_STATUS_ADDR) & ECONOPET_REG_STATUS_TEXT_MODE_MASK) == 0) {
        vet(time_us_64() - start < MENU_CHARSET_TIMEOUT_US,
            "menu.rom did not select the lowercase character set");
        menu_service();
    }
    system_state.video_text_mode = video_text_mode_lower;

    // DVI selects its font during blank scanlines, not immediately on assignment.
    const uint64_t selected = time_us_64();
    while (time_us_64() - selected < MENU_CHARSET_SETTLE_US) {
        menu_service();
    }
}

// Runs the boot picker or on-demand dashboard after menu_enter() starts
// menu.rom. Reuses the applied configuration and browser state
// between sessions, reinitializing on boot, first use, or an invalid saved index.
// Auto-boots a configured default without input. Otherwise temporarily routes
// terminal input and video to the firmware menu, pulses NMI only on boot,
// and services input with demand-driven redraws until the controller exits.
// Restores terminal routing before applying the selected machine and disks.
// Returns with the CPU halted so menu_enter() can reset into the loaded machine.
void menu_config_show(const window_t* const window, const setup_sink_t* const setup_sink, bool is_boot) {
    static menu_ui_t menu;
    static bool initialized;
    const menu_config_catalog_t catalog = menu_config_enumerate(0, 0, NULL);
    if (is_boot || !initialized || (size_t) menu.config_index >= catalog.count) {
        menu_ui_init(&menu, window, menu_service);
        initialized = true;
    }

    // Boot directly into a configured default without input, painting, or the menu tune.
    if (is_boot && catalog.default_index >= 0) {
        load_config(setup_sink, menu.config_index, true);
        menu.config_changed = false;
        return;
    }

    // Temporarily route terminal input and video through the firmware menu.
    const term_mode_t mode = system_state.term_mode;
    const term_input_dest_t input = system_state.term_input_dest;
    system_state.term_mode = term_mode_video;
    system_state.term_input_dest = term_input_to_firmware;
    if (mode != term_mode_video) display_term_begin();
    keyboard_menu_set_active(true);

    // Show the menu as soon as the selected character set is ready.
    menu_wait_for_charset();

    // Keep menu.rom running while browsing configurations and changing live disks.
    menu_ui_begin(&menu, window, is_boot);
    // The power-on picker requests the diagnostic asynchronously. Runtime and auto-boot stay silent.
    if (is_boot) pet_nmi();
    // Keep input responsive, repainting only when the controller requests it.
    while (tui_controller_is_active(&menu.controller)) {
        if (tui_controller_needs_render(&menu.controller)) {
            tui_controller_render(&menu.controller);
            display_task();
        }
        menu_service();
        // Apply a burst in order before repainting, rather than transmitting a frame per key.
        int key;
        while (tui_controller_is_active(&menu.controller) && (key = input_getch()) != EOF) {
            if (key == 't' || key == 'T') {
                set_cpu(CPU_HALT);
                test_ram();
            } else {
                tui_controller_dispatch(&menu.controller, menu_ui_action_from_key(key));
            }
        }
    }

    // Do not replay held menu gestures or pending releases when returning to
    // runtime.
    keyboard_menu_set_active(false);
    input_controls_consume();

    // Blank the menu before configuration reload changes the character ROM or
    // reset changes the charset.
    set_cpu(CPU_HALT);
    menu_screen_clear();

    // Leave terminal-menu routing before setup callbacks select the target machine.
    if (mode != term_mode_video) display_term_end();
    system_state.term_mode = mode;
    system_state.term_input_dest = input;
    load_config(setup_sink, menu.config_index, menu.config_changed);
    menu.config_changed = false;
}
