// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "menu_config.h"

#include "config/config.h"
#include "diag/log/log.h"
#include "display/display.h"
#include "driver.h"
#include "fatal.h"
#include "ieee/ieee_drive.h"
#include "input/input.h"
#include "input/input_controls.h"
#include "input/input_routing.h"
#include "input/pet_keyboard.h"
#include "menu_screen.h"
#include "menu_session.h"
#include "menu_ui.h"
#include "pet.h"
#include "roms/roms.h"
#include "usb/keyboard.h"

#define MENU_CHARSET_TIMEOUT_US 100000u
// Allow at least one 60 Hz DVI frame for core 1 to select the new font.
#define MENU_CHARSET_SETTLE_US 20000u
#define MENU_INPUT_POLL_US 100u

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

// Return an explicit picker/dashboard exit without loading configuration or resetting.
// Reuse catalog/browser state, preserve terminal ownership, and keep input/drives alive.
// Boot defaults bypass painting and the tune. Runtime scanning requires an active session.
menu_result_t menu_config_run(const window_t* const window, bool is_boot) {
    static menu_ui_t menu;
    static bool initialized;
    const menu_config_catalog_t catalog = menu_config_enumerate(0, 0, NULL);
    if (is_boot || !initialized || (size_t) menu.config_index >= catalog.count) {
        menu_ui_init(&menu, window, menu_service);
        initialized = true;
    }

    // Boot directly into a configured default without input, painting, or the menu tune.
    if (is_boot && catalog.default_index >= 0) {
        menu.config_changed = false;
        return (menu_result_t){MENU_EXIT_LOAD_CONFIG, menu.config_index};
    }

    // Finish boot diagnostics before firmware takes the physical keyboard bus.
    if (is_boot) {
        log_info("diagnostic ROM: happy tune");
        spi_write_at(MENU_ROM_COMPLETION_ADDRESS, 0);
        pet_nmi();
        menu_rom_wait(menu_service);
        vet(cpu_suspend(true), "could not suspend boot diagnostics");
        pet_keyboard_begin_boot();
    }

    // Route remote keys to the menu without taking ownership of the terminal.
    while (pet_keyboard_active() && !pet_keyboard_ready()) {
        menu_service();
        sleep_us(MENU_INPUT_POLL_US);
    }
    input_routing_set_menu_active(true);

    // Show the menu as soon as the selected character set is ready.
    if (!menu_session_active()) {
        menu_wait_for_charset();
    }

    // Render the firmware UI without executing the diagnostic or guest CPU.
    menu_ui_begin(&menu, window, is_boot);
    // Keep input responsive, repainting only when the controller requests it.
    while (tui_controller_is_active(&menu.controller)) {
        if (tui_controller_needs_render(&menu.controller)) {
            tui_controller_render(&menu.controller);
            display_task();
        }
        menu_service();
        const input_system_request_t request = input_routing_take_request();
        if (request.action == input_system_request_reset) {
            menu.exit = MENU_EXIT_RESET;
            menu.controller.active = false;
        }
        // Apply a burst in order before repainting, rather than transmitting a frame per key.
        tui_action_t action;
        while (tui_controller_is_active(&menu.controller)
            && (action = input_routing_navigation()) != tui_action_none) {
            tui_controller_dispatch(&menu.controller, action);
        }
    }

    // Suppress menu gestures and require fresh physical release before handing back the bus.
    if (pet_keyboard_active()) {
        pet_keyboard_wait_release_begin();
    }
    while (pet_keyboard_active() && !pet_keyboard_released()) {
        menu_service();
        sleep_us(MENU_INPUT_POLL_US);
    }
    input_routing_set_menu_active(false);
    menu.config_changed = false;
    return (menu_result_t){menu.exit, menu.config_index};
}

// Apply the boot picker's configuration. Runtime ownership uses menu_config_run() directly.
void menu_config_show(const window_t* const window, const setup_sink_t* const setup_sink, bool is_boot) {
    const menu_result_t result = menu_config_run(window, is_boot);
    if (is_boot) {
        pet_keyboard_end();
        set_cpu(CPU_RESET);
        vet(cpu_suspend(false), "could not release boot diagnostics");
    }

    // Blank the menu before configuration reload changes the character ROM or
    // reset changes the charset.
    set_cpu(CPU_HALT);
    menu_screen_clear();

    load_config(setup_sink, result.config_index, result.action == MENU_EXIT_LOAD_CONFIG);
}
