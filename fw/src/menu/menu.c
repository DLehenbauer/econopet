// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "menu.h"

#include "cpu.h"
#include "diag/log/log.h"
#include "display/char_encoding.h"
#include "display/display.h"
#include "display/dvi/dvi.h"
#include "display/window.h"
#include "driver.h"
#include "fatal.h"
#include "global.h"
#include "hw.h"
#include "ieee/ieee_drive.h"
#include "input/input.h"
#include "input/pet_keyboard.h"
#include "menu_config.h"
#include "pet.h"
#include "roms/checksum.h"
#include "roms/roms.h"
#include "sd/sd.h"
#include "system/state.h"
#include "system/system.h"
#include "tape.h"
#include "usb/keymap.h"
#include "usb/keyboard.h"

// When navigating the SD card's file system, this is the maximum number
// of we will display to the user/cache in memory.
#define MAX_FILE_SLOTS 25

/** Service cooperative work while the owner prepares boot input. */
static void menu_prepare_service(void) {
    input_task();
    ieee_drive_task();
}

/**
 * Stream a 2-byte-prefixed .prg file into PET base SRAM and update BASIC end pointers.
 *
 * Rejects unreadable or empty files and refuses to overwrite the caller's reserved SRAM.
 */
bool load_prg_bounded(const char* filename, const uint16_t end_limit) {
    vet(end_limit <= VIDEO_RAM_ADDRESS, "PRG load limit must not exceed base SRAM");
    // Open the .prg image for binary reading on the mounted SD card.
    FILE* const file = fopen(filename, "rb");
    if (file == NULL) {
        return false;
    }

    // Read the 2-byte little-endian load address header.
    uint8_t header[2];
    if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
        fclose(file);
        return false;
    }
    uint32_t dest = (uint32_t)header[0] | ((uint32_t)header[1] << 8);

    // Stream the program body into PET SRAM without crossing the exclusive destination ceiling.
    uint8_t* buffer = acquire_temp_buffer();
    size_t bytes_read;
    size_t total_bytes = 0;
    bool ok = true;

    while ((bytes_read = fread(buffer, 1, TEMP_BUFFER_SIZE, file)) > 0) {
        if (dest + bytes_read > end_limit) {
            ok = false;
            break;
        }
        spi_write(dest, buffer, bytes_read);
        dest += (uint32_t)bytes_read;
        total_bytes += bytes_read;
    }
    if (ferror(file) != 0 || total_bytes == 0) {
        ok = false;
    }

    // Release temporary storage and close the file before touching zero-page state on error.
    release_temp_buffer(&buffer);
    if (fclose(file) != 0) {
        ok = false;
    }
    if (!ok) {
        return false;
    }

    // Advance BASIC VARTAB/ARYTAB/STREND ($2A..$2F) and KERNAL EAL/EAH ($C9..$CA) past the loaded bytes.
    const uint16_t end_addr = (uint16_t)dest;
    for (uint16_t zp = 0x2au; zp <= 0x2fu; zp += 2u) {
        spi_write_at(zp, (uint8_t)(end_addr & 0xffu));
        spi_write_at(zp + 1u, (uint8_t)(end_addr >> 8));
    }
    spi_write_at(0xc9u, (uint8_t)(end_addr & 0xffu));
    spi_write_at(0xcau, (uint8_t)(end_addr >> 8));
    return true;
}

/** Load an ordinary PRG with the full base SRAM available below video RAM. */
bool load_prg(const char* filename) {
    return load_prg_bounded(filename, VIDEO_RAM_ADDRESS);
}

typedef struct action_context_s {
    system_state_t* system_state;
    bool diagnostic;
    bool load_failed;
    char* error_out;
    size_t error_size;
} action_context_t;

typedef enum {
    action_load_success,
    action_load_open_failed,
    action_load_read_failed,
} action_load_result_t;

/** Return whether a prior diagnostic ROM load failure should suppress later setup actions. */
static bool action_context_should_skip(const action_context_t* const context) {
    return context->diagnostic && context->load_failed;
}

/** Stream one configuration ROM into PET memory and report recoverable file errors. */
static action_load_result_t load_config_file(const char* const filename, uint32_t address, uint8_t* const checksum_out) {
    FILE* const file = sd_open(filename, "rb");
    if (file == NULL) {
        return action_load_open_failed;
    }

    uint8_t* temp_buffer = acquire_temp_buffer();
    size_t bytes_read;
    uint8_t checksum = 0;
    while ((bytes_read = fread(temp_buffer, 1, TEMP_BUFFER_SIZE, file)) > 0) {
        checksum_add(temp_buffer, bytes_read, &checksum);
        spi_write(address, temp_buffer, bytes_read);
        address += bytes_read;
    }
    const bool read_failed = ferror(file) != 0;
    release_temp_buffer(&temp_buffer);
    const bool close_failed = fclose(file) != 0;
    if (read_failed || close_failed) {
        return action_load_read_failed;
    }

    *checksum_out = checksum;
    return action_load_success;
}

void action_load(void* const context, const char* filename, uint32_t address) {
    action_context_t* const action_context = context;
    if (action_context_should_skip(action_context)) {
        return;
    }

    log_debug("0x%04" PRIx32, address);
    uint8_t checksum;
    const action_load_result_t result = load_config_file(filename, address, &checksum);
    if (result != action_load_success) {
        if (action_context->diagnostic) {
            action_context->load_failed = true;
            if (result == action_load_open_failed) {
                snprintf(action_context->error_out, action_context->error_size, "Failed to open file '%s'", filename);
            } else {
                snprintf(action_context->error_out, action_context->error_size, "Failed to read file '%s'", filename);
            }
            return;
        }
        if (result == action_load_open_failed) {
            fatal("Failed to open file '%s'", filename);
        }
        fatal("Failed to read file '%s'", filename);
    }

    log_debug("-%04" PRIx32 ": %s ($%02x)", address - 1, filename, checksum);
}

void action_patch(void* context, uint32_t address, const binary_t* binary) {
    if (action_context_should_skip(context)) {
        return;
    }

    log_debug("0x%04" PRIx32 ": patching %zu bytes", address, binary->size);
    spi_write(address, binary->data, binary->size);
}

void action_copy(void* context, uint32_t source, uint32_t destination, uint32_t length) {
    if (action_context_should_skip(context)) {
        return;
    }

    log_debug("0x%04" PRIx32 ": copying %" PRIu32 " bytes from 0x%04" PRIx32, source, length, destination);
    uint8_t* temp_buffer = acquire_temp_buffer();

    if (destination > source) {
        while (length > 0) {
            size_t chunk_size = MIN(length, TEMP_BUFFER_SIZE);
            spi_read(source + length - chunk_size, chunk_size, temp_buffer);
            spi_write(destination + length - chunk_size, temp_buffer, chunk_size);
            length -= chunk_size;
        }
    } else {
        uint32_t offset = 0;
        while (offset < length) {
            size_t chunk_size = MIN(length - offset, TEMP_BUFFER_SIZE);
            spi_read(source + offset, chunk_size, temp_buffer);
            spi_write(destination + offset, temp_buffer, chunk_size);
            offset += chunk_size;
        }
    }

    release_temp_buffer(&temp_buffer);
}

void action_set_options(void* context, options_t* options) {
    action_context_t* const ctx = context;
    if (action_context_should_skip(ctx)) {
        return;
    }

    switch (options->columns) {
        case 40:
            ctx->system_state->pet_display_columns = pet_display_columns_40;
            break;
        default:
            vet(
                options->columns == 80,
                "Invalid 'columns:' in config (got %lu)",
                (unsigned long) options->columns
            );
            ctx->system_state->pet_display_columns = pet_display_columns_80;
            break;
    }

    // Validate and set video RAM mask (must be 0-3)
    vet(options->video_ram_mask <= pet_video_ram_mask_4kb,
        "Invalid video RAM mask in config (got %" PRIu32 ", expected 0-3)", options->video_ram_mask);
    system_state_set_video_ram_mask(ctx->system_state, (uint8_t) options->video_ram_mask);

    write_pet_model(ctx->system_state);

    tape_init(options->tape_enabled ? &options->tape : NULL);

    // CPU is halted here (load_config left it not-ready) and re-reset by
    // pet_reset().
    cpu_type_t cpu = (cpu_type_t) options->cpu;
    if (cpu == CPU_AUTO) {
        cpu = physical_cpu_present() ? CPU_PHYS_6502 : CPU_SOFT_6502;
    }
    set_cpu_type_machine(cpu, options->superpet_io || cpu == CPU_SOFT_6809);

    log_debug("Set options: %" PRIu32 " columns, video RAM mask %" PRIu32, options->columns, options->video_ram_mask);
}

// Mounts a configured slot, ejecting an incompatible partner only on success.
void action_mount(void* context, uint32_t device, uint32_t drive, const char* filename) {
    if (action_context_should_skip(context)) {
        return;
    }
    const unsigned int slot = (device - 8) * DOS_MEDIA_SLOTS_PER_UNIT + drive;
    const dos_media_result_t result = ieee_drive_try_mount(slot, filename, NULL);
    vet(result == DOS_MEDIA_OK, "IEEE device %lu drive %lu could not mount '%s' (error %u)",
        (unsigned long)device, (unsigned long)drive, filename, (unsigned int)result);
}

void menu_init(void) {
    read_pet_model(&system_state);
    usb_keymap_reset(&system_state);
}

static uint8_t checksum_ram(uint32_t start_addr, uint32_t end_addr) {
    uint8_t checksum = 0;
    uint8_t* temp_buffer = acquire_temp_buffer();
    
    uint32_t addr = start_addr;
    uint32_t remaining_bytes = end_addr - start_addr;

    while (remaining_bytes > 0) {
        size_t chunk_size = MIN(remaining_bytes, TEMP_BUFFER_SIZE);
        spi_read(addr, chunk_size, temp_buffer);
        checksum_add(temp_buffer, chunk_size, &checksum);
        
        addr += chunk_size;
        remaining_bytes -= chunk_size;
    }

    release_temp_buffer(&temp_buffer);
    return checksum;
}

void action_fix_checksum(void* context, uint32_t start_addr, uint32_t end_addr, uint32_t fix_addr, uint32_t expected) {
    if (action_context_should_skip(context)) {
        return;
    }

    uint8_t actual_sum = checksum_ram(start_addr, end_addr + 1);

    if (actual_sum != expected) {
        uint8_t current_byte = spi_read_at(fix_addr);
        uint8_t adjusted_byte = checksum_fix(current_byte, actual_sum, expected);

        log_debug("0x%04" PRIx32 ": fixing checksum for $%04" PRIx32 "-%04" PRIx32 " ($%02x -> $%02" PRIx32 ")",
                  fix_addr, start_addr, end_addr, actual_sum, expected);

        spi_write_at(fix_addr, adjusted_byte);

        assert(checksum_ram(start_addr, end_addr + 1) == expected);
    }
}

/**
 * Sanitize YAML set-action options for a diagnostic target run so virtual tape,
 * 6809 selection, and SuperPET I/O cannot interfere with 6502 target control.
 */
static void action_set_diag_options(void* const context, options_t* const options) {
    // Copy the parsed options and force stock 6502 PET behavior with virtual tape disabled.
    options_t sanitized = *options;
    sanitized.tape_enabled = false;
    sanitized.cpu = CPU_AUTO;
    sanitized.superpet_io = false;
    action_set_options(context, &sanitized);
}

/** Configuration setup policy, separate from the owner's final reset transition. */
typedef enum {
    CONFIG_PREPARE_GUEST,
    CONFIG_PREPARE_DIAGNOSTIC,
} config_prepare_policy_t;

/** Load shared configuration actions with the selected mount and target-CPU policy. */
static bool prepare_config(const int index, const config_prepare_policy_t policy, char* const error_out,
                           const size_t error_size) {
    // Bind common setup actions while isolating diagnostic mounts and option sanitization.
    const bool diagnostic = policy == CONFIG_PREPARE_DIAGNOSTIC;
    action_context_t action_context = {
        .system_state = &system_state,
        .diagnostic = diagnostic,
        .error_out = error_out,
        .error_size = error_size,
    };
    const setup_sink_t setup_sink = {
        .context = &action_context,
        .on_load = action_load,
        .on_patch = action_patch,
        .on_copy = action_copy,
        .on_mount = diagnostic ? NULL : action_mount,
        .on_set_options = diagnostic ? action_set_diag_options : action_set_options,
        .on_fix_checksum = action_fix_checksum,
        .system_state = &system_state,
    };

    // Reset shared input state and establish diagnostic CPU selection before profile actions run.
    tape_deinit();
    usb_keymap_reset(&system_state);
    if (diagnostic) {
        set_cpu(CPU_HALT);
        set_cpu_type_machine(physical_cpu_present() ? CPU_PHYS_6502 : CPU_SOFT_6502, false);
    }
    load_config(&setup_sink, index);
    if (action_context.load_failed) {
        return false;
    }
    spi_write_at(ECONOPET_WB_AUDIO_ADDR, ECONOPET_REG_AUDIO_ENABLE_MASK);
    return true;
}

/** Prepare the selected normal configuration and transition its owner back to the guest. */
static void apply_config(const int config_index) {
    (void)prepare_config(config_index, CONFIG_PREPARE_GUEST, NULL, 0);
    system_reset_guest();
}

/**
 * Load a /config.yaml profile by ID for a diagnostic target without user mounts
 * or guest-mode transition, leaving the 6502 reset and halted.
 */
bool menu_apply_diag_config(const char* const id, char* const error_out, const size_t error_size) {
    vet(error_out != NULL && error_size != 0, "diagnostic config error buffer is required");
    error_out[0] = '\0';

    // Resolve the configuration's zero-based catalog index before mutating hardware state.
    const int index = menu_config_find_id(id);
    if (index < 0) {
        snprintf(error_out, error_size, "missing config '%s' in /config.yaml", id);
        return false;
    }

    // Load shared actions without replacing scratch mounts or accepting non-6502 options.
    if (!prepare_config(index, CONFIG_PREPARE_DIAGNOSTIC, error_out, error_size)) {
        return false;
    }

    // Pulse PET hardware reset (parking IEEE pins if enabled) and hold the CPU halted in diagnostic mode.
    pet_reset();
    set_cpu(CPU_HALT);
    return true;
}

void menu_boot(void) {
    log_info("-- Enter Boot Menu --");
    const menu_config_catalog_t catalog = menu_config_enumerate(0, 0, NULL);
    if (catalog.default_index >= 0) {
        apply_config(catalog.default_index);
        return;
    }

    tape_deinit();
    display_clear();
    start_menu_rom(MENU_ROM_BOOT_NORMAL);
    menu_rom_wait(menu_prepare_service);
    if (!system_suspend()) {
        fatal("could not suspend boot system for menu");
    }

    const window_t window = display_window();
    const menu_result_t result = menu_config_run(&window, menu_ui_root_picker);
    system_restore(false);
    if (result.action == MENU_EXIT_LOAD_CONFIG) {
        apply_config(result.config_index);
    } else {
        fatal("boot menu returned without selecting a configuration");
    }
}

void menu_enter(void) {
    log_info("-- Enter Menu --");
    if (!system_suspend()) {
        log_warn("menu entry unavailable");
        return;
    }

    const window_t window = display_window();

    const menu_result_t result = menu_config_run(&window, menu_ui_root_dashboard);
    system_restore(result.action == MENU_EXIT_RESUME);
    if (result.action == MENU_EXIT_LOAD_CONFIG) {
        apply_config(result.config_index);
    } else if (result.action == MENU_EXIT_RESET) {
        system_reset_guest();
    }

    log_info("-- Exit Menu --");
}
