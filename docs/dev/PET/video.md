# Video

## Non-CRTC

Measurements from a 2001-32N (1979):

Signal | Frequency  | Period   | Width  | Polarity    | Source
-------|------------|----------|--------|-------------|-----------
Crystal| 16.007 MHz |        - |      - |         -   | I1 pin 1
CPU    | 1.0009 MHz |        - |      - |         -   | 6502 pin 37
HSync  | 15.63 KHz  |    64us  |   24us | Active High | Video pin 5 (consistent with 16.007 MHz / 1024 = 15.632 KHz)
VSync  | 60.12 Hz   | 16.64ms  | 1.28ms | Active Low  | Video pin 3 (consistent with 15.632 KHz / 260 lines = ~60.122 Hz)
Video  |          - |        - |      - | Active Low  | -

### CRTC for 9" monitor

Closest CRTC settings:

Register | Value | Description
---------|-------|-----------------------------------------------
 R0      |   63  | H_TOTAL = 8 MHz pixel clock / 8 pixel char / (64 chars - 1) = 15.625 KHz
 R1      |   40  | H_DISPLAYED = 40 columns
 R2      |   48  | H_SYNC_POS
 R3[3:0] |   15  | H_SYNC_WIDTH = 15
 R3[7:4] |    0  | V_SYNC_WIDTH = 16 -- (Ideally = 20 -- See note below)
 R4      |   31  | V_TOTAL = 15.625 KHz / ((33 rows - 1) * 8 lines per row) = 61.04 Hz
 R5      |    4  | V_LINE_ADJUST = 15.625 KHz / (33 rows * 8 lines per row + 5 lines) = 60.10 Hz
 R6      |   25  | V_DISPLAYED = 25 rows
 R7      |   28  | V_SYNC_POS
 R9      |    7  | SCAN_LINE = 8 pixel character height (-1)

Note that the maximum V_SYNC_WIDTH of 16 is a little short.  Ideally, V_SYNC_WIDTH would be 20, but
this is outside the range supported by the CRTC.

## CRTC

### Address decoding

Address decoding selects asserts the CRTC's CS line (Chip Select) for $E880-E8FF.
The CRTC has a single RS (Register Select) input that is tied to A0.

### Timing (CRTC)

Measurements from 8032 (60 Hz) at power on:

Signal | Frequency  | Period   | Width  | Polarity    | Source
-------|------------|----------|--------|-------------|-----------
HSync  |     20 KHz |    50us  |   15us | Active Low  | Video pin 5
VSync  | 60.062 Hz  | 16.65ms  |  800us | Active Low  | Video pin 3
Video  |          - |        - |      - | Active High | -

## Reference

* non-CRTC
  * [Restoring the Early PET Computer 9" VDU](https://www.worldphaco.com/uploads/RESTORING%20THE%20%20PET%20COMPUTER%209.pdf)
  * [attiny2313_petvid](https://github.com/skibo/attiny2313_petvid)
  * [PET/CBM 4 State Machine](https://forum.vcfed.org/index.php?attachments/cbm4state-jpg.1251230/)
  * [PetVideoSim](https://github.com/skibo/PetVideoSim) ([VCDs](https://github.com/skibo/PetVideoSim/releases))
* CRTC
  * [Operation](http://www.6502.org/users/andre/hwinfo/crtc/crtc.html)
  * [Internals](http://www.6502.org/users/andre/hwinfo/crtc/internals/index.html)
  * [Wikipedia](https://en.wikipedia.org/wiki/Motorola_6845)
  * Register Values
    * [Spreadsheet](https://view.officeapps.live.com/op/view.aspx?src=https%3A%2F%2Finchocks.co.uk%2Fcommodore%2FPET%2FPET_CRTC.xls)
    * [SJGray](https://github.com/sjgray/cbm-edit-rom/blob/master/docs/CRTC%20Registers.txt)
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
* [Hunter](https://gitlab.com/rabenauge/hunter/)
