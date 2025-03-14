# ROMs

## ROM 1.0

Early production PET 2001 units used MOS 6540 ROMs, which were proprietary to Commodore and had 28 pins.

Later revisions used standard 2316 ROMs, which are 24-pin devices.

The 28-pin and 24-pin ROMs have different IC and part numbers, but their contents are identical.

Address | Location | 6540 IC# | 6540 Part# | 2316B IC# | 2316B Part# | CRC-32
--------|----------|----------|------------|-----------|-------------|----------
  $C000 |    H1    | 6540-011 |  901439-01 | 2316B-01  |  901447-01  | a055e33a
  $D000 |    H2    | 6540-013 |  901439-02 | 2316B-03  |  901447-03  | d349f2d4
  $E000 |    H3    | 6540-015 |  901439-03 | 2316B-05  |  901447-05  | 9e1c5cea
  $F000 |    H4    | 6540-016 |  901439-04 | 2316B-06  |  901447-06  | 661a814a
  $C800 |    H5    | 6540-012 |  901439-05 | 2316B-02  |  901447-02  | 69fd8a8f
  $D800 |    H6    | 6540-014 |  901439-06 | 2316B-04  |  901447-04  | 850544eb
  $F800 |    H7    | 6540-018 |  901439-07 | 2316B-07  |  901447-07  | c4f47ad1
  N/A   |    A2    | 6540-010 |  901439-08 | 2316B-08  |  901447-08  | 54f32f45

[Disassembly](https://www.zimmers.net/anonftp/pub/cbm/src/pet/rom-1.html)

## ROM 2.0

Highlights:

* Corrects an intermittent bug in the edit software.
* Improves the garbage collection routines.

Identical to ROM 1.0 except H1 ($C000).

Address | Location | 6540 IC# | 6540 Part# | 2316B IC# | 2316B Part# | CRC-32
--------|----------|----------|------------|-----------|-------------|----------
  $C000 | H1       | 6540-019 |  901439-09 | 2316B-09  |  901447-09  | 03cf16d0

### PET 2001-8N (VICE)

ROM         | File                            | Length | CRC-32
------------|---------------------------------|--------|----------
Basic       | basic-1.901439-09-05-02-06.bin  |   8192 | aff78300
Edit        | edit-1-n.901439-03.bin          |   2048 | 9e1c5cea
Kernel      | kernal-1.901439-04-07.bin       |   4096 | f0186492
Characters  | characters-1.901447-08.bin      |   2048 | 54f32f45

The VICE roms are a binary concatenation of the corresponding ROMs.

## ROM 3.0

Highlights:

* Support for commodore disk system.
* Fixes bug limiting the dimensions of arrays.
* Improved garbage collection.
* Adds ROM set for business keyboards
* New character ROM (swaps upper/lower case)

### PET 2001 (Chicklet?, 28 pin)

The 28-pin ROM upgrade set did not include a new character ROM.  Therefore, upper/lowercase continues to be swapped (like in BASIC 1-2).

Address | Location | IC#      | Part#     | CRC-32
--------|----------|----------|-----------|----------
  $C000 | H1       | 6540-020 | 901439-13 | 6aab64a5
  $D000 | H2       | 6540-022 | 901439-15 | 97f7396a
  $E000 | H3       | 6540-024 | 901439-17 | e459ab32
  $F000 | H4       | 6540-025 | 901439-18 | 8745fc8a
  $C800 | H5       | 6540-021 | 901439-14 | a8f6ff4c
  $D800 | H6       | 6540-023 | 901439-16 | 4cf8724c
  $F800 | H7       | 6540-026 | 901439-19 | fd2c1f87
  N/A   | A2       | 6540-010 | 901439-08 | 54f32f45

### PET 2001 (Chicklet?, 24 pin)

The 24-pin ROM upgrade set did not include a new character ROM.  Therefore, upper/
lowercase continues to be swapped (like in BASIC1-2).

However, users could install 901447-10 if desired.

Address | Location | IC#       | Part#     | CRC-32
--------|----------|-----------|-----------|----------
  $C000 | H1       |  2332-007 | 901465-01 | 63a7fe4a
  $D000 | H2       |  2332-008 | 901465-02 | ae4cb035
  $E000 | H3       | 2316B-011 | 901447-24 | e459ab32
  $F000 | H4       |  2332-009 | 901465-03 | f02238e2
  $C800 | H5       | -         | -         | -
  $D800 | H6       | -         | -         | -
  $F800 | H7       | -         | -         | -
  N/A   | A2       | 2316B-004 | 901447-08 | 54f32f45

### PET 2001 (Graphic Keyboard)

Address | Location | IC#       | Part#     | CRC-32
--------|----------|-----------|-----------|----------
  $9000 | D3       | -         | -         | -
  $A000 | D4       | -         | -         | -
  $B000 | D5       | -         | -         | -
  $C000 | D6       | 2332-007  | 901465-01 | 63a7fe4a
  $D000 | D7       | 2332-008  | 901465-02 | ae4cb035
  $E000 | D8       | 2316B-011 | 901447-24 | e459ab32
  $F000 | D9       | 2332-009  | 901465-03 | f02238e2
  N/A   | F10      | 2316B-004 | 901447-10 | d8408674

### PET 30xx (VICE)

ROM         | File                            | Length | CRC-32
------------|---------------------------------|--------|----------
Basic       | basic-2.901465-01-02.bin        |   8192 | cf35e68b
Edit        | edit-2-n.901447-24.bin          |   2048 | e459ab32
Kernel      | kernal-2.901465-03.bin          |   4096 | f02238e2
Characters  | characters-2.901447-10.bin      |   2048 | d8408674

The VICE roms are a binary concatenation of the corresponding ROMs.

### PET 2001 (Business Keyboard)

Business keyboard is identical except for D8.

Address | Location | IC#       | Part#     | CRC-32
--------|----------|-----------|-----------|----------
  $E000 | D8       |2316B-024  | 901474-01 | 05db957e

### PET 3032B (VICE)

Business keyboard is identical to 30XX except for Edit rom.

ROM         | File                            | Length | CRC-32
------------|---------------------------------|--------|----------
Edit        | edit-2-b.901474-01.bin          |   2048 | 05db957e

## ROM 4.0

Highlights:

* Added the disk commands
* Greatly improved the garbage collection
* Adds ROM set for 80 column displays

### PET 2001 & 4000 (Graphics Keyboard, NoCRTC)

Address | Location | IC#       | Part#     | CRC-32
--------|----------|-----------|-----------|----------
  $9000 | D3       | -         | -         | -
  $A000 | D4       | -         | -         | -
  $B000 | D5       |  2332     | 901465-19 | 3a5f5721
  $C000 | D6       |  2332-059 | 901465-20 | 0fc17b9c
  $D000 | D7       |  2332-096 | 901465-21 | 36d91855
  $E000 | D8       | 2316B-034 | 901447-29 | e5714d4c
  $F000 | D9       |  2332-075 | 901465-22 | cc5298a1
  N/A   | F10      | 2316B-004 | 901447-10 | d8408674

### PET 2001 & 4000 (Business Keyboard, NoCRTC)

Business keyboard is identical except for D8.

Address | Location | IC#       | Part#     | CRC-32
--------|----------|-----------|-----------|----------
  $E000 | D8       | 2316B-035 | 901474-02 | 75ff4af7

### PET 4000 (CRTC)

Same ICs as NoCRTC except UD7.

Address | Location | IC#       | Part#     | CRC-32
--------|----------|-----------|-----------|----------
  $F000 | UD6      |  2332-075 | 901465-22 | cc5298a1
  $E000 | UD7      |  2316B    | 901499-01 | 5f85bdf8
  $D000 | UD8      |  2332-096 | 901465-21 | 36d91855
  $C000 | UD9      |  2332-059 | 901465-20 | 0fc17b9c
  $B000 | UD10     |  2332     | 901465-19 | 3a5f5721
  $A000 | UD11     | -         | -         | -
  $9000 | UD12     | -         | -         | -
  N/A   | F10      | 2316B-004 | 901447-10 | d8408674

### PET 8000 (CRTC, 60Hz)

Identical except for UD7.

Address | Location | IC#       | Part#     | CRC-32
--------|----------|-----------|-----------|----------
  $E000 | UD7      | 2316B-041 | 901474-03 | 5674dd5e

### PET 8000 (CRTC, 50Hz)

Identical except for UD7.

Address | Location | IC#       | Part#     | CRC-32
--------|----------|-----------|-----------|----------
  $B000 | UD7      | 2316B-059 | 901474-04 | c1ffca3a

## ROM 4.1

ROM 4.1 is identical to ROM 4.0 except for D5/UD10.
As with ROM 4.0, D8/UD7 must match keyboard type.

### PET 2001 & 4000 (NoCRTC)

Address | Location | IC#       | Part#     | CRC-32
--------|----------|-----------|-----------|----------
  $B000 | D5       |  2332-120 | 901465-23 | ae3deac0

### PET 8000 (CRTC, 60Hz)

ROM 4.1 is identical to ROM 4.0 except for D8.
As with ROM 4.0, D8 must match keyboard type.

Address | Location | IC#       | Part#     | CRC-32
--------|----------|-----------|-----------|----------
  $B000 | UD10     |  2332-120 | 901465-23 | ae3deac0

### PET 40xx (VICE)

ROM         | File                            | Length | CRC-32
------------|---------------------------------|--------|----------
Basic       | basic-4.901465-23-20-21.bin     |  12288 | 2a940f0a
Edit        | edit-4-40-n-50Hz.901498-01.bin  |   2048 | 3370e359
Kernel      | kernal-4.901465-22.bin          |   4096 | cc5298a1
Characters  | characters-2.901447-10.bin      |   2048 | d8408674

## Appendix A: 6540 ROMs (28-pin)

Some static PET units use 28-pin 6540 ROMs instead of the 24-pin 23xx ROMs standardized
on by Commodore in later models.

Mainboard | ROMs  | RAM
----------|-------|------
320008    | 6540  | 6550
320081    | 2316B | 6550
320132    | 6540  | 2114
320137    | 2316B | 2114

The content of the 6540 ROMs are identical to the their 23xx counterparts.

## Appendix B: ROM Table

Model       | Type                    | Function                                       | Part#      | Size | Address   | IC        | Version | Revision | Copyright | CRC-32     | Code
------------|-------------------------|------------------------------------------------|------------|------|-----------|-----------|---------|----------|-----------|------------|-------
CBM 30xx    | Character (8 Scanlines) | PET Character (Basic 3/4) (F10) [2316-004]     | 901447-10  | 2KB  | -         | 2316B-004 | -       | -        | -         | 0xd8408674 | BINARY
CBM 30xx    | Character (8 Scanlines) | PET Character (Swedish) (F10)                  | 901447-14  | 2KB  | -         | 2316B     | -       | -        | -         | 0x48c77d29 | BINARY
CBM 30xx    | Diagnostic              | COMMODORE PET ROM TESTER                       | 901447-18  | 2KB  | 9800-9FFF | 2316B     | -       | -        | -         | 0x081c7aad | 6502
CBM 30xx    | Basic                   | PET Basic 3 $C0 (H1) [6316-007]                | 901447-20  | 2KB  | C000-C7FF | 2316B-007 | 3       | -        | -         | 0x6aab64a5 | 6502
CBM 30xx    | Basic                   | PET Basic 3 $C (D6) [2332-007]                 | 901465-01  | 4KB  | C000-CFFF | 2332-007  | 3       | -        | -         | 0x63a7fe4a | 6502
CBM 30xx    | Basic                   | PET Basic 3 $C8 (H5)                           | 901447-21  | 2KB  | C800-CFFF | 2316B-008 | 3       | -        | -         | 0xa8f6ff4c | 6502
CBM 30xx    | Basic                   | PET Basic 3 $D0 (H2)                           | 901447-22  | 2KB  | D000-D7FF | 2316B-009 | 3       | -        | -         | 0x97f7396a | 6502
CBM 30xx    | Basic                   | PET Basic 3 $D (D7) [2332-008]                 | 901465-02  | 4KB  | D000-DFFF | 2332-008  | 3       | -        | -         | 0xae4cb035 | 6502
CBM 30xx    | Basic                   | PET Basic 3 $D8 (H6)                           | 901447-23  | 2KB  | D800-DFFF | 2316B     | 3       | -        | -         | 0x4cf8724c | 6502
CBM 30xx    | Editor                  | PET Edit 3 40 B NoCRTC (D8) [2316-024]         | 901474-01  | 2KB  | E000-E7FF | 2316B     | 3       | -        | -         | 0x05db957e | 6502
CBM 30xx    | Editor                  | PET Edit 3 40 N NoCRTC (D8, H3) [2316-011]     | 901447-24  | 2KB  | E000-E7FF | 2316B-011 | 3       | -        | -         | 0xe459ab32 | 6502
CBM 30xx    | Kernal                  | PET Basic 3 $F0 (H4)                           | 901447-25  | 2KB  | F000-F7FF | 2316B-012 | 3       | -        | -         | 0x8745fc8a | 6502
CBM 30xx    | Kernal                  | PET Kernal 3 $F (D9) [2332-009]                | 901465-03  | 4KB  | F000-FFFF | 2332-009  | 3       | -        | -         | 0xf02238e2 | 6502
CBM 30xx    | Kernal                  | PET Basic 3 $F8 (H7)                           | 901447-26  | 2KB  | F800-FFFF | 2316B-013 | 3       | -        | -         | 0xfd2c1f87 | 6502
CBM 40xx    | Basic                   | PET Basic 4 $B (D5, UD10)                      | 901465-19  | 4KB  | B000-BFFF | 2332      | 4       | -        | -         | 0x3a5f5721 | 6502
CBM 40xx    | Basic                   | PET Basic 4 $C (D6, UD9) [6332-059]            | 901465-20  | 4KB  | C000-CFFF | 2332-059  | 4       | -        | -         | 0x0fc17b9c | 6502
CBM 40xx    | Basic                   | PET Basic 4 $D (D7, UD8) [6332-096]            | 901465-21  | 4KB  | D000-DFFF | 2332-096  | 4       | -        | -         | 0x36d91855 | 6502
CBM 40xx    | Editor                  | PET Edit 4 40 B NoCRTC (D8) [6316-035]         | 901474-02  | 2KB  | E000-E7FF | 2316B-035 | 4       | -        | -         | 0x75ff4af7 | 6502
CBM 40xx    | Editor                  | PET Edit 4 40 N CRTC 50Hz (UD7)                | 901498-01  | 2KB  | E000-E7FF | 2316B     | 4       | -        | -         | 0x3370e359 | 6502
CBM 40xx    | Editor                  | PET Edit 4 40 N CRTC 50Hz (UD7)                | editor.bin | 2KB  | E000-E7FF | 2716      | 4       | -        | -         | 0xeb9f6e75 | 6502
CBM 40xx    | Editor                  | PET Edit 4 40 N CRTC 60Hz (UD7)                | 901499-01  | 2KB  | E000-E7FF | 2316B     | 4       | -        | -         | 0x5f85bdf8 | 6502
CBM 40xx    | Editor                  | PET Edit 4 40 N NoCRTC (D8) [6316-034]         | 901447-29  | 2KB  | E000-E7FF | 2316B-034 | 4       | -        | -         | 0xe5714d4c | 6502
CBM 40xx    | Editor                  | PET Edit 4 CRTC 60Hz (PET 9")                  | 970150-07  | 2KB  | E000-E7FF | 2716      | 4       | -        | -         | -          | 6502
CBM 40xx    | Kernal                  | PET Kernal 4 $F (D9, UD6) [6332-075]           | 901465-22  | 4KB  | F000-FFFF | 2332-075  | 4       | -        | -         | 0xcc5298a1 | 6502
CBM 40xx    | Basic                   | PET Basic 4.1 $B (D5, UD10) [6332-120]         | 901465-23  | 4KB  | B000-BFFF | 2332-120  | 4.1     | -        | -         | 0xae3deac0 | 6502
CBM 80xx    | Software                | COMTEXT (UD12)                                 | 324482-03  | 4KB  | 9000-9FFF | 2532      | -       | -        | -         | 0xcc01c40a | 6502
CBM 80xx    | -                       | Diagnostics 80 Col                             | 901481-01  | 4KB  | F000-FFFF | 2332      | -       | -        | -         | 0x24e4a616 | 6502
CBM 80xx    | Expansion               | PET High Speed Graphik Rev 1B                  | 324381-01B | 4KB  | A000-AEFF | 2332      | -       | 1B       | -         | 0xc8e3bff9 | 6502
CBM 80xx    | Editor                  | PET Edit 4 80 B CRTC 50Hz (UD7) [2316-059]     | 901474-04  | 2KB  | E000-E7FF | 2316B-059 | 4       | -        | -         | 0xc1ffca3a | 6502
CBM 80xx    | Editor                  | PET Edit 4 80 B CRTC 60Hz (UD7) [2316-041]     | 901474-03  | 2KB  | E000-E7FF | 2316B-041 | 4       | -        | -         | 0x5674dd5e | 6502
CBM 80xx    | Editor                  | PET Edit 4 80 CRTC 60Hz (UD7)                  | 901499-03  | 2KB  | E000-E7FF | 2316B     | 4       | -        | -         | -          | 6502
CBM 80xx CR | Basic                   | PET Basic 4 & Kernal 4 (UE7) [26011B-632]      | 324746-01  | 16KB | B000-FFFF | 23128-632 | 4       | -        | 1983      | 0x03a25bb4 | 6502
CBM 80xx CR | Editor                  | PET Edit 4 80 8296 DIN 50Hz (UE8)              | 324243-01  | 4KB  | E000-EFFF | 2532      | 4       | -        | 1982      | 0x4000e833 | 6502
CBM 8296    | Diagnostic              | DIAG 8296 v1.3                                 | 324806-01  | 4KB  | F000-FFFF | 2332      | 1.3     | -        | -         | 0xc670e91c | 6502
PET 2001    | -                       | -                                              | 901447-16  | 2KB  | -         | 2316B     | -       | -        | -         | -          | -
PET 2001    | Character (8 Scanlines) | PET Character (Basic 1/2) (A2) [6540-010]      | 901439-08  | 2KB  | -         | 6540-010  | -       | -        | -         | 0x54f32f45 | BINARY
PET 2001    | Character (8 Scanlines) | PET Character (Basic 1/2) (F10, A2)            | 901447-08  | 2KB  | -         | 2316B-08  | -       | -        | -         | 0x54f32f45 | BINARY
PET 2001    | Character (8 Scanlines) | PET Character Japanese                         | 901447-12  | 2KB  | -         | 2316B     | -       | -        | -         | 0x2c9c8d89 | BINARY
PET 2001    | Diagnostic              | Diagnostic Clip                                | 901447-30  | 2KB  | 9800-9FFF | 2316B     | -       | -        | -         | 0x73fe1901 | 6502
PET 2001    | Basic                   | PET Col80 $B                                   | 901465-15  | 4KB  | B000-BFFF | 2332      | -       | -        | -         | -          | 6502
PET 2001    | Basic                   | PET Col80 $C                                   | 901465-16  | 4KB  | C000-CFFF | 2332      | -       | -        | -         | -          | 6502
PET 2001    | Basic                   | PET Col80 $D                                   | 901465-17  | 4KB  | D000-DFFF | 2332      | -       | -        | -         | -          | 6502
PET 2001    | Kernal                  | -                                              | 901465-04  | 4KB  | F000-FFFF | 2332      | -       | -        | -         | -          | 6502
PET 2001    | Kernal                  | PET Col80 $F                                   | 901465-18  | 4KB  | F000-FFFF | 2332      | -       | -        | -         | -          | 6502
PET 2001    | Basic                   | PET Basic 1 $C0 (H1)                           | 901447-01  | 2KB  | C000-C7FF | 2316B-01  | 1       | -        | -         | 0xa055e33a | 6502
PET 2001    | Basic                   | PET Basic 1 $C0 (H1) [6540-011]                | 901439-01  | 2KB  | C000-C7FF | 6540-011  | 1       | -        | -         | 0xa055e33a | 6502
PET 2001    | Basic                   | PET Basic 1/2 $C8 (H5)                         | 901447-02  | 2KB  | C800-CFFF | 2316B-02  | 1       | -        | -         | 0x69fd8a8f | 6502
PET 2001    | Basic                   | PET Basic 1/2 $C8 (H5) [6540-012]              | 901439-05  | 2KB  | C800-CFFF | 6540-012  | 1       | -        | -         | 0x69fd8a8f | 6502
PET 2001    | Basic                   | PET Basic 1/2 $D0 (H2)                         | 901447-03  | 2KB  | D000-D7FF | 2316B-03  | 1       | -        | -         | 0xd349f2d4 | 6502
PET 2001    | Basic                   | PET Basic 1/2 $D0 (H2) [6540-013]              | 901439-02  | 2KB  | D000-D7FF | 6540-013  | 1       | -        | -         | 0xd349f2d4 | 6502
PET 2001    | Basic                   | PET Basic 1/2 $D8 (H6)                         | 901447-04  | 2KB  | D800-DFFF | 2316B-04  | 1       | -        | -         | 0x850544eb | 6502
PET 2001    | Basic                   | PET Basic 1/2 $D8 (H6) [6540-014]              | 901439-06  | 2KB  | D800-DFFF | 6540-014  | 1       | -        | -         | 0x850544eb | 6502
PET 2001    | Editor                  | PET Basic 1/2 $E0 (H3)                         | 901447-05  | 2KB  | E000-E7FF | 2316B-05  | 1       | -        | -         | 0x9e1c5cea | 6502
PET 2001    | Editor                  | PET Basic 1/2 $E0 (H3) [6540-015]              | 901439-03  | 2KB  | E000-E7FF | 6540-015  | 1       | -        | -         | 0x9e1c5cea | 6502
PET 2001    | Kernal                  | PET Basic 1/2 $F0 (H4)                         | 901447-06  | 2KB  | F000-F7FF | 2316B-06  | 1       | -        | -         | 0x661a814a | 6502
PET 2001    | Kernal                  | PET Basic 1/2 $F0 (H4) [6540-016]              | 901439-04  | 2KB  | F000-F7FF | 6540-016  | 1       | -        | -         | 0x661a814a | 6502
PET 2001    | Kernal                  | PET Basic 1/2 $F8 (H7)                         | 901447-07  | 2KB  | F800-FFFF | 2316B-07  | 1       | -        | -         | 0xc4f47ad1 | 6502
PET 2001    | Kernal                  | PET Basic 1/2 $F8 (H7) [6540-018]              | 901439-07  | 2KB  | F800-FFFF | 6540-018  | 1       | -        | -         | 0xc4f47ad1 | 6502
PET 2001    | Basic                   | PET Basic 2 $C0 (H1)                           | 901447-09  | 2KB  | C000-C7FF | 2316B-09  | 2       | -        | -         | 0x03cf16d0 | 6502
PET 2001    | Basic                   | PET Basic 2 $C0 (H1) [6540-019]                | 901439-09  | 2KB  | C000-C7FF | 6540-019  | 2       | -        | -         | 0x03cf16d0 | 6502
PET 2001    | Basic                   | PET Basic 3 $C0 (H1) [6540-020]                | 901439-13  | 2KB  | C000-C7FF | 6540-020  | 3       | -        | -         | 0x6aab64a5 | 6502
PET 2001    | Basic                   | PET Basic 3 $C8 (H5) [6540-021]                | 901439-14  | 2KB  | C800-CFFF | 6540-021  | 3       | -        | -         | 0xa8f6ff4c | 6502
PET 2001    | Basic                   | PET Basic 3 $D0 (H2) [6540-022]                | 901439-15  | 2KB  | D000-D7FF | 6540-022  | 3       | -        | -         | 0x97f7396a | 6502
PET 2001    | Basic                   | PET Basic 3 $D8 (H6) [6540-023]                | 901439-16  | 2KB  | D800-DFFF | 6540-023  | 3       | -        | -         | 0x4cf8724c | 6502
PET 2001    | Editor                  | PET Basic 3 $E0 (H3) [6540-024]                | 901439-17  | 2KB  | E000-E7FF | 6540-024  | 3       | -        | -         | 0xe459ab32 | 6502
PET 2001    | Kernal                  | PET Basic 3 $F0 (H4) [6540-025]                | 901439-18  | 2KB  | F000-F7FF | 6540-025  | 3       | -        | -         | 0x8745fc8a | 6502
PET 2001    | Kernal                  | PET Basic 3 $F8 (H7) [6540-026]                | 901439-19  | 2KB  | F800-FFFF | 6540-026  | 3       | -        | -         | 0xfd2c1f87 | 6502

## Reference

* [Files](https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/index.html)
* Disassembly
  * [Original Commodore Sources](https://github.com/mist64/cbmsrc)
  * [Basic, Editor, and Kernal from 8032 4.0 CRTC 60Hz Business](https://www.zimmers.net/anonftp/pub/cbm/src/pet/pet_rom4_disassembly.txt)
  * [Editor from 4032 4.0 CRTC 60Hz Graphics](https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-4-40-n-60hz-901499-01.dis.txt)
  * [SJGray's Disassemblies](https://github.com/sjgray/cbm-edit-rom/tree/master/disassemblies)
* [Memory Map](https://www.commodore.ca/manuals/pdfs/commodore_pet_memory_map.pdf)
* Genealogy [1](http://penguincentral.com/retrocomputing/PET/petroms.pdf) [2](https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/Commodore%20ROM%20Genealogy.pdf)
* [PET-Parts.txt](https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/PET-parts.txt)
* [Hash Codes](http://mhv.bplaced.net/cbmroms/cbmroms.php)

## Tools

* [Ghidra](https://github.com/NationalSecurityAgency/ghidra)
  * `winget install Microsoft.OpenJDK.21`
* [WFDis](https://www.white-flame.com/wfdis/)