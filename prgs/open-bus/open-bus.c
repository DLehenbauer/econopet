/* SPDX-License-Identifier: CC0-1.0 */
/* https://github.com/dlehenbauer/econopet */

/*
 * open-bus.c -- Test program for PET/CBM open bus behavior.
 *
 * Verifies that reading unmapped I/O addresses ($E800) returns the last
 * byte transferred on the data bus, and that the result varies by
 * addressing mode -- especially for page-crossing cases where the NMOS
 * 6502 inserts a speculative read that changes the bus value.
 *
 * The 65C02 suppresses the speculative read on page crossings, so this
 * program detects the CPU type and adjusts expectations accordingly.
 *
 * See docs/dev/PET/compat.md for details.
 */

#include <conio.h>
#include <peekpoke.h>

static unsigned char result;
static unsigned char passed;
static unsigned char total;
static unsigned char is_cmos;           /* nonzero if 65C02 detected */

static void check(const char *name, unsigned char exp, unsigned char act)
{
    ++total;
    cputs(name);
    gotox(22);
    cprintf("e:%02x a:%02x ", exp, act);
    if (exp == act) {
        ++passed;
        cputs("ok");
    } else {
        revers(1);
        cputs("fail");
        revers(0);
    }
    cputs("\r\n");
}

int main(void)
{
    unsigned char crossing_exp;

    clrscr();
    cputs("=== open bus test ===\r\n\r\n");

    /* Detect CPU type independently of open bus behavior.  Opcode $1A is
       INA (increment A) on the 65C02 but a single-byte NOP on the NMOS 6502.
       Load A with 0 and execute INA: if A becomes 1, we have a 65C02. */
    __asm__("LDA #$00");
    __asm__("INA");
    __asm__("STA %v", is_cmos);

    if (is_cmos) {
        cputs("cpu: 65c02 (no speculative read)\r\n\r\n");
        crossing_exp = 0xE7;
    } else {
        cputs("cpu: 6502 (nmos)\r\n\r\n");
        crossing_exp = PEEK(0xE700u);
    }

    /* --- Absolute addressing --- */

    /* LDA $E800: last bus byte is operand high byte ($E8). */
    __asm__("LDA $E800");
    __asm__("STA %v", result);
    check("abs $e800", 0xE8, result);

    /* LDA $E800,Y with Y=0: no page cross, same as absolute. */
    __asm__("LDY #$00");
    __asm__("LDA $E800,Y");
    __asm__("STA %v", result);
    check("abs,y no cross", 0xE8, result);

    /* LDA $E7FF,Y with Y=1: page cross, speculative read from $E700. */
    __asm__("LDY #$01");
    __asm__("LDA $E7FF,Y");
    __asm__("STA %v", result);
    check("abs,y crossing", crossing_exp, result);

    /* LDA $E7FF,X with X=1: page cross, speculative read from $E700. */
    __asm__("LDX #$01");
    __asm__("LDA $E7FF,X");
    __asm__("STA %v", result);
    check("abs,x crossing", crossing_exp, result);

    /* --- Indirect addressing --- */

    /* LDA (zp),Y with zp=$E800, Y=0: last fetch is pointer high ($E8). */
    __asm__("LDA #$00");
    __asm__("STA ptr1");
    __asm__("LDA #$E8");
    __asm__("STA ptr1+1");
    __asm__("LDY #$00");
    __asm__("LDA (ptr1),Y");
    __asm__("STA %v", result);
    check("(zp),y no cross", 0xE8, result);

    /* LDA (zp),Y with zp=$E7FF, Y=1: page cross, speculative read from $E700. */
    __asm__("LDA #$FF");
    __asm__("STA ptr1");
    __asm__("LDA #$E7");
    __asm__("STA ptr1+1");
    __asm__("LDY #$01");
    __asm__("LDA (ptr1),Y");
    __asm__("STA %v", result);
    check("(zp),y crossing", crossing_exp, result);

    /* LDA (zp,X) with zp=$E800, X=0: last fetch is pointer high ($E8). */
    __asm__("LDA #$00");
    __asm__("STA ptr1");
    __asm__("LDA #$E8");
    __asm__("STA ptr1+1");
    __asm__("LDX #$00");
    __asm__("LDA (ptr1,X)");
    __asm__("STA %v", result);
    check("(zp,x)", 0xE8, result);

    /* --- Summary --- */
    cprintf("\r\n%u/%u passed\r\n", passed, total);
    return 0;
}
