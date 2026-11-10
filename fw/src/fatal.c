// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "fatal.h"

#include "diag/log/log.h"
#include "sd/sd.h"

#define MORSE_DOT_US (100 * 1000u)
#define MORSE_DASH_UNITS 3u
#define MORSE_SYMBOL_GAP_UNITS 1u
#define MORSE_WORD_GAP_UNITS 7u
#define FATAL_LOG_DUMP_INTERVAL_US (UINT64_C(10) * 1000u * 1000u)

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

// Blink the status LED according to the given Morse pattern.
static void blink(const char* pattern) {
    // Each pattern is a complete fatal message, even when it is one letter.
    // Separate complete messages from prior output by a full Morse word gap.
    sleep_us((MORSE_WORD_GAP_UNITS - MORSE_SYMBOL_GAP_UNITS) * MORSE_DOT_US);

    for (const char* symbol = pattern; *symbol != '\0'; ++symbol) {
        // Separate symbols within a letter by the symbol gap.
        sleep_us(MORSE_SYMBOL_GAP_UNITS * MORSE_DOT_US);

        // Determine the duration for the current symbol.
        char ch = *symbol;
        uint64_t time;

        switch (ch) {
            case '.':
                time = MORSE_DOT_US;
                break;
            default:
                assert(ch == '-');
                time = MORSE_DASH_UNITS * MORSE_DOT_US;
                break;
        }

        // Turn the LED on for the duration of the current symbol, then turn it
        // off.  The symbol gap will be applied before the next symbol is
        // applied at the beginning of the loop.
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_us(time);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
    }
}

// Initialize serial and LED diagnostics before any optional display access.
static const fatal_message_t* fatal_begin(fatal_error_t error) {
    // Invalid codes must still reach the fatal loop, even without assertions.
    if ((unsigned int)error >= FATAL_ERROR_COUNT) {
        error = FATAL_ERROR_UNSPECIFIED;
    }
    const fatal_message_t* const entry = &fatal_messages[error];

    // Restore UART settings for the current clocks.
    stdio_init_all();
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, false);

    return entry;
}

// Repeat Morse and periodically replay serial diagnostics without accessing the display.
static void __attribute__((noreturn)) fatal_repeat(
    const fatal_message_t* entry, const char* message) {
    // The sentinel makes the first loop iteration report immediately.
    uint64_t last_log_dump_us = -FATAL_LOG_DUMP_INTERVAL_US;

    while (true) {
        const uint64_t now_us = time_us_64();
        if (now_us - last_log_dump_us >= FATAL_LOG_DUMP_INTERVAL_US) {
            log_dump(LOG_LEVEL_DEBUG);
            fprintf(stdout, "Error (%c): %s\r\n", entry->letter, message);
            fflush(stdout);
            last_log_dump_us = now_us;
        }

        blink(entry->pattern);
    }
}

void fatal_halt(fatal_error_t error, const char* message) {
    const fatal_message_t* const entry = fatal_begin(error);
    fatal_repeat(entry, message);
}

// Host targets provide fatal() and vetted_malloc() through their existing mocks.
#if defined(PICO_RP2040)
#include "display/display.h"
#include "display/window.h"
#include "driver.h"
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
    system_state.video_text_mode = video_text_mode_lower;

    // Present the final error directly on both displays before entering the fatal loop.
    spi_write(0x8000, system_state.video_char_buffer, PET_MAX_VIDEO_RAM_BYTES);
    display_window_show(&window);
}

// Funnel formatted errors and allocation failures through the same Morse endpoint.
static void __attribute__((noreturn)) fatal_no_alloc(const char* message) {
    static bool rendering;

    // Recover serial output before rendering, then leave core 1 driving DVI.
    const fatal_message_t* const entry = fatal_begin(FATAL_ERROR_UNSPECIFIED);

    // Display SPI access can stall, so make the diagnosis available on UART first.
    fprintf(stdout, "Error (%c): %s\r\n", entry->letter, message);
    fflush(stdout);

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
