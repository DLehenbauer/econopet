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
#include "display/window.h"

// Define constants for special keys
#define KEY_UP    1000
#define KEY_DOWN  1001
#define KEY_RIGHT 1002
#define KEY_LEFT  1003
#define KEY_HOME  1004
#define KEY_END   1005
#define KEY_PGUP  1006
#define KEY_PGDN  1007

extern int term_input_char();
extern void term_present();

void term_begin();
void term_display(const window_t* const window);
void term_end();
int term_getch();
