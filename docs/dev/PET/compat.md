# PET/CBM Compatibility Notes

This document collects technical notes, observations, and implementation details relevant to achieving a high degree of compatibility with original Commodore PET/CBM hardware.

The details described here are unlikely to be exploited by most software, but may be relevant for supporting modern demos and games that rely on undocumented or unintended hardware behaviors.

## Address Decoding

The PET/CBM address space is not fully decoded, resulting in mirroring and contention of I/O devices across the `$E800-$E88F` range.

Software could exploit mirroring to access devices at non-standard addresses.  Contention could theoretically be used to write the same value to multiple devices simultaneously.

### Video Memory

On models with less than 4KB of video RAM, the video memory is mirrored across the `$8000-$8FFF` range as follows:

* 40 column PET has 1k of screen RAM, mirrored 4 times at `$8000`, `$8400`, `$8800`, `$8C00`.
* 80 column PET has 2k of screen RAM, mirrored 2 times at `$8000`, `$8800`.

Programs use this to detect the amount of video RAM available by writing to $8000 and observing which addresses reflect the change.

#### Example

The [No Pets Allowed](https://www.pouet.net/prod.php?which=56410) demo uses this technique:

```text
.C:0517  A9 21       LDA #$21
.C:0519  8D 00 84    STA $8400
.C:051c  A9 20       LDA #$20
.C:051e  8D 00 80    STA $8000
.C:0521  AD 00 84    LDA $8400
.C:0524  C9 21       CMP #$21
.C:0526  F0 59       BEQ $0581  ; Quit if $8000 does not overwrite $8400
```

### I/O Decoding

The following table shows the decoding logic for early and later PET/CBM models:

Device         | Early Decoding    | Later Decoding
---------------|-------------------|---------------
PIA1 (6821)    | `$Exxx & A11 & A4`| `$E8xx & A4`
PIA2 (6821)    | `$Exxx & A11 & A5`| `$E8xx & A5`
VIA (6522)     | `$Exxx & A11 & A6`| `$E8xx & A6`
CRTC (6845)    | -                 | `$E8xx & A7`

#### Contention

The following table summarizes which devices are active at each address in the `$E800-$E8FF` range, along with any bus contention that may occur:

Address | Active (Early Decoding) | Active (Later Decoding) | Contention
--------|-------------------------|-------------------------|-----------
`$E80x` | -                       | -                       | -
`$E81x` | PIA1                    | PIA1                    | -
`$E82x` | PIA2                    | PIA2                    | -
`$E83x` | PIA1, PIA2              | PIA1, PIA2              | Both
`$E84x` | VIA                     | VIA                     | -
`$E85x` | PIA1, VIA               | PIA1, VIA               | Both
`$E86x` | PIA2, VIA               | PIA2, VIA               | Both
`$E87x` | PIA1, PIA2, VIA         | PIA1, PIA2, VIA         | Both
`$E88x` | -                       | CRTC                    | -
`$E89x` | PIA1                    | CRTC, PIA1              | Later only
`$E8Ax` | PIA2                    | CRTC, PIA2              | Later only
`$E8Bx` | PIA1, PIA2              | CRTC, PIA1, PIA2        | Both
`$E8Cx` | VIA                     | CRTC, VIA               | Later only
`$E8Dx` | PIA1, VIA               | CRTC, PIA1, VIA         | Both
`$E8Ex` | PIA2, VIA               | CRTC, PIA2, VIA         | Both
`$E8Fx` | PIA1, PIA2, VIA         | CRTC, PIA1, PIA2, VIA   | Both

#### Mirroring

When a given device is active, its registers are mirrored according to the following table:

Device | Mirrors
-------|---------
PIA    | `x0`, `x4`, `x8`, `xC`
VIA    | `x0`
CRTC   | `x0`, `x2`, `x4`, `x6`, `x8`, `xA`, `xC`, `xE`

##### Safe (Non-Contended) Mirrors

The following ranges contain mirrored images of devices without bus contention from other devices:

Device | Range         | Safe Decoding | Mirrors
-------|---------------|---------------|-----------------------------------------------
PIA1   | `$E810-$E81F` | Both          | `x0`, `x4`, `x8`, `xC`
PIA2   | `$E820-$E82F` | Both          | `x0`, `x4`, `x8`, `xC`
CRTC   | `$E880-$E88F` | Later         | `x0`, `x2`, `x4`, `x6`, `x8`, `xA`, `xC`, `xE`
PIA1   | `$E890-$E89F` | Early         | `x0`, `x4`, `x8`, `xC`
PIA2   | `$E8A0-$E8AF` | Early         | `x0`, `x4`, `x8`, `xC`
VIA    | `$E8C0-$E8CF` | Early         | `x0`

## Bus Holding Behavior in Later PET/CBM Models

In later PET/CBM models, reading from an unmapped address holds the previous byte transferred on the data bus. For example, `LDA $E800` (AD 00 E8) will return `$E8` since `$E800` is unmapped and `$E8` was the last byte previously transferred.

This has the effect of making it appear that unmapped regions are filled with the high byte of the corresponding memory address:

```text
.m e800 e80f
.:  e800  E8 E8 E8 E8 E8 E8 E8 E8
.:  e808  E8 E8 E8 E8 E8 E8 E8 E8
```

### Example

The CBM 64KB Memory Expansion test (`mem.8032.prg`) uses this technique to test that IO peek-through works correctly by expecting to see $E8 at $E800:

```text
.C:f974  AD 00 E8    LDA $E800
.C:f977  C9 E8       CMP #$E8
```

The VICE 3.9 emulator approximates the bus holding behavior by returning the high byte of any address read.  This is imperfect and can be detected by using indirect addressing:

```text
A9 00       LDA #$00
85 FB       STA $FB
A9 E8       LDA #$E8
85 FC       STA $FC
A0 00       LDY #$00
B1 FB       LDA ($FB),Y
```

A real PET would return `$FB` (the last byte transferred before the read cycle), but VICE 3.9 returns `$E8`.

## 50/60 Hz Detection

Software that relies on interrupt timing may need to differentiate between 50 Hz and 60 Hz PET/CBM models.

One method to differentiate between 50 and 60 Hz models is to measure the the number of CPU cycles between interrupts:

* 50 Hz: 20 ms ~= 20,000 cycles.
* 60 Hz: 16.7 ms ~= 16,667 cycles.

### Example

The [No Pets Allowed](https://www.pouet.net/prod.php?which=56410) demo uses this technique:

```text
.C:055e  AC 44 E8    LDY $E844
.C:0561  AE 45 E8    LDX $E845
.C:0564  AD D7 05    LDA $05D7
.C:0567  C9 B7       CMP #$B7
.C:0569  B0 01       BCS $056C  ; Quit
.C:056b  60          RTS
```
