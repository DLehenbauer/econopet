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

#include <inttypes.h>
#include "log.h"

// Ring buffer for a single log level
typedef struct {
    log_entry_t* entries;   // Pointer to entry array
    uint8_t capacity;       // Number of entries (power of 2)
    uint8_t mask;           // capacity - 1, for bitwise AND indexing
    uint8_t head;           // Next write position
    uint8_t count;          // Number of valid entries (0..capacity)
} log_ring_t;

// Static entry storage for each level
static log_entry_t debug_entries[LOG_DEBUG_ENTRIES];
static log_entry_t info_entries[LOG_INFO_ENTRIES];
static log_entry_t warn_entries[LOG_WARN_ENTRIES];

// Ring buffers for each level
static log_ring_t log_rings[LOG_LEVEL_COUNT];

// Timestamp of log_init() call for relative time display
static uint64_t log_boot_time_us = 0;

// Timestamp of previous entry for tie-breaking when timestamps are equal
static uint64_t log_last_timestamp_us = 0;

// Level name prefixes for serial output
static const char* const level_prefixes[LOG_LEVEL_COUNT] = {
    "D",  // DEBUG
    "I",  // INFO
    "W"   // WARN
};

void log_init(void) {
    // Record boot time for relative timestamps
    log_boot_time_us = time_us_64();
    log_last_timestamp_us = log_boot_time_us;

    // Initialize DEBUG ring
    log_rings[LOG_LEVEL_DEBUG].entries = debug_entries;
    log_rings[LOG_LEVEL_DEBUG].capacity = LOG_DEBUG_ENTRIES;
    log_rings[LOG_LEVEL_DEBUG].mask = LOG_DEBUG_ENTRIES - 1;
    log_rings[LOG_LEVEL_DEBUG].head = 0;
    log_rings[LOG_LEVEL_DEBUG].count = 0;

    // Initialize INFO ring
    log_rings[LOG_LEVEL_INFO].entries = info_entries;
    log_rings[LOG_LEVEL_INFO].capacity = LOG_INFO_ENTRIES;
    log_rings[LOG_LEVEL_INFO].mask = LOG_INFO_ENTRIES - 1;
    log_rings[LOG_LEVEL_INFO].head = 0;
    log_rings[LOG_LEVEL_INFO].count = 0;

    // Initialize WARN ring
    log_rings[LOG_LEVEL_WARN].entries = warn_entries;
    log_rings[LOG_LEVEL_WARN].capacity = LOG_WARN_ENTRIES;
    log_rings[LOG_LEVEL_WARN].mask = LOG_WARN_ENTRIES - 1;
    log_rings[LOG_LEVEL_WARN].head = 0;
    log_rings[LOG_LEVEL_WARN].count = 0;

    // Static assertions to verify power-of-2 sizes
    _Static_assert((LOG_DEBUG_ENTRIES & (LOG_DEBUG_ENTRIES - 1)) == 0,
                   "LOG_DEBUG_ENTRIES must be a power of 2");
    _Static_assert((LOG_INFO_ENTRIES & (LOG_INFO_ENTRIES - 1)) == 0,
                   "LOG_INFO_ENTRIES must be a power of 2");
    _Static_assert((LOG_WARN_ENTRIES & (LOG_WARN_ENTRIES - 1)) == 0,
                   "LOG_WARN_ENTRIES must be a power of 2");
}

static void log_vevent(log_level_t level, const char* format, va_list args) {
    assert(level < LOG_LEVEL_COUNT);

    log_ring_t* ring = &log_rings[level];
    log_entry_t* entry = &ring->entries[ring->head];

    // Record timestamp, ensuring monotonically increasing order
    uint64_t now = time_us_64();
    if (now <= log_last_timestamp_us) {
        now = log_last_timestamp_us + 1;
    }
    log_last_timestamp_us = now;
    entry->timestamp_us = now;

    // Format message
    vsnprintf(entry->message, sizeof(entry->message), format, args);

    // Advance head with wrap-around using bitwise AND (overwrites oldest entry when full)
    ring->head = (ring->head + 1) & ring->mask;

    // Note: When count == capacity, new entries overwrite the oldest ones. The head continues
    //       to wrap, and count stays at capacity.
    if (ring->count < ring->capacity) {
        ring->count++;
    }

#if LOG_ECHO_SERIAL
    // Echo to serial with level prefix and relative timestamp
    uint64_t relative_us = entry->timestamp_us - log_boot_time_us;
    uint32_t seconds = (uint32_t)(relative_us / 1000000);
    uint32_t millis = (uint32_t)((relative_us % 1000000) / 1000);
    printf("[%4" PRIu32 ".%03" PRIu32 "] %s: %s\n", seconds, millis, level_prefixes[level], entry->message);
#endif
}

void log_event(log_level_t level, const char* format, ...) {
    va_list args;
    va_start(args, format);
    log_vevent(level, format, args);
    va_end(args);
}

void log_debug(const char* format, ...) {
    va_list args;
    va_start(args, format);
    log_vevent(LOG_LEVEL_DEBUG, format, args);
    va_end(args);
}

void log_info(const char* format, ...) {
    va_list args;
    va_start(args, format);
    log_vevent(LOG_LEVEL_INFO, format, args);
    va_end(args);
}

void log_warn(const char* format, ...) {
    va_list args;
    va_start(args, format);
    log_vevent(LOG_LEVEL_WARN, format, args);
    va_end(args);
}

uint64_t log_uptime_us(void) {
    return time_us_64() - log_boot_time_us;
}

void log_iter_init(log_iterator_t* iter, log_level_t min_level) {
    iter->min_level = min_level;

    for (int level = 0; level < LOG_LEVEL_COUNT; level++) {
        if (level < min_level) {
            iter->pos[level] = 0;
            iter->remaining[level] = 0;
        } else {
            log_ring_t* ring = &log_rings[level];
            // Start position is (head - count) with wrap-around
            iter->pos[level] = (ring->head - ring->count) & ring->mask;
            iter->remaining[level] = ring->count;
        }
    }
}

const log_entry_t* log_iter_next(log_iterator_t* iter, log_level_t* out_level) {
    // Find the level with the oldest (smallest timestamp) entry
    log_level_t oldest_level = LOG_LEVEL_COUNT;
    uint64_t oldest_timestamp = UINT64_MAX;

    for (int level = iter->min_level; level < LOG_LEVEL_COUNT; level++) {
        if (iter->remaining[level] > 0) {
            log_ring_t* ring = &log_rings[level];
            const log_entry_t* entry = &ring->entries[iter->pos[level]];

            if (entry->timestamp_us < oldest_timestamp) {
                oldest_timestamp = entry->timestamp_us;
                oldest_level = level;
            }
        }
    }

    // No more entries
    if (oldest_level == LOG_LEVEL_COUNT) {
        return NULL;
    }

    // Get the entry and advance the iterator for that level
    log_ring_t* ring = &log_rings[oldest_level];
    const log_entry_t* result = &ring->entries[iter->pos[oldest_level]];

    iter->pos[oldest_level] = (iter->pos[oldest_level] + 1) & ring->mask;
    iter->remaining[oldest_level]--;

    if (out_level != NULL) {
        *out_level = oldest_level;
    }

    return result;
}

uint32_t log_entry_count(log_level_t min_level) {
    uint32_t total = 0;

    for (int level = min_level; level < LOG_LEVEL_COUNT; level++) {
        total += log_rings[level].count;
    }

    return total;
}
