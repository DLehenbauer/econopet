// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

typedef struct {
    int unused;
} mutex_t;

typedef struct {
    int unused;
} recursive_mutex_t;

/** Initialize a host no-op mutex. */
static inline void mutex_init(mutex_t* mutex) {
    (void)mutex;
}

/** Enter a host no-op mutex. */
static inline void mutex_enter_blocking(mutex_t* mutex) {
    (void)mutex;
}

/** Exit a host no-op mutex. */
static inline void mutex_exit(mutex_t* mutex) {
    (void)mutex;
}

/** Initialize a host no-op recursive mutex. */
static inline void recursive_mutex_init(recursive_mutex_t* mutex) {
    (void)mutex;
}

/** Enter a host no-op recursive mutex. */
static inline void recursive_mutex_enter_blocking(recursive_mutex_t* mutex) {
    (void)mutex;
}

/** Exit a host no-op recursive mutex. */
static inline void recursive_mutex_exit(recursive_mutex_t* mutex) {
    (void)mutex;
}

#define auto_init_recursive_mutex(name) static recursive_mutex_t name
