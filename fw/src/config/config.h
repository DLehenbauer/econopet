// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include "config/config_setup.h"
#include "display/window.h"

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
