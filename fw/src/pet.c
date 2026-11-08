// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "pet.h"

#include "driver.h"
#include "ieee/ieee_drive.h"
#include "input/input_routing.h"
#include "input/keyboard_core.h"
#include "menu/menu.h"
#include "sd/activity.h"
#include "system_state.h"
#include "usb/keyboard.h"

#define STORAGE_IDLE_POLL_US 1000u

void pet_wait_for_storage_idle(void) {
    for (;;) {
        ieee_drive_task();
        if (sd_activity_quiet())
            return;
        sleep_us(STORAGE_IDLE_POLL_US);
    }
}

void pet_task(void) {
    if (input_routing_pending_request().action == input_system_none || !sd_activity_quiet())
        return;
    const input_system_request_t request = input_routing_take_request();
    // Consume only when execution can begin, leaving input and drive servicing to the main loop.
    if (request.action == input_system_request_reset)
        pet_reset();
    else
        menu_enter(false);
}

// Performs a full PET reset, including CPU, IEEE-488 drive, and USB keyboard
// state.
//
// NOTE: PET/fabric resets *must* go through `pet_reset()`. Pulling the CPU
//       /RESB pin low independently is unsupported, as firmware does not detect
//       or recover from an independent fabric reset.
void pet_reset() {
    // Per the W65C02S datasheet (sections 3.10 - 3.11):
    //
    //  * The CPU requires a minimum of 2 clock cycles to reset.  The CPU clock is 1 MHz (1 us period).
    //  * Deasserting RDY prevents the CPU from advancing it's state on negative PHI2 edges.
    // 
    // (See: https://www.westerndesigncenter.com/wdc/documentation/w65c02s.pdf)

    // Out of paranoia, deassert CPU 'reset' to ensure the CPU observes a clean reset pulse.
    set_cpu(CPU_HALT);
    sleep_us(4);

    ieee_drive_reset();
    input_routing_reset();
    keyboard_core_reset(&system_state);
    dispatch_key_events();

    memset(system_state.video_char_buffer, 0x20, 0x800);            // Clear video character buffer
    memset(system_state.video_char_buffer + 0x800, 0x0f, 0x800);    // Set color buffer to white on black
    memset(pet_key_matrix, 0xff, sizeof(pet_key_matrix));           // Clear keyboard matrix
    memset(usb_key_matrix, 0xff, sizeof(usb_key_matrix));           // Clear USB keyboard matrix
    
    // Assert CPU 'reset' while execution remains suspended.
    set_cpu(CPU_RESET);
    sleep_us(4);
    
    // Finally, deassert CPU 'reset' and allow the CPU to execute instructions.
    set_cpu(CPU_READY);
}

void pet_nmi() {
    // Out of paranoia, deassert CPU 'NMI' to ensure the CPU observes a clean pulse.
    set_cpu(CPU_READY);
    sleep_us(4);
    
    // Assert CPU 'nmi'.
    set_cpu(CPU_READY | CPU_NMI);
    sleep_us(4);
    
    // Finally, deassert CPU 'NMI'.
    set_cpu(CPU_READY);
}
