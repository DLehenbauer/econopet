# Video

## Non-CRTC (9" monitor)

Thomas Skibo documented the Non-CRTC (9") video timings [here](https://github.com/skibo/attiny2313_petvid).

Some additional measurements from a 2001-32N (1979):

Signal | Frequency  | Period   | Width  | Polarity    | Source
-------|------------|----------|--------|-------------|-----------
Crystal| 16.007 MHz |        - |      - |         -   | I1 pin 1
CPU    | 1.0009 MHz |        - |      - |         -   | 6502 pin 37
HSync  | 15.63 KHz  |    64us  |   24us | Active High | Video pin 5 (consistent with 16.007 MHz / 1024 = 15.632 KHz)
VSync  | 60.12 Hz   | 16.64ms  | 1.28ms | Active Low  | Video pin 3 (consistent with 15.632 KHz / 260 lines = ~60.122 Hz)
Video  |          - |        - |      - | Active Low  | -

### EconoPET 9" timing override

The 9" mode overrides the CRTC register outputs to reproduce the timing
[documented by Thomas Skibo](https://github.com/skibo/attiny2313_petvid) as closely as is possible using the CRTC.  Conveniently, the math works out such that 1us = one 8-pixel character.

Register | Value | Description
---------|-------|-----------------------------------------------
 R0      |   63  | H_TOTAL = 8 MHz pixel clock / 8 pixel char / (64 chars - 1) = 15.625 KHz
 R1      |   40  | H_DISPLAYED = 40 columns
 R2      |   48  | H_SYNC_POS = 48 us into each line, 16 us before the next line
 R3[3:0] |   24  | H_SYNC_WIDTH = 24 us (EconoPET internally extends H_SYNC_WIDTH to 5 bits)
 R3[7:4] |   20  | V_SYNC_WIDTH = 20 scan-line lines (EconoPET internally extends V_SYNC_WIDTH to 5 bits)
 R4      |   31  | V_TOTAL = 15.625 KHz / ((33 rows - 1) * 8 lines per row) = 61.04 Hz
 R5      |    4  | V_LINE_ADJUST = 15.625 KHz / (33 rows * 8 lines per row + 5 lines) = 60.10 Hz
 R6      |   25  | V_DISPLAYED = 25 rows
 R7      |   28  | V_SYNC_POS = character-row position
 R9      |    7  | SCAN_LINE = 8 pixel character height (-1)

HSYNC is active high for 24 us every 64 us. The CRTC display-enable signal
starts 16 us after the internal HSYNC rising edge and lasts 40 us. The video
output pipeline adds approximately 1.9 us relative to the HORZ output
pipeline, producing a measured 17.8 us from HORZ rising edge to video data.
VSYNC is active low for 20 lines (1.28 ms), with a 260-line frame period of
16.64 ms (about 60.1 Hz).

The CRTC can position VSYNC only on an 8-line character-row boundary. R7=28
starts it at frame line 224, four lines after the measured line 220. This gives
24 blank lines before VSYNC and 16 after it, rather than the measured 20/20
split. VSYNC also changes at the line boundary, 48 us before HSYNC, rather than
the measured 5 us. R7=27 has the same four-line error in the opposite direction,
so R7=28 retains the existing closest CRTC configuration.

`video_crtc_timing_tb` verifies these timings under Verilator.

## CRTC

### Address decoding

Address decoding asserts the CRTC's CS line (Chip Select) for $E880-E8FF.
The CRTC has a single RS (Register Select) input that is tied to A0.

### Timing (CRTC)

Measurements from a North American 8032 (60 Hz) at power on:

Signal | Frequency  | Period   | Width  | Polarity    | Source
-------|------------|----------|--------|-------------|-----------
HSync  |     20 KHz |    50us  |   15us | Active Low  | Video pin 5
VSync  | 60.062 Hz  | 16.65ms  |  800us | Active Low  | Video pin 3
Video  |          - |        - |      - | Active High | -

`video_crtc_timing_tb` verifies these power-on timings and their original CRTC
counter positions.

## Reference

* non-CRTC
  * [Restoring the Early PET Computer 9" VDU](https://www.worldphaco.com/uploads/RESTORING%20THE%20%20PET%20COMPUTER%209.pdf)
  * [attiny2313_petvid](https://github.com/skibo/attiny2313_petvid)
  * [PET/CBM 4 State Machine](https://forum.vcfed.org/index.php?attachments/cbm4state-jpg.1251230/)
  * [PetVideoSim](https://github.com/skibo/PetVideoSim) ([VCDs](https://github.com/skibo/PetVideoSim/releases))
* CRTC
  * [Operation](http://www.6502.org/users/andre/hwinfo/crtc/crtc.html)
  * [Internals](http://www.6502.org/users/andre/hwinfo/crtc/internals/index.html)
  * [CRTC - BeebWiki](https://beebwiki.mdfs.net/CRTC)
  * [The Amstrad CPC CRTC 
Compendium](https://www.cpcwiki.eu/imgs/4/4a/ACCC1.8-EN.pdf)
  * [Wikipedia](https://en.wikipedia.org/wiki/Motorola_6845)
  * Register Values
    * [Spreadsheet](https://view.officeapps.live.com/op/view.aspx?src=https%3A%2F%2Finchocks.co.uk%2Fcommodore%2FPET%2FPET_CRTC.xls)
    * [SJGray](https://github.com/sjgray/cbm-edit-rom/blob/master/docs/CRTC%20Registers.txt)
  * [Reverse Engineering](https://stardot.org.uk/forums/viewtopic.php?t=22008)
  * [Part Info](https://www.amiga-stuff.com/hardware/crtc.html)
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
