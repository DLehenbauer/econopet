.segment "CODE"

.proc reset_handler
        JMP reset_handler
.endproc

; Note that the 6502 automatically suspends IRQ and NMI until the reset handler returns.
.proc nmi_handler
        ; Set Interrupt Disable Flag to prevent IRQs (restored automatically by RTI)
        SEI
    
        ; PC and status register saved by cpu.  We need to save A/X/Y.
        PHA                 ; Save accumulator
        TXA                 ; Transfer X register to accumulator
        PHA                 ; Save X register
        TYA                 ; Transfer Y register to accumulator
        PHA                 ; Save Y register

        ; Initialize PIA1 for reading keyboard input
        LDA #$0F
        STA $E810
        LDA #$3C
        STA $E811
        LDA #$3D
        STA $E813

    keyscan_loop:
        INC $E810           ; Select next keyboard column

        ; Busy wait after selecting new column to give PIA1 an opportunity
        ; to register new input.

    outer_loop:
        LDX #$33            ; Load X
    inner_loop:
        DEX                 ; 2 cycles
        BNE inner_loop      ; 3 cycles if taken, 2 cycles if not
        BNE outer_loop      ; 3 cycles if taken, 2 cycles if not

        ; Read pressed keys in current column
        LDA $E812

        ; See if vector table has been restored:
        LDA $FFFE           ; Load the current low byte of the IRQ vector
        CMP #<irq_handler   ; Compare with the low byte of irq_handler
        BNE exit_menu       ; Branch if not equal
        
        LDA $FFFF           ; Load the current high byte of the IRQ vector
        CMP #>irq_handler   ; Compare with the high byte of irq_handler
        BEQ keyscan_loop    ; Branch if not equal
    
    ; Restore A/X/Y.  PC and status register restored by RTI.
    exit_menu:
        PLA                 ; Restore Y register
        TAY                 ; Transfer accumulator back to Y register
        PLA                 ; Restore X register
        TAX                 ; Transfer accumulator back to X register
        PLA                 ; Restore accumulator
        RTI                 ; Return from NMI
.endproc

.proc irq_handler
    RTI
.endproc

.segment "VECTORS"      ; $FFFA-$FFFF

vectors:
    .word nmi_handler   ; NMI vector (low byte at $FFFA, high byte at $FFFB)
    .word reset_handler ; Reset vector (low byte at $FFFC, high byte at $FFFD)
    .word irq_handler   ; IRQ/BRK vector (low byte at $FFFE, high byte at $FFFF)
