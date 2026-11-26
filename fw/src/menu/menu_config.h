// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include "config/config_setup.h"
#include "config/config.h"
#include "display/window.h"
#include "menu_ui.h"

typedef struct {
    size_t count;
    int default_index;
} menu_config_catalog_t;

// Enumerates every model and optionally copies a bounded page of names.
// When present, default must precede configs.
menu_config_catalog_t menu_config_enumerate(size_t first, size_t capacity,
                                           char names[][CONFIG_TEXT_CAPACITY]);

/** Return the zero-based configuration index for an exact ID in /config.yaml, or -1 if absent. */
int menu_config_find_id(const char* id);

/** Reload setup actions with the CPU halted, replacing all configured disk mounts. */
void load_config(const setup_sink_t* setup_sink, int selected_config);

/** Run the interactive hierarchy, initializing catalog state when needed. */
menu_result_t menu_config_run(const window_t* window, menu_ui_root_t root);
