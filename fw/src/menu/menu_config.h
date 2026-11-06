// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include "config/config_setup.h"
#include "config/config.h"
#include "display/window.h"

typedef struct {
    size_t count;
    int default_index;
} menu_config_catalog_t;

// Enumerates every model and optionally copies a bounded page of names.
// When present, default must precede configs.
menu_config_catalog_t menu_config_enumerate(size_t first, size_t capacity,
                                           char names[][CONFIG_TEXT_CAPACITY]);

// Reloads setup actions with the CPU halted. When apply_mounts is true, existing
// disks are unmounted before YAML mounts are applied.
// Otherwise all live disks are preserved.
void load_config(const setup_sink_t* setup_sink, int selected_config, bool apply_mounts);

// Runs the boot picker or on-demand hierarchy over menu.rom, then reloads its choices.
void menu_config_show(const window_t* window, const setup_sink_t* setup_sink, bool is_boot);
