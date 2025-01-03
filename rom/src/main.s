.segment "CODE"

.proc reset_handler
        LDA #$0F
        STA $E810
        LDA #$3C
        STA $E811
        LDA #$3D
        STA $E813
    keyscan_loop:
        INC $E810           ; Select next keyboard column
        LDA $E812           ; Read pressed keys in column
        JMP keyscan_loop
.endproc

.proc nmi_handler
    JMP nmi_handler
.endproc

.proc irq_handler
    JMP vectors
.endproc

.segment "VECTORS"      ; $FFFA-$FFFF

vectors:
    .word nmi_handler   ; NMI vector (low byte at $FFFA, high byte at $FFFB)
    .word reset_handler ; Reset vector (low byte at $FFFC, high byte at $FFFD)
    .word irq_handler   ; IRQ/BRK vector (low byte at $FFFE, high byte at $FFFF)
