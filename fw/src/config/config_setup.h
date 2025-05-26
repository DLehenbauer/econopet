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
 #include "model.h"
 
 typedef struct binary_s {
    uint8_t* data;
    size_t size;
    size_t capacity;
    size_t expected;
} binary_t;

typedef struct options_s {
    uint32_t columns;   // Number of columns (default: 40)
} options_t;

typedef void (*on_load_fn_t)(void* user_data, const char* filename, uint32_t address);
typedef void (*on_patch_fn_t)(void* user_data, uint32_t address, const binary_t* binary);
typedef void (*on_copy_fn_t)(void* user_data, uint32_t source, uint32_t destination, uint32_t length);
typedef void (*on_set_options_fn_t)(void* user_data, options_t* options);
typedef void (*on_set_keymap_fn_t)(void* user_data, const char* filename);

typedef struct setup_sink_s {
    // 'context' is used by 'load_config' to filter callbacks to only the selected config.
    void* const context;

    // Callbacks invoked for actions in the config file.
    const on_load_fn_t on_load;
    const on_patch_fn_t on_patch;
    const on_copy_fn_t on_copy;
    const on_set_options_fn_t on_set_options;
    const on_set_keymap_fn_t on_set_keymap;

    // 'model_flags' is used to evaluate 'if' conditions in the YAML config file.
    model_t* model;
} setup_sink_t;
