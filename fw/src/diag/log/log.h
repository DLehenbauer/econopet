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

#pragma once

#include "pch.h"

/**
 * Diagnostic Logging System
 *
 * Provides timestamped logging with separate ring buffers per log level to
 * ensure important messages are preserved even when verbose logging is enabled.
 *
 * Levels (lowest to highest priority):
 *   DEBUG - Verbose diagnostic information
 *   INFO  - Routine milestones and status updates
 *   WARN  - Important events to preserve (reset reason, recoverable errors, etc.)
 *
 * Each level has its own ring buffer, so high-volume DEBUG messages won't
 * push out WARN entries.
 */

// Configuration: message length (adjust to maintain 64-byte entry size)
#ifndef LOG_MESSAGE_LENGTH
#define LOG_MESSAGE_LENGTH 56
#endif

// Configuration: entries per level (must be power of 2 for efficient indexing)
#ifndef LOG_DEBUG_ENTRIES
#define LOG_DEBUG_ENTRIES 16
#endif

#ifndef LOG_INFO_ENTRIES
#define LOG_INFO_ENTRIES 16
#endif

#ifndef LOG_WARN_ENTRIES
#define LOG_WARN_ENTRIES 8
#endif

// Configuration: echo log messages to serial (printf)
#ifndef LOG_ECHO_SERIAL
#define LOG_ECHO_SERIAL 1
#endif

/**
 * Log severity levels, ordered from lowest to highest priority.
 */
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO  = 1,
    LOG_LEVEL_WARN  = 2,
    LOG_LEVEL_COUNT = 3
} log_level_t;

/**
 * A single log entry with timestamp and message.
 * Size is 64 bytes (power of 2) for efficient ring buffer indexing.
 */
typedef struct {
    uint64_t timestamp_us;              // Microseconds since boot (time_us_64)
    char message[LOG_MESSAGE_LENGTH];   // Null-terminated message
} log_entry_t;

_Static_assert(sizeof(log_entry_t) == 64, "log_entry_t must be 64 bytes");

/**
 * Initialize the logging system. Call early in main() before other subsystems.
 */
void log_init(void);

/**
 * Log a debug message (verbose diagnostics).
 * @param format printf-style format string
 */
void log_debug(const char* format, ...) __attribute__((format(printf, 1, 2)));

/**
 * Log an info message (routine milestones).
 * @param format printf-style format string
 */
void log_info(const char* format, ...) __attribute__((format(printf, 1, 2)));

/**
 * Log a warning message (important events to preserve).
 * @param format printf-style format string
 */
void log_warn(const char* format, ...) __attribute__((format(printf, 1, 2)));

/**
 * Log a message at the specified level.
 * @param level Log level
 * @param format printf-style format string
 */
void log_event(log_level_t level, const char* format, ...) __attribute__((format(printf, 2, 3)));

/**
 * Get the current uptime in microseconds (for relative timestamp display).
 * @return Microseconds since log_init() was called
 */
uint64_t log_uptime_us(void);

/**
 * Iterator for reading log entries across all levels in chronological order.
 */
typedef struct {
    uint8_t pos[LOG_LEVEL_COUNT];       // Current position in each ring
    uint8_t remaining[LOG_LEVEL_COUNT]; // Entries remaining in each ring
    log_level_t min_level;              // Minimum level to include
} log_iterator_t;

/**
 * Initialize a log iterator to read entries from oldest to newest.
 * @param iter Iterator to initialize
 * @param min_level Minimum log level to include (use LOG_LEVEL_DEBUG for all)
 */
void log_iter_init(log_iterator_t* iter, log_level_t min_level);

/**
 * Get the next log entry in chronological order.
 * @param iter Iterator state
 * @param out_level If not NULL, receives the level of the returned entry
 * @return Pointer to next entry, or NULL if no more entries
 */
const log_entry_t* log_iter_next(log_iterator_t* iter, log_level_t* out_level);

/**
 * Get the total number of entries currently stored across all levels.
 * @param min_level Minimum log level to count
 * @return Total entry count
 */
uint32_t log_entry_count(log_level_t min_level);
