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

 #include "../pch.h"
 
 typedef struct binary_s {
    uint8_t* data;
    size_t size;
    size_t capacity;
    size_t expected;
} binary_t;

typedef void (*on_load_fn_t)(void* user_data, const char* filename, uint32_t address);
typedef void (*on_set_scanmap_fn_t)(void* user_data, uint32_t address, const binary_t* scanmap_n, const binary_t* scanmap_b);

typedef struct setup_sink_s {
    void* const context;
    const on_load_fn_t on_action_load;
    const on_set_scanmap_fn_t on_action_set_scanmap;
} setup_sink_t;
