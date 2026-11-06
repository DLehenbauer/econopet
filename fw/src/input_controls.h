// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define INPUT_LONG_PRESS_US 500000u

typedef struct {
    // Start of the current eligible hold, in time_us_64() microseconds.
    uint64_t since;
    // The control remains eligible (button pressed or both Shifts held alone).
    bool held;
    // Suppress this hold until eligibility ends (consumed or ignored at startup).
    bool blocked;
} input_hold_t;

// Records eligibility transitions without losing release/repress between polls.
void input_hold_update(input_hold_t* hold, bool eligible);
// Reports a continuously held, unconsumed long press.
bool input_hold_ready(const input_hold_t* hold);
// Requires a held source to be released before it can qualify again.
void input_hold_consume(input_hold_t* hold);

// Samples the active-high physical button and debounces its state.
void input_button_update(bool pressed);
// Takes a button/chord event classified only by hold duration, or EOF if none is ready.
int input_controls_getch(void);
// Consumes currently held entry controls and discards pending button releases.
void input_controls_consume(void);
