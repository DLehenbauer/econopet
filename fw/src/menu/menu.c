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
#include "usb/keyboard.h"

// When navigating the SD card's file system, this is the maximum number
// of we will display to the user/cache in memory.
#define MAX_FILE_SLOTS 25

const uint screen_width  = 40;
const uint screen_height = 25;
/** Service cooperative work while the owner prepares boot input. */
static void menu_prepare_service(void) {
    input_task();
    ieee_drive_task();
}

#define CHUNK_SIZE 2048

void load_prg(const char *filename) {
    FILE *file = sd_open(filename, "rb"); // Open file in binary read mode
    if (!file) {
        perror("Failed to open file");
        return;
    }

    // First two bytes contain the destination address
    uint16_t dest;
    if (fread(&dest, 1, sizeof(dest), file) != sizeof(dest)) {
        return;
    }

    // Read remaining bytes to the destination address.
    uint8_t buffer[CHUNK_SIZE];
    size_t bytes_read;
    size_t total_bytes = 0;

    while ((bytes_read = fread(buffer, 1, CHUNK_SIZE, file)) > 0) {
        spi_write(dest, buffer, bytes_read);
        dest += bytes_read;
        total_bytes += bytes_read;

        // Check for read errors (other than EOF)
        if (bytes_read < CHUNK_SIZE && ferror(file)) {
            perror("Error reading file");
            break;
        }
    }

    fclose(file);  // Close the file

    // TODO: Review fixup of BASIC pointers
    uint16_t size = total_bytes + 0x3FF;
    spi_write_at(0xc9, size & 0xFF);
    spi_write_at(0x2a, size & 0xFF);
    spi_write_at(0xca, size >> 8);
    spi_write_at(0x2b, size >> 8);
}

typedef struct action_context_s {
    system_state_t* system_state;
} action_context_t;

void action_load(void* const context, const char* filename, uint32_t address) {
    (void) context;

    log_debug("0x%04" PRIx32, address);
    FILE *file = sd_open(filename, "rb"); // Open file in binary read mode
    if (!file) {
        fatal("Failed to open file '%s'", filename);
    }

    // Read remaining bytes to the destination address.
    uint8_t* temp_buffer = acquire_temp_buffer();
    size_t bytes_read;
    uint8_t checksum = 0;

    while ((bytes_read = fread(temp_buffer, 1, TEMP_BUFFER_SIZE, file)) > 0) {
        checksum_add(temp_buffer, bytes_read, &checksum);
        spi_write(address, temp_buffer, bytes_read);
        address += bytes_read;

        // Check for read errors (other than EOF)
        if (bytes_read < TEMP_BUFFER_SIZE && ferror(file)) {
            fatal("Failed to read file '%s'", filename);
        }
    }

    log_debug("-%04" PRIx32 ": %s ($%02x)", address - 1, filename, checksum);

    release_temp_buffer(&temp_buffer);
    fclose(file);
}

void action_patch(void* context, uint32_t address, const binary_t* binary) {
    (void) context;

    log_debug("0x%04" PRIx32 ": patching %zu bytes", address, binary->size);
    spi_write(address, binary->data, binary->size);
}

void action_copy(void* context, uint32_t source, uint32_t destination, uint32_t length) {
    (void)context;

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
    action_context_t* ctx = (action_context_t*) context;

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

    // Load USB keymap if specified
    if (options->usb_keymap[0] != '\0') {
        read_keymap(options->usb_keymap, ctx->system_state);
    }

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
    (void)context;
    const unsigned int slot = (device - 8) * DOS_MEDIA_SLOTS_PER_UNIT + drive;
    const dos_media_result_t result = ieee_drive_try_mount(slot, filename);
    vet(result == DOS_MEDIA_OK, "IEEE device %lu drive %lu could not mount '%s' (error %u)",
        (unsigned long)device, (unsigned long)drive, filename, (unsigned int)result);
}

void read_keymap_callback(size_t offset, uint8_t* buffer, size_t bytes_read, void* context) {
    memcpy(context + offset, buffer, bytes_read);
}

void read_keymap(const char* filename, system_state_t* config) {
    // Read the keymap file and store it in the configuration.
    void* keymap = (void*) config->usb_keymap_data[0];
    sd_read_file(filename, read_keymap_callback, keymap, sizeof(config->usb_keymap_data));
    log_debug("USB keymap: '%s'", filename);
}

void action_set_keymap(void* context, const char* filename) {
    action_context_t* ctx = (action_context_t*) context;
    read_keymap(filename, ctx->system_state);
}

/** Start each configuration with the default before its ordered keymap overrides. */
static void read_default_keymap(void) {
    char keymap_path[SD_PATH_MAX];
    sd_make_path(keymap_path, SD_DIR_UKM, "us.bin");
    read_keymap(keymap_path, &system_state);
}

void menu_init(void) {
    read_pet_model(&system_state);
    read_default_keymap();
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
    (void) context;

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

static void apply_config(const int config_index) {
    action_context_t action_context = {
        .system_state = &system_state,
    };
    const setup_sink_t setup_sink = {
        .context = &action_context,
        .on_load = action_load,
        .on_patch = action_patch,
        .on_copy = action_copy,
        .on_mount = action_mount,
        .on_set_options = action_set_options,
        .on_fix_checksum = action_fix_checksum,
        .system_state = &system_state,
    };

    tape_deinit();
    read_default_keymap();
    load_config(&setup_sink, config_index);
    spi_write_at(ECONOPET_WB_AUDIO_ADDR, ECONOPET_REG_AUDIO_ENABLE_MASK);
    system_reset_guest();
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

    const window_t window =
        window_create(system_state.video_char_buffer, screen_width, screen_height);
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

    uint8_t* const video_char_buffer = system_state.video_char_buffer;
    const window_t window = window_create(video_char_buffer, screen_width, screen_height);

    const menu_result_t result = menu_config_run(&window, menu_ui_root_dashboard);
    system_restore(result.action == MENU_EXIT_RESUME);
    if (result.action == MENU_EXIT_LOAD_CONFIG) {
        apply_config(result.config_index);
    } else if (result.action == MENU_EXIT_RESET) {
        system_reset_guest();
    }

    log_info("-- Exit Menu --");
}
