// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include "config/config_setup.h"
#include "display/window.h"

// Enumerates configurations without executing their setup actions.
#define CONFIG_ENUMERATE (-1)

// Stored configuration IDs and names hold 40 text bytes plus the terminating null.
// The parser and menu retain the existing truncation behavior for longer strings.
#define CONFIG_TEXT_CAPACITY 41

typedef void (*on_enter_config_fn_t)(void* context);
typedef void (*on_exit_config_fn_t)(void* context, const char* id, const char* name);
typedef void (*on_default_fn_t)(void* context, const char* id);

// Struct for sinking parsed data
typedef struct config_sink_s {
    void* const context;
    const on_enter_config_fn_t on_enter_config;
    const on_exit_config_fn_t on_exit_config;
    const on_default_fn_t on_default;
    const setup_sink_t* const setup;
} config_sink_t;

// Parses the selected zero-based configuration, or enumerates with CONFIG_ENUMERATE.
void parse_config_file(const char* filename, const config_sink_t* const sink, int target_index);
