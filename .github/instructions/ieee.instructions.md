---
description: 'ROM-evidenced compatibility requirements for Commodore IEEE-488 disk-drive emulation'
applyTo: 'fw/src/ieee/**/*, fw/test/**/*ieee*, fw/test/**/*disk*, gw/**/*ieee*, docs/**/*ieee*'
---

# IEEE Drive Compatibility Instructions

Treat externally observable behavior of the virtual drive as a compatibility
contract with the original Commodore peripheral. Require primary-source
evidence for implementation decisions and code-review conclusions. Do not infer
behavior from the current EconoPET implementation or its tests.

## Compatibility Standard

For native Commodore behavior, require the implementation to match the relevant
4040 ROM or the selected 8050/8250 ROM revision and hardware mode in every
observable respect:

- command and secondary-address decoding
- TALK, LISTEN, UNTALK, UNLISTEN, OPEN, and CLOSE state transitions
- channel allocation, readiness, isolation, reuse, and teardown
- data bytes, byte ordering, EOI placement, EOF behavior, and repeated reads
- command-channel collection, termination, execution, and status clearing
- exact status numbers, text, punctuation, track, sector, and drive fields
- filename parsing, drive prefixes, wildcards, file types, and access modes
- directory byte stream, BASIC line structure, entries, free-block count, and
  final EOI
- D64 and D80 geometry, directory layout, BAM interpretation, sector links, and
  final-sector length
- REL record positioning, record lengths, side-sector behavior, overflow,
  read/write errors, and channel interaction
- reset, initialization, device addressing, dual-drive behavior, and per-unit
  state
- IEEE-488 electrical polarity, open-collector composition, handshake ordering,
  ATN behavior, and EOI timing

Treat a difference as a compatibility defect unless primary evidence proves it
belongs to the selected peripheral or ROM revision.

Extensions without an original-hardware counterpart, such as EconoPET's flat
`.hdd` container, must be explicitly labeled as extensions. Isolate them so
native D64/D80 and IEEE behavior remains unchanged.

## Source Priority

Use evidence in this order:

1. Relevant Commodore drive ROM source or disassembly.
2. Relevant PET KERNAL source for the controller side of the exchange.
3. Original manuals, schematics, datasheets, and captures from real hardware.
4. Established emulators and FPGA implementations as corroboration.
5. Existing EconoPET code and tests only as descriptions of current behavior.

Never use a secondary source to override the relevant Commodore ROM. When
sources disagree, identify the target model and ROM revision, follow the
primary source, and document the discrepancy.

## Primary ROM Sources

Use the exact files below. Start from `master` to understand composition, then
follow calls and constants through every routine that controls the changed
behavior.

### Commodore 4040 and D64 behavior

- Build/source index:
  [DOS_4040/master](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/master)
- IEEE handshake, addressing, command-byte decoding, EOI, TALK, and LISTEN:
  [DOS_4040/ieee](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/ieee)
- OPEN names, drive prefixes, file type/mode parsing, directory and direct
  channels:
  [DOS_4040/open](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/open)
- CLOSE and channel teardown:
  [DOS_4040/close](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/close)
- Read/write channel construction and channel state:
  [DOS_4040/opchnl](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/opchnl)
- Command-channel parsing, dispatch, syntax, and termination:
  [DOS_4040/parsex](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/parsex)
  and
  [DOS_4040/idle](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/idle)
- Exact status numbers and strings:
  [DOS_4040/erproc](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/erproc)
- Directory stream construction and final EOI:
  [DOS_4040/lstdir](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/lstdir)
- File lookup and transfer:
  [DOS_4040/lookup](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/lookup),
  [DOS_4040/getact](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/getact),
  and
  [DOS_4040/trnsfr](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/trnsfr)
- Block and memory commands:
  [DOS_4040/block](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/block)
  and
  [DOS_4040/memrw](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/memrw)
- REL behavior:
  [DOS_4040/record](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/record),
  [DOS_4040/fndrel](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/fndrel),
  [DOS_4040/rel1](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/rel1),
  [DOS_4040/rel2](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/rel2),
  [DOS_4040/rel3](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/rel3),
  and
  [DOS_4040/rel4](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/rel4)
- Geometry, constants, BAM, and disk initialization:
  [DOS_4040/equate](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/equate),
  [DOS_4040/dskint](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/dskint),
  and
  [DOS_4040/romtbl](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/romtbl)

### Commodore 8050/8250 shared DOS source and D80 behavior

`DOS_8250` is the shared DOS 2.7 source base used by full-size 8050 and 8250
drives. The same main ROM can branch on the hardware's single-sided 8050 or
double-sided 8250 mode. It is not a universal source for earlier DOS 2.5
revisions, vendor-specific controller ROMs, or the later 8250LP.

Select the exact ROM revision and hardware mode before drawing a compatibility
conclusion. Use the corresponding DOS 8250 files whenever D80 geometry,
8050/8250 status, or model-specific behavior is involved. EconoPET's current
D80 path primarily targets single-sided 8050 behavior:

- [DOS_8250/master](https://github.com/mist64/cbmsrc/blob/master/DOS_8250/master)
- [DOS_8250/ieee](https://github.com/mist64/cbmsrc/blob/master/DOS_8250/ieee)
- [DOS_8250/open](https://github.com/mist64/cbmsrc/blob/master/DOS_8250/open)
- [DOS_8250/close](https://github.com/mist64/cbmsrc/blob/master/DOS_8250/close)
- [DOS_8250/opchnl](https://github.com/mist64/cbmsrc/blob/master/DOS_8250/opchnl)
- [DOS_8250/parsex](https://github.com/mist64/cbmsrc/blob/master/DOS_8250/parsex)
- [DOS_8250/erproc](https://github.com/mist64/cbmsrc/blob/master/DOS_8250/erproc)
- [DOS_8250/lstdir](https://github.com/mist64/cbmsrc/blob/master/DOS_8250/lstdir)
- [DOS_8250/block](https://github.com/mist64/cbmsrc/blob/master/DOS_8250/block)
- [DOS_8250/memrw](https://github.com/mist64/cbmsrc/blob/master/DOS_8250/memrw)
- [DOS_8250/record](https://github.com/mist64/cbmsrc/blob/master/DOS_8250/record)
- [DOS_8250/fndrel](https://github.com/mist64/cbmsrc/blob/master/DOS_8250/fndrel)
- [DOS_8250/rel1](https://github.com/mist64/cbmsrc/blob/master/DOS_8250/rel1)
- [DOS_8250/rel2](https://github.com/mist64/cbmsrc/blob/master/DOS_8250/rel2)
- [DOS_8250/rel3](https://github.com/mist64/cbmsrc/blob/master/DOS_8250/rel3)
- [DOS_8250/rel4](https://github.com/mist64/cbmsrc/blob/master/DOS_8250/rel4)
- [DOS_8250/equate](https://github.com/mist64/cbmsrc/blob/master/DOS_8250/equate)
- [DOS_8250/dskint](https://github.com/mist64/cbmsrc/blob/master/DOS_8250/dskint)
- [DOS_8250/map](https://github.com/mist64/cbmsrc/blob/master/DOS_8250/map)
- [DOS_8250/sidsec](https://github.com/mist64/cbmsrc/blob/master/DOS_8250/sidsec)

### PET controller-side behavior

Use these PET KERNAL 4.0 sources to verify what the emulated drive must observe
and return:

- IEEE send/receive primitives, TALK, LISTEN, UNTALK, UNLISTEN, secondary
  addressing, EOI, and handshakes:
  [KERNAL_PET_4.0_1979-10-23/ob1src](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_4.0_1979-10-23/ob1src)
- LOAD and OPEN byte sequences:
  [KERNAL_PET_4.0_1979-10-23/ob2src](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_4.0_1979-10-23/ob2src)
- SAVE and CLOSE byte sequences:
  [KERNAL_PET_4.0_1979-10-23/ob3src](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_4.0_1979-10-23/ob3src)
- CHKIN/CHRIN and channel input behavior:
  [KERNAL_PET_4.0_1979-10-23/ob4src](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_4.0_1979-10-23/ob4src)

### SuperPET Waterloo 6809 controller behavior

Use the exact Waterloo revision 12 ROM set downloaded by
[`.devcontainer/download.sh`](../../.devcontainer/download.sh). Use the linked
6809 disassemblies for navigation and the pinned Zimmers.net binaries as the
primary byte-level evidence:

| Address range | Part | Disassembly | ROM binary | MD5 |
|---------------|------|-------------|------------|-----|
| `$A000-$BFFF` | `970018-12` | [waterloo-a000-bfff.asm](https://github.com/prachwal/personal-004/blob/main/docs/pet/disassembly/waterloo-a000-bfff.asm) | [waterloo-a000-bfff.970018-12.bin](https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/SuperPET/waterloo-a000-bfff.970018-12.bin) | `84cb402449c6107b7d2b636cb28f0042` |
| `$C000-$DFFF` | `970019-12` | [waterloo-c000-dfff.asm](https://github.com/prachwal/personal-004/blob/main/docs/pet/disassembly/waterloo-c000-dfff.asm) | [waterloo-c000-dfff.970019-12.bin](https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/SuperPET/waterloo-c000-dfff.970019-12.bin) | `0f9bd7123d99892ce80f1e7e438c2194` |
| `$E000-$FFFF` | `970034-12` | [waterloo-e000-ffff.asm](https://github.com/prachwal/personal-004/blob/main/docs/pet/disassembly/waterloo-e000-ffff.asm) | [waterloo-e000-ffff-970034-12.bin](https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/SuperPET/waterloo-e000-ffff-970034-12.bin) | `c03098d26dbb1a23737d3286f264bc97` |

These are generated linear
[Capstone 6809 disassemblies](https://github.com/prachwal/personal-004/blob/main/docs/pet/disassembly/README.md),
not original Waterloo source or an annotated reconstruction. They can decode
embedded strings and tables as instructions. Confirm conclusions against the
pinned ROM bytes, call sites, and reachable control flow.

Use the
[Waterloo investigation notes](https://github.com/prachwal/personal-004/blob/main/docs/pet/waterloo-investigation.md)
as a secondary call-path guide, not as primary compatibility evidence. The
notes distinguish verified traces from open hypotheses. Preserve that
distinction in implementations and reviews.

The Waterloo 6809 directly controls the PET's PIA2 and VIA. It does not invoke
the 6502 KERNAL as an intermediary. Its IEEE implementation spans the
`$A000-$BFFF` and `$C000-$DFFF` ROMs:

- `$BE6F-$BEFA` transmits one byte and polls NRFD and NDAC.
- `$BEFB-$BF97` receives one byte, samples EOI and DIO, releases NDAC, waits
  for DAV to release, then re-arms NDAC.
- `$C072-$C175` performs higher-level address, role, ATN, and termination
  transitions.

The standard PET I/O range `$E800-$EFFF` lies inside the 6809 ROM address
range but must override ROM. In particular, `$E820-$E83F` must select PIA2 and
`$E840-$E87F` must select the VIA. This is required by the direct register
accesses above, matches
[VICE `petmem.c`](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/pet/petmem.c),
and is implemented by
[`address_decoding.sv`](../../gw/EconoPET/src/address_decoding.sv). A test that
loads the Waterloo ROM without this I/O hole cannot establish IEEE behavior.

For TALK termination and counted-read continuation, trace these routines:

- `$C13C` delays, calls `$C14F`, sends UNTALK byte `$5F`, then calls `$C16E`
  to release ATN.
- `$C14F-$C16D` releases NRFD by setting `$E840` bit 1, releases NDAC by
  setting `$E821` bit 3, waits for DAV to release through `$E840` bit 7, then
  clears `$E840` bit 2 to assert ATN.
- `$C16E-$C175` sets `$E840` bit 2 to release ATN.

Tests that claim Waterloo compatibility must reproduce this register-write and
handshake order. Do not substitute ATN-before-NDAC termination. A fast FPGA
talker can already have the next byte's DAV asserted when `$C14F` releases
NDAC, so review FIFO advancement and resumed TALK data explicitly.

Do not use PET 6502 BASIC or KERNAL memory-map labels as symbols for the
Waterloo 6809 ROMs merely because an address matches. Derive routine boundaries
and behavior from the pinned Waterloo binaries and their call sites.

The Waterloo module-load path is not yet a compatibility oracle. The verified
call chain begins at `$AC30`, dispatches through jump-table entry `$B0AE` to
`$B23B`, and reaches `$D412`. The linked investigation observed its failed
load returning before IEEE traffic, but explicitly leaves `$D417` onward and
the real disk protocol unresolved. Do not infer TALK, LISTEN, filename, or
status behavior from the displayed `Loading 'disk/1.PASCAL'` text or the
`Program not found` result. Trace the remaining ROM path or capture working
hardware before using that flow as compatibility evidence.

## Corroborating Sources

Use at least one independent secondary source for ordinary changes and two when
the ROM is ambiguous, timing-sensitive, or model-dependent.

- Michael Steil's Commodore Peripheral Bus series:
  - [Part 1: IEEE-488](https://www.pagetable.com/?p=1023)
  - [Part 2: TALK/LISTEN](https://www.pagetable.com/?p=1031)
  - [Part 3: Commodore DOS](https://www.pagetable.com/?p=1038)
- VICE ROM-level IEEE-drive emulation:
  - [vice/src/drive/ieee/ieee.c](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/drive/ieee/ieee.c)
  - [vice/src/drive/iecieee/iecieee.c](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/drive/iecieee/iecieee.c)
- MAME original-hardware models:
  - [c2040.cpp](https://github.com/mamedev/mame/blob/master/src/devices/bus/ieee488/c2040.cpp)
  - [c8050.cpp](https://github.com/mamedev/mame/blob/master/src/devices/bus/ieee488/c8050.cpp)
  - [ieee488.cpp](https://github.com/mamedev/mame/blob/master/src/devices/bus/ieee488/ieee488.cpp)
  - [c4040_dsk.cpp](https://github.com/mamedev/mame/blob/master/src/lib/formats/c4040_dsk.cpp)
  - [d80_dsk.cpp](https://github.com/mamedev/mame/blob/master/src/lib/formats/d80_dsk.cpp)
- PET Universal for MiSTer:
  - [rtl/ieee_drive/ieee_drive.sv](https://github.com/raparici/PET_Universal_MiSTer/blob/main/rtl/ieee_drive/ieee_drive.sv)
  - [rtl/ieee_drive/ieeedrv_drv.sv](https://github.com/raparici/PET_Universal_MiSTer/blob/main/rtl/ieee_drive/ieeedrv_drv.sv)
  - [rtl/ieee_drive/ieeedrv_logic.sv](https://github.com/raparici/PET_Universal_MiSTer/blob/main/rtl/ieee_drive/ieeedrv_logic.sv)
- Repository summaries and original-hardware references:
  - [docs/dev/PET/ieee.md](../../docs/dev/PET/ieee.md)
  - [PET and the IEEE-488 Bus](http://www.primrosebank.net/computers/pet/documents/PET_and_the_IEEE488_Bus_text.pdf)
  - [Rockwell R6520 PIA](http://archive.6502.org/datasheets/rockwell_r6520_pia.pdf)
  - [Western Design Center W65C22 VIA](https://www.westerndesigncenter.com/wdc/documentation/w65c22.pdf)

## Required Workflow

1. Define the exact observable behavior being changed and select the target
   peripheral and ROM revision.
2. Trace the complete EconoPET path across gateware, FIFOs/registers, firmware,
   disk-image access, and caller-visible output.
3. Trace the corresponding primary ROM routine from entry through all relevant
   calls, tables, flags, and error exits. Do not rely on routine names or
   comments alone.
4. Record the exact bytes, state transitions, status, EOI behavior, and edge
   cases established by the primary source.
5. Corroborate the conclusion with the required independent sources. Explain
   any disagreement.
6. Implement the change that matches the selected Commodore behavior.
7. Add tests derived from the primary behavior, including negative, boundary,
   state-transition, repeated-operation, and multi-unit/channel cases.
8. Run the set of relevant tests, then the complete affected suite.
9. Include the compatibility evidence in the final implementation summary or
   code-review report.

## Required Evidence

For every changed or reviewed behavior, provide a table with this shape:

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|

Use exact file links and routine labels. State the ROM-derived rule in concrete
terms, such as command bytes, channel state, returned bytes, error code, EOI
position, track/sector values, or handshake transitions.

Do not claim compatibility based only on:

- passing existing tests
- agreement with VICE, MAME, MiSTer, or pagetable
- comments in EconoPET code
- general Commodore DOS documentation
- behavior observed in one happy-path program

If primary evidence is unavailable or ambiguous, stop and report the gap. Do
not describe the behavior as compatible until it is resolved through another
ROM revision, a real-hardware trace, or equivalent primary evidence.

## Implementation and Review Gates

Report an incomplete implementation when changed behavior lacks a primary ROM
citation, an exact compatibility conclusion, or a ROM-derived test.

Report a compatibility defect when EconoPET bytes, state transitions, status,
EOI, filesystem interpretation, or handshake behavior differs from the
selected Commodore source.

Report insufficient evidence when a conclusion relies only on secondary
sources or existing EconoPET behavior.

Report a model-selection defect when 4040 behavior is applied to an 8050/8250
path, or the reverse, without evidence that the ROMs agree.

Report an extension-boundary defect when EconoPET-specific behavior leaks into
native D64/D80, channel, status, or IEEE semantics.

For code reviews, treat every gate above as actionable. For implementations,
resolve each gate before declaring the task complete.

## Validation

Use the relevant commands:

```sh
cmake --build --preset fw_test
ctest --preset fw --output-on-failure
ctest --preset gw -R 'ieee_(sys_)?tb' --output-on-failure
```

Run both Icarus and Verilator variants when gateware behavior changes:
`ieee_tb`, `ieee_tb_vl`, `ieee_sys_tb`, and `ieee_sys_tb_vl`.

Tests must assert exact compatibility outputs. Avoid tests that check only that
an operation succeeds.
