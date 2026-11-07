// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "fatal.h"

#include "sd/sd.h"

#if defined(PICO_RP2040) || defined(ECONOPET_TEST_FATAL_MORSE)
#include "hardware/gpio.h"

#define MORSE_DOT_US 100000u
#define MORSE_DASH_UNITS 3u
#define MORSE_REPEAT_GAP_UNITS 3u

// Morse encoding for a fatal error. The caller supplies the diagnostic text.
typedef struct {
    char letter;
    const char* pattern;
} fatal_message_t;

// Keep messages consistent with the diagnostic letter registry in fatal.h.
static const fatal_message_t fatal_messages[FATAL_ERROR_COUNT] = {
    [FATAL_ERROR_FPGA_CONFIG] = {
        .letter = 'C',
        .pattern = "-.-.",
    },
    [FATAL_ERROR_UNSPECIFIED] = {
        .letter = 'E',
        .pattern = ".",
    },
};

// Emit one letter with the bootloader's dot, dash, and spacing durations.
static void blink(const char* pattern) {
    for (const char* symbol = pattern; *symbol != '\0'; ++symbol) {
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_us((*symbol == '-' ? MORSE_DASH_UNITS : 1u) * MORSE_DOT_US);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
        sleep_us(MORSE_DOT_US);
    }
    sleep_us(MORSE_REPEAT_GAP_UNITS * MORSE_DOT_US);
}

// Initialize serial diagnostics and report before any optional display access.
static const fatal_message_t* fatal_begin(fatal_error_t error, const char* message) {
    // Invalid codes must still reach the fatal loop, even without assertions.
    if ((unsigned int)error >= FATAL_ERROR_COUNT) {
        error = FATAL_ERROR_UNSPECIFIED;
    }
    const fatal_message_t* const entry = &fatal_messages[error];

    // Restore UART settings for the current clocks, even before normal stdio setup.
    // This build uses UART stdio. USB host initialization is separate.
    stdio_init_all();

    // Retain the serial diagnosis even when no display services are available.
    fprintf(stdout, "Error (%c): %s\n", entry->letter, message);
    fflush(stdout);

    return entry;
}

// Halt with repeated serial and Morse diagnostics without accessing the display.
static void __attribute__((noreturn)) fatal_repeat(
    const fatal_message_t* entry, const char* message) {
    // Separate the error from the boot/activity LED state before repeating it.
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, false);
    sleep_us(MORSE_REPEAT_GAP_UNITS * MORSE_DOT_US);
    while (true) {
        blink(entry->pattern);

        // Resend during the dark gap so terminals connected later receive the details.
        fprintf(stdout, "Error (%c): %s\n", entry->letter, message);
        fflush(stdout);
    }
}

void fatal_halt(fatal_error_t error, const char* message) {
    const fatal_message_t* const entry = fatal_begin(error, message);
    fatal_repeat(entry, message);
}
#endif

// Host targets provide fatal() and vetted_malloc() through their existing mocks.
#if defined(PICO_RP2040)
#include "display/display.h"
#include "display/window.h"
#include "roms/roms.h"
#include "system_state.h"

#define FATAL_COLUMNS 40
#define FATAL_ROWS    25
#define FATAL_PREFIX  "E: "
#define FATAL_MESSAGE_SIZE \
    (FATAL_COLUMNS * FATAL_ROWS - (sizeof(FATAL_PREFIX) - 1) + 1)

// Render fatal details without allocating storage. Core 1 keeps driving DVI.
static void fatal_display_message(const char* const message) {
    start_menu_rom(MENU_ROM_BOOT_ERROR);

    const window_t window = window_create(system_state.video_char_buffer, FATAL_COLUMNS, FATAL_ROWS);
    display_window_begin(&window);

    uint8_t* const pOut = window_puts(&window, window.start, FATAL_PREFIX);
    window_reverse(&window, window.start, 2);
    window_puts(&window, pOut, message);
    
    system_state.video_source = video_source_firmware;  // Copy from `video_char_buffer` to $8000
    system_state.term_mode = term_mode_video;           // Also copy to the terminal
    system_state.video_text_mode = video_text_mode_lower;
    display_task();
}

// Funnel formatted errors and allocation failures through the same Morse endpoint.
static void __attribute__((noreturn)) fatal_no_alloc(const char* message) {
    static bool rendering;

    // Recover serial output before rendering, then leave core 1 driving DVI.
    const fatal_message_t* const entry = fatal_begin(FATAL_ERROR_UNSPECIFIED, message);

    // A failure inside the renderer must reach Morse rather than recursively render.
    if (!rendering) {
        rendering = true;
        fatal_display_message(message);
    }
    fatal_repeat(entry, message);
}

void fatal(const char* const format, ...) {
    static char message[FATAL_MESSAGE_SIZE];

    const int saved_errno = errno;
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    size_t length;
    if (written < 0) {
        strcpy(message, "could not format fatal error");
        length = strlen(message);
    } else {
        const size_t formatted_length = (size_t)written;
        length = MIN(formatted_length, sizeof(message) - 1);
    }

    if (saved_errno != 0 && length < sizeof(message) - 1) {
        snprintf(
            message + length,
            sizeof(message) - length,
            "\n\n(%d): %s",
            saved_errno,
            strerror(saved_errno)
        );
    }

    fatal_no_alloc(message);
}

void* vetted_malloc(size_t __size) {
    void* p = malloc(__size);
    if (p == NULL) {
        fatal_no_alloc("malloc failed");
    }
    return p;
}
#endif

void vet_path_length(size_t length) {
    vet(length < SD_PATH_MAX, "SD path exceeds %u characters (got %zu)",
        (unsigned int) (SD_PATH_MAX - 1), length);
}
