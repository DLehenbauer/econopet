// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "menu.h"

#include "input.h"
#include "input_controls.h"
#include "pet.h"
#include "sd/activity.h"

// Retain the requested action while the main loop continues servicing the drive.
void menu_task(void) {
    static int pending = EOF;

    // If we don't have a pending action, check for new input.
    if (pending == EOF) {
        const int key = input_getch();
        if (key == KEY_BTN_SHORT || key == KEY_BTN_LONG || key == KEY_SHIFT_SHIFT) {
            pending = key;
        }
    }

    // If there was no input, or the SD activity is not quiet, return to the
    // main loop without taking action.
    if (pending == EOF || !sd_activity_quiet()) {
        return;
    }

    // At this point, we have a pending action and SD activity is quiet. Consume
    // and dispatch the action.
    const int key = pending;
    pending = EOF;
    input_controls_consume();
    if (key == KEY_BTN_SHORT) {
        pet_reset();
    } else {
        menu_enter(/* is_boot: */ false);
    }
}
