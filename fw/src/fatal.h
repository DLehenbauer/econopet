// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stddef.h>

/**
 * @brief Registered firmware errors and shared Morse diagnostic letter
 * allocations.
 *
 * Keep this registry and the message table in fatal.c in sync when adding
 * codes. Use a distinct code for recognizable early-boot failures. Other fatal
 * errors and invariant violations share E, with details in the displayed
 * message.
 *
 * Reserve F, I, and W for bootloader fatal errors. Also reserve N. Although
 * nonfatal, N repeats while the bootloader polls for firmware.
 *
 * Transient bootloader status reports (e.g., blinking 'E' once to indicate
 * entering firmware) may share letters with firmware errors, since they are
 * distinguishable by the repetition pattern:
 *
 * | Letter | Repeats | Owner      | Meaning |
 * | ------ | ------- | ---------- | --- |
 * |   C    | yes     | Firmware   | Configuring FPGA failed |
 * |   D    | no      | Bootloader | Could not delete the update file (nonfatal) |
 * |   E    | no      | Bootloader | Entering firmware (nonfatal) |
 * |   E    | yes     | Firmware   | Error (unspecified fatal error, including invariant violations) |
 * |   F    | yes     | Bootloader | Flash programming failed |
 * |   I    | yes     | Bootloader | Invalid UF2 |
 * |   N    | yes     | Bootloader | No firmware installed (nonfatal, but repeats while polling) |
 * |   S    | no      | Bootloader | Programming skipped (nonfatal) |
 * |   W    | yes     | Bootloader | Watchdog reset without valid firmware |
 */
typedef enum {
    FATAL_ERROR_FPGA_CONFIG,  ///< C: FPGA configuration or its startup self-test failed.
    FATAL_ERROR_UNSPECIFIED,  ///< E: Other fatal errors, including invariant violations.
    FATAL_ERROR_COUNT         ///< Number of registered errors, not a reportable code.
} fatal_error_t;

/**
 * Format fatal details into fixed storage and halt with Morse E.
 * Reports on stdout and displays the details once before repeating Morse.
 * Preserves errno in the diagnostic when nonzero. Does not return.
 *
 * @pre The FPGA and display subsystem are initialized.
 * Use fatal_halt() instead for early-boot failures.
 */
void fatal(const char* const format, ...)
    __attribute__((format(printf, 1, 2), noreturn));

/**
 * @brief Report without using the display, then repeat Morse until reset.
 *
 * Use directly for early-boot errors before the FPGA and displays are ready.
 * Initializes stdio once, then prints the caller's message and registered
 * letter, followed by retained diagnostic logs.
 *
 * Takes ownership of the activity LED. No display or SD card is required.
 * Uses the bootloader's 100 ms dot, 300 ms dash and spacing convention.
 *
 * @param error Registered fatal error. Unrecognized values fall back to E.
 * @param message Borrowed, null-terminated error details, consumed synchronously.
 * @note Does not return.
 */
void fatal_halt(fatal_error_t error, const char* message) __attribute__((noreturn));

// Verifies a full SD-card path's byte length (excluding null, including directory
// prefixes) is below SD_PATH_MAX. A failed check is fatal, never truncating.
void vet_path_length(size_t length);

// Allocate memory or report a fatal error without allocating a message.
// Requires initialized FPGA/displays, like fatal().
void* vetted_malloc(size_t __size);

/**
 * Firmware assertion macro. If the condition is false, displays the formatted
 * error message on stdout and displays, then repeats Morse E.
 * Requires initialized FPGA/displays, like fatal().
 */
#define vet(cond, fmt, ...) \
    do { if (!(cond)) fatal(fmt, ##__VA_ARGS__); } while (0)

/**
 * Verify that 'index' is in the range [0, length).  Calls fatal() on failure.
 */
#define vet_index(index, length) \
    vet((size_t)(index) < (size_t)(length), \
        "index out of range: %zu (length %zu)", (size_t)(index), (size_t)(length))
