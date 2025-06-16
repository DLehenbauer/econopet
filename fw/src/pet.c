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

#include "driver.h"
#include "usb/keyboard.h"
#include "video/video.h"
#include "pet.h"

void pet_reset() {
    // Per the W65C02S datasheet (sections 3.10 - 3.11):
    //
    //  * The CPU requires a minimum of 2 clock cycles to reset.  The CPU clock is 1 MHz (1 us period).
    //  * Deasserting RDY prevents the CPU from advancing it's state on negative PHI2 edges.
    // 
    // (See: https://www.westerndesigncenter.com/wdc/documentation/w65c02s.pdf)

    // Out of paranoia, deassert CPU 'reset' to ensure the CPU observes a clean reset pulse.
    // (We set 'ready' to false to prevent the CPU from executing instructions.)
    set_cpu(/* ready: */ false, /* reset: */ false, /* nmi: */ false);
    sleep_us(4);

    memset(video_char_buffer, 0x20, VIDEO_CHAR_BUFFER_BYTE_SIZE);   // Clear video character buffer
    memset(pet_key_matrix, 0xff, sizeof(pet_key_matrix)); // Clear keyboard matrix
    memset(usb_key_matrix, 0xff, sizeof(usb_key_matrix)); // Clear USB keyboard matrix
    
    // Assert CPU 'reset'.  Execution continues to be suspended by deasserting 'ready'.
    set_cpu(/* ready: */ false, /* reset: */ true, /* nmi: */ false);
    sleep_us(4);
    
    // Finally, deassert CPU 'reset' and assert 'ready' to allow the CPU to execute instructions.
    set_cpu(/* ready: */ true,  /* reset: */ false, /* nmi: */ false);
}

void pet_nmi() {
    // Out of paranoia, deassert CPU 'NMI' to ensure the CPU observes a clean pulse.
    // (We set 'ready' to false to prevent the CPU from executing instructions.)
    set_cpu(/* ready: */ false, /* reset: */ false, /* nmi: */ false);
    sleep_us(4);
    
    // Assert CPU 'nmi'.  Execution continues to be suspended by deasserting 'ready'.
    set_cpu(/* ready: */ false, /* reset: */ false, /* nmi: */ true);
    sleep_us(4);
    
    // Finally, deassert CPU 'NMI' and assert 'ready' to allow the CPU to execute instructions.
    set_cpu(/* ready: */ true,  /* reset: */ false, /* nmi: */ false);
}
