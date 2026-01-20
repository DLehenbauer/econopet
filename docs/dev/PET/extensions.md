# PET Extension Standard Proposal

This document proposes a shared standard for memory-mapped extensions on the Commodore PET/CBM series. The goal is to let modern expansion devices and replacement mainboards provide a common interface that software can detect and use.

## Goals

The main goals of this proposal are:

1. To preserve full compatibility with existing software that targets the original PET/CBM hardware.
2. To be extensible, allowing new features to be added in a backwards-compatible manner.
3. To provide software with a straightforward means to detect and utilize extensions across different hardware implementations.
4. To support standardization of extensions without limiting individuals from adding their own custom features.

## Core Concept: Banked Control Registers

Extended features are enabled through the typically unmapped 16-byte window at `$E800-$E80F`. To provide room for many extensions, while minimizing I/O space usage, this window is banked similar to the CRTC registers.

Writing to `$E800` selects the active bank (0-255). The remaining 15 bytes (`$E801-$E80F`) then map to the registers of the selected bank.

Address         | Function          | Description
----------------|-------------------|----------------
`$E800`         | **Bank Select**   | Select active bank (0-255).
`$E801`-`$E80F` | **Banked Window** | Registers for the selected bank.

## Bank Allocation

The proposed standard allocates the 256 possible banks as follows:

* Standard extensions begin at bank `$00` and grow upwards.
* Non-standard / device-specific extensions begin at bank `$FF` and grow downwards.

## Standard Extensions

Standard extensions are features implemented by multiple mainboards or expansion devices. The first defined standard extension should provide a mechanism to identify the board type and hardware/firmware versions.

> **Note:** Most extensions will be "non-standard" and specific to a particular mainboard or expansion device. Only when multiple parties agree to implement an extension in a compatible manner should it be promoted to a "standard" extension.

## Non-Standard / Device-Specific Extensions

Non-standard extensions are specific to a particular mainboard or expansion device. These banks may be used for any purpose and are expected to overlap/conflict between different devices in the `$FF` downward-growing range.

Portable software wishing to use non-standard extensions should first perform board identification to verify it is running on the expected hardware.

## Backwards Compatibility

A goal of this standard is to be undetectable until enabled, preserving full compatibility with existing software that targets the original PET/CBM hardware. This includes software that does unconventional things, such as diagnostic tests and modern games and demos.

### Interaction with 64KB RAM Expansion

The Commodore 64KB RAM Expansion can be configured to map `$E800-$E80F` to RAM banks 2/3. This is controlled through a control register at `$FFF0`.

Bit | Function
----|---------
7   | Enable expansion memory
6   | Enable I/O peek-through (`$E800-$E8FF`)
5   | Enable screen peek-through (`$8000-$8FFF`)
4   | Reserved
3   | Select bank 2 or 3 (`$C000-$FFFF`)
2   | Select bank 0 or 1 (`$8000-$BFFF`)
1   | Write protect `$8000-$BFFF` (excludes screen peek-through)
0   | Write protect `$C000-$FFFF` (excludes I/O peek-through)

For compatibility, the extension control registers should be disabled whenever bit 7 (Enable expansion memory) is set, unless bit 6 (Enable I/O peek-through) is also set.

### Write Enable Bit at $FFF0 (Bit 4)

As a secondary precaution, implementors may choose to use a write-enable bit to prevent accidental writes to extension registers. `$FFF0` is the control register for the Commodore 64KB Memory Expansion. Bit 4 of this register is reserved and historically unused, and is repurposed here to gate access to the extension control register window.

At POR, or whenever bit 4 is cleared, `$E800` is reset to bank `$E8` and the control register overlay at `$E800-$E80F` is disabled. Software must set bit 4 to `1` to enable writes to `$E800` to select a different bank.

### Bus Holding Behavior

In later PET/CBM models, reading from an unmapped address holds the previous byte transferred on the data bus. For example, `LDA $E800` (opcode `AD 00 E8`) will return `$E8` since the address `$E800` is unmapped and `$E8` was the last byte previously transferred.

This has the effect of making it appear that unmapped regions are filled with the high byte of the corresponding memory address:

```text
.m e800 e80f
.:  e800  E8 E8 E8 E8 E8 E8 E8 E8
.:  e808  E8 E8 E8 E8 E8 E8 E8 E8
```

For example, Commodore's 64KB Memory Expansion test (`mem.8032.prg`) uses this technique to test that I/O peek-through works correctly by asserting a read to `$E800` returns `$E8`:

```text
.C:f974  AD 00 E8    LDA $E800
.C:f977  C9 E8       CMP #$E8
```

While not strictly required by the standard, it is recommended that implementors preserve or emulate this behavior for reads to bank `$E8`.

### Behavior

* At Power-On Reset (POR),
  * `$FFF0` Bit 4 should be initialized to `0`.
  * `$E800` should be initialized to bank `$E8`.
* To access extension registers, software must:
  1. Set `$FFF0` Bit 4 to `1` to enable writes to `$E800`.
  2. Write to `$E800` to select a bank other than `$E8` (which is read-only).

This two-step process makes the chances of an accidental write to extension registers vanishingly small.

## Candidates

As a starting point, the following extensions have been implemented on the EconoPET and are potential candidates for standardization:

* Toggle 40/80 columns
* Partial ColourPET support:
  * 40 or 80 columns
  * Color RAM at `$8800`
  * Color data interpreted as a pair of 4-bit indexes into a 16-color palette.
  * Currently the palette is hard-coded to RGBI, but could be customizable (R3G3B2).
  * Could support separate custom palettes for FG vs. BG (for a combined 32 on-screen colors).
* (FPGA emulated) SID chip overlaid at `$8Fxx`.
* Video RAM mask (`$00` = 1KB, `$01` = 2KB, `$11` = 4/8KB).

(Next on my list is exposing the character ROM data for custom fonts.)
