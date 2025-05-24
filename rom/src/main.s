.segment "CODE"

notes:
        .byte $75, $9D, $D2, $EC

uhoh:
        .byte $EC, $EC, $A7, $A7


.proc delay
        LDA #$30
    loop:
        SEC         ; Set carry flag
        SBC #$01    ; Subtract 1 from A
        BNE loop    ; Loop until A = 0
        RTS
.endproc

; On entry, $00-$01 points to the notes table. Y = number of notes.
.proc beep
        LDY #$03        ; Last note

        LDA #$10        ; VIA ACR
        STA $E84B

        LDA #$55
        STA $E84A
        DEY

next_note:
        LDA notes,Y     ; Load note from address at $00/$01 + Y offset
        STA $E848
        LDX #$00
loop:
        JSR delay
        DEX
        BNE loop

        DEY
        BPL next_note

        ; VIA SR/ACR: No sound
        LDX #$00
        STX $E84A   ; VIA SR
        STX $E84B   ; VIA ACR

        RTS
.endproc

.proc init_io
        ; Disable all interrupts
        LDA #$7F
        STA $E84E   ; VIA IER : [7] 1=Enable, 0=Disable, [5:0] Mask

        ; PIA1 Port A : [7] Diag, [6] EOI In, [5] Cass Sw #1, [4] Cass Sw #2, [3:0] Key Row Select
        LDA #$0F
        STA $E810   ; PIA1 PA
        
        ASL A       ; A = $1E

        ; VIA Port B : [7] DAV in, [6] NRFD in, [5] retrace, [4] Cass Mt, [3] Cass WR, [2] ATN out, [1] NRFD out, [0] NDAC in
        STA $E840   ; VIA PB
        STA $E842   ; VIA DDRB
        
        LDX #$FF

        ; PIA2 Port B: IEEE data out
        STX $E822   ; PIA2 PB

        STX $E845   ; VIA T1 HI

        ; PIA1 CB2: Cassette #1 motor
        ;      CB1: Screen retrace
        LDA #$3D
        STA $E813   ; PIA1 CRB
        
        LDA #$3C
        
        ; PIA2 CA2: IEEE NDAC out
        ;      CA1: IEEE ATN in
        STA $E821   ; PIA2 CRA

        ; PIA2 CB2: IEEE DAV out
        ;      CB1: IEEE SRQ in
        STA $E823   ; PIA2 CRB

        ; PIA1 CA2: Cassette #1 motor
        ;      CA1: Cassette #1 read 
        STA $E811   ; PIA1 CRA

        ; VIA SR/ACR: No sound
        LDX #$00
        STX $E84A   ; VIA SR
        STX $E84B   ; VIA ACR

        ; Init CRTC
        LDA #$0E                    ; VIA CA2: Graphics mode ($0C = Upper, $0E = Lower)
        STA $E84C

        LDY #$0E                    ; Start at the first byte of crtc_table
 	foreach_in_crtc_table:
        LDA crtc_table,Y            ; Load the byte at crtc_table + Y
        STY $E880                   ; Store Y to the CRTC register
        STA $E881                   ; Store A to the CRTC register
        DEY                         ; Increment Y to move to the next byte
        BPL foreach_in_crtc_table   ; Branch if X >= 0 (loop until all bytes are processed)
        RTS

    crtc_table:
        .byte $31, $28, $29, $0F, $31, $00, $19, $25
        .byte $00, $07, $00, $00, $10, $00, $00, $00
        .byte $00
.endproc

.proc keyscan
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
        ; to detect new input.

    outer_loop:
        LDX #$37            ; Load X
    inner_loop:
        DEX                 ; 2 cycles
        BNE inner_loop      ; 3 cycles if taken, 2 cycles if not
        BNE outer_loop      ; 3 cycles if taken, 2 cycles if not

        ; Read pressed keys in current column
        LDA $E812

        ; Continue scanning with next column
        JMP keyscan_loop
.endproc

.proc reset_handler
        JSR init_io
        JSR beep
        JMP keyscan
.endproc

.proc nmi_handler
    RTI
.endproc

.proc irq_handler
    RTI
.endproc

.segment "VECTORS"      ; $FFFA-$FFFF

vectors:
    .word nmi_handler   ; NMI vector (low byte at $FFFA, high byte at $FFFB)
    .word reset_handler ; Reset vector (low byte at $FFFC, high byte at $FFFD)
    .word irq_handler   ; IRQ/BRK vector (low byte at $FFFE, high byte at $FFFF)
