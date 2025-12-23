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

#include <dirent.h>
#include "filesystem/vfs.h"
#include "fatal.h"
#include "system_state.h"
#include "global.h"
#include "driver.h"
#include "hw.h"
#include "pet.h"
#include "roms/roms.h"
#include "sd/sd.h"
#include "term.h"
#include "display/dvi/dvi.h"
#include "menu.h"
#include "menu_config.h"
#include "display/window.h"
#include "usb/keyboard.h"
#include "roms/checksum.h"
#include "display/char_encoding.h"

// When navigating the SD card's file system, this is the maximum number
// of we will display to the user/cache in memory.
#define MAX_FILE_SLOTS 25

const uint screen_width  = 40;
const uint screen_height = 25;

#define CH_SPACE 0x20

static bool read_button_debounced() {
    static uint64_t last_change_time = 0;
    static bool raw_state = true;       // Last raw button state (initial: pressed for capacitor charging)
    static bool debounced_state = true; // Debounced button state
    
    const uint64_t DEBOUNCE_TIME_US = 50000;  // 50ms debounce time

    bool current_raw = !gpio_get(MENU_BTN_GP);  // Active low button
    uint64_t current_time = time_us_64();

    // Check if raw button state has changed
    if (current_raw != raw_state) {
        raw_state = current_raw;
        last_change_time = current_time;
        return debounced_state;  // Return current debounced state during transition
    }

    // Check if we're still in debounce period
    if ((current_time - last_change_time) < DEBOUNCE_TIME_US) {
        return debounced_state;  // Still debouncing, return current state
    }

    // Button state is stable, update debounced state
    debounced_state = current_raw;
    return debounced_state;
}

static ButtonAction get_button_action() {
    // Track button press/release events and timing for short/long press detection
    static bool is_pressed = true;     // Initial state matches debounced initial state
    static bool was_handled = true;
    static uint64_t press_start;
    
    const uint64_t LONG_PRESS_TIME_US = 500000; // 500ms for long press

    bool was_pressed = is_pressed;
    is_pressed = read_button_debounced();
    
    uint64_t current_time = time_us_64();
    ButtonAction action = None;

    if (!is_pressed) {
        // Button is not currently pressed.
        if (was_pressed && !was_handled) {
            // Button has just been released and did not previously exceed the
            // long-press threshold.
            action = ShortPress;
            printf("MENU button: short press\n");
        }

    } else {
        // The button is currently pressed.
        if (!was_pressed) {
            // Button has just been pressed (after debounce).  Record the start time.
            press_start = current_time;
            was_handled = false;
        } else if (!was_handled && (current_time - press_start > LONG_PRESS_TIME_US)) {
            // Button has been held for more than 500ms.  Consider this a long press.
            was_handled = true;
            action = LongPress;
            printf("MENU button: long press\n");
        }
    }

    return action;
}

void term_present() {
    spi_write(0x8000, video_char_buffer, VIDEO_CHAR_BUFFER_BYTE_SIZE);
    fflush(stdout);
}

int term_input_char() {
    int keycode = keyboard_getch();
    if (keycode != EOF) {
        return keycode;
    }

    if (uart_is_readable(uart_default)) {
        return uart_getc(uart_default);
    }

    switch (get_button_action()) {
        case ShortPress:
            return KEY_DOWN;
        case LongPress:
            return '\n';
        default:
            break;
    }

    return EOF;
}

uint screen_offset(uint x, uint y) {
    assert(x < screen_width && y < screen_height);
    uint offset = y;
    offset *= screen_width;
    offset += x;
    return offset;
}

void screen_clear() {
    memset(video_char_buffer, CH_SPACE, screen_width * screen_height);
}

void screen_print(uint x, uint y, const char* str, bool reverse) {
    uint offset = screen_offset(x, y);
    const uint8_t* pCh = (uint8_t*) str;
    if (reverse) {
        while (*pCh != 0) {
            video_char_buffer[offset++] = (ascii_to_vrom(*(pCh++)) | 0x80);
        }
    } else {
        while (*pCh != 0) {
            video_char_buffer[offset++] = ascii_to_vrom(*(pCh++));
        }
    }
}

void screen_reverse(uint x, uint y, uint length) {
    uint offset = screen_offset(x, y);
    
    assert((offset + length) <= (screen_width * screen_height));
    
    while (length--) {
        video_char_buffer[offset++] ^= 0x80;
    }
}

static struct dirent file_slots[MAX_FILE_SLOTS] = { 0 };
static char full_path [PATH_MAX + 1] = { 0 };

typedef enum {
    STATUS_ERROR,
    STATUS_SUCCESS_MORE_ENTRIES,
    STATUS_SUCCESS_END_OF_DIR
} DirStatus;

DirStatus file_slots_read(char* path, uint position) {
    struct dirent* entry = NULL;
    uint y = 0;

    DIR* dir = opendir(path);
    if (dir == NULL) {
        goto Cleanup;
    }

    // Skip entries until we arrive at the desired position or reach the end of the directory.
    while (position--) {
        entry = readdir(dir);
        if (entry == NULL) {
            goto Cleanup;
        }
    }

    while (((entry = readdir(dir)) != NULL) && (y < MAX_FILE_SLOTS)) {
        memcpy(&file_slots[y++], entry, sizeof(struct dirent));
    }

Cleanup:
    if (y < MAX_FILE_SLOTS) {
        file_slots[y].d_name[0] = '\0';
    }

    DirStatus status;

    if (errno != 0) {
        status = STATUS_ERROR;
        perror("readdir");
    } else if (entry == NULL) {
        status = STATUS_SUCCESS_END_OF_DIR;
    } else {
        status = STATUS_SUCCESS_MORE_ENTRIES;
    }

    if (dir != NULL) {
        closedir(dir);
    }

    return status;
}

int path_concat(char *dest, size_t dest_size, const char *path, const char *filename) {
    if (!dest || !path || !filename) {
        return -1; // Invalid arguments
    }

    // Check if dest_size is sufficient
    size_t path_len = strlen(path);
    size_t filename_len = strlen(filename);
    size_t total_len = path_len + 1 + filename_len + 1; // 1 for '/', 1 for null terminator

    if (total_len > dest_size) {
        return -2; // Destination buffer too small
    }

    // Copy the path
    strncpy(dest, path, dest_size);
    dest[dest_size - 1] = '\0'; // Ensure null termination

    // Append a separator if needed
    if (path_len > 0 && path[path_len - 1] != '/') {
        strncat(dest, "/", dest_size - strlen(dest) - 1);
    }

    // Append the filename
    strncat(dest, filename, dest_size - strlen(dest) - 1);

    return 0; // Success
}

char* directory() {
    screen_clear();
    file_slots_read("/", 0);
    
    for (int y = 0; y < MAX_FILE_SLOTS; y++) {
        if (file_slots[y].d_name[0] == '\0') {
            break;
        }
        screen_print(0, y, file_slots[y].d_name, false);
    }

    int selected = 0;
    screen_reverse(0, selected, screen_width);

    while (true) {
        ButtonAction action = get_button_action();

        switch (action) {
            case None:
                break;
            case ShortPress:
                screen_reverse(0, selected, screen_width);
                selected++;
                selected %= screen_height;
                screen_reverse(0, selected, screen_width);
                break;
            case LongPress:
                path_concat(full_path, sizeof(full_path), "/", file_slots[selected].d_name);
                return full_path;
        }
    }

    __builtin_unreachable();
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

void menu_init() {
    gpio_init(MENU_BTN_GP);

    // Enable pull-up resistor as the button is active low
    gpio_pull_up(MENU_BTN_GP);
    gpio_set_dir(MENU_BTN_GP, GPIO_IN);
}

typedef struct action_context_s {
    system_state_t* system_state;
} action_context_t;

void action_load(void* const context, const char* filename, uint32_t address) {
    (void) context;

    printf("0x%04lx", address);
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

    printf("-%04lx: %s ($%02x)\n", address - 1, filename, checksum);

    release_temp_buffer(&temp_buffer);
    fclose(file);
}

void action_patch(void* context, uint32_t address, const binary_t* binary) {
    (void) context;

    printf("0x%4lx: patching %zu bytes bytes\n", address, binary->size);
    spi_write(address, binary->data, binary->size);
}

void action_copy(void* context, uint32_t source, uint32_t destination, uint32_t length) {
    (void)context;

    printf("0x%04lx: copying %lu bytes from 0x%04lx\n", source, length, destination);
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

    printf("Set options: %lu columns, video RAM mask %lu\n", options->columns, options->video_ram_mask);
}

void read_keymap_callback(size_t offset, uint8_t* buffer, size_t bytes_read, void* context) {
    memcpy(context + offset, buffer, bytes_read);
}

void read_keymap(const char* filename, system_state_t* config) {
    // Read the keymap file and store it in the configuration.
    void* keymap = (void*) config->usb_keymap_data[0];
    sd_read_file(filename, read_keymap_callback, keymap, sizeof(config->usb_keymap_data));
    printf("USB keymap: '%s'\n", filename);
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

        printf("0x%04lx: fixing checksum for $%04lx-%04lx ($%02x -> $%02lx)\n", 
            fix_addr, start_addr, end_addr, actual_sum, expected);

        spi_write_at(fix_addr, adjusted_byte);

        assert(checksum_ram(start_addr, end_addr + 1) == expected);
    }
}

void menu_enter() {
    printf("-- Enter Menu --\n");

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

    pet_reset();

    printf("-- Exit Menu --\n");
}

void menu_task() {
    ButtonAction action = get_button_action();

    // If no button action, return to main PET loop.
    if (action == None) {
        return;
    }

    // If short press, reset and then return to main PET loop.
    if (action == ShortPress) {
        pet_reset();
        return;
    }

    menu_enter();
}
