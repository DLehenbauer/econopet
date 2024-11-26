# IEEE

## Signals

 Name | Description
------|------------
DIO1-8 | Data Input/Output 1-8, used for transmitting and receiving data or commands.
EOI  | End Or Identify, indicates the last byte of a data transfer or is used in parallel polling.
DAV  | Data Valid, signals when data is ready on the data bus for reading.
NRFD | Not Ready For Data, indicates that a device is not ready to accept data.
NDAC | Not Data Accepted, signals that a device has not yet accepted the data.
IFC  | Interface Clear, used to initialize the bus and set all devices to a known state.
SRQ  | Service Request, indicates that a device requires attention from the controller.
ATN  | Attention, asserted only by the controller to gain attention and to denote address/control information on the data bus.
SHIELD| Ground shield for reducing electrical noise.
REN  | Remote Enable, allows the controller to set devices into remote or local mode.
GND  | Signal Ground, provides a reference point for all signals on the bus.

## Addresses

Outputs are captured on write
Inputs are intercepted on read

Chip | Address | Register | Bit | Pin   | Signal
-----|---------|----------|-----|-------|------------
PIA1 | E810    | ORA      |  0  | PA0   | KEY_A (Out)
PIA1 | E810    | ORA      |  1  | PA1   | KEY_B (Out)
PIA1 | E810    | ORA      |  2  | PA2   | KEY_C (Out)
PIA1 | E810    | ORA      |  3  | PA3   | KEY_D (Out)
PIA1 | E810    | PIBA     |  4  | PA4   | CASS1_SWITCH (In)
PIA1 | E810    | PIBA     |  5  | PA5   | CASS2_SWITCH (In)
PIA1 | E810    | PIBA     |  6  | PA6   | EOI (In)
PIA1 | E810    | PIBA     |  7  | PA7   | DIAG (In)
PIA1 | E811    | CRA      |  3  | CA2   | EOI (Out)
PIA2 | E820    | PIBA     | 0-7 | PA0-7 | DIO1-8 (In)
PIA2 | E821    | CRA      |  3  | CA2   | NDAC (Out)
PIA2 | E821    | CRA      |  7  | CA1   | ATN (In)
PIA2 | E822    | ORB      | 0-7 | PB0-7 | DIO1-8 (Out)
PIA2 | E823    | CRB      |  3  | CB2   | DAV (Out)
PIA2 | E823    | CRB      |  7  | CB1   | SRQ (In)
VIA  | E840    | IRB      |  0  | PB0   | NDAC (In)
VIA  | E840    | ORB      |  1  | PB1   | NRFD (Out)
VIA  | E840    | ORB      |  2  | PB2   | ATN (Out)
VIA  | E840    | ORB      |  3  | PB3   | CASS_WRITE (Out)
VIA  | E840    | ORB      |  4  | PB4   | CASS2_MOTOR (Out)
VIA  | E840    | IRB      |  5  | PB5   | VERT (In)
VIA  | E840    | IRB      |  6  | PB6   | NRFD (In)
VIA  | E840    | IRB      |  7  | PB7   | DAV (In)

REN is held permanently low by the PET
IFC is tied to RESB

## ROM

PIA Addressing

Address | CRA (Bit 2) | CRB (Bit 2) | Read | Write
--------|-------------|-------------|------|-------
0 | 1 | x | PIBA | ORA
0 | 0 | x | DDRA | DDRA
1 | x | x | CRA | CRA
2 | x | 1 | PIBB | ORB
2 | x | 0 | DDRB | DDRB
3 | x | x | CRB | CRB

```
; cint1 Initialize I/O
...

; Configure PIA 1 Port B:
; PB0-3: KEYA-D (Select Key Row)
 E631   LDA #$0F
 E633   STA $E810 ; ORA = 0000_1111 (Key Row = F)
 
; Configures VIA Port B:
; PB0: NDAC (Input)
; PB1: NRFD (Output = 1)
; PB2: ATN  (Output = 1)
; PB3: CASS_WRITE (Output = 1)
; PB4: CASS2_MOTOR (Output = 1)
; PB5: VERT (Input)
; PB6: NRFD (Input)
; PB7: DAV (Input)
 E636   ASL       ; A = 0001_1110
 E637   STA $E840 ; ORB  = 0001_1110
 E63A   STA $E842 ; DDRB = 0001_1110

; Configure PIA 2 Port B
; PB0-7: DIO1-8
 E63D   STX $E822 ; ORB  = 1111_1111

; Configure VIA T1 High Order Counter
 E640   STX $E845 ; = FF

; 
 E643   LDA #$3D
 E645   STA $E813 ; CRB = 0011_1101
 E648   BIT $E812

 E64B   LDA #$3C
 E64D   STA $E821 ; CRA = 0011_1100
 E650   STA $E823 ; CRB = 0011_1100
 E653   STA $E811 ; CRA = 0011_1100
 E656   STX $E822 ; ORB = 1111_1111
 E659   LDA #$0E

; Send TALK Command on IEEE Bus
 F0D2 LDA #$40
 F0D4 BIT $20A9 ; BIT used as 2-byte jump that skips LDA #$20 (A9 20)

; *** Resyncing ***
; Send LISTEN Command on IEEE Bus
 F0D5   LDA #$20

; TALK/LISTEN continue here
 F0D7   PHA
 F0D8   LDA $E840 ; VIA
 F0DB   ORA #$02
 F0DD   STA $E840 ; VIA
 F0E0   LDA #$3C
 F0E2   STA $E821
 F0E5   BIT $A0
 F0E7   BEQ $F0FA
 F0E9   LDA #$34
 F0EB   STA $E811
 F0EE   JSR $F109 ; - Send Data On IEEE Bus
 F0F1   LDA #$00
 F0F3   STA $A0   ; Flag: IEEE Bus-Output Char. Buffered
 F0F5   LDA #$3C
 F0F7   STA $E811
 F0FA   PLA
 F0FB   ORA $D4   ; Current Device Number
 F0FD   STA $A5   ; Buffered Character for IEEE Bus
 F0FF   LDA $E840 ; VIA
 F102   BPL $F0FF
 F104   AND #$FB
 F106   STA $E840 ; VIA
```

## Reference

* [Commodore Peripheral Bus](https://www.pagetable.com/?p=1018)
  * [Part 1: IEEE-488](https://www.pagetable.com/?p=1023)
  * [Part 2: The TALK/LISTEN Layer](https://www.pagetable.com/?p=1031)
  * [Part 3: The Commodore DOS Layer](https://www.pagetable.com/?p=1038)
* [PET and the IEEE488 Bus](http://www.primrosebank.net/computers/pet/documents/PET_and_the_IEEE488_Bus_text.pdf)
* [The Hewlett-Packard Interface Bus (HP-IB)](https://www.hp9845.net/9845/tutorials/hpib/)
* [ROM Disassembly](https://www.zimmers.net/anonftp/pub/cbm/src/pet/pet_rom4_disassembly.txt)
* Datasheets
  * [Western Digital W65C21](https://www.westerndesigncenter.com/wdc/documentation/w65c21.pdf)
  * [Rockwell R6520](http://archive.6502.org/datasheets/rockwell_r6520_pia.pdf)
  * [Western Digital W65C22](https://www.westerndesigncenter.com/wdc/documentation/w65c22.pdf)
