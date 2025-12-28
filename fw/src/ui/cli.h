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
 * CLI - Command Line Interface
 * 
 * Provides a simple command-line interface over the serial console for:
 * - Viewing log messages with optional filtering
 * - Remote control of the PET via VT-100 terminal
 */

/**
 * Initialize the CLI subsystem.
 * Call once at startup before entering the main loop.
 */
void cli_init(void);

/**
 * Process a single character of CLI input.
 * Called from the input task when in CLI mode.
 * 
 * @param ch The character received from the UART
 */
void cli_process_char(int ch);

/**
 * Check if CLI is currently in remote mode.
 * Used by input routing to determine if Ctrl+C should exit remote mode.
 * 
 * @return true if in remote mode, false if at CLI prompt
 */
bool cli_in_remote_mode(void);

/**
 * Exit remote mode and return to CLI prompt.
 * Called when Ctrl+C is received during remote mode.
 */
void cli_exit_remote(void);
