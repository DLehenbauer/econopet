/* SPDX-License-Identifier: CC0-1.0 */
/* https://github.com/dlehenbauer/econopet */

/*
 * bp.c -- STP instruction test for breakpoint subsystem.
 *
 * Prints a banner then executes a STP ($DB) opcode. The MCU breakpoint
 * handler should detect the halt and log that the address is not in
 * its breakpoint table (a "wild" STP). It will clear the halt and
 * resume the CPU, which immediately re-fetches the same STP and halts
 * again. This produces a repeating pattern in the MCU log that
 * confirms the breakpoint detection and resume path are working.
 */

#include <conio.h>

static unsigned char count;

int main(void)
{
    clrscr();
    cputs("=== stp test ===\r\n\r\n");
    cputs("executing stp...\r\n");

    /* Loop back to execute STP again so the pattern repeats even if
       the CPU happens to skip past the STP byte. */
    for (;;) {
        __asm__("STP");
        cprintf("stp executed %u times\r\n", ++count);
        ++count;
    }

    return 0;
}
