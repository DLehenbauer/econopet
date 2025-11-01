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
#include "config/config_setup.h"
#include "menu/window.h"

typedef void (*on_enter_config_fn_t)(void* context);
typedef void (*on_exit_config_fn_t)(void* context, const char* name);

// Struct for sinking parsed data
typedef struct config_sink_s {
    void* const context;
    const on_enter_config_fn_t on_enter_config;
    const on_exit_config_fn_t on_exit_config;
    const setup_sink_t* const setup;
} config_sink_t;

void parse_config_file(const char* filename, const config_sink_t* const sink, int target_index);
