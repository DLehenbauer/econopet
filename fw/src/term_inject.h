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

#include <stdbool.h>

/**
 * Inject an ASCII character as a PET keystroke.
 * 
 * This function queues the character to be injected. The actual keystroke
 * injection happens during input_task() processing, which handles press/release
 * timing and syncs the keyboard matrix to the FPGA.
 * 
 * @param ch The ASCII character to inject (0-127)
 * @return true if the character was queued, false if queue is full or char unsupported
 */
bool term_inject_char(int ch);

/**
 * Process pending keystroke injections.
 * 
 * This function should be called from input_task() to handle the timing of
 * key press and release events. It modifies usb_key_matrix and relies on
 * sync_state() being called afterward to send the matrix to the FPGA.
 */
void term_inject_task(void);
