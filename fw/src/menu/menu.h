// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include "system_state.h"

void menu_enter(void);
void menu_task(void);
void read_keymap(const char* filename, system_state_t* config);
