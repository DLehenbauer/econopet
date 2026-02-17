# Tape

The Commodore PET supports two cassette drives (Cassette #1 and Cassette #2) for loading and saving programs. The PET Datasette (C2N) is a simple audio cassette mechanism with no intelligence of its own. All encoding, decoding, and motor control are performed by the PET's KERNAL software using the PIA and VIA I/O chips.

## Signals

Signal         | Chip | Address | Register | Bit | Direction | Description
---------------|------|---------|----------|-----|-----------|----------------------------------
CASS1_SWITCH   | PIA1 | E810    | PIBA     |  4  | In        | Cassette #1 sense (0 = key pressed, 1 = no key pressed)
CASS2_SWITCH   | PIA1 | E810    | PIBA     |  5  | In        | Cassette #2 sense (0 = key pressed, 1 = no key pressed)
CASS1_READ     | PIA1 | E811    | CRA      |  7  | In (CA1)  | Cassette #1 read data
CASS1_MOTOR    | PIA1 | E811    | CRA      |  3  | Out (CA2) | Cassette #1 motor (0 = On, 1 = Off)
CASS_WRITE     | VIA  | E840    | ORB      |  3  | Out       | Cassette write signal
CASS2_MOTOR    | VIA  | E840    | ORB      |  4  | Out       | Cassette #2 motor (0 = On, 1 = Off)

## Sense Pin

The cassette sense pin (PIA1 PA4 for Cassette #1) reflects the physical state of the cassette deck's mechanical switches. It is active-low: bit 4 reads 0 when a key is pressed, and 1 when no key is pressed.

The sense pin is asserted (driven low) whenever the user presses the PLAY, REW (rewind), or F.FWD (fast forward) keys on the Datasette. The KERNAL does not distinguish which key was pressed. It only checks whether any transport key is currently held down.

The STOP/EJECT key is mechanical only and releases all other keys, which causes the sense pin to go inactive (high).

## Loading a Program

When the user types `LOAD "program"`, the following sequence occurs:

1. BASIC calls the KERNAL LOAD routine, which opens Cassette #1 as the default tape device (device 1).

2. The KERNAL displays:

   ```text
   PRESS PLAY ON TAPE
   ```

3. The KERNAL enters a loop polling PIA1 PA4 (the sense pin), waiting for it to go low. The tape mechanism is idle at this point because the motor is off. The user must physically press the PLAY key on the Datasette.

4. Once the sense pin goes low, the KERNAL turns the motor on by clearing the CA2 output on PIA1 (setting CRA bit 3 to drive CA2 low). The tape begins moving.

5. The KERNAL reads data from the tape via the CA1 input on PIA1. Each pulse edge on the read line triggers an interrupt (or is polled), and the KERNAL measures the time between transitions to decode the bit stream. The PET tape format encodes data as pairs of pulses with varying periods to represent 0 and 1 bits.

6. The KERNAL searches for a tape header that matches the requested filename. If the header does not match, it continues searching. If the header matches, the KERNAL displays:

   ```text
   FOUND "program"
   LOADING
   ```

7. The KERNAL reads the program data into memory.

8. When the load is complete (or on error), the KERNAL turns the motor off by setting CA2 high. The tape stops.

## Motor Control

The KERNAL controls the Cassette #1 motor through the CA2 output of PIA1 (active-low: 0 = motor on, 1 = motor off). Cassette #2 motor is controlled via VIA PB4. The motor is only energized when the KERNAL is actively reading or writing the tape. At all other times the motor is off, even if PLAY is held down.

Because the cassette mechanism has no fast-forward or rewind motor control from the PET, the REW and F.FWD operations are entirely mechanical and always run at full speed regardless of motor state.

## Tape Encoding

The PET uses a simple pulse-width encoding scheme:

- A short pulse pair represents a 0 bit.
- A long pulse pair represents a 1 bit.
- A medium pulse pair is used as a sync/marker.

Each byte is recorded twice on tape for error detection. The first copy is followed by an inter-record gap, then the second copy. During playback, the KERNAL compares both copies and reports a `?LOAD ERROR` if they disagree.

## Tape Data Format

A tape file consists of:

1. **Leader** - a series of sync pulses used by the KERNAL to lock onto the data rate.
2. **Header block** - contains the file type, start address, end address, and filename (up to 16 characters).
3. **Data blocks** - the file contents, broken into 192-byte blocks.
4. **Repeat** - the entire sequence (header and data) is recorded a second time for error correction.

## KERNAL Routines (ROM 4)

The following describes the tape-related KERNAL routines from the
[ROM 4 disassembly](https://www.zimmers.net/anonftp/pub/cbm/src/pet/pet_rom4_disassembly.txt).

### Jump Table Entry Points

Address | Label  | Description
--------|--------|----------------------------------------------
FFD5    | load   | JMP $F401 (Perform [load])
FFD8    | save   | JMP $F6DD (Perform [save])
FFDB    | verify | JMP $F4F6 (Perform [verify])

### I/O Messages

The KERNAL stores tape-related messages in a table at $F000. Each message is
terminated by setting bit 7 of the final character.

Offset | Message
-------|-------------------
$41    | PRESS PLAY
$4D    | & RECORD
$56    | ON TAPE
$5F    | LOAD
$64    | WRITING
$6D    | VERIFY

### Zero-Page and Low-RAM Variables

Address | Label                | Description
--------|----------------------|---------------------------------------------------
$96     | ST                   | KERNAL I/O Status Word
$9D     | VERCK                | Flag: 0 = Load, 1 = Verify
$B2     | RITEFLAG             | Flag: Tape Byte-Received
$B7     | (temp)               | Bit counter (8 bits per byte)
$B9     | (temp)               | Pulse pair toggle (alternates 0/1)
$BD     | (temp)               | Block sequence indicator
$C0     | (index)              | Index to tape error log (pass 1)
$C1     | (index)              | Tape pass 2 error log index
$C2     | READFLAGS            | Tape read state: $00=scan, $0F=count, $40=load, $80=end
$C3     | SHORTCNT             | Checksum / seconds before tape write
$C7-C8  | BUFPTR               | Pointer: Tape buffer / screen scrolling
$C9-CA  | ENDADDR              | Tape end address / end of program
$CB     | TCON                 | Tape timing constant (adaptive)
$CE     | READTMR              | Tape read timer flag (IRQ enabled for Timer 1)
$D4     | DEVNUM               | Current device number (1 = Cassette #1, 2 = Cassette #2)
$D6-D7  | TAPBUF               | Pointer: Start of tape buffer
$DD     | CASSCHAR             | Cassette temp (current byte being read/written)
$DE     | BLKCNT               | Cassette read/write block count
$F9     | MOTORLOCK1           | Tape motor interlock #1 (Cassette #1)
$FA     | MOTORLOCK2           | Tape motor interlock #2 (Cassette #2)
$FB-FC  | IOSTART              | I/O start address

### Load Flow (F401)

The BASIC `LOAD` command enters the KERNAL at `$F401`:

```text
F401  verfyt   LDA #$00
F403           STA $9D          ; VERCK = 0 (Load, not Verify)
F405           JSR $F47D        ; slpara - Get Parameters For LOAD/SAVE
F408           JSR $F6CC        ; Set I/O start/end addresses from BASIC pointers
F40B           LDA #$FF
F40D  wait     CMP $9B          ; Wait for STOP key debounce
F40F           BNE wait
F411           CMP $9B
F413           BNE wait
F415           JSR $F356        ; Dispatch by device number
```

The dispatcher at `$F356` branches based on the device number (`$D4`):

- Device 0: syntax error
- Device 3: screen (syntax error)
- Device >= 4: IEEE-488 load (at `$F361`)
- Device 1-2: tape load (falls through to `$F3D4`)

### Tape Load Path (F3D4)

For tape devices, execution continues at `$F3D4`:

```text
F3D4           JSR $F695        ; Get Buffer Address (Cassette #1 or #2)
F3D7           JSR $F857        ; Print "PRESS PLAY ON TAPE"
F3DA           JSR $F449        ; Wait for STOP or sense
F3DD  search   LDA $D1          ; Length of Current File Name
F3DF           BEQ $F3E9        ; If empty name, accept any file
F3E1           JSR $F4D3        ; Read next header block, compare filename
F3E4           BNE $F3EE        ; Found match
F3E6           JMP $F5AD        ; File not found error
F3E9           JSR $F5E5        ; Read next header (any name)
F3EC           BEQ $F3E6        ; Error
F3EE           CPX #$01         ; Check file type (1 = relocatable program)
F3F0           BNE search       ; Not a program header, keep searching
F3F2           LDA $96          ; Check ST
F3F4           AND #$10
F3F6           BNE $F46C        ; Error
F3F8           JSR $F67B        ; Copy header addresses to I/O pointers
F3FB           JSR $F46D        ; Print "LOADING" or "VERIFYING" message
F3FE           JMP $F8A3        ; Enter main tape read loop
```

### Print "PRESS PLAY ON TAPE" (F857)

This routine prints the prompt and waits for the user to press a transport key:

```text
F857           JSR $F87A        ; Check Tape Status (test sense pin)
F85A           BEQ $F88B        ; Already pressed, return
F85C           LDY #$41
F85E           JSR $F185        ; Print "PRESS PLAY"
F861           LDY #$56
F863           JSR $F185        ; Print "ON TAPE "
F866           LDA $D4          ; Device number
F868           ORA #$30         ; Convert to ASCII digit
F86A           JSR $E202        ; Print device number
F86D  poll     JSR $F935        ; Check for STOP key
F870           JSR $F87A        ; Check Tape Status
F873           BNE poll         ; Loop until sense pin asserted
F875           LDY #$AA
F877           JMP $F185        ; Print CR and return
```

### Check Tape Status (F87A)

This routine tests the cassette sense pin for the active device:

```text
F87A           LDA #$10         ; Bit 4 mask (Cassette #1)
F87C           LDX $D4          ; Current Device Number
F87E           DEX
F87F           BEQ $F883        ; Device 1, use bit 4
F881           LDA #$20         ; Bit 5 mask (Cassette #2)
F883           BIT $E810        ; Test PIA1 Port A
F886           BNE $F88B        ; NZ = no key pressed (sense high)
F888           BIT $E810        ; Double-read for debounce
F88B           RTS              ; Z flag: 0 = pressed, 1 = not pressed
```

For Cassette #1, bit 4 of PIA1 Port A ($E810) is tested. For Cassette #2,
bit 5 is tested.

### Initiate Tape Read (F89A)

Sets up the tape system for reading a block:

```text
F89A           LDA #$00
F89C           STA $96          ; Clear ST
F89E           STA $9D          ; Clear verify flag
F8A0           JSR $F6AB        ; Set Buffer Start/End Pointers
F8A3           JSR $F92B        ; Check for STOP key / wait for tape ready
F8A6           JSR $F857        ; Print "PRESS PLAY ON TAPE"
F8A9           SEI
F8AA           LDA #$00
F8AC           STA $C2          ; Read flags = scan mode
F8AE           STA $CE          ; Clear read timer flag
F8B0           STA $CB          ; Clear timing constant
F8B2           STA $C0          ; Clear error log index (pass 1)
F8B4           STA $C1          ; Clear error log index (pass 2)
F8B6           STA $B2          ; Clear byte-received flag
F8B8           LDX $D4          ; Device number
F8BA           DEX
F8BB           BEQ $F8C4        ; Cassette #1
F8BD           LDA #$90
F8BF           STA $E84E        ; VIA IER: enable CB1 interrupt (Cass #2)
F8C2           BNE $F8C7
F8C4           INC $E811        ; PIA1 CRA: enable CA1 interrupt (Cass #1)
F8C7           LDX #$0E         ; IRQ vector index for tape read
F8C9           BNE $F8E0        ; Jump to Common Tape Code
```

### Common Tape Code (F8E0)

Shared setup for both read and write operations. Installs the appropriate IRQ
handler and turns on the motor:

```text
F8E0           JSR $FCE0        ; Set IRQ Vector (from table at $FD4C)
F8E3           LDA #$02
F8E5           STA $DE          ; Block count = 2 (each block recorded twice)
F8E7           JSR $FBC9        ; New Character Setup (reset bit counter)
F8EA           DEC $E813        ; PIA1 CRB: toggle motor control bit
F8ED           LDX $D4          ; Device number
F8EF           DEX
F8F0           BNE $F8FB        ; Not Cassette #1
F8F2           LDA #$34
F8F4           STA $E813        ; PIA1 CRB = $34: CA2 low = Motor ON (Cass #1)
F8F7           STA $F9          ; Set motor interlock #1
F8F9           BNE $F905
F8FB           LDA $E840        ; Read VIA Port B
F8FE           STX $FA          ; Set motor interlock #2
F900           AND #$EF         ; Clear bit 4 = Motor ON (Cass #2)
F902           STA $E840        ; Write VIA Port B
F905           LDX #$FF         ; Delay loop: let the motor spin up
F907  delay    LDY #$FF
F909  inner    DEY
F90A           BNE inner
F90C           DEX
F90D           BNE delay
F90F           STA $E849        ; Reset VIA Timer 2
F912           CLI              ; Enable interrupts, begin tape I/O
```

### Wait For Tape (F913)

After enabling interrupts, the KERNAL waits for the IRQ-driven tape routines
to complete:

```text
F913  waitlp   LDA #$E4
F915           CMP $91          ; Check if IRQ vector still points to tape handler
F917           BEQ $F92A        ; Done (vector restored to normal)
F919           JSR $F935        ; Check for STOP key
F91C           BIT $E813        ; Check PIA1 CRB (CA1 interrupt flag)
F91F           BPL waitlp       ; Loop while tape I/O in progress
F921           BIT $E812        ; Clear interrupt flag by reading PIA1 Port B
F924           JSR $F768        ; Update Jiffy Clock
F927           JMP waitlp
```

### Read Tape Bits (F976)

The core ISR for tape reading. Measures the time between pulses using VIA
Timer 2 and classifies each pulse as short (0), medium (sync), or long (1):

```text
F976           LDX $E849        ; Read VIA Timer 2 high byte
F979           LDY #$FF
F97B           TYA
F97C           SBC $E848        ; Subtract Timer 2 low byte
F97F           CPX $E849        ; Check for timer rollover
F982           BNE $F976        ; Retry if timer changed during read
F984           STX $CC          ; Save high byte
F986           TAX
F987           STY $E848        ; Reset Timer 2 low = $FF
F98A           STY $E849        ; Reset Timer 2 high = $FF
...
F998           LDA $CB          ; Tape Timing Constant (adaptive)
F99A           CLC
F99B           ADC #$3C         ; Threshold offset
...
F9A3           CMP $CC          ; Compare measured period
F9A5           BCS $F9F1        ; Short pulse (< threshold) = 0 bit
...
F9B4           ADC #$30         ; Next threshold
F9B6           ADC $CB
F9B8           CMP $CC
F9BA           BCS $F9D8        ; Medium pulse = sync marker
F9BC           INX
F9BD           ADC #$26         ; Next threshold
F9BF           ADC $CB
F9C1           CMP $CC
F9C3           BCS $F9DC        ; Long pulse = 1 bit
```

The adaptive timing constant (`$CB`) is continuously adjusted based on the
measured pulse widths, allowing the KERNAL to compensate for tape speed
variations.

### Store Tape Characters (FA9C)

After 8 bit-pairs are received, the assembled byte is stored and verified:

```text
FA9C           JSR $FBC9        ; New Character Setup (reset for next byte)
FA9F           STA $B2          ; Clear byte-received flag
...
FAFC           LDA $CF          ; End of tape marker?
FAFE           BEQ $FB0A
FB00           LDA #$04
FB02           JSR $FBC4        ; Set Status Bit (short block error)
FB05           LDA #$00
FB07           JMP $FB8B
FB0A           JSR $FD0B        ; Check if buffer pointer reached end address
FB0D           BNE $FB12        ; More data to store
FB0F           JMP $FB89        ; Block complete
FB12           LDX $BD          ; Block sequence (1st or 2nd copy)
FB14           DEX
FB15           BEQ $FB44        ; 2nd copy: verify against 1st
FB17           LDA $9D          ; Load or Verify?
FB19           BEQ $FB27        ; Load: store directly
FB1B           LDY #$00
FB1D           LDA $DD          ; Byte from tape
FB1F           CMP ($C7),Y      ; Compare with memory (verify mode)
FB21           BEQ $FB27
FB23           LDA #$01
FB25           STA $D0          ; Mark read error
FB27           LDA $D0          ; Check for errors
FB29           BEQ $FB77        ; No error, store byte
...
FB77           LDA $9D          ; Load or Verify?
FB79           BNE $FB81        ; Verify: skip store
FB7B           LDA $DD          ; Byte from tape
FB7D           LDY #$00
FB7F           STA ($C7),Y      ; Store to memory via buffer pointer
```

### Wind Up Tape I/O (FCC0)

Called when tape I/O is complete (or aborted). Disables tape interrupts and
stops the motor:

```text
FCC0           PHP
FCC1           SEI
FCC2           JSR $FCEB        ; Kill Tape Motor
FCC5           LDA #$7F
FCC7           STA $E84E        ; VIA IER: disable all VIA interrupts
FCCA           LDA #$3C
FCCC           STA $E811        ; PIA1 CRA: disable CA1 interrupt
FCCF           LDA #$3D
FCD1           STA $E813        ; PIA1 CRB: restore default
FCD4           LDX #$0C
FCD6           JSR $FCE0        ; Set IRQ Vector back to normal (keyboard scan)
FCD9           PLP
FCDA           RTS
```

### Kill Tape Motor (FCEB)

Turns off both cassette motors:

```text
FCEB           LDA #$3C
FCED           STA $E813        ; PIA1 CRB = $3C: CA2 high = Motor OFF (Cass #1)
FCF0           LDA $E840        ; Read VIA Port B
FCF3           ORA #$10         ; Set bit 4 = Motor OFF (Cass #2)
FCF5           STA $E840        ; Write VIA Port B
FCF8           RTS
```

### Motor Interlock (E47A, in Main IRQ)

The main IRQ handler (at `$E455`) includes a motor interlock that automatically
controls the motors based on the sense pins. This runs on every interrupt
(during keyboard scanning), independent of tape I/O:

```text
E47A           LDY #$00
E47C           LDA $E810        ; Read PIA1 Port A
E47F           AND #$F0         ; Isolate upper nibble (sense pins + EOI + DIAG)
E481           STA $E810        ; Write back (updates key scan outputs)
E484           LDA $E810        ; Re-read to get stable input
E487           ASL              ; Shift bit 4 (Cass #1 sense) into bit 5
E488           ASL              ; Shift into bit 6
E489           ASL              ; Shift into bit 7 (now in N flag)
E48A           BPL $E495        ; Branch if Cass #1 sense = 1 (no key pressed)
E48C           STY $F9          ; Key pressed: clear motor interlock #1
E48E           LDA $E813
E491           ORA #$08         ; Set CRB bit 3: CA2 forced high = motor OFF
E493           BNE $E49E
E495           LDA $F9          ; Motor interlock flag
E497           BNE $E4A1        ; If set, keep motor running
E499           LDA $E813        ;
E49C           AND #$F7         ; Clear CRB bit 3: CA2 forced low = motor ON
E49E           STA $E813
E4A1           BCC $E4AC        ; Now check Cassette #2 (carry from ASL chain)
E4A3           STY $FA          ; Key pressed: clear motor interlock #2
E4A5           LDA $E840        ; Read VIA Port B
E4A8           ORA #$10         ; Set bit 4 = motor OFF
E4AA           BNE $E4B5
E4AC           LDA $FA
E4AE           BNE $E4B8
E4B0           LDA $E840
E4B3           AND #$EF         ; Clear bit 4 = motor ON
E4B5           STA $E840
E4B8           JSR $E4BE        ; Continue to keyboard scan
```

When no transport key is pressed (sense high), the interlock turns the motor
off. When the tape I/O routines set the interlock flag (`$F9`/`$FA`), the
motor stays on regardless of the sense pin state. This prevents the motor
from turning off during brief sense pin glitches while tape I/O is active.

### Initiate Tape Write (F8CB)

Sets up the tape system for writing:

```text
F8CB           JSR $F6AB        ; Set Buffer Start/End Pointers
F8CE           JSR $F92B        ; Check for STOP key
F8D1           LDA #$14
F8D3           STA $C3          ; Leader duration (20 units)
F8D5           JSR $F88C        ; Print "PRESS PLAY & RECORD..."
F8D8           SEI
F8D9           LDA #$A0
F8DB           STA $E84E        ; VIA IER: enable Timer 2 interrupt
F8DE           LDX #$08         ; IRQ vector index for tape write leader
F8E0           (falls through to Common Tape Code)
```

### Write Tape Leader (FC99)

Writes the sync leader that precedes each data block:

```text
FC99           LDA #$78
FC9B           JSR $FBE1        ; Write sync pulse (medium length)
FC9E           BNE $FC83        ; Return from interrupt
FCA0           DEC $BD          ; Decrement leader pulse counter
FCA2           BNE $FC83        ; More leader pulses needed
FCA4           JSR $FBC9        ; New Character Setup
FCA7           DEC $C3          ; Decrement seconds counter
FCA9           BPL $FC83        ; More leader seconds needed
FCAB           LDX #$0A         ; Switch to data write IRQ handler
FCAD           JSR $FCE0        ; Set IRQ Vector
```

### Write Transition to Tape (FBD8)

Writes a single pulse to the tape by toggling VIA PB3 (CASS_WRITE):

```text
FBD8           LDA $DD          ; Current byte being written
FBDA           LSR              ; Shift out low bit
FBDB           LDA #$60         ; Short pulse duration (0 bit)
FBDD           BCC $FBE1
FBDF           LDA #$B0         ; Long pulse duration (1 bit)
FBE1           LDX #$00
FBE3           STA $E848        ; Load VIA Timer 2 low byte
FBE6           STX $E849        ; Load VIA Timer 2 high byte (start timer)
FBE9           LDA $E840        ; Read VIA Port B
FBEC           EOR #$08         ; Toggle bit 3 (CASS_WRITE)
FBEE           STA $E840        ; Write VIA Port B
FBF1           AND #$08         ; Check new state of CASS_WRITE
FBF3           RTS              ; (NZ = high, Z = low)
```

The timer values determine the pulse width: `$60` for a short pulse (0 bit)
and `$B0` for a long pulse (1 bit). The sync/leader uses `$78` (medium).

### Set IRQ Vector (FCE0)

Installs different IRQ handlers depending on the current tape operation phase:

```text
FCE0           LDA $FD4C,X      ; Load handler address low byte from table
FCE3           STA $90          ; Store to IRQ vector low byte
FCE5           LDA $FD4D,X      ; Load handler address high byte
FCE8           STA $91          ; Store to IRQ vector high byte
FCEA           RTS
```

The KERNAL switches between multiple IRQ handlers during tape operations.
The index (X register) selects the appropriate handler:

Index | Purpose
------|------------------------------
$08   | Write tape leader (FC99)
$0A   | Write data to tape (FBF4)
$0C   | Normal IRQ (keyboard scan, E455)
$0E   | Read tape bits (F976)

### Helper Routines

Address | Label               | Description
--------|---------------------|---------------------------------------------------
F695    | Get Buffer Address  | Sets `$D6-D7` to the tape buffer: $027A (Cass #1) or $033A (Cass #2)
F6AB    | Set Buffer Pointers | Copies buffer address to `$FB-FC` (start) and `$C9-CA` (end = start + $C0)
F84B    | Bump Tape Pointer   | Increments the byte index within the tape buffer; returns Z=1 when the buffer is full
FBC4    | Set Status Bit      | ORs a value into ST (`$96`)
FBC9    | New Character Setup | Resets the bit counter to 8 and clears temporaries for the next byte
FBBB    | Reset Tape Pointer  | Copies `$FB-FC` back to `$C7-C8` to restart reading from the buffer beginning

### Save Flow (F6DD)

The BASIC `SAVE` command enters the KERNAL at `$F6DD`:

```text
F6DD  savet    JSR $F47D        ; slpara - Get Parameters For LOAD/SAVE
F6E0           JSR $F6CC        ; Set I/O addresses from BASIC pointers
F6E3           LDA $D4          ; Device number
F6E5           BNE $F6EC
F6E7           LDY #$74         ; Device 0: "DEVICE NOT PRESENT"
F6E9           JMP $F5AF        ; Error
F6EC           CMP #$03
F6EE           BEQ $F6E7        ; Device 3 (screen): error
F6F0           BCC $F742        ; Device 1-2: tape save
...
F742           JSR $F695        ; Get Buffer Address
F745           JSR $F88C        ; Print "PRESS PLAY & RECORD..."
```

The `SAVE` path writes a header block (type $01 for relocatable, $04 for
non-relocatable) followed by the data, then repeats both for the second copy.

## Reference

* [Commodore PET I/O Map](https://www.zimmers.net/anonftp/pub/cbm/maps/)
* [Commodore Tape Format](https://www.c64-wiki.com/wiki/Datassette_Encoding)
* [PET ROM 4 Disassembly](https://www.zimmers.net/anonftp/pub/cbm/src/pet/pet_rom4_disassembly.txt)
* [ROM Disassembly (annotated)](https://www.zimmers.net/anonftp/pub/cbm/src/pet/pet_rom4_disassembly.txt)
* Datasheets
  * [Rockwell R6520 PIA](http://archive.6502.org/datasheets/rockwell_r6520_pia.pdf)
  * [Western Digital W65C22 VIA](https://www.westerndesigncenter.com/wdc/documentation/w65c22.pdf)
