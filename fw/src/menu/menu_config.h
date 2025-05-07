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
#include "menu/window.h"

typedef void (*setup_sink_on_action_load_fn)(const char* file, uint32_t address);

typedef struct setup_sink_s {
    const setup_sink_on_action_load_fn on_action_load;
} setup_sink_t;

void menu_config_show(const window_t* const window, const setup_sink_t* const setup_sink);
