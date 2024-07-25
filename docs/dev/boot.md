With no I/O chips, the Commodore PET will boot to the Terminal interface Monitor (TIM).

This is because the ROM reads that status of PIA1/PA7 to determine if the DIAG pin is
pulled low.

```asm
FD3E LDA $E810  ; Read PIA1 Port A (normally return $ff)
FD41 BMI $FD46  ; Test PA7 (UserPort pin 5) -- bit 7 = DIAG (0: TIM, 1: BASIC)
FD43 JMP $D472  ; Jump to MONITOR
FD46 JMP $D3B6  ; Jump to BASIC initialization
```
