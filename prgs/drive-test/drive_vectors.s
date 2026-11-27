; SPDX-License-Identifier: CC0-1.0
; https://github.com/dlehenbauer/econopet

; ROM 4 IEEE entries from the repository's revision-bound KERNAL map.
.export _drive_bus_init
.export _drive_bus_release
.export _drive_listen
.export _drive_talk
.export _drive_secondary
.export _drive_talk_secondary
.export _drive_ciout
.export _drive_acptr
.export _drive_unlisten
.export _drive_untalk
.export _drive_long_timeout
.export _drive_test_wait_via
.export _drive_test_wait_budget
.export _drive_send_bytes
.export _drive_recv_check
.export _drive_fail_halt
.export __STACKSIZE__
__STACKSIZE__ = $0200
.constructor reserve_target_mailbox, 32
.export _drive_bad_offset
.export _drive_bad_expected
.export _drive_bad_actual
.export _drive_bad_eoi
.export _drive_timeout_bus
.import _drive_timeout
.import _payload
.importzp ptr1

VIA_PORT_B = $e840
PIA1_PORT_A = $e810
PIA1_CONTROL_A = $e811
PIA2_PORT_A = $e820
PIA2_CONTROL_A = $e821
PIA2_PORT_B = $e822
PIA2_CONTROL_B = $e823
DEVICE = $d4
STATUS = $96
NRFD = $02
ATN = $04
NDAC_INPUT = $01
NRFD_INPUT = $40
DAV_INPUT = $80
EOI_INPUT = $40
CONTROL_LOW = $34
CONTROL_HIGH = $3c
SHORT_BUDGET = 12
FORMAT_BUDGET = 180

.segment "BSS"
source_byte: .res 1
source_valid: .res 1
received_byte: .res 1
wait_mask: .res 1
wait_expected: .res 1
wait_stage: .res 1
wait_outer: .res 1
stream_rem_lo: .res 1
stream_rem_hi: .res 1
stream_cmp_lo: .res 1
stream_cmp_hi: .res 1
_drive_test_wait_budget:
wait_budget: .res 1
_drive_bad_offset: .res 2
_drive_bad_expected: .res 1
_drive_bad_actual: .res 1
_drive_bad_eoi: .res 1
_drive_timeout_bus: .res 1

.segment "ONCE"

; Clamp CC65's software stack ($55/$56) and BASIC MEMSIZ/FRETOP ($34/$35, $30/$31)
; below the reserved $7FF0-$7FFF target diagnostic mailbox before main() runs.
reserve_target_mailbox:
    lda $56
    cmp #$7f
    bcc @done
    bne @clamp
    lda $55
    cmp #$f1
    bcc @done
@clamp:
    lda #$f0
    sta $55
    sta $34
    sta $30
    lda #$7f
    sta $56
    sta $35
    sta $31
@done:
    rts

.segment "CODE"

; Initialize the PET PIA/VIA IEEE-488 ports via ROM 4 CINT and reset buffered state.
_drive_bus_init:
    ; Call ROM 4 CINT ($E000) to configure screen and I/O port directions.
    jsr $e000

    ; Clear the deferred CIOUT byte latch and install the default handshake timeout budget.
    lda #0
    sta source_valid
    lda #SHORT_BUDGET
    sta wait_budget
    rts

; Select FORMAT_BUDGET when A != 0, or restore SHORT_BUDGET when A == 0.
_drive_long_timeout:
    ; Branch on the boolean argument in A to pick the outer polling loop count.
    cmp #0
    beq @short
    lda #FORMAT_BUDGET
    bne @set
@short:
    lda #SHORT_BUDGET
@set:
    sta wait_budget
    rts

; Poll VIA Port B until (VIA_PORT_B & wait_mask) == wait_expected, aborting on timeout.
_drive_test_wait_via:
wait_via:
    ; Seed the 24-bit software countdown from wait_budget (X=256, Y=256 inner loops).
    lda wait_budget
    sta wait_outer
    ldx #0
    ldy #0
@loop:
    lda VIA_PORT_B
    and wait_mask
    cmp wait_expected
    beq @ready
    dex
    bne @loop
    dey
    bne @loop
    dec wait_outer
    bne @loop

    ; Snapshot the stuck VIA Port B lines, release controller outputs, and report wait_stage.
    lda VIA_PORT_B
    sta _drive_timeout_bus
    jsr _drive_bus_release
    lda wait_stage
    jmp _drive_timeout
@ready:
    rts

; Release all PET IEEE-488 controller outputs without issuing UNLISTEN or UNTALK.
_drive_bus_release:
    ; Drive EOI, NDAC, and DAV manual C2 outputs high (idle).
    lda #CONTROL_HIGH
    sta PIA1_CONTROL_A
    sta PIA2_CONTROL_A
    sta PIA2_CONTROL_B

    ; Float inverted DIO lines ($FF -> bus idle) and release ATN and NRFD on VIA Port B.
    lda #$ff
    sta PIA2_PORT_B
    lda VIA_PORT_B
    ora #(ATN | NRFD)
    sta VIA_PORT_B
    rts

; Perform a complete source handshake to transmit the byte in A under the current ATN/EOI state.
send_byte:
    ; Drive the bitwise-inverted data byte onto PIA2 Port B before waiting for NRFD high and NDAC low.
    eor #$ff
    sta PIA2_PORT_B
    lda #(NRFD_INPUT | NDAC_INPUT)
    sta wait_mask
    lda #NRFD_INPUT
    sta wait_expected
    lda #1
    sta wait_stage
    jsr wait_via

    ; Assert DAV low via PIA2 CB2 and wait for listeners to release NDAC high.
    lda #CONTROL_LOW
    sta PIA2_CONTROL_B
    lda #NDAC_INPUT
    sta wait_mask
    sta wait_expected
    lda #2
    sta wait_stage
    jsr wait_via

    ; Release DAV high and return DIO outputs to idle ($FF).
    lda #CONTROL_HIGH
    sta PIA2_CONTROL_B
    lda #$ff
    sta PIA2_PORT_B
    rts

; Flush a buffered CIOUT byte with EOI asserted before changing bus addressing.
flush_byte:
    ; Return immediately when no deferred byte is waiting in source_byte.
    lda source_valid
    beq @done

    ; Pull EOI low on PIA1 CA2, transmit source_byte, then restore EOI high and clear source_valid.
    lda #CONTROL_LOW
    sta PIA1_CONTROL_A
    lda source_byte
    jsr send_byte
    lda #CONTROL_HIGH
    sta PIA1_CONTROL_A
    lda #0
    sta source_valid
@done:
    rts

; Wait for any active talker to release DAV high, then assert ATN low and release NRFD high.
assert_atn:
    ; Ensure DAV is high (idle) before claiming attention on the bus.
    lda #DAV_INPUT
    sta wait_mask
    sta wait_expected
    lda #3
    sta wait_stage
    jsr wait_via

    ; Release NRFD and pull ATN low on VIA Port B.
    lda VIA_PORT_B
    ora #NRFD
    and #($ff - ATN)
    sta VIA_PORT_B
    rts

; Deassert ATN high while preserving the current NRFD output state.
release_atn:
    lda VIA_PORT_B
    ora #ATN
    sta VIA_PORT_B
    rts

; Address zero-page DEVICE ($D4) as a listener ($20 | device) under ATN.
_drive_listen:
    ; Flush any pending data byte with EOI, release NDAC, assert ATN, and send LISTEN.
    jsr flush_byte
    lda #CONTROL_HIGH
    sta PIA2_CONTROL_A
    jsr assert_atn
    lda DEVICE
    ora #$20
    jmp send_byte

; Address zero-page DEVICE ($D4) as a talker ($40 | device) under ATN.
_drive_talk:
    ; Flush any pending data byte with EOI, release NDAC, assert ATN, and send TALK.
    jsr flush_byte
    lda #CONTROL_HIGH
    sta PIA2_CONTROL_A
    jsr assert_atn
    lda DEVICE
    ora #$40
    jmp send_byte

; Transmit the secondary address in A under ATN, then release ATN for listener data transfer.
_drive_secondary:
    jsr send_byte
    jmp release_atn

; Transmit the talk secondary address in A, arm NRFD/NDAC low as acceptor, and release ATN.
_drive_talk_secondary:
    ; Send the secondary address byte while ATN remains asserted.
    jsr send_byte

    ; Assert NRFD and NDAC low before releasing ATN (matching PET ROM 4 TKSA).
    lda VIA_PORT_B
    and #($ff - NRFD)
    sta VIA_PORT_B
    lda #CONTROL_LOW
    sta PIA2_CONTROL_A
    jmp release_atn

; Queue one output byte in source_byte, flushing the prior byte without EOI first.
_drive_ciout:
    ; Save the new byte, transmit any previously buffered byte with EOI high, and latch the new byte.
    pha
    lda source_valid
    beq @store
    lda source_byte
    jsr send_byte
@store:
    pla
    sta source_byte
    lda #1
    sta source_valid
    rts

; Receive one byte from an active talker into A, setting bit 6 ($40) in STATUS ($96) on EOI.
_drive_acptr:
    ; Assert NDAC low and release NRFD high to invite the talker's next byte, then wait for DAV low.
    lda #CONTROL_LOW
    sta PIA2_CONTROL_A
    lda VIA_PORT_B
    ora #NRFD
    sta VIA_PORT_B
    lda #DAV_INPUT
    sta wait_mask
    lda #0
    sta wait_expected
    lda #4
    sta wait_stage
    jsr wait_via

    ; Pull NRFD low, latch EOI ($40) into STATUS if PIA1 PA6 is low, and read inverted DIO from PIA2 PA.
    lda VIA_PORT_B
    and #($ff - NRFD)
    sta VIA_PORT_B
    lda PIA1_PORT_A
    and #EOI_INPUT
    bne @no_eoi
    lda STATUS
    ora #$40
    sta STATUS
@no_eoi:
    lda PIA2_PORT_A
    eor #$ff
    sta received_byte

    ; Release NDAC high to acknowledge the byte, wait for DAV high, then reassert NDAC low.
    lda #CONTROL_HIGH
    sta PIA2_CONTROL_A
    lda #DAV_INPUT
    sta wait_mask
    sta wait_expected
    lda #5
    sta wait_stage
    jsr wait_via
    lda #CONTROL_LOW
    sta PIA2_CONTROL_A
    lda received_byte
    rts

; Flush any buffered byte with EOI and send UNLISTEN ($3F) under ATN.
_drive_unlisten:
    jsr flush_byte
    jsr assert_atn
    lda #$3f
    jsr send_byte
    jmp release_atn

; Cancel any pending talker handshake and send UNTALK ($5F) under ATN.
_drive_untalk:
    ; Assert ATN before releasing NRFD and NDAC (matching PET ROM 4) to preempt a pending talker byte.
    lda VIA_PORT_B
    and #($ff - ATN)
    sta VIA_PORT_B
    ora #NRFD
    sta VIA_PORT_B
    lda #CONTROL_HIGH
    sta PIA2_CONTROL_A
    jsr assert_atn
    lda #$5f
    jsr send_byte
    jmp release_atn

; Stream AX (16-bit length: A = low, X = high) bytes from _payload through _drive_ciout.
_drive_send_bytes:
    ; Return immediately when length == 0 so empty-file writes send no data bytes before UNLISTEN.
    sta stream_rem_lo
    stx stream_rem_hi
    ora stream_rem_hi
    beq @done

    ; Point ptr1 at _payload and feed each byte through _drive_ciout until the 16-bit counter hits zero.
    lda #<_payload
    sta ptr1
    lda #>_payload
    sta ptr1+1
@loop:
    ldy #0
    lda (ptr1),y
    jsr _drive_ciout
    inc ptr1
    bne @no_carry
    inc ptr1+1
@no_carry:
    lda stream_rem_lo
    bne @dec_lo
    dec stream_rem_hi
@dec_lo:
    dec stream_rem_lo
    lda stream_rem_lo
    ora stream_rem_hi
    bne @loop
@done:
    rts

; Receive _drive_bad_offset (preset to total expected byte count) bytes from an active talker,
; comparing the first AX bytes against _payload and verifying exact final-byte EOI placement.
; Returns A=1/X=0 on match, or A=0/X=0 with _drive_bad_* populated on the first mismatch.
_drive_recv_check:
    ; Capture compare_length (AX) and total_length (_drive_bad_offset), then reset diagnostic outputs.
    sta stream_cmp_lo
    stx stream_cmp_hi
    lda _drive_bad_offset
    sta stream_rem_lo
    lda _drive_bad_offset+1
    sta stream_rem_hi
    lda #0
    sta _drive_bad_offset
    sta _drive_bad_offset+1
    sta _drive_bad_eoi
    lda #<_payload
    sta ptr1
    lda #>_payload
    sta ptr1+1
@loop:
    ; Exit with success once all expected bytes have been received and checked.
    lda stream_rem_lo
    ora stream_rem_hi
    bne @have_more
    jmp @ok
@have_more:
    ; Clear STATUS ($96) and receive the next byte via _drive_acptr.
    lda #0
    sta STATUS
    jsr _drive_acptr
    tax

    ; Compare the received byte against _payload[offset] while stream_cmp > 0.
    lda stream_cmp_lo
    ora stream_cmp_hi
    beq @check_eoi
    ldy #0
    txa
    cmp (ptr1),y
    beq @cmp_ok
    stx _drive_bad_actual
    lda (ptr1),y
    sta _drive_bad_expected
    lda #0
    ldx #0
    rts
@cmp_ok:
    lda stream_cmp_lo
    bne @dec_cmp_lo
    dec stream_cmp_hi
@dec_cmp_lo:
    dec stream_cmp_lo
@check_eoi:
    ; Decrement remaining count and require EOI iff this byte is the final byte of the stream.
    lda stream_rem_lo
    bne @dec_rem_lo
    dec stream_rem_hi
@dec_rem_lo:
    dec stream_rem_lo
    lda stream_rem_lo
    ora stream_rem_hi
    beq @expect_eoi
    lda STATUS
    and #$40
    bne @unexpected_eoi
    jmp @next
@expect_eoi:
    lda STATUS
    and #$40
    beq @missing_eoi
@next:
    ; Advance the payload pointer and zero-based byte offset for the next iteration.
    inc ptr1
    bne @inc_off
    inc ptr1+1
@inc_off:
    inc _drive_bad_offset
    beq @inc_off_hi
    jmp @loop
@inc_off_hi:
    inc _drive_bad_offset+1
    jmp @loop
@unexpected_eoi:
    ; Record early EOI (expected 0, actual 1) and return 0.
    lda #0
    sta _drive_bad_expected
    lda #1
    sta _drive_bad_actual
    sta _drive_bad_eoi
    lda #0
    ldx #0
    rts
@missing_eoi:
    ; Record missing final-byte EOI (expected 1, actual 0) and return 0.
    lda #1
    sta _drive_bad_expected
    sta _drive_bad_eoi
    lda #0
    sta _drive_bad_actual
    ldx #0
    rts
@ok:
    lda #1
    ldx #0
    rts

; Spin in place after failure reporting so VICE or hardware breakpoints trap cleanly.
_drive_fail_halt:
    jmp _drive_fail_halt
