.include  "pet.inc"

.segment "CODE"

; Jump table for reset vector at $FF00 - each entry is 3 bytes (JMP instruction).
; The firmware selects the boot tune by writing $FF00 + (entry * 3) to the reset vector.
reset_vector_table:
        JMP reset_happy         ; Entry 0: Normal boot ($FF00)
        JMP reset_uhoh          ; Entry 1: Error boot ($FF03)

; PIA1 and PIA2 declarations haven't yet been picked up by Ubuntu 'cc65' package.
; https://github.com/cc65/cc65/commit/12f9a2f1f87b90d0ec880bc4e76be10996150b9f

; ---------------------------------------------------------------------------
; I/O: 6520 PIA1

PIA1            := $E810        ; PIA1 base address
PIA1_PORTA      := PIA1+$0      ; Port A (PA) and data direction register A (DDRA)
PIA1_PACTL      := PIA1+$1      ; Port A control register (CRA)
PIA1_PORTB      := PIA1+$2      ; Port B (PB) and data direction register B (DDRB)
PIA1_PBCTL      := PIA1+$3      ; Port B control register (CRB)

; ---------------------------------------------------------------------------
; I/O: 6520 PIA2

PIA2            := $E820        ; PIA2 base address
PIA2_PORTA      := PIA2+$0      ; Port A (PA) and data direction register A (DDRA)
PIA2_PACTL      := PIA2+$1      ; Port A control register (CRA)
PIA2_PORTB      := PIA2+$2      ; Port B (PB) and data direction register B (DDRB)
PIA2_PBCTL      := PIA2+$3      ; Port B control register (CRB)

; $00-$01 points to the notes table
; X = VIA_SR (octave / waveform)
.proc play_tune
        LDY #$10        ; VIA ACR (sound on)
        STY VIA_CR
        STX VIA_SR

    next_note:
        LDX #$00
        LDA ($00,X)     ; Load note from address at $00/$01
        BEQ done        ; If note is zero, exit
        STA VIA_T2CL
        INC $00         ; Increment low byte of address
    loopX:
        LDY #$30
    loopY:
        DEY 
        BNE loopY       ; Loop until Y = 0

        DEX
        BNE loopX       ; Loop until X = 0
        BEQ next_note   ; Then go to next note

    done:
        LDA #$00
        STA VIA_SR
        STA VIA_CR
        RTS
.endproc

.proc play_happy_tune
        ; Initialize pointer to notes at $00-$01
        LDA #<notes     ; Low byte of notes address
        STA $00
        LDA #>notes     ; High byte of notes address
        STA $01

        LDX #$55        ; VIA SR (octave / waveform)

        BNE play_tune   ; Tail call optimization
notes:
        .byte $EC, $D2, $9D, $75, $00
.endproc

.proc play_uhoh_tune
        ; Initialize pointer to notes at $00-$01
        LDA #<notes     ; Low byte of notes address
        STA $00
        LDA #>notes     ; High byte of notes address
        STA $01

        LDX #$17        ; VIA SR (octave / waveform)

        BNE play_tune   ; Tail call optimization
notes:
        .byte $A7, $A7, $EC, $EC, $00
.endproc

.proc init_io
        ; After reset, PIA data direction registers default to input mode (all bits 0),
        ; and control registers have bit 2 set (DDR access mode enabled).

        LDA #$7F        ; Disable all interrupts
        STA VIA_IER     ; [7] 1=Enable, 0=Disable, [5:0] Mask

        LDA #$0F        ; PIA1 DDRA: 7-4 inputs, 3-0 outputs
        STA PIA1_PORTA  ; [7] Diag in, [6] EOI in, [5] Cass Sw #1 in, [4] Cass Sw #2 in, [3:0] Key Row Select out

        ASL A           ; A = $1E
        STA VIA_PB      ; [7] DAV=x,  [6] NRFD=x,  [5] retrace=x,  [4] Cass #2 Mt=1,   [3] Cass WR=1,   [2] ATN=1,   [1] NRFD=1,   [0] NDAC=x
        STA VIA_DDRB    ; [7] DAV in, [6] NRFD in, [5] retrace in, [4] Cass #2 Mt out, [3] Cass WR out, [2] ATN out, [1] NRFD out, [0] NDAC in
        
        LDX #$FF
        STX PIA2_PORTB  ; [7:0] IEEE data out
        STX VIA_T1CH    ; VIA T1 HI

        ; From: https://www.zimmers.net/anonftp/pub/cbm/maps/PETio.txt
        ;
        ; bit   meaning
        ; ---   -------
        ;  7    CA1 active transition flag. 1= 0->1, 0= 1->0
        ;  6    CA2 active transition flag. 1= 0->1, 0= 1->0
        ;  5    CA2 direction	      1 = out	       | 0 = in
        ;  4    CA2 control   Handshake=0 | Manual=1   | Active: High=1 Low=0
        ;  3    CA2 control   On Read=0	  | CA2 High=1 | IRQ on=1, IRQ off=0
        ;                     Pulse  =1   | CA2 Low=0  |
        ;  2    Port A control: DDRA = 0, IORA = 1
        ;  1    CA1 control: Active High = 1, Low = 0
        ;  0    CA1 control: IRQ on=1, off = 0

        LDA #$3D        ; 0011 1101
        STA PIA1_PBCTL  ; CB2: IEEE EOI out=1,   PB: IORA, CB1: Screen retrace in (generates IRQ)
        
        LDA #$3C        ; 0011 1100
        STA PIA2_PACTL  ; CA2: IEEE NDAC out=1,  PA: IORA, CA1: IEEE ATN in
        STA PIA2_PBCTL  ; CB2: IEEE DAV out=1,   PB: IORA, CB1: IEEE SRQ in
        STA PIA1_PACTL  ; CA2: Cass #1 Mt out=1, PA: IORA, CA1: Cass #1 Rd in

        ; VIA SR/ACR: No sound
        LDX #$00
        STX VIA_SR   ; VIA SR
        STX VIA_CR   ; VIA ACR

        ; From: https://www.zimmers.net/anonftp/pub/cbm/maps/PETio.txt
        ;
        ; 7-5 CB2 control
        ;     7  direction
        ;         1 = output
        ;         1 0 = do handshake
        ;         1 0 0 = on write
        ;         1 0 1 = pulse?
        ;         1 1 = manual CB2 control
        ;         1 1 0 = CB1 low
        ;         1 1 1 = CB1 high
        ;         0 = input
        ;         0 0 x = Active low:  1->0
        ;         0 1 x = Active high: 0->1
        ;         0 x 0 = Clear Interrupt condition on write of 1 in IFR or r/w of ORB
        ;         0 x 1 = Clear Interrupt condition on write of 1 in IFR only
        ;     4   CB1 control
        ;         0 = active transition low
        ;         1 = active transition high
        ; 3-1 CA2 control (similar to CB2 control)
        ;     3  direction
        ;         1 = output
        ;         1 0 = do handshake
        ;         1 0 0 = on read
        ;         1 0 1 = pulse
        ;         1 1 = manual CA2 control
        ;         1 1 0 = CA1 low
        ;         1 1 1 = CA1 high
        ;         0 = input
        ;         0 0 x = Active low:  1->0
        ;         0 1 x = Active high: 0->1
        ;         0 x 0 = Clear Interrupt condition on write of 1 in IFR or r/w of ORB
        ;         0 x 1 = Clear Interrupt condition on write of 1 in IFR only
        ;     0   CA1 control
        ;         0 = active transition low
        ;         1 = active transition high

        LDA #$0E                    ; 0000 1110
        STA VIA_PCR                 ; CB2: in active low, CB1: Cass #2 Rd in active low, CA2 Graphic out=1 (Lower), CA1: active low

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
    start:
        ; Original keyscan resets these on each interrupt.  Probably unnecessary.
        LDA #$3C
        STA PIA1_PACTL
        LDA #$3D
        STA PIA1_PBCTL
        LDY #$09            ; Start on keyboard row 9

    keyscan_loop:
        STY PIA1_PORTA      ; Select row

        ; Busy wait to give PIA1 an opportunity to detect new input.  The delay below is
        ; approximately the same number of cycles that the original keyscan routine used
        ; to check bits in the currently selected row.

        LDX #$16            ; Load X
    inner_loop:
        DEX                 ; 2 cycles
        BNE inner_loop      ; 3 cycles if taken, 2 cycles if not

debounce:
        LDA PIA1_PORTB      ; Read pressed keys in current row
        CMP PIA1_PORTB      ; Compare with previous value
        BNE debounce        ; If not equal, wait for stable state

        DEY                 ; Move to next row (descending)
        BMI start           ; If Y < 0, wrap around to row 9.
        BPL keyscan_loop    ; Continue scanning
.endproc

.proc reset_happy
        JSR init_io
        JSR play_happy_tune
        JMP keyscan
.endproc

.proc reset_uhoh
        JSR init_io
        JSR play_uhoh_tune
        JMP keyscan
.endproc

.proc nmi_handler
        ; Save processor state
        PHA             ; Save accumulator
        TXA
        PHA             ; Save X register
        TYA
        PHA             ; Save Y register
        
        JSR play_uhoh_tune

        ; Restore processor state
        PLA
        TAY             ; Restore Y register
        PLA
        TAX             ; Restore X register
        PLA             ; Restore accumulator
        RTI
.endproc

.proc irq_handler
        RTI
.endproc

.segment "VECTORS"      ; $FFFA-$FFFF

vectors:
    .word nmi_handler         ; NMI vector (low byte at $FFFA, high byte at $FFFB)
    .word reset_vector_table  ; Reset vector (low byte at $FFFC, high byte at $FFFD)
    .word irq_handler         ; IRQ/BRK vector (low byte at $FFFE, high byte at $FFFF)
