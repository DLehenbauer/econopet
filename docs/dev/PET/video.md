# Video

## Non-CRTC

PETs without a CRTC chip used 9" are equipped with 9" monitors, which require:

* HSync frequency is 15.632 KHz (~NTSC)
* H/VSync are active high
* Video is active low

### Timing (Non-CRTC)

Measurements from a 2001-32N (1979):

Signal | Frequency  | Source
-------|------------|----------
Crystal| 16.007 MHz | I1 pin 1
CPU    | 1.0009 MHz | 6502 pin 37
HSync  | 15.63 KHz  | Video pin 5 (consistent with 16.007 MHz / 1024 = 15.632 KHz)
VSync  | 60.12 Hz   | Video pin 3 (consistent with 15.632 KHz / 260 lines = ~60.122 Hz)

Closest CRTC settings:

Register | Value | Description
---------|-------|-----------------------------------------------
 R0      |   63  | H_TOTAL = 8 MHz pixel clock / 8 pixel char / (64 chars - 1) = 15.625 KHz
 R1      |   40  | H_DISPLAYED = 40 columns
 R2      |   48  | H_SYNC_POS
 R3[3:0] |    1  | H_SYNC_WIDTH = 1
 R3[7:4] |    5  | V_SYNC_WIDTH = 5
 R4      |   32  | V_TOTAL = 15.625 KHz / ((33 rows - 1) * 8 lines per row) = 61.04 Hz
 R5      |    5  | V_LINE_ADJUST = 15.625 KHz / (33 rows * 8 lines per row + 5 lines) = 60.10 Hz
 R6      |   25  | V_DISPLAYED = 25 rows
 R7      |   28  | V_SYNC_POS
 R9      |    7  | SCAN_LINE = 8 pixel character height (-1)
 R12[4]  |    0  | TA12 inverts video: 0 = 9" monitor, 1 = 12" monitor

From [SJGray's cbm-edit-rom](https://github.com/sjgray/cbm-edit-rom/blob/master/crtc-reg-normal.asm):

```asm
;---------------------- 40/80x25, 60 Hz, 15.748 kHz (NTSC) for External Monitor (inverted video)
!IF REFRESH=3 {
;                             0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15  16  17
CRT_CONFIG_TEXT:     !byte $3f,$28,$32,$12,$1e,$06,$19,$1C,$00,$07,$00,$00,$10,$00,$00,$00,$00,$00
; Decimal                    63  40  50 1/2  30   6  25  28   0   7   0   0  16   0   0   0   0   0
CRT_CONFIG_GRAPHICS: !byte $3f,$28,$32,$12,$1e,$06,$19,$1C,$00,$07,$00,$00,$10,$00,$00,$00,$00,$00
}
```

## CRTC

PETs with a CRTC chip are equipped with 12" monitors, which require:

* HSync frequency is 20 KHz
* H/VSync are active low
* Video is active high

### Address decoding

Address decoding selects asserts the CRTC's CS line (Chip Select) for $E880-E8FF.
The CRTC has a single RS (Register Select) input that is tied to A0.

### Timing (CRTC)

Measurements from 8032 power on:

Signal | Frequency | Duty                          | Source
-------|-----------|-------------------------------|---------------------
HSync  | 20 KHz    | 70-70.126%                    | Video connector (J7)
VSync  | 60.062 Hz | 95.195% (800us negative pulse)| Video connector (J7)

## Reference

* CRTC
  * [Operation](http://www.6502.org/users/andre/hwinfo/crtc/crtc.html)
  * [Internals](http://www.6502.org/users/andre/hwinfo/crtc/internals/index.html)
  * [Wikipedia](https://en.wikipedia.org/wiki/Motorola_6845)
  * [Register Values](https://github.com/sjgray/cbm-edit-rom/blob/master/docs/CRTC%20Registers.txt)
  * [Reverse Engineering](https://stardot.org.uk/forums/viewtopic.php?t=22008)
  * Datasheet
    * [Motorolla MC6845](https://archive.org/details/bitsavers_motorolada_1431515/page/n9/mode/2up)
    * [Motorolla AN-851](https://archive.org/details/bitsavers_motorolaapaMC6845CRTCSimplifiesVideoDisplayControl_9722748/mode/2up)
    * [Rockwell R6545](http://archive.6502.org/datasheets/rockwell_r6545-1_crtc.pdf)
    * [C6845 CRT Controller IP](https://colorcomputerarchive.com/repo/Documents/Datasheets/SY6845E-C6845%20CRT%20Controller%20(CAST).pdf)
    * [DB6845 CRTC IP](https://www.digitalblocks.com/files/DB6845-DS-V1_1.pdf)
* VGA
  * [VGA Timings](http://martin.hinner.info/vga/timing.html)
  * [TinyVGA Timings](http://www.tinyvga.com/vga-timing)
* DVI
  * [CEA-861-D](https://ia903002.us.archive.org/1/items/CEA-861-D/CEA-861-D.pdf)
* NTSC / PAL
  * [Timing Characteristics](http://www.kolumbus.fi/pami1/video/pal_ntsc.html)

## Tools

* [VGA Timing Calculator](https://www.epanorama.net/faq/vga2rgb/calc.html)
* [Pixel Clock Calculator](https://www.monitortests.com/pixelclock.php)
