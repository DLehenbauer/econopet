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

#include "pch.h"
#include "menu.h"

#include <dirent.h>

#include "diag/log/log.h"
#include "display/char_encoding.h"
#include "display/display.h"
#include "display/dvi/dvi.h"
#include "display/window.h"
#include "driver.h"
#include "fatal.h"
#include "filesystem/vfs.h"
#include "global.h"
#include "hw.h"
#include "input.h"
#include "menu_config.h"
#include "pet.h"
#include "roms/checksum.h"
#include "roms/roms.h"
#include "sd/sd.h"
#include "system_state.h"
#include "usb/keyboard.h"

// When navigating the SD card's file system, this is the maximum number
// of we will display to the user/cache in memory.
#define MAX_FILE_SLOTS 25

const uint screen_width  = 40;
const uint screen_height = 25;

#define CH_SPACE 0x20

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

    log_debug("0x%04lx", address);
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

    log_debug("-%04lx: %s ($%02x)", address - 1, filename, checksum);

    release_temp_buffer(&temp_buffer);
    fclose(file);
}

void action_patch(void* context, uint32_t address, const binary_t* binary) {
    (void) context;

    log_debug("0x%04lx: patching %zu bytes", address, binary->size);
    spi_write(address, binary->data, binary->size);
}

void action_copy(void* context, uint32_t source, uint32_t destination, uint32_t length) {
    (void)context;

    log_debug("0x%04lx: copying %lu bytes from 0x%04lx", source, length, destination);
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
            vet(options->columns == 80, "Invalid 'columns:' in config (got %d)", options->columns);
            ctx->system_state->pet_display_columns = pet_display_columns_80;
            break;
    }

    // Validate and set video RAM mask (must be 0-3)
    vet(options->video_ram_mask <= 3,
        "Invalid video RAM mask in config (got %lu, expected 0-3)", options->video_ram_mask);
    system_state_set_video_ram_mask(ctx->system_state, (uint8_t) options->video_ram_mask);

    // Load USB keymap if specified
    if (options->usb_keymap[0] != '\0') {
        read_keymap(options->usb_keymap, ctx->system_state);
    }

    write_pet_model(ctx->system_state);

    log_debug("Set options: %lu columns, video RAM mask %lu", options->columns, options->video_ram_mask);
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

        log_debug("0x%04lx: fixing checksum for $%04lx-%04lx ($%02x -> $%02lx)", 
            fix_addr, start_addr, end_addr, actual_sum, expected);

        spi_write_at(fix_addr, adjusted_byte);

        assert(checksum_ram(start_addr, end_addr + 1) == expected);
    }
}

void menu_enter(void) {
    log_info("-- Enter Menu --");

    system_state.video_source = video_source_firmware;
    
    start_menu_rom(MENU_ROM_BOOT_NORMAL);
    memset(video_char_buffer + 0x800, 0x0F, VIDEO_CHAR_BUFFER_BYTE_SIZE - 0x800);
    
    const window_t window = window_create(video_char_buffer, screen_width, screen_height);

    action_context_t action_context = {
        .system_state = &system_state,
    };

    const setup_sink_t setup_sink = {
        .context = &action_context,
        .on_load = action_load,
        .on_patch = action_patch,
        .on_copy = action_copy,
        .on_set_options = action_set_options,
        .on_fix_checksum = action_fix_checksum,
        .system_state = &system_state,
    };

    menu_config_show(&window, &setup_sink);

    system_state.video_source = video_source_pet;
    pet_reset();

    log_info("-- Exit Menu --");
}

void menu_task(void) {
    // Check for button events from the input queue
    int key = input_getch();

    switch (key) {
        case KEY_BTN_SHORT:
            pet_reset();
            break;
        case KEY_BTN_LONG:
            menu_enter();
            break;
        default:
            break;
    }
}
