# Bring-Up Notes

With just CPU & RAM, the Commodore PET will boot to the Terminal interface Monitor (TIM).

It enters the TIM because because the ROM reads that status of PIA1/PA7 to determine if the DIAG pin is pulled low:

```asm
FD3E LDA $E810  ; Read PIA1 Port A (normally return $ff)
FD41 BMI $FD46  ; Test PA7 (UserPort pin 5) -- bit 7 = DIAG (0: TIM, 1: BASIC)
FD43 JMP $D472  ; Jump to MONITOR
FD46 JMP $D3B6  ; Jump to BASIC initialization
```

With PIA1 (or a kludge to return 1xxx_xxxx) the boot will continue to the BASIC ready prompt.
However, there will be no cursor.
This is because the PET relies an V-Sync to trigger interrupts (again via PIA, IIRC).
Here are options are:
* Generate IRQBs.
* Generate V-Sync so that PIA1 produces the IRQBs.
