// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include "config/config_setup.h"
#include "display/window.h"

// Unmounts all disks, then applies the selected setup actions in configuration order.
void load_config(const setup_sink_t* setup_sink, int selected_config);

// Loads the automatic default or a manually selected configuration.
void menu_config_show(const window_t* const window, const setup_sink_t* const setup_sink, bool is_boot);
