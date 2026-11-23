// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>

// Host fixtures are single-threaded. Track lock balance rather than emulate blocking.
typedef struct {
    bool initialized;
    bool locked;
} mutex_t;

typedef struct {
    bool initialized;
    unsigned int depth;
} recursive_mutex_t;

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize single-threaded ownership tracking. */
void mutex_init(mutex_t* mutex);

/** Acquire an uncontended mutex, rejecting unsupported contention. */
void mutex_enter_blocking(mutex_t* mutex);

/** Release a held mutex, rejecting unmatched unlocks. */
void mutex_exit(mutex_t* mutex);

/** Initialize single-threaded recursive ownership tracking. */
void recursive_mutex_init(recursive_mutex_t* mutex);

/** Acquire one nesting level of an initialized recursive mutex. */
void recursive_mutex_enter_blocking(recursive_mutex_t* mutex);

/** Release one nesting level, rejecting unmatched unlocks. */
void recursive_mutex_exit(recursive_mutex_t* mutex);

#ifdef __cplusplus
}
#endif

// Match the SDK's statically initialized recursive mutex lifetime.
#define auto_init_recursive_mutex(name) static recursive_mutex_t name = {true, 0}
