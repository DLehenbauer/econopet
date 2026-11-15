// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include "system/state.h"

void menu_init(void);
void menu_boot(void);
void menu_enter(void);
void read_keymap(const char* filename, system_state_t* config);
