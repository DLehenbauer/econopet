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
#include "global.h"
#include "driver.h"
#include "hw.h"
#include "pet.h"
#include "roms/roms.h"
#include "sd/sd.h"
#include "term.h"
#include "video/video.h"
#include "menu.h"
#include "menu_config.h"
#include "window.h"
#include "usb/keyboard.h"

// When navigating the SD card's file system, this is the maximum number
// of we will display to the user/cache in memory.
#define MAX_FILE_SLOTS 25

const uint screen_width  = 40;
const uint screen_height = 25;

#define CH_SPACE 0x20

static ButtonAction get_button_action() {
    // Initial state is pressed/handled to give debouncing capacitor time to charge.
    static bool is_pressed = true;
    static bool was_handled = true;
    static uint64_t press_start;

    bool was_pressed = is_pressed;
    is_pressed = !gpio_get(MENU_BTN_GP);
    ButtonAction action = None;

    if (!is_pressed) {
        // Button is not current pressed.
        if (was_pressed && !was_handled) {
            // Button has just been released and did not previously exceed the
            // long-press threshold.
            action = ShortPress;
            printf("MENU button: short press\n");
        }

    } else {
        // The button is currently pressed.
        if (!was_pressed) {
            // Button has just been pressed.  Record the start time of the press.
            press_start = time_us_64();
            was_handled = false;
        } else if (!was_handled && (time_us_64() - press_start > 500000)) {
            // Button has been held for more that 500ms.  Consider this a long press.
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

// Lower-case (POKE 59468,14)
// This table maps from ASCII to the character ROM offsets.
static const uint8_t ascii_to_vrom[] = {
    /*              0               1               2               3               4               5               6               7               8               9               A               B               C               D               E               F  */
    /* 0 | NUL */ 0x00, /* SOH */ 0x01, /* STX */ 0x02, /* ETX */ 0x03, /* EOT */ 0x04, /* ENQ */ 0x05, /* ACK */ 0x06, /* BEL */ 0x07, /*  BS */ 0x08, /*  HT */ 0x09, /*  LF */ 0x0A, /*  VT */ 0x0B, /*  FF */ 0x0C, /*  CR */ 0x0D, /*  SO */ 0x0E, /*  SI */ 0x0F,
    /* 1 | DLE */ 0x10, /* DC1 */ 0x11, /* DC2 */ 0x12, /* DC3 */ 0x13, /* DC4 */ 0x14, /* NAK */ 0x15, /* SYN */ 0x16, /* ETB */ 0x17, /* CAN */ 0x18, /*  EM */ 0x19, /* SUB */ 0x1A, /* ESC */ 0x1B, /*  FS */ 0x1C, /*  GS */ 0x1D, /*  RS */ 0x1E, /*  US */ 0x1F,
    /* 2 |  SP */ 0x20, /*   ! */ 0x21, /*   " */ 0x22, /*   # */ 0x23, /*   $ */ 0x24, /*   % */ 0x25, /*   & */ 0x26, /*   ' */ 0x27, /*   ( */ 0x28, /*   ) */ 0x29, /*   * */ 0x2A, /*   + */ 0x2B, /*   , */ 0x2C, /*   - */ 0x2D, /*   . */ 0x2E, /*   / */ 0x2F,
    /* 3 |   0 */ 0x30, /*   1 */ 0x31, /*   2 */ 0x32, /*   3 */ 0x33, /*   4 */ 0x34, /*   5 */ 0x35, /*   6 */ 0x36, /*   7 */ 0x37, /*   8 */ 0x38, /*   9 */ 0x39, /*   : */ 0x3A, /*   ; */ 0x3B, /*   < */ 0x3C, /*   = */ 0x3D, /*   > */ 0x3E, /*   ? */ 0x3F,
    /* 4 |   @ */ 0x00, /*   A */ 0x41, /*   B */ 0x42, /*   C */ 0x43, /*   D */ 0x44, /*   E */ 0x45, /*   F */ 0x46, /*   G */ 0x47, /*   H */ 0x48, /*   I */ 0x49, /*   J */ 0x4A, /*   K */ 0x4B, /*   L */ 0x4C, /*   M */ 0x4D, /*   N */ 0x4E, /*   O */ 0x4F,
    /* 5 |   P */ 0x50, /*   Q */ 0x51, /*   R */ 0x52, /*   S */ 0x53, /*   T */ 0x54, /*   U */ 0x55, /*   V */ 0x56, /*   W */ 0x57, /*   X */ 0x58, /*   Y */ 0x59, /*   Z */ 0x5A, /*   [ */ 0x1B, /*   \ */ 0x1C, /*   ] */ 0x1D, /*   ^ */ 0x1E, /*   _ */ 0x64,
    /* 6 |   ` */ 0x27, /*   a */ 0x01, /*   b */ 0x02, /*   c */ 0x03, /*   d */ 0x04, /*   e */ 0x05, /*   f */ 0x06, /*   g */ 0x07, /*   h */ 0x08, /*   i */ 0x09, /*   j */ 0x0A, /*   k */ 0x0B, /*   l */ 0x0C, /*   m */ 0x0D, /*   n */ 0x0E, /*   o */ 0x0F,
    /* 7 |   p */ 0x10, /*   q */ 0x11, /*   r */ 0x12, /*   s */ 0x13, /*   t */ 0x14, /*   u */ 0x15, /*   v */ 0x16, /*   w */ 0x17, /*   x */ 0x18, /*   y */ 0x19, /*   z */ 0x1A, /*   { */ 0x6B, /*   | */ 0x5B, /*   } */ 0x73, /*   ~ */ 0x71, /* DEL */ 0x7F,
};

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
            video_char_buffer[offset++] = (ascii_to_vrom[*(pCh++)] | 0x80);
        }
    } else {
        while (*pCh != 0) {
            video_char_buffer[offset++] = ascii_to_vrom[*(pCh++)];
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
    
    for (uint y = 0; y < MAX_FILE_SLOTS; y++) {
        if (file_slots[y].d_name[0] == '\0') {
            break;
        }
        screen_print(0, y, file_slots[y].d_name, false);
    }

    uint selected = 0;
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

void action_load(void* const context, const char* filename, uint32_t address) {
    (void) context;

    printf("0x%04lx: %s\n", address, filename);
    FILE *file = sd_open(filename, "rb"); // Open file in binary read mode
    if (!file) {
        fatal("Failed to open file '%s'", filename);
    }

    // Read remaining bytes to the destination address.
    uint8_t* temp_buffer = acquire_temp_buffer();
    size_t bytes_read;

    while ((bytes_read = fread(temp_buffer, 1, TEMP_BUFFER_SIZE, file)) > 0) {
        spi_write(address, temp_buffer, bytes_read);
        address += bytes_read;

        // Check for read errors (other than EOF)
        if (bytes_read < TEMP_BUFFER_SIZE && ferror(file)) {
            fatal("Failed to read file '%s'", filename);
        }
    }

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
    (void) context;

    model_t model = get_model();

    if (options->columns == 80) {
        model.flags |= model_flag_80_cols;
    } else {
        model.flags &= ~model_flag_80_cols;
    }

    set_model(model);

    printf("Set options: %lu columns\n", options->columns);
}

void menu_enter() {
    printf("-- Enter Menu --\n");

    // Ensure CPU is suspended while initializing ROMs.
    set_cpu(/* ready */ false, /* reset */ false, /* nmi: */ false);
    spi_write(/* dest: */ 0xFF00, /* pSrc: */ rom_menu_ff00,  sizeof(rom_menu_ff00));   // Load menu ROM
    pet_reset();

    set_video(/* cols80: */ false);
    spi_write(/* dest: */ 0x8800, /* pSrc: */ rom_chars_8800, sizeof(rom_chars_8800));
    
    const window_t window = window_create(video_char_buffer, screen_width, screen_height);

    model_t model = get_model();

    const setup_sink_t setup_sink = {
        .context = NULL,
        .on_action_load = action_load,
        .on_action_patch = action_patch,
        .on_action_copy = action_copy,
        .on_action_set_options = action_set_options,
        .model = &model,
    };

    menu_config_show(&window, &setup_sink);

    set_model(model);
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
