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

// Callback type for handling parsed configs
typedef void (*enter_config_callback_t)(void* user_data);
typedef void (*exit_config_callback_t)(void* user_data, const char* name);
typedef void (*action_load_callback_t)(void* user_data, const char* file, uint32_t address);

// Struct for sinking parsed data
typedef struct config_sink_s {
    const enter_config_callback_t on_enter_config;
    const exit_config_callback_t on_exit_config;
    const action_load_callback_t on_action_load;
    void* const user_data; // User data for callbacks
} config_sink_t;

void parse_config_file(const char* filename, const config_sink_t* const sink);
