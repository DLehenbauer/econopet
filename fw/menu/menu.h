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

typedef enum {
    // Button is not pressed or currently being held.
    None        = 0,

    // Button has been released. Time held is below long press threshold.
    ShortPress  = 1,

    // Button has been held longer than long-press threshold.
    LongPress   = 2
} ButtonAction;

void menu_init_start();
void menu_init_end();
bool menu_task();
