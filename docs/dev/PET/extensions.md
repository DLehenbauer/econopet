# PET Extension Standard Proposal

This document outlines a proposal for a shared standard for memory-mapped extensions on the Commodore PET/CBM series.  The goal is to allow modern expansion devices and replacement mainboards to provide a common interface for software to detect and utilize additional hardware features.

## Core Concept: Banked Registers

For maximum backwards compatibility with the original CBM/PET hardware, and to minimize the impact on the limited I/O space of the PET, this standard proposes that control registers occupy a single 16-byte window at `$E800-$E80F` with a banking mechanism.

Address         | Function                 | Description
----------------|--------------------------|----------------
`$E800`         | **Bank Select Register** | Writing a value selects the active bank.
`$E801`-`$E80F` | **Banked Window**        | The function of these 15 bytes depends on the selected bank.

Note that while control registers reside exclusively in the `$E800-$E80F` range, the effect of these registers may extend to other memory-mapped regions.  For example, a control register might toggle access to a SID chip overlaid at `$8Fxx` or expand the number of indexable registers of the CRTC.

## Bank Allocation

The proposed standard allocates the 256 possible banks as follows:

* Standard extensions begin at bank $00 and grow upwards.
* Non-standard / board specific extensions begin at bank $FF and grow downwards.

## Standard Extensions

Standard extensions are features implemented by multiple mainboards or expansion devices. The first defined standard extension should provide a mechanism to identify the board type and hardware/firmware versions.

> **Note:** Most extensions will be "non-standard" and specific to a particular mainboard or expansion device.  Only when multiple parties agree to implement a particular extension in a compatible manner should it be promoted to a "standard" extension.

## Non-Standard / Board-Specific Extensions

Non-standard extensions are specific to a particular mainboard or expansion device. These banks may be used for any purpose and are expected to overlap/conflict between different devices.

Portable software wishing to use non-standard extensions should first perform board identification to verify it is running on the expected hardware.

## Backwards Compatibility

A primary goal of this standard is to allow software to remain compatible with unmodified PET/CBM hardware while detecting and leveraging extensions when available.

Implementations targeting strict backwards compatibility should additionally implement:

1. "Bus holding" emulation for bank `$E8`.
2. A write-enable bit at `$FFF0` (Bit 4).

These behaviors are described in more detail below.

### Bank $E8 is Reserved/Special

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

### Write Enable Bit at $FFF0 (Bit 4)

As a secondary precaution, implementors may choose to use a write-enable bit to prevent accidental writes to extension registers. `$FFF0` is the control register for the Commodore 64KB Memory Expansion.  Bit 4 of this register is reserved and historically unused.

### Behavior

* At Power-On Reset (POR),
  * `$FFF0` Bit 4 should be initialized to `0`.
  * `$E800` should be initialized to bank `$E8`
* To access extension registers, software must:
    1. Set `$FFF0` Bit 4 to `1` to enable writes to `$E800`.
    2. Write to `$E800` to select a bank other than `$E8` (which is read-only).

This two-step process makes the chances of an accidental write to extension registers vanishingly small.

## Candidates

As a starting point, the following extensions have been implemented on the EconoPET and are potential candidates for standardization:

* Toggle 40/80 columns
* Partial ColourPET support:
  * 40 or 80 columns
  * Colour ram at $8800
  * Colour data interpretted as a pair of 4-bit indexes into a 16-color palette.
  * Currently palette hard coded to RGBI, but could be customizable (R3G3B2).
  * Could support separate custom palettes for FG vs. BG (for a combined 32 onscreen colors.)
* (FPGA emulated) SID chip overlaid at `$8Fxx`.
* Video RAM mask ($00 = 1KB, $01 = 2KB, $11 = 4/8KB).

(Next on my list is exposing the character ROM data for custom fonts.)
