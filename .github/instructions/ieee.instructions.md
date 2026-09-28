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

PET ROM 1 and the uncorrected early ROM 2 IEEE input implementation are not
native compatibility targets. Cover the installed corrected PET KERNAL 2.0
image, grouped below as ROM 2/3, and PET KERNAL 4.0/4.1:

- PET KERNAL 2.0 reconstructed IEEE primitives:
  [KERNAL_PET_2.0_REC/ob1src](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_2.0_REC/ob1src)
- PET KERNAL 2.0 reconstructed LOAD/OPEN, SAVE/CLOSE, and channel-I/O paths:
  [ob2src](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_2.0_REC/ob2src),
  [ob3src](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_2.0_REC/ob3src),
  and
  [ob4src](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_2.0_REC/ob4src)
- PET KERNAL 4.0 reconstructed IEEE primitives and controller sequences:
  [KERNAL_PET_4.0_REC/ob1src](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_4.0_REC/ob1src)

- IEEE send/receive primitives, TALK, LISTEN, UNTALK, UNLISTEN, secondary
  addressing, EOI, and handshakes:
  [KERNAL_PET_4.0_1979-10-23/ob1src](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_4.0_1979-10-23/ob1src)
- LOAD and OPEN byte sequences:
  [KERNAL_PET_4.0_1979-10-23/ob2src](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_4.0_1979-10-23/ob2src)
- SAVE and CLOSE byte sequences:
  [KERNAL_PET_4.0_1979-10-23/ob3src](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_4.0_1979-10-23/ob3src)
- CHKIN/CHRIN and channel input behavior:
  [KERNAL_PET_4.0_1979-10-23/ob4src](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_4.0_1979-10-23/ob4src)
- ROM 4 disk statement parsing, command construction, and public vectors:
  [dos.syntax](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_4.0_REC/dos.syntax),
  [dos.write](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_4.0_REC/dos.write),
  and
  [ob7src](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_4.0_REC/ob7src)

Use "ROM 2/3" below only for the installed corrected KERNAL 2.0 image. It does
not mean that every early PET KERNAL has the same `ACPTR` implementation.

| ROM group | Address range | Part | ROM binary | MD5 |
|-----------|---------------|------|------------|-----|
| ROM 2/3 KERNAL 2.0 | `$F000-$FFFF` | `901465-03` | [kernal-2.901465-03.bin](https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/kernal-2.901465-03.bin) | `51a38bfef8f9e72cb64bf7d874b4c8c6` |
| ROM 4 disk statements | `$D000-$DFFF` | `901465-21` | [basic-4-d000.901465-21.bin](https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/basic-4-d000.901465-21.bin) | `ab780e94772dca756a0678a17b5bc3a2` |
| ROM 4 KERNAL 4.0/4.1 | `$F000-$FFFF` | `901465-22` | [kernal-4.901465-22.bin](https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/kernal-4.901465-22.bin) | `16ec21443ea5431ab63d511061054e6f` |

Use the repository's generated
[PET memory map](../../docs/dev/PET/memorymap-gen.csv) as the authoritative
cross-version symbol map. Its `2.0` and `4.0` columns supply the two address
columns below. Preserve the map's case-sensitive names: `DOPEN` is the public
vector while `dopen` is the disk implementation; `STOP` is BASIC's statement
handler while `stop` is the KERNAL routine; `LIST4` and `list4` are unrelated
BASIC and IEEE labels. Reconstructed listings use uppercase names in separate
assembly units, so do not join them to this map by uppercasing symbol names.
The addresses, instruction boundaries, and behavior were then
checked against the
[PET2](https://github.com/ethandicks/cbmsymbols/blob/master/listings/PET2.txt)
and
[PET4](https://github.com/ethandicks/cbmsymbols/blob/master/listings/PET4.txt)
assembler listings, the
[8032 ROM 4 disassembly](https://www.zimmers.net/anonftp/pub/cbm/src/pet/pet_rom4_disassembly.txt),
and the installed binaries. Every listing byte present in the KERNAL 2 range
matched `901465-03`. Every listed byte in the ROM 4 `$D000` and `$F000` ranges
matched except the source placeholders at `CKSUMD` (`$DE9D`) and `CKSUMF`
(`$FD5C`), which contain generated checksum bytes in the installed images.
Source labels describe internal structure and are not automatically safe entry
points.

The dated 1979 KERNAL 4 source is not the final byte-level revision authority.
Its LOAD path calls `SECND` after TALK, while the reconstructed source and
installed `901465-22` call `TKSA` at `$F379`. The production path therefore
performs the talk-side receive-line setup before releasing ATN. No separate
KERNAL 4.1 F-ROM was found. The patched BASIC B-ROM still uses `901465-22`.

#### Callable IEEE transport entries

These routines directly manipulate PIA2 or VIA IEEE lines, or deliberately
enter the byte handshake. Use `ISOUR` and `ACPTR` for electrical handshake
tests. Use the addressing and termination entries for controller-sequence
tests.

| ROM 2/3 address | ROM 4 address | Source label or role | Entry evidence | Observable behavior and test relevance |
|-----------------|---------------|----------------------|----------------|----------------------------------------|
| `$E1DE` | `$E000` | `CINT`, editor-resident system initialization | The authoritative map names both entries. ROM 2/3 starts `A9 7F 8D 4E E8`. ROM 4 is editor-dependent and starts either the implementation or a jump to it. | Initializes system I/O including the IEEE-facing PIA and VIA registers. It is useful for full-machine startup, but it is outside the KERNAL ROM and is not a focused bus-reset primitive. |
| `$F0B6` | `$F0D2` | `TALK` | ROM 2/3 starts `A9 40 D0 02 A9 20`. ROM 4 starts `A9 40 2C A9 20`. Called by LOAD and CHKIN paths, and by ROM 4 `EREAD`. | Enters `LIST1` with the TALK base `$40`, combines it with `FA`, asserts ATN, and sends MTA. It does not send the secondary address. |
| `$F0BA` | `$F0D5` | `LISTN` | Both start `A9 20 48 AD 40 E8`. Called by OPEN, SAVE, CLOSE, CHKOUT, and ROM 4 command paths. | Enters `LIST1` with the LISTEN base `$20`, combines it with `FA`, asserts ATN, and sends MLA. |
| `$F0BC` | `$F0D7` | `LIST1`, command-byte sender | Both start `48 AD 40 E8 09 02 8D 40`. Called by `UNLSN`, and reached by `TALK`, `LISTN`, and `UNTLK`. | Releases the receive handshake lines, flushes a pending `CIOUT` byte with EOI when required, waits for DAV release, asserts ATN, and sends `A OR FA` through `ISOUR`. Use only with a valid base command in A and a valid `FA`. |
| `$F0EE` | `$F109` | `ISOUR`, physical byte output | Both start `A9 3C 8D 23 E8 AD 40 E8`. Called by `LIST1`, `SECND`, `TKSA`, and `CIOUT`. | Takes the logical byte in `BSOUR` and complements it for PIA2 output, waits for NRFD and NDAC, applies the VIA timer policy (with ROM 4's optional STOP-key retry), then releases DAV and DIO. This is the focused controller-output handshake. |
| `$F128` | `$F143` | `SECND` | ROM 2/3 starts `85 A5 20 EE F0 AD 40 E8`. ROM 4 starts `85 A5 20 09 F1 AD 40 E8`. Called by OPEN, SAVE, CLOSE, and CHKOUT. | Sends A as a listener secondary command through `ISOUR`, then falls into `SCATN` to release ATN. |
| `$F12D` | `$F148` | `SCATN`, release ATN | Both start `AD 40 E8 09 04 8D 40 E8`. Called by `SECND` and `TKATN`, and reached after UNLISTEN. | Sets VIA port-B bit 2 to release ATN. Use it for a focused ATN-polarity test, not as a complete address sequence. |
| `$F164` | `$F193` | `TKSA` | ROM 2/3 starts `85 A5 20 EE F0 20 46 F1`. ROM 4 starts `85 A5 20 09 F1 20 75 F1`. Called by CHKIN. ROM 4 LOAD and `EREAD` also call it. | Sends A as a talker secondary command, establishes controller-listener handshake state through `TKATN`, and releases ATN. |
| `$F169` | `$F198` | `TKATN` | ROM 2/3 starts `20 46 F1 4C 2D F1`. ROM 4 starts `20 75 F1 4C 48 F1`. Called by `TKSA` and by CHKIN for a negative secondary address. | Asserts NRFD, prepares PIA2 for input, and then jumps to `SCATN`. It is the correct talk-to-data transition entry. |
| `$F16F` | `$F19E` | `CIOUT`, buffered IEEE output | Both start `24 A0 30 04 C6 A0 D0 05`. Called by `BSOUT`, OPEN, and SAVE paths. | Buffers A in `BSOUR` and sends the previously buffered byte when required. The final pending byte is flushed with EOI by the next command or UNLISTEN. Do not expect one call to produce the current byte immediately. |
| `$F17F` | `$F1AE` | `UNTLK` | ROM 2/3 starts `A9 5F D0 02 A9 3F`. ROM 4 starts `AD 40 E8 29 FB 8D 40 E8`. Called by LOAD, `CLRCHN`, and ROM 4 status reads. | Sends `$5F`. ROM 2/3 enters `LIST1`, which releases NRFD and NDAC before asserting ATN. ROM 4 asserts ATN first, then enters the shared command path. Keep these termination-order tests separate. |
| `$F183` | `$F1B9` | `UNLSN` | ROM 2/3 starts `A9 3F EA EA 20 BC F0`. ROM 4 starts `A9 3F 20 D7 F0 D0 88`. Called by OPEN, SAVE, CLOSE, `CLRCHN`, and ROM 4 disk paths. | Sends `$3F` through `LIST1`, flushing a pending output byte with EOI first, then releases ATN through `SCATN`. |
| `$F18C` | `$F1C0` | `ACPTR`, physical byte input | Both start `A9 34 8D 21 E8 AD 40 E8`. Called by LOAD, `BASIN`, and ROM 4 `EREAD`. | Asserts NDAC, releases NRFD, waits for DAV, asserts NRFD, samples EOI and complemented DIO, releases NDAC, waits for DAV release, then re-arms NDAC while leaving NRFD asserted. This is the focused controller-input handshake. |
| `$F146` | `$F175` | `ER001`, receive-state setup/recovery | ROM 2/3 starts `AD 40 E8 29 FD 8D 40 E8`. ROM 4 has the same bytes. Both `TKATN` implementations call it with `JSR`. | Asserts NRFD and NDAC and returns CR in A. Does not release ATN. A valid prepared-state helper, also entered by receive-timeout handling. |

The transport routines merge status into `SATUS`/ST at `$96`: `$01` is an
output/listener timeout, `$02` is an input timeout, `$40` is EOI, and `$80` is
device not present. VERIFY sets `SATUS` to `$10` on a compare failure (it does not OR that bit into the previous status).

#### Callable KERNAL channel and file entries

These entries intentionally invoke the transport routines. They are valid
integrated tests, but they include file-table, parser, screen, cassette, and
error-message behavior that is unrelated to the electrical handshake.

| ROM 2/3 address | ROM 4 address | Source label or role | Entry evidence | Behavior and test relevance |
|-----------------|---------------|----------------------|----------------|-----------------------------|
| `$F466` | `$F4A5` | `OPENI`, IEEE OPEN sender | ROM 2/3 starts `A5 D3 30 F5 A4 D1 F0 F1`. ROM 4 starts `A5 D3 30 F5 A4 D1 F0 F1`. Called by generic OPEN, LOAD, and SAVE. | With `FA`, `SA`, `FNLEN`, and `FNADR` prepared, sends MLA, `$F0 OR SA`, the filename through `CIOUT`, and UNLISTEN. This is the generic end-to-end OPEN transaction. |
| `$F2AE` | `$F2E2` | `CLOS5`/`FCLOSE`, close logical file in A | ROM 2/3 starts `20 8D F2 D0 4D 20 99 F2`. ROM 4 starts `20 C1 F2 D0 4D 20 CD F2`. Called by generic CLOSE. ROM 4 `DCLOSE` also jumps here. | Looks up the logical file, calls `CLSE1` for IEEE devices, and removes the table entry. |
| `$F6F0` | `$F72F` | `CLSE1`, IEEE CLOSE sender | ROM 2/3 starts `24 D3 30 78 20 BA F0 A5`. ROM 4 starts `24 D3 30 78 20 D5 F0 A5`. Called by CLOSE and after LOAD/SAVE. | If SA is nonnegative, sends MLA, `$E0 OR (SA AND $0F)`, and UNLISTEN. A negative SA deliberately suppresses the close command. |
| `$F322` | `$F356` | `LD15`, LOAD/VERIFY device dispatcher | ROM 2/3 starts `A5 D4 D0 03 4C 03 CE C9`. ROM 4 starts `A5 D4 D0 03 4C 00 BF C9`. Called by LOAD and the resident monitor. | For IEEE devices, sends OPEN, TALK, the secondary address, reads the two-byte load address and data through `ACPTR`, UNTALKs, and closes. ROM 2/3 uses `SECND` after TALK. ROM 4 uses `TKSA` and checks status before the second address byte. |
| `$F3C9` | `$F408` | `LOADNP`, load with prepared parameters | ROM 2/3 starts `20 8D F6 A9 FF C5 9B D0`. ROM 4 starts `20 CC F6 A9 FF C5 9B D0`. ROM 4 `DLOAD` jumps here. | Copies BASIC bounds, waits for the keyboard switch state, and calls `LD15`. Use this only when the LOAD parameters have already been prepared. |
| `$F6A1` | `$F6E0` | `SV3`, save with prepared parameters | ROM 2/3 starts `20 8D F6 A5 D4 D0 05 A0`. ROM 4 starts `20 CC F6 A5 D4 D0 05 A0`. ROM 4 `DSAVE` jumps here. | Copies BASIC bounds and falls into `SV5`. It is an integrated prepared-state entry, not an IEEE primitive. |
| `$F6A4` | `$F6E3` | `SV5`, SAVE device dispatcher | ROM 2/3 starts `A5 D4 D0 05 A0 74 4C 70`. ROM 4 starts `A5 D4 D0 05 A0 74 4C AF`. Called by SAVE and the resident monitor. | For IEEE devices, sends OPEN, MLA, secondary `$61`, the start address and data through `CIOUT`, then UNLISTENs and closes. |
| `$F770` | `$F7AF` | `CHKIN` | Both start `48 8A 48 98 48 A9 00 85`. Public vector `$FFC6`. | Selects logical file X. For IEEE input it sends TALK and the stored secondary address, then makes the device the default input. ROM 4 first invalidates cached `DS$`. |
| `$F7BC` | `$F7FE` | `CHKOUT`/`CKOUT` | Both start `48 8A 48 98 48 A9 00 85`. Public vector `$FFC9`. | Selects logical file X. For IEEE output it sends LISTEN and the stored secondary address, then makes the device the default output. ROM 4 first invalidates cached `DS$`. |
| `$F272` | `$F2A6` | `CLRCHN`/`CLRCH` | ROM 2/3 starts `A5 B0 C9 04 90 03 20 83`. ROM 4 starts `A5 B0 C9 04 90 03 20 B9`. Public vector `$FFCC`. | Sends UNLISTEN for an active IEEE output and UNTALK for an active IEEE input, then restores screen and keyboard defaults. |
| `-` | `$F563` | `OP94`, prepared generic OPEN | ROM 4 starts `A5 D2 F0 F6 A0 0E 20 C1`. Called by `DOPEN` and `APPEND`. | Validates prepared `LA`, `FA`, `SA`, `FNLEN`, and `FNADR`, installs the file-table entry, then reaches `OPENI` for IEEE. ROM 2/3 has equivalent inline code but no archived caller or source entry at its initial `LDA LA`. |

#### ROM 4 disk-statement and status entries

ROM 4's disk commands are not implemented in `901465-22`. The KERNAL adds
`JMP` trampolines at `$FF93-$FFBD`, but their implementations are in
`901465-21` at `$D000-$DFFF`. ROM 2/3 uses `$FF93-$FFBD` for resident-monitor
code and has none of these entries. The addresses below name the implementation
first and the public trampoline second.

| ROM 2/3 address | ROM 4 address | Source label or BASIC statement | Entry evidence | Command behavior and test relevance |
|-----------------|---------------|---------------------------------|----------------|-------------------------------------|
| `-` | `$DAC7` via `$FF93` | `CONCAT` | Starts `20 68 DC 20 1D D8 A0 22`. `$FF93` is `JMP $DAC7`. | Parses and sends `C<dest-drive>:<dest>=<dest-drive>:<dest>,<source-drive>:<source>` on command channel 15 through `TRANS`. |
| `-` | `$D942` via `$FF96` | `DOPEN` | Starts `20 68 DC 20 2E D8 29 22`. `$FF96` is `JMP $D942`. | Parses `DOPEN`, allocates a secondary address, builds the drive/name/type/mode/record-length string, and enters `OP94`. |
| `-` | `$DA07` via `$FF99` | `DCLOSE` | Starts `20 68 DC 29 F3 F0 03 4C`. `$FF99` is `JMP $DA07`. | Clears cached disk status and closes one logical file through `CLOS5`, or every open file matching the selected primary device address `FA` through `DCLALL` (both disk drive numbers). |
| `-` | `$D7AF` via `$FF9C` | `RECORD` | Starts `A9 01 8D 3A 03 20 76 00`. `$FF9C` is `JMP $D7AF`. | Parses channel, record number, and optional position, then `BOBREC` sends binary `P`, channel, record low/high, and position on command channel 15. |
| `-` | `$D9D2` via `$FF9F` | `FORMAT`/`HEADER` | Starts `20 68 DC 20 04 D8 29 11`. `$FF9F` is `JMP $D9D2`. | Validates name and optional disk ID, closes files on the drive, obtains confirmation in direct mode, and sends `N<drive>:<name>[,<id>]`. |
| `-` | `$DA65` via `$FFA2` | `COLECT`, BASIC `COLLECT` | Starts `20 68 DC 20 18 D8 20 1B`. `$FFA2` is `JMP $DA65`. | Closes files on the drive and sends `V[<drive>]` on command channel 15. |
| `-` | `$DA7E` via `$FFA5` | `BACKUP` | Starts `20 68 DC 29 30 C9 30 F0`. `$FFA5` is `JMP $DA7E`. | Closes affected files and sends `D<destination-drive>=<source-drive>`. |
| `-` | `$DAA7` via `$FFA8` | `COPY` | Starts `20 68 DC 29 30 C9 30 D0`. `$FFA8` is `JMP $DAA7`. | Sends the parsed `C<destination-drive>:<destination>=<source-drive>:<source>` command through `TRANS`. |
| `-` | `$D977` via `$FFAB` | `APPEND` | Starts `20 68 DC 20 2E D8 29 E2`. `$FFAB` is `JMP $D977`. | Allocates a secondary address, builds `<drive>:<name>,A`, and enters prepared generic OPEN through `OP94`. |
| `-` | `$DB0D` via `$FFAE` | `DSAVE` | Starts `20 68 DC 20 0B D8 29 66`. `$FFAE` is `JMP $DB0D`. | Builds the disk filename, optionally prefixes `@`, then enters `SV3` to perform the normal KERNAL SAVE transaction. |
| `-` | `$DB3A` via `$FFB1` | `DLOAD` | Starts `20 68 DC 20 0B D8 29 E6`. `$FFB1` is `JMP $DB3A`. | Builds the disk filename, clears VERIFY mode, and enters `LOADNP` to perform the normal KERNAL LOAD transaction. |
| `-` | `$D873` via `$FFB4` | `CATLOG`, BASIC `DIRECTORY`/`CATALOG` | Starts `A5 D2 85 B3 20 68 DC 20`. `$FFB4` is `JMP $D873`. | Opens `$` or `$<drive>` as logical file 14, reads and formats the directory stream, then closes it. This exercises OPEN, TALK, repeated `ACPTR`, and CLOSE. |
| `-` | `$DB55` via `$FFB7` | `RENAME` | Starts `20 68 DC 20 24 D8 29 E4`. `$FFB7` is `JMP $DB55`. | Sends `R<drive>:<new>=<drive>:<old>` through command channel 15. |
| `-` | `$DB66` via `$FFBA` | `SCRTCH`, BASIC `SCRATCH` | Starts `20 68 DC 20 04 D8 20 9E`. `$FFBA` is `JMP $DB66`. | Obtains confirmation in direct mode, sends `S<drive>:<name>`, then reads and optionally prints the disk status. |
| `-` | `$D991` | `ERRCHL`, user status entry | Starts `A5 0D D0 16 A9 28 85 0D`. Source marks it `ENTRY FOR USER`. | Reuses an allocated `DS$` buffer or falls into `GETDS` to allocate one, then performs the same status-channel read. |
| `-` | `$D995` via `$FFBD` | `GETDS`, BASIC `DS$`/`READDS` | Starts `A9 28 85 0D 20 1D C6 86`. `$FFBD` is `JMP $D995`. | Allocates the 40-byte `DS$` buffer, defaults `FA` to 8, TALKs on secondary `$6F`, reads through CR with `ACPTR`, and UNTALKs. This is the ROM 4 integrated status-channel entry. |

#### Wrappers and helpers without direct IEEE register access

The generic I/O wrappers can cause bus traffic through the entries above. The
parsers and state helpers do not establish any gateware or drive-firmware
behavior by themselves.

| ROM 2/3 address | ROM 4 address | Source label or role | Entry evidence | Relevance |
|-----------------|---------------|----------------------|----------------|-----------|
| `$F521` via `$FFC0` | `$F560` via `$FFC0` | `OPEN`, BASIC OPEN parser | ROM 2/3 starts `20 CE F4 A5 D2 F0 F6 A0`. ROM 4 starts `20 0D F5 A5 D2 F0 F6 A0`. | Parses BASIC text, installs a file-table entry, and calls `OPENI` only for IEEE devices. Use `OPENI` or `OP94` for a prepared-state transaction. |
| `$F2A9` via `$FFC3` | `$F2DD` via `$FFC3` | `CLOSE`, BASIC CLOSE parser | ROM 2/3 starts `20 CE F4 A5 D2 20 8D F2`. ROM 4 starts `20 0D F5 A5 D2 20 C1 F2`. | Parses BASIC text, then enters `CLOS5`. |
| `$F3C2` via `$FFD5` | `$F401` via `$FFD5` | `LOAD`, BASIC LOAD/VERIFY front end | ROM 2/3 starts `A9 00 85 9D 20 3E F4`. ROM 4 starts `A9 00 85 9D 20 7D F4`. | Parses BASIC LOAD arguments, waits for key-switch stability, and calls `LD15`. |
| `$F69E` via `$FFD8` | `$F6DD` via `$FFD8` | `SAVE`, BASIC SAVE front end | ROM 2/3 starts `20 3E F4 20 8D F6 A5 D4`. ROM 4 starts `20 7D F4 20 CC F6 A5 D4`. | Parses BASIC SAVE arguments, copies BASIC bounds, and enters `SV5`. |
| `$F4B7` via `$FFDB` | `$F4F6` via `$FFDB` | `VER`, BASIC VERIFY front end | ROM 2/3 starts `A9 01 85 9D 20 C6 F3 A5`. ROM 4 starts `A9 01 85 9D 20 05 F4 A5`. | Sets `VERCK`, reuses the LOAD path, and compares received bytes instead of storing them. It sets SATUS to `$10` on a mismatch. |
| `$F1D1` via `$FFE4` | `$F205` via `$FFE4` | `GETIN` | Both start `A9 00 85 96 A5 AF D0 17`. | Dispatches keyboard, tape, or IEEE input. The IEEE branch jumps to `ACPTR`, but GETIN adds current-device policy. |
| `$F1E1` via `$FFCF` | `$F215` via `$FFCF` | `BASIN`/`CHRIN` | Both start `A5 AF D0 0B A5 C6 85 A4`. | Dispatches screen, tape, or IEEE input. Use `ACPTR` for an electrical receive test. |
| `$F232` via `$FFD2` | `$F266` via `$FFD2` | `BSOUT`/`CHROUT` | Both start `48 A5 B0 C9 03 D0 04 68`. | Dispatches screen, tape, or IEEE output. The IEEE branch jumps to buffered `CIOUT`. |
| `$F26E` via `$FFE7` | `$F2A2` via `$FFE7` | `CLALL` | ROM 2/3 starts `A9 00 85 AE A5 B0 C9 04`. ROM 4 starts `A9 00 85 AE A5 B0 C9 04`. | Clears the open-file count and falls into `CLRCHN`. It aborts file bookkeeping rather than issuing CLOSE commands for each file. |
| `$F43E` | `$F47D` | `PARS1` | Both start `A2 00 86 96 86 D1 86 D3`. | Parses LOAD/SAVE arguments only. It performs no IEEE access. |
| `$F4CE` | `$F50D` | `PARS2` | Both start `A2 00 86 D3 86 96 86 D1`. | Parses OPEN/CLOSE arguments only. It performs no IEEE access. |
| `$F28D` | `$F2C1` | `JLTLK`, file-table lookup | Both start `A6 AE CA 30 16 DD 51 02`. | Finds logical file A in LAT, returning index X and comparison flags. No bus access. |
| `$F299` | `$F2CD` | `JZ100`, load file parameters | Both start `BD 51 02 85 D2 BD 5B 02`. | Loads LA, FA and SA from file-table index X. No bus access. |
| `$FB7F` | `$FBC4` | `UDST`, status-bit merge | Both start `05 96 85 96 60`. | ORs A into `SATUS` and returns. It records EOI and timeout results but does not perform a handshake. |
| `-` | `$DC68` | `DOSPAR`, ROM 4 disk parser | Starts `A2 00 8E 3E 03 86 D2 8E`. | Parses disk-statement parameters into `LA`, `FA`, drive, filename, record, and option fields. No bus access occurs. |
| `-` | `$DA1B` | `DCLALL`, close all files on a device | Starts `A5 D4 A6 AE CA 30 0E DD`. | Walks the file table and invokes `CLOS10` for matching `FA` values, irrespective of disk drive number. It adds no electrical behavior. |
| `-` | `$DBFA` | `SENDP`, ROM 4 command builder | Starts `A2 00 8D 41 03 20 E1 DB`. | Expands a `TABLD` template into `TBUFF` and sets `FNLEN`/`FNADR`. It does not transmit the command. |
| `-` | `$DBE1` | `OLDCLR`, ROM 4 status-cache reset | Starts `98 48 A5 0D F0 0A A0 28`. | Invalidates the allocated `DS$` string and clears `SATUS`. It performs no bus access. |

The shared `$FFC0-$FFEA` entries are three-byte `JMP` trampolines to generic
OPEN, CLOSE, CHKIN, CHKOUT, CLRCHN, CHRIN, CHROUT, LOAD, SAVE, VERIFY, SYS,
STOP, GETIN, CLALL, and UDTIM implementations. The ROM 4-only `$FF93-$FFBD`
disk vectors are listed above. Do not treat ROM 2/3 bytes at `$FF93-$FFBD` as
those vectors. That range contains resident-monitor code in `901465-03`.
Unlike later Commodore KERNALs, these PET OPEN, CLOSE, LOAD, SAVE, and VERIFY
targets parse the current BASIC text stream. They are not `SETNAM`/`SETLFS`
register APIs.

#### Shared PET public I/O vectors

These vectors are callable ABI entries distinct from their implementation
addresses. The ROM 4 disk-only vectors are paired with their targets above.
SYS (`$FFDE`) and UDTIM (`$FFEA`) are generic execution/time services, not file
or IEEE entries, and are recorded as exclusions in the audit ledger.

| ROM 2/3 address | ROM 4 address | Source role | Exact bytes | Target and calling convention |
|-----------------|---------------|-------------|-------------|-------------------------------|
| `$FFC0` | `$FFC0` | `OPEN` public vector | ROM 2: `4C 21 F5`. ROM 4: `4C 60 F5`. | JMP to ROM 2/3 `$F521` or ROM 4 `$F560`. Uses the implementation calling convention above. |
| `$FFC3` | `$FFC3` | `CLOSE` public vector | ROM 2: `4C A9 F2`. ROM 4: `4C DD F2`. | JMP to ROM 2/3 `$F2A9` or ROM 4 `$F2DD`. Uses the implementation calling convention above. |
| `$FFC6` | `$FFC6` | `CHKIN` public vector | ROM 2: `4C 70 F7`. ROM 4: `4C AF F7`. | JMP to ROM 2/3 `$F770` or ROM 4 `$F7AF`. Uses the implementation calling convention above. |
| `$FFC9` | `$FFC9` | `CHKOUT` public vector | ROM 2: `4C BC F7`. ROM 4: `4C FE F7`. | JMP to ROM 2/3 `$F7BC` or ROM 4 `$F7FE`. Uses the implementation calling convention above. |
| `$FFCC` | `$FFCC` | `CLRCHN` public vector | ROM 2: `4C 72 F2`. ROM 4: `4C A6 F2`. | JMP to ROM 2/3 `$F272` or ROM 4 `$F2A6`. Uses the implementation calling convention above. |
| `$FFCF` | `$FFCF` | `CHRIN` public vector | ROM 2: `4C E1 F1`. ROM 4: `4C 15 F2`. | JMP to ROM 2/3 `$F1E1` or ROM 4 `$F215`. Uses the implementation calling convention above. |
| `$FFD2` | `$FFD2` | `CHROUT` public vector | ROM 2: `4C 32 F2`. ROM 4: `4C 66 F2`. | JMP to ROM 2/3 `$F232` or ROM 4 `$F266`. Uses the implementation calling convention above. |
| `$FFD5` | `$FFD5` | `LOAD` public vector | ROM 2: `4C C2 F3`. ROM 4: `4C 01 F4`. | JMP to ROM 2/3 `$F3C2` or ROM 4 `$F401`. Uses the implementation calling convention above. |
| `$FFD8` | `$FFD8` | `SAVE` public vector | ROM 2: `4C 9E F6`. ROM 4: `4C DD F6`. | JMP to ROM 2/3 `$F69E` or ROM 4 `$F6DD`. Uses the implementation calling convention above. |
| `$FFDB` | `$FFDB` | `VERIFY` public vector | ROM 2: `4C B7 F4`. ROM 4: `4C F6 F4`. | JMP to ROM 2/3 `$F4B7` or ROM 4 `$F4F6`. Uses the implementation calling convention above. |
| `$FFE1` | `$FFE1` | `STOP` public vector | ROM 2: `4C 0F F3`. ROM 4: `4C 43 F3`. | JMP to ROM 2/3 `$F30F` or ROM 4 `$F343`. Uses the implementation calling convention above. |
| `$FFE4` | `$FFE4` | `GETIN` public vector | ROM 2: `4C D1 F1`. ROM 4: `4C 05 F2`. | JMP to ROM 2/3 `$F1D1` or ROM 4 `$F205`. Uses the implementation calling convention above. |
| `$FFE7` | `$FFE7` | `CLALL` public vector | ROM 2: `4C 6E F2`. ROM 4: `4C A2 F2`. | JMP to ROM 2/3 `$F26E` or ROM 4 `$F2A2`. Uses the implementation calling convention above. |

#### Internal continuations and prepared-state entries

A prepared-state entry is callable when its documented register, flag, memory,
and bus prerequisites hold. It is not the same as a stack-dependent continuation.
The following first group contains internal phases. The second group contains
legitimate alternate entries that must remain in the callable inventory.

| ROM 2/3 address | ROM 4 address | Label | Why it is not a standalone entry |
|-----------------|---------------|-------|----------------------------------|
| `$F0DF` | `$F0FA` | `LIST2` | It begins with `PLA` to recover the byte pushed by `LIST1`. A direct JSR consumes the caller's return address as command data and corrupts the stack. |
| `$F0E4` | `$F0FF` | `LIST4` | It assumes `LIST1` already released the receive lines and placed the addressed command in `BSOUR`. A direct call can send stale data under an invalid line state. |
| `$F103/$F10D/$F112/$F11D` | `$F11E/$F128/$F12D/$F138` | `ISR1`/`ISR0`/`ISR2`/`ISR3` | These are wait, timeout, and cleanup phases inside `ISOUR`. They require DIO, DAV, the timer, and saved state established by the prologue. |
| `$F177/$F17C` | `$F1A6/$F1AB` | `CI2`/`CI4` | These are the send-previous-byte and buffer-current-byte branches inside `CIOUT`. `CI2` relies on CIOUT's A value and `CI4` bypasses the required prior-byte decision. |
| `$F199/$F19E/$F1BA/$F1C5` | `$F1CD/$F1D2/$F1EE/$F1F9` | `ACP00`/`ACP01`/`ACP03`/`ACP05` | These are timer, wait, sample, and DAV-release phases inside `ACPTR`. They assume the input handshake was initialized at the real entry. |
| `$F7A1/$F7AE/$F7B1` | `$F7E3/$F7F0/$F7F3` | `JX3301`/`JX340`/`JX350` | These are mid-CHKIN addressing and status labels. They assume CHKIN saved the selected device on the stack. Their common tail executes `PLA`, so a direct JSR corrupts the return address. |
| `-` | `$F831` | `JX3701` | ROM 4 reaches this after CHKOUT saves the selected device and clears `DS$`. Direct entry skips that setup and later consumes an unsaved stack byte. ROM 2/3 has no separate label before its LISTEN call. |
| `$F7F8/$F7FB` | `$F83D/$F840` | `JX380`/`JX390` | These are mid-CHKOUT secondary-address and status labels. They assume CHKOUT's saved registers and device byte. The common tail executes `PLA`. |

Prepared-state entries (the prerequisites are part of the calling contract):

| ROM 2/3 address | ROM 4 address | Label | Required state and use |
|-----------------|---------------|-------|------------------------|
| `$F2B3` | `$F2E7` | `CLOS10` | Callable close-and-remove entry, explicitly called by ROM 4 `DCLALL`. X must identify an existing logical-file table entry. It calls `JZ100`, saves X, performs device close, then removes the table entry. Use `CLOS5` when starting with a logical file number in A. |
| `$F526` | `$F565` | `FOPEN` | The label is a `BEQ` that depends on flags set by the preceding `LDA LA` or, for ROM 4 CATALOG, by a prior call. ROM 4's safe prepared-parameter entry is `OP94` at `$F563`. ROM 2/3 has no source-labelled equivalent. |
| `$F475` | `$F4B4` | `OPENIB` | It starts by sending a secondary command. It assumes the device is already LISTEN-addressed and that `FNLEN`/`FNADR` describe the message. ROM 4 `TRANS1` uses it, but it is not a complete OPEN entry. |
| `-` | `$D9B3` | `EREAD` | It is the bus-reading tail of `GETDS`. It assumes a valid `DS$` descriptor and buffer pointer. Use `$D995`/`$FFBD` for the complete status operation. |
| `-` | `$DA31` | `BOBREC` | It is the command-building tail reached after `RECORD` parsing. It assumes prepared logical-file and record fields before jumping to `TRANS1`. |
| `-` | `$DA98` | `TRANS` | It expects a command-template offset in Y and a template length in A, then calls `SENDP` and falls into `TRANS1`. It is a callable command sender with those explicit parameters. |
| `-` | `$DA9B` | `TRANS1` | It sends a prepared `FNLEN`/`FNADR` buffer on command channel 15 through `OPENIB`. It does not parse or build a command. |

#### Additional called and prepared-state PET helpers

These are part of the file/disk call graph, including parser and error helpers.
They require the calling state described below. They are not all electrical
primitives, and a routine that deliberately unwinds a parser or error stack
must not be used as an ordinary leaf call. Source labels, instruction bytes,
call sites, and excluded continuations are retained in the
[audit ledger](../../docs/dev/PET/ieee-rom-entry-audit.json).

| ROM 2/3 address | ROM 4 address | Source label | Entry bytes | Required state and relevance |
|-----------------|---------------|--------------|-------------|------------------------------|
| `$F301` | `$F335` | `STOP1` | ROM 2: `A5 9B C9 EF D0 07`. ROM 4: `A5 9B C9 EF D0 07`. | Tests the STOP key and clears active channels on STOP. Changes keyboard state. Required by LOAD/SAVE, directory and timeout paths. |
| `$F30F` | `$F343` | `STOP` | ROM 2: `20 01 F3 4C 3F C7`. ROM 4: `20 35 F3 4C C6 B7`. | Calls STOP1, then jumps to BASIC stop handling. Public vector `$FFE1`. May unwind into BASIC rather than return normally. |
| `$F3C6` | `$F405` | `LD10` | ROM 2: `20 3E F4 20 8D F6`. ROM 4: `20 7D F4 20 CC F6`. | Alternate LOAD parser used by VERIFY. Requires VERCK already selected and a valid BASIC text pointer. Falls through LOADNP. |
| `$F68D` | `$F6CC` | `SV60` | ROM 2: `A5 2A 85 C9 A5 2B`. ROM 4: `A5 2A 85 C9 A5 2B`. | Copies BASIC start/end bounds to transfer pointers. No bus access. |
| `$FB76` | `$FBBB` | `RD300` | ROM 2: `A5 FC 85 C8 A5 FB`. ROM 4: `A5 FC 85 C8 A5 FB`. | Copies STAL/STAH to SAL/SAH for SAVE. Despite its location in tape source, it is also a called IEEE SAVE helper. |
| `$FCC6` | `$FD0B` | `WRT62` | ROM 2: `A5 C8 C5 CA D0 04`. ROM 4: `A5 C8 C5 CA D0 04`. | Compares SAL/SAH with EAL/EAH for SAVE completion. Requires valid pointers. No bus access. |
| `$F460` | `$F49F` | `PR070` | ROM 2: `20 16 F5 4C 78 D6`. ROM 4: `20 55 F5 4C D4 C8`. | Parses a comma and byte argument through PR150 and GETBYT. Requires BASIC parser state. |
| `$F519` | `$F558` | `PR130` | ROM 2: `20 76 00 D0 F7 4C`. ROM 4: `20 76 00 D0 F7 4C`. | Checks that an argument follows at the current BASIC text pointer. Missing arguments raise a BASIC syntax error. |
| `$F50E` | `$F54D` | `PR140` | ROM 2: `20 76 00 D0 02 68`. ROM 4: `20 76 00 D0 02 68`. | Optional-argument probe. On end-of-statement it discards its own return address, returning from the enclosing parser. Requires that enclosing parser frame. |
| `$F516` | `$F555` | `PR150` | ROM 2: `20 F8 CD 20 76 00`. ROM 4: `20 F5 BE 20 76 00`. | Consumes a required comma then enters PR130. Requires BASIC parser state. |
| `-` | `$BFC1` | `ISVDS` | ROM 4: `C9 44 D0 0E C0 D3`. | Prepared BASIC string-variable dispatch with A/Y holding the variable identity. Recognizes DS$, calls CHKDS, and returns its cached descriptor through STRLIT; other names return through STRRTS. |
| `-` | `$BFFC` | `CHKDS` | ROM 4: `A5 0D D0 D3 4C BD`. | Callable DS/DS$ cache guard. Returns if DSDESC is populated; otherwise jumps through READDS at $FFBD. This routine crosses the B-ROM/C-ROM boundary at $C000. |
| `-` | `$C01C` | `QDSAV` | ROM 4: `C9 44 D0 20 C0 53`. | Prepared BASIC numeric-variable dispatch with A/Y holding the variable identity. Recognizes DS, calls CHKDS, and converts the first two status-string digits to a BASIC number. Other variables continue through GOMOVF. |
| `$F4FD` | `$F53C` | `PR200` | ROM 2: `20 9F CC 20 7D D5`. ROM 4: `20 98 BD 20 B5 C7`. | Evaluates a filename string and sets FNLEN/FNADR. Requires BASIC expression and string workspace. |
| `$F156` | `$F185` | `MSG` | ROM 2: `B9 00 F0 08 29 7F`. ROM 4: `B9 00 F0 08 29 7F`. | Prints a high-bit-terminated ROM message starting at offset Y through editor PRT. Presentation helper, no IEEE transport. |
| `$F315` | `$F349` | `SPMSG` | ROM 2: `20 1D F3 D0 F4 4C`. ROM 4: `20 51 F3 D0 F4 4C`. | Prints MSG only in direct mode. Requires Y to name a message offset. |
| `$F31D` | `$F351` | `TXTST` | ROM 2: `A5 78 C9 02 60 A5`. ROM 4: `A5 78 C9 02 60 A5`. | Tests the BASIC text pointer for direct mode. No bus access. |
| `$F40A` | `$F449` | `LD300` | ROM 2: `20 1D F3 D0 1E A0`. ROM 4: `20 51 F3 D0 1E A0`. | Prints SEARCHING and the prepared filename when appropriate. Uses MSG and LD105. |
| `$F41D` | `$F45C` | `LD105` | ROM 2: `A4 D1 F0 0C A0 00`. ROM 4: `A4 D1 F0 0C A0 00`. | Prints FNLEN bytes from FNADR through current output. That output can itself be IEEE, so this is not necessarily screen-only. |
| `$F42E` | `$F46D` | `LD400` | ROM 2: `A0 5F A5 9D F0 02`. ROM 4: `A0 5F A5 9D F0 02`. | Selects LOADING or VERIFYING messages using VERCK and SPMSG. |
| `$F570` | `$F5AF` | `ERMSG` | ROM 2: `20 6E F2 A9 0D 20`. ROM 4: `20 A2 F2 A9 0D 20`. | Closes active channels through CLALL, prints the selected error, and jumps to BASIC error handling. Does not return normally. |
| `-` | `$D92F` | `ENTRY0` | ROM 4: `A0 61 C8 98 A6 AE`. | Allocates an unused secondary address starting at `$62`, comparing SAT entries. Does not send OPEN. |
| `-` | `$D931` | `ENTRY1` | ROM 4: `C8 98 A6 AE CA 30`. | Alternate allocator entry with initial secondary candidate in Y (incremented before testing). Called through ENTRY0 fall-through. |
| `-` | `$DBFC` | `SENDP1` | ROM 4: `8D 41 03 20 E1 DB`. | Command-template builder with initial output offset X, template offset Y, and length A. Preserves an existing prefix such as `@`. |
| `-` | `$DC4C` | `TRANR` | ROM 4: `86 D1 A9 53 85 DA`. | Sets FNLEN from X and FNADR to TBUFF. Used after both template expansion and binary REL-position construction. |
| `-` | `$DAD4` | `RSFN` | ROM 4: `A5 D1 8D 3A 03 A5`. | Copies filename-1 descriptor to filename-2 scratch fields, then falls into RDFN. Requires buffer index X and template index Y. |
| `-` | `$DAE1` | `RDFN` | ROM 4: `98 48 AC 3A 03 F0`. | Appends filename-2 to TBUFF at X, preserving Y. Used by SENDP. No bus access. |
| `-` | `$DAFD` | `RID` | ROM 4: `AD 3F 03 9D 53 03`. | Appends two disk-ID bytes at TBUFF,X and advances X. No bus access. |
| `-` | `$DC57` | `RWRT` | ROM 4: `AD 3D 03 F0 04 A9`. | Returns `L` for a REL record length or `W` while setting default type `S`. Used by template expansion. |
| `-` | `$D804` | `CHK1` | ROM 4: `29 E6 F0 03 4C 00`. | Checks HEADER/SCRATCH option mask, then CHK2 required filename. Requires parsed PARCHK and A. |
| `-` | `$D80B` | `CHK2` | ROM 4: `AD 3E 03 29 01 C9`. | Requires filename-1 in PARCHK and returns PARCHK in A. |
| `-` | `$D818` | `CHK3` | ROM 4: `29 E7 D0 EC 60 29`. | Rejects invalid CATALOG/COLLECT options in A. |
| `-` | `$D81D` | `CHK4` | ROM 4: `29 C4 D0 E7 AD 3E`. | Checks COPY/CONCAT options, then CHK5 required filenames. |
| `-` | `$D824` | `CHK5` | ROM 4: `29 03 C9 03 D0 DE`. | Requires both filenames in PARCHK and returns PARCHK in A. |
| `-` | `$D82E` | `CHK6` | ROM 4: `29 05 C9 05 D0 D4`. | Requires logical file and filename for DOPEN/APPEND. |
| `-` | `$DE2C` | `ON` | ROM 4: `20 70 00 C9 55 D0`. | Consumes the ON clause and requires U, then enters UNIT. |
| `-` | `$DE33` | `UNIT` | ROM 4: `20 87 DE E0 20 B0`. | Parses a unit in the ROM-accepted range 3 through 31, stores FA, and sets its PARCHK bit. |
| `-` | `$DE49` | `NEWNAM` | ROM 4: `D0 D5 20 98 BD 20`. | Filename-expression helper. Requires Z set by caller option check, valid BASIC string workspace, and PARCHK. Returns filename length and pointer. |
| `-` | `$DE87` | `GETVAL` | ROM 4: `20 70 00 D0 03 4C`. | Advances CHRGET then enters GTVL2 to parse a byte value. |
| `-` | `$DE8A` | `GTVL2` | ROM 4: `D0 03 4C 00 BF 90`. | Alternate numeric parser entry. Requires A and condition flags from CHRGET/CHRGOT. Handles an expression in parentheses. |
| `-` | `$DB99` | `DDIREC` | ROM 4: `A5 78 C9 02 60 20`. | Tests direct/program mode from TXTPTR. No bus access. |
| `-` | `$DB9E` | `RUSURE` | ROM 4: `20 99 DB D0 32 A0`. | Confirmation helper for HEADER/SCRATCH. Uses CLRCH and BASIN, returns carry set on rejection in direct mode. |
| `-` | `$DBD7` | `BADDIS` | ROM 4: `20 99 DB D0 FA A0`. | Prints BAD DISK only in direct mode, otherwise returns. Presentation helper. |
| `-` | `$DE9E` | `PATCH5` | ROM 4: `85 96 91 0E 88 60`. | GETDS helper at `$DE9E`, after checksum data `$DE9D`. Clears SATUS, stores the high backlink byte through DSDESC, and decrements Y. |
| `-` | `$D9F8` | `FERRS` | ROM 4: `20 91 D9 A0 00 B1`. | Prepared status-check tail of HEADER. Reads ERRCHL and treats first status digit >= `2` as BAD DISK. |
| `-` | `$DB78` | `NUMSCR` | ROM 4: `20 99 DB D0 1B 20`. | Prepared SCRATCH completion entry. Reads and prints disk status only in direct mode. |
| `-` | `$D91A` | `SUBA` | ROM 4: `20 23 D9 20 66 F2`. | Directory-output helper: selects saved output through SUBB, writes A through BSOUT, then CLRCHN. Requires CATALOG state. |
| `-` | `$D923` | `SUBB` | ROM 4: `A6 BA E0 03 F0 05`. | Restores CATALOG output selection using CNTDN/WSW, possibly calling CHKOUT. Requires saved CATALOG state. |
| `-` | `$D87D` | `CATALG` | ROM 4: `A0 00 A2 01 AD 3E`. | Prepared CATALOG entry after parsing and option checks. Requires PARCHK, FA, DRIVE1 and saved WSW. Builds and reads the directory. |

IEEE initialization is editor-resident, not KERNAL-resident. The ROM 2/3
source places `CINT` at `$E1DE`. ROM 4 non-CRTC editor `901447-29` starts its
implementation at `$E000`. The
[4032 CRTC editor](https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-4-40-n-60hz-901499-01.dis.txt)
uses `$E000` as a jump to `$E036`, and the cited 8032 editor uses `$E000` as a
jump to `$E04B`. Use the public `$E000` editor entry for ROM 4 startup tests and
load the matching editor ROM. Do not invent one fixed ROM 4 `CINT`
implementation address.

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

### Revision-12 IEEE routine map

The archived `OPSYSDIS*` and `MAP*` files on TPUG disks `(s)t8.d64`, `(s)t9.d64`,
and `(s)ta.d64` are an annotated 1983 navigation aid, not a revision authority.
`NOTE_FROM_JOHN` says that a disassembler bug may have produced bad code and only
comments identify sections the author considered reliable. `NOTE_FROM_DICK`
identifies the files as John A. Toebes VIII's paged operating-system disassembly
and partial map, but neither note identifies the ROM revision. Do not use their
labels or addresses without checking the pinned binaries.

The tables below were cross-checked against the revision-12 images whose MD5
values are listed above. An address is a callable entry only when the binary
starts on the archived instruction boundary and the archived disassembly shows
a caller or a complete leaf routine with its own return. Names present in `OPSYSDIS*` but absent from `MAP*` (for example `ByteOut_`,
`sysrdbyt`, and `openf`) are archived disassembly labels, not invented roles.
Neither archive is original Waterloo source. Inferred descriptions are marked
as roles in the tables.

#### Callable bus and disk-command entries

These entries either access the IEEE registers directly or deliberately invoke
the lower-level routines to produce an IEEE transaction. Use the electrical
entries for focused gateware tests and the disk-command entries for integrated
gateware and firmware tests.

| Address | Archived label or role | Entry evidence | Revision-12 behavior and test relevance |
|---------|------------------------|----------------|-----------------------------------------|
| `$BCD0` | `addrbcd0`, generic OPEN sender | Starts `34 06 32 7E CC 00 14`; called by `ieeopen` and `sysdirop`. | Addresses the device as listener with OPEN secondary offset `$F0`, sends the filename, and terminates it with EOI and UNLISTEN. This is the normal end-to-end disk OPEN entry. |
| `$BD11` | `addrbd11`, CLOSE/role release | Starts `34 06 CC 00 15`; called by `$D331` and `sysdircl`. | Sends a CLOSE secondary command (`$E0 + SA`) when required, then ends TALK or LISTEN if this FCB owns the active role. |
| `$BD51` | `addrbd51`, REL OPEN sender | Starts `34 06 32 7E CC 00 16`; called by `ieeopen` at `$D308`. | Sends the REL filename followed by `L,` and the record length as the EOI byte, then UNLISTENs. |
| `$BD96` | `addrbd96`, SCRATCH sender | Starts `34 06 C6 0F AE E4`; called by `sysscrat`. | Selects command channel 15, sends `S` plus the parsed name, then sends CR with EOI and UNLISTENs. |
| `$BDC6` | `addrbdc6`, RENAME sender | Starts `34 06 C6 0F AE E4`; called by `sysrenam`. | Selects command channel 15 and sends `R` plus the new and old names separated by `=` before EOI and UNLISTEN. |
| `$BE23` | `addrbe23`, drive initialize sender | Starts `34 06 C6 0F AE E4`; called by `sysmount`. | Selects command channel 15 and sends `I0` or `I1` followed by EOI and UNLISTEN. |
| `$BE53` | final-byte and UNLISTEN helper | Starts `34 06 4F E6 61 8D 05`; called by the OPEN and REL-position paths. | Sends one EOI-marked byte through `$BE5F`, then calls `$C0D0`. It is useful for verifying final-byte EOI and command termination together. |
| `$BE5F` | EOI byte output | Starts `34 06 BD C1 76`; called by `$BE53`, `$D3DE`, and write paths. | Calls `$C176`, sends the byte through `$BE6F`, then calls `$C17E`. This is the focused EOI-send entry. |
| `$BE6F` | `ByteOut_` | Starts `34 06 CC 00 17 BD E7 6B`; called throughout the command and write paths. | Drives complemented DIO through PIA2 port B, waits for NRFD and NDAC, reports absent-device or timeout status through the active control block, and releases DAV and DIO on exit. |
| `$BEFB` | byte input | Starts `32 7F CC 00 18 BD E7 6B`; called by status, read, and directory paths. | Asserts NDAC and NRFD, waits for DAV, samples EOI and complemented DIO, releases NDAC, waits for DAV release, then re-arms NDAC and releases DIO. |
| `$BF98` | REL position sender | Starts `34 06 32 E8 EF CC 00 19`; called by `sysseek`. | Sends five bytes `P`, channel, record low, record high, and `$00` on command channel 15. The trailing position byte `$00`, not record high, carries EOI through `$BE53`, followed by UNLISTEN. |
| `$BFEF` | status-channel reader | Starts `34 06 32 E8 ED CC 00 1A`; called by seek, scratch, rename, mount, read, and error paths. | Talks on command channel 15, reads through CR, UNTALKs, and treats status prefixes `00` and `01` as success. |
| `$C072` | `SetLstnr` | Starts `34 06 CC 00 1B BD E7 6B`; called by all listener-side senders. | Resolves any prior role, asserts ATN, sends MLA and MSA, then releases ATN. This is the focused LISTEN-addressing entry. |
| `$C0D0` | `UNListen` | Starts `CC 00 1C BD E7 6B`; called by `$BE53` and role replacement. | Calls `$C14F`, sends `$3F`, releases ATN, and clears the active-role flag. |
| `$C0DD` | `SetTalkr` | Starts `34 06 CC 00 1D BD E7 6B`; called by read and directory paths. | Resolves any prior role, asserts ATN, sends MTA and MSA, asserts NRFD, then releases ATN. This is the focused TALK-addressing entry. |
| `$C13C` | `UNTalk_` | Starts `CC 00 1E BD E7 6B`; called by close and role replacement. | Calls `$C14F`, sends `$5F`, releases ATN, and clears the active-role flag. |
| `$C14F` | `ATNDown_` | Starts `4F F6 E8 40 CA 02`; called by the address and termination routines. | Releases NRFD and NDAC, waits for DAV to release, then asserts ATN. Use it to verify controller line ordering, not disk protocol. |
| `$C16E` | `ATNUp_` | Starts `4F F6 E8 40 CA 04`; called by the address and termination routines. | Releases ATN through the shared `$C1C4` store. Use it to verify the VIA mapping and ATN polarity. |
| `$C176` | `EOIDown_` | Starts `4F F6 E8 11 C4 F7`; called by `$BE5F`. | Asserts EOI through PIA1 control register `$E811`. |
| `$C17E` | `EOIUp_` | Starts `4F F6 E8 11 CA 08`; called by `$BE5F`. | Releases EOI through PIA1 control register `$E811`. |
| `$C188` | `IEEEInit` | Starts `0F 77 4F F6 E8 11 CA 38`; called by `sysioini` at `$C1F5`. | Configures the PIAs and VIA, releases DIO and EOI, establishes the initial ATN/NRFD state, and clears the active role. This is the correct focused initialization entry. |

#### Internal labels that are not independent entries

Do not start a ROM test at an address merely because `MAP*` names it, unless
it is useful to do so for white-box testing.

| Address | Label | Why it is not an independent entry |
|---------|-------|------------------------------------|
| `$BEA2` | byte-output continuation | It is inside `$BE6F`, after the prologue, active-control-block lookup, absent-device check, and DIO setup. Entering here does not implement `ByteOut_`. |
| `$C137` | `SetFin_` | It is the shared tail of `$C072` and `$C0DD`. It calls `$C16E`, then falls through to `$C139`, which removes their saved `D` before returning. A direct `JSR $C137` corrupts the caller's stack. |
| `$C139` | `SetTdone` | It is only the `LEAS 2,S; RTS` epilogue for `$C072` and `$C0DD`. |
| `$C147` | `CMDFin_` | It is the shared command-completion tail reached from `$C0D0` and `$C13C`, not a separately called routine. |
| `$C184` | `EOISet_` | It is the shared final store and return reached by branches from `$C176` and `$C17E`. The archive has no caller that treats it as a standalone EOI operation. |
| `$C1C4` | `ATNSet_` | It is the shared final store and return reached by branches from `$C14F` and `$C16E` and by fall-through from `$C188`. The archive has no standalone call to it. |

The following archived numeric labels are not instruction boundaries in the
pinned revision. They must not be promoted to entries, even for white-box tests:

| Address | Archived anchor | Binary evidence and disposition |
|---------|-----------------|---------------------------------|
| `$B29C` | `addrb29c` | Operand of `JSR $C9D6` at `$B29B` (`BD C9 D6`). The complete close wrapper starts at `$B292`. |
| `$C6E7` | `addrc6e7` | Displacement of `BEQ $C6ED` at `$C6E6` (`27 05`), inside sysnl. |
| `$C6FF` | `addrc6ff` | Indexed postbyte of `STB [,S]` at `$C6FE` (`E7 F4`), inside sysnl. |
| `$D33C` | `addrd33c` | Register mask of `PSHS D` at `$D33B` (`34 06`). `$D33B` is the IEEE read adapter entry. |
| `$DD75` | `addrdd75` | Address operand of `STB $E810` at `$DD74` (`F7 E8 10`), inside keyboard initialization. |
| `$FF86` | `addrff86` | Opcode byte following the page prefix of `LDY #$0100` at `$FF85` (`10 8E 01 00`), inside reset initialization. |

#### Wrappers and parser helpers without direct IEEE register access

None of these routines directly accesses `$E810`, `$E811`, `$E820-$E823`, or
`$E840`. The `$Cxxx` dispatchers and `$Dxxx` adapters can still cause bus
traffic by calling the `$BCxx-$C1xx` routines, so they are valid full-stack
entry points but not electrical primitive tests. The `$Exxx` helpers only
parse or mutate FCB state and are not evidence that the gateware or firmware
drive implementation works.

| Address | Map label or inferred role | Entry evidence | Relevance |
|---------|----------------------------|----------------|-----------|
| `$C1F5` | `sysioini` | Starts `BD E7 7A 4F 5F 34 06`; target of the `$B0A8` public trampoline. | Initializes all system I/O and eventually calls `$C188`. Use `$C188`, not `$C1F5`, for a focused IEEE initialization test. |
| `$C244` | `sysopen` | Starts `34 06 32 7E 4F 5F`; reached through the public file API. | Parses and dispatches all device types, calling `$D28C` only for IEEE. It is an application-level OPEN entry, not an IEEE primitive. |
| `$C2A5` | `sysclose` | Starts `34 06 32 7E CC 00 01`. | Dispatches IEEE close through `$D331`; it adds FCB and generic-device handling. |
| `$C2D8` | `sysread` | Starts `34 06 32 7C CC 00 02`. | Implements record buffering and format handling, then uses `$C0DD`, `$D33B`, and `$BFEF` for IEEE files. |
| `$C52C` | `syswrite` | Starts `34 06 32 7D CC 00 03`. | Implements record sizing and buffering, then uses `$C072`, `$D37B`, or `$BE5F` for IEEE files. |
| `$C6C6` | `sysnl` | Starts `34 06 32 7C CC 00 08`. | Implements end-of-record policy and dispatches IEEE termination through `$D3DE` or `$BE5F`. |
| `$C7F7` | `sysskip` | Starts `34 06 32 7D CC 00 09`. | Skips to the next record by calling `sysread` or `sysnl`; it has no IEEE logic of its own. |
| `$C846` | `sysseek` | Starts `34 06 32 7E CC 00 0A`. | Validates REL access, adds one to the caller record number, dispatches positioning through `$BF98`, and reads status through `$BFEF`. A returned `50` prefix is cleared for seek. |
| `$C8C9` | `sysscrat` | Starts `34 06 32 7E CC 00 0B`. | Parses and dispatches disk SCRATCH through `$BD96`, then reads status through `$BFEF`. |
| `$C90A` | `sysrenam` | Starts `34 06 32 7E CC 00 0C`. | Parses and dispatches disk RENAME through `$BDC6`, then reads status through `$BFEF`. |
| `$C963` | `sysmount` | Starts `34 06 32 7E CC 00 0D`. | Parses and dispatches disk initialize through `$BE23`, then reads status through `$BFEF`. |
| `$C9D6` | `geterror` | Starts `34 06 32 7E CC 00 0E`. | Applies cached FCB error policy and calls `$BFEF` only when a disk status read is required. |
| `$CA99` | `sysdirop` | Starts `34 06 32 7E CC 00 11`. | Opens a directory through `$BCD0`, addresses TALK through `$C0DD`, and consumes its two-byte header through `$BEFB`. |
| `$CAEA` | `sysdirrd` | Starts `34 06 32 7E CC 00 12`. | Reads and reformats directory records by repeated calls to `$BEFB`. |
| `$CB98` | `sysdircl` | Starts `34 06 32 7E CC 00 13`. | Dispatches disk directory close through `$BD11`. |
| `$D28C` | `ieeopen` | Starts `34 06 AE E4 E6 02`; called by `sysopen`. | Rewrites mode suffixes and dispatches to `$BD51` for REL or `$BCD0` otherwise. Use only for full OPEN-path testing. |
| `$D331` | IEEE close adapter | Starts `34 06 EC E4 BD BD 11`; called by `sysclose`. | A stack adapter around `$BD11`. It adds no IEEE behavior. |
| `$D33B` | IEEE read adapter | Starts `34 06 32 7F 4F AE 61`; called by `sysread`. | Checks cached EOF/EOR flags, calls `$BEFB`, and updates FCB status. It adds no electrical behavior. |
| `$D37B` | IEEE write adapter | Starts `34 06 4F AE E4 E6 0C`; called by `syswrite` through `outbyte`. | Clears FCB status and performs printer character translation before calling `$BE6F`. It adds no disk handshake behavior. |
| `$D3DE` | IEEE end-record adapter | Starts `34 06 CC 00 0D BD BE 5F`; called by `sysnl`. | Sends CR through `$BE5F` and updates printer state. It adds no electrical behavior. |
| `$E1EE` | `chkfname` | Starts `34 06 CC 00 0F BD E7 6B`. | Parses file type, device, secondary address, drive, and disk filename before `sysopen`. No bus access occurs. |
| `$E2EE` | `chkdirpm` | Starts `34 06 CC 00 10 BD E7 6B`. | Parses directory or mount parameters and constructs `$0` or `$1`. No bus access occurs. |
| `$E373` | `chkdevnm` | Starts `32 7E 9E 13 6F 02`. | Maps a textual device name to an FCB device code and defaults an omitted device to disk 8. No bus access occurs. |
| `$E3D1` | `chkieedv` | Starts `32 7E BD E6 D5 ED E4`. | Parses an IEEE primary address, defaulting disk to 8 and printer to 4. No bus access occurs. |
| `$E408` | `chkieesa` | Starts `32 7E 9E 68 E6 84`. | Parses a `-secondary-address` suffix or calls `$E570` for a disk default. No bus access occurs. |
| `$E444` | `chkdskdr` | Starts `32 7E 9E 68 E6 84`. | Parses a `/drive` suffix and defaults to drive 0. No bus access occurs. |
| `$E47D` | `chddevcd` | Starts `9E 13 E6 02 26 0A`. | Validates the device/file separator and marks a disk-style filename. No bus access occurs. |
| `$E4C6` | `chkrcdsz` | Starts `34 06 9E 11 EC 01`. | Validates the requested record size against the selected format. No bus access occurs. |
| `$E570` | `ieegetsa` | Starts `32 7C C6 02 9E 13`. | Allocates an unused secondary address, starting at `$02`, from the active FCB list. No bus access occurs. |
| `$E5B9` | `ieedname` | Starts `BD E4 7D 9E 11 AE 04`. | Forms the disk filename with drive prefix and default `,SEQ` or `,PRG` type. No bus access occurs. |

#### Additional Waterloo application and support entries

For normal Waterloo calls, the first word argument is in D, subsequent word
arguments start at `2,S` on callee entry, and the caller removes those extra
arguments. `CALL.MACRO` implements this convention. D/B carry return values.
Use DP=0 and initialized system RAM. Application FCB pointers and their
parameter-block pointers at FCB+4 are distinct: the transport routines take
the latter, while the file API takes the former. Byte senders take the byte
in B and use the active parameter block at `$02`. `SetLstnr` takes a parameter
block in D plus a stacked secondary-command offset. `SetTalkr` takes the
parameter block in D. Preserve these distinctions in ROM tests.

| Address | Archived label or inferred role | Entry bytes | Required state and relevance |
|---------|---------------------------------|-------------|------------------------------|
| `$F303` | `monitor file-load command (unlabelled in OPSYSDIS.26)` | `32 E8 E6 BD B0 C6 E7 E4` | Reads a filename from default input, opens mode L through openf_, calls $F39E, and closes the FCB. Complete 26-byte-frame entry, distinct from loadcmd at $AC20. |
| `$F39E` | `addrf39e / monitor module reader` | `34 06 32 74 1F 41 C6 06` | FCB in D. Reads six-byte headers and payloads through $F3FB, checks errorf_, selects the bank via $EFFC for type 1, stops on type 2, and restores bank zero. Requires initialized monitor state. |
| `$F3FB` | `addrf3fb / monitor byte reader` | `34 06 EC 64 34 06 C3 00` | FCB in D, destination and nonzero count on stack. Copies bytes through fgetchar_ at $B0D8. Decrements the count after each byte; zero is not an empty transfer. |
| `$AC20` | `loadcmd` | `34 06 32 7D EC 63 34 06` | Filename string in D. Opens using mode L through openf_, invokes loadprog on success, and closes through closef_. `$AC30` is an internal argument-load instruction, not its entry. |
| `$AC59` | `loadprog` | `34 06 32 73 4F 5F DD 2A` | FCB in D. Reads six-byte module headers and payloads through progread, supports banked loads, checks errorf_, and sets the program-start vector. Requires initialized banks and FCB state. |
| `$ACFC` | `progread` | `34 06 EC 66 34 06 EC 66` | FCB in D, buffer and count on stack. Stack adapter to public sysread_ at `$B108`. |
| `$B1C1` | `initstd` | `BD D4 06 4F 5F DD 6B CC` | Initializes the FCB pool and default terminal input/output FCBs. Establishes defaults used by the application wrappers, not an IEEE reset. |
| `$B1E5` | `getchar` | `DC 6B 7E B2 A6 34 06 4F` | Reads one character from default input FCB `$6B` through fgetchar. |
| `$B1EA` | `putchar` | `34 06 4F E6 61 34 06 DC` | Byte in B. Writes through default output FCB `$6D` and fputchar. |
| `$B1F9` | `putnl` | `DC 6D 7E B3 24 34 06 EC` | Ends a record on default output FCB through fputnl. |
| `$B1FE` | `getrec` | `34 06 EC 64 34 06 EC 62` | Buffer in D, capacity on stack. Reads a record from default input through fgetrec. |
| `$B20F` | `putrec` | `34 06 EC 64 34 06 EC 62` | Buffer in D, length on stack. Writes a record to default output through fputrec. |
| `$B221` | `printf` | `35 10 34 06 34 10 1F 41` | Format string in D and variadic stack arguments. Repackages the return address and calls the archived sprintf formatter on default output. |
| `$B23B` | `openf` | `34 06 32 E8 CA BD D4 12` | Filename in D, mode-string pointer on stack. Allocates FCB, copies strings to workspace, calls sysopen and geterror, frees failed opens, and returns FCB or zero. |
| `$B292` | `closef` | `34 06 EC E4 BD C2 A5 EC` | FCB in D. Calls sysclose, geterror and freefcb. Generic file API, not the low-level CLOSE sender. |
| `$B2A6` | `fgetchar` | `34 06 32 7F AE 61 E6 98` | FCB in D. Reads through sysread and implements cached EOR/EOF and CR policy. |
| `$B2FD` | `fputchar` | `34 06 32 7F 4F E6 66 E7` | FCB in D, character word on stack. Calls syswrite for one byte and updates cached status. |
| `$B324` | `fputnl` | `34 06 CC FF FF 34 06 EC` | FCB in D. Calls sysnl with reset-status argument `$FFFF`. |
| `$B333` | `fgetrec` | `34 06 32 7E 4F 5F ED E4` | FCB in D, buffer and count on stack. Skips previous record, invokes sysread, returns count, and caches status. |
| `$B367` | `fputrec` | `34 06 EC E4 BD C7 F7 AE` | FCB in D, buffer and count on stack. Calls sysskip, syswrite and sysnl, caching status. |
| `$B3AA` | `fprintf` | `34 06 1F 41 C6 04 3A 34` | FCB in D, format and substitutions on stack. Calls the shared formatter. |
| `$B3BA` | `sprintf` | `34 06 32 E8 E5 EC F8 1F` | Archived name, not a memory-only C sprintf. FCB in D, argument-block pointer on stack. Formats through fputchar/fputnl and can therefore cause IEEE output. |
| `$B4A3` | `fseek` | `34 06 EC E4 BD C7 F7 AE` | FCB in D, record number on stack. Ends current record through sysskip, invokes sysseek and caches status. |
| `$B4C9` | `eor` | `34 06 AE E4 E6 03 C1 01` | FCB in D. Tests cached status at FCB+3 against 1. No bus transaction. |
| `$B4D3` | `eof` | `34 06 AE E4 E6 03 C1 02` | FCB in D. Tests cached status at FCB+3 against 2. No bus transaction. |
| `$B4E5` | `errorf` | `34 06 32 7F EC 61 26 07` | FCB in D, or zero for global status. A nonzero FCB can invoke geterror and a disk status read. |
| `$B510` | `errormsg` | `34 06 CC 03 00 32 62 39` | Returns pointer `$0300` to the global message buffer. Does not read the disk. |
| `$B518` | `scratchf` | `34 06 32 E8 D5 BD D4 12` | Filename in D. Allocates temporary FCB and workspace, dispatches sysscrat and frees the FCB. |
| `$B543` | `renamef` | `34 06 32 E8 AC BD D4 12` | Old filename in D, new-name pointer on stack. Allocates temporary FCB/workspace and dispatches sysrenam. |
| `$B58B` | `mount` | `34 06 32 E8 D5 BD D4 12` | Device/drive string in D. Allocates temporary FCB/workspace and dispatches sysmount. |
| `$B5BD` | `timeout` | `34 06 AE E4 AE 04 34 10` | FCB in D, timeout word on stack. Sets parameter-block+7. Zero selects unbounded handshake waits. No bus transaction. |
| `$B5D2` | `diropenf` | `34 06 32 E8 D5 BD D4 12` | Device/drive string in D. Allocates an FCB and calls sysdirop. Returns FCB or zero. Binary prologue allocates 43 bytes despite archive placeholder. |
| `$B610` | `dirreadf` | `34 06 EC 64 34 06 EC 62` | Directory FCB in D, destination buffer on stack. Calls sysdirrd and caches status. |
| `$B627` | `dirclosef` | `34 06 EC E4 BD CB 98 EC` | Directory FCB in D. Calls sysdircl then freefcb. |
| `$B636` | `global status clear` | `0F 6A 8E 03 00 6F 84 39` | Clears `$6A` and first message byte at `$0300`. Called by system I/O and allocation. No bus transaction. |
| `$B63E` | `global EOF setter` | `C6 02 D7 6A CC 03 00 34` | Sets `$6A=2` and copies eof to `$0300`. Does not receive a byte. |
| `$B650` | `timeout error setter` | `CC B6 6A 34 06 C6 03 D7` | Loads timeout message and falls through `$B653`. Sets global error 3. Does not perform a handshake. |
| `$B653` | `global error-message setter` | `34 06 C6 03 D7 6A CC 03` | Message pointer in D. Own prologue saves D, sets `$6A=3`, and copies the message to `$0300`. Independent callable entry, not an unsafe tail. |
| `$C4D2` | `sysrdbyt / MAP sysread7` | `34 06 32 7D CC 00 06 BD` | FCB in D. Complete byte-input dispatcher, hook 6. Resolves FCB+4 and calls `$D33B` for IEEE. It is not a sysread continuation despite the MAP name. |
| `$C669` | `outbyte` | `34 06 32 7E CC 00 07 BD` | FCB in D, byte on stack. Complete output dispatcher, hook 7. Resolves FCB+4 and calls `$D37B` for IEEE. |
| `$C9A5` | `getacmod` | `34 06 EC 64 BD B7 B1 34` | Parameter block in D, mode string on stack. Decodes mode through the table at `$B18C` and sets access flags or error. No bus access. |
| `$CA15` | `chekcod2` | `34 06 AE E4 E6 98 04 C1` | FCB in D. Tests parameter-block status for EOF (2), returning boolean D. Complete leaf helper even without a direct caller in the audited graph. |
| `$CA20` | `chekcod1` | `34 06 AE E4 E6 98 04 C1` | FCB in D. Tests parameter-block status for EOR (1), returning boolean D. Called repeatedly by sysread. |
| `$D406` | `initfcbs` | `CC 05 7F DD 75 DD 73 9E` | Initializes FCB-pool bounds `$73/$75` to `$057F` and clears its sentinel. Called by initstd. No bus access. |
| `$D412` | `asignfcb` | `32 7A BD B6 36 DC 75 ED` | Allocates a 26-byte slot downward within `$0480-$057F`, links it into the FCB ring and initializes status. Returns FCB in D or zero plus an error. No bus access. |
| `$D483` | `freefcb` | `34 06 32 7C EC 64 C3 FF` | FCB in D, valid circular FCB list required. Unlinks and marks the slot free, coalesces the pool boundary. No bus access. |
| `$E476` | `invfname` | `9E 68 E6 84 26 3D 39 9E` | Checks the remaining filename text at `$68`, using the existing parser state. Returns if empty, otherwise enters the invalid-filename error tail. |
| `$E4DF` | `record-format parser` | `32 7E 4F 5F 9E 11 ED 01` | Uses parser globals `$11/$13/$68`. Initializes and parses record format and optional record size. Called by chkfname. No bus access. |
| `$E6C0` | `alphabetic-prefix scanner` | `32 7E DC 68 ED E4 4F E6` | Returns the length of the alphabetic prefix at `$68`. Used by device and format parsing. |
| `$E6D5` | `numeric-prefix scanner` | `32 7E DC 68 ED E4 4F E6` | Returns the length of the numeric prefix at `$68`. Used by primary, secondary, drive and record-size parsing. |
| `$E6F1` | `consume filename prefix` | `34 06 DC 68 34 06 E3 62` | Count in D. Removes that many bytes from the string at `$68` using the overlapping copy helper. No bus access. |
| `$E76B` | `system I/O hook dispatch` | `1F 01 E6 89 05 80 27 06` | Hook index in D. Tests byte at `$0580+index`, otherwise returns. If enabled, jumps indirectly through `$05C0+index`. Clear hooks for native-ROM tests. |
| `$E77A` | `clear system I/O hooks` | `32 7E CC 05 80 ED E4 83` | Clears `$0580-$05FF`. Called by sysioini. Prevents user hooks from changing native I/O behavior. |
| `$DD48` | `keyboard/PIA1 initialization` | `C6 1E F7 01 2A 7F 01 2B` | Called by terminal initialization. Programs `$E810/$E811/$E813`, including releasing EOI. Shared hardware setup, not a complete IEEE initialization. |
| `$FE74` | `early PIA1/clock initialization` | `32 79 C6 FF F7 EF F1 7F` | Called during restart. Clears PIA1 control and data/direction registers before sysioini configures IEEE. Not a focused bus operation. |
| `$F000` | `restart1` | `10 CE 02 20 BD FE 74 BD` | Boot entry sets S itself, calls `$FE74` and system I/O initialization, then enters startup. Not a subroutine returning to a test caller. |
| `$FF80` | `reset-vector target` | `8E FF B1 C6 07 10 8E 01` | Reached through hardware vector `$FFFE`. Initializes interrupt dispatch and jumps to restart1. Do not invoke with JSR expecting a return. |

#### Waterloo public file and disk vectors

Each row is an independently verified three-byte JMP. These application ABI
entries are relevant even though the trampoline itself has no electrical
behavior. Date/time, generic request, and keyboard-enable vectors interleaved
in this block are excluded from the file/disk API and recorded in the ledger.

| Address | Public label | Implementation target | Exact bytes |
|---------|--------------|-----------------------|-------------|
| `$B09F` | `diropenf_` | `$B5D2` | `7E B5 D2` |
| `$B0A2` | `dirreadf_` | `$B610` | `7E B6 10` |
| `$B0A5` | `dirclose_` | `$B627` | `7E B6 27` |
| `$B0A8` | `sysioini_` | `$C1F5` | `7E C1 F5` |
| `$B0AB` | `initstd_` | `$B1C1` | `7E B1 C1` |
| `$B0AE` | `openf_` | `$B23B` | `7E B2 3B` |
| `$B0B1` | `closef_` | `$B292` | `7E B2 92` |
| `$B0B4` | `fseek_` | `$B4A3` | `7E B4 A3` |
| `$B0B7` | `printf_` | `$B221` | `7E B2 21` |
| `$B0BA` | `putrec_` | `$B20F` | `7E B2 0F` |
| `$B0BD` | `putchar_` | `$B1EA` | `7E B1 EA` |
| `$B0C0` | `putnl_` | `$B1F9` | `7E B1 F9` |
| `$B0C3` | `getrec_` | `$B1FE` | `7E B1 FE` |
| `$B0C6` | `getchar_` | `$B1E5` | `7E B1 E5` |
| `$B0C9` | `fprintf_` | `$B3AA` | `7E B3 AA` |
| `$B0CC` | `fputrec_` | `$B367` | `7E B3 67` |
| `$B0CF` | `fputchar_` | `$B2FD` | `7E B2 FD` |
| `$B0D2` | `fputnl_` | `$B324` | `7E B3 24` |
| `$B0D5` | `fgetrec_` | `$B333` | `7E B3 33` |
| `$B0D8` | `fgetchar_` | `$B2A6` | `7E B2 A6` |
| `$B0DB` | `eor_` | `$B4C9` | `7E B4 C9` |
| `$B0DE` | `eof_` | `$B4D3` | `7E B4 D3` |
| `$B0E1` | `errorf_` | `$B4E5` | `7E B4 E5` |
| `$B0E4` | `errormsg_` | `$B510` | `7E B5 10` |
| `$B0E7` | `mount_` | `$B58B` | `7E B5 8B` |
| `$B0EA` | `scratchf_` | `$B518` | `7E B5 18` |
| `$B0ED` | `renamef_` | `$B543` | `7E B5 43` |
| `$B105` | `timeout_` | `$B5BD` | `7E B5 BD` |
| `$B108` | `sysread_` | `$C2D8` | `7E C2 D8` |
| `$B10B` | `syswrite_` | `$C52C` | `7E C5 2C` |
| `$B10E` | `sysnl_` | `$C6C6` | `7E C6 C6` |

The standard PET I/O range `$E800-$EFFF` lies inside the 6809 ROM address
range but must override ROM. In particular, `$E820-$E83F` must select PIA2 and
`$E840-$E87F` must select the VIA. This is required by the direct register
accesses above, matches
[VICE `petmem.c`](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/pet/petmem.c),
and is implemented by
[`address_decoding.sv`](../../gw/EconoPET/src/address_decoding.sv). A test that
loads the Waterloo ROM without this I/O hole cannot establish IEEE behavior.

For TALK termination and counted-read continuation, trace these routines:

- `$C13C` calls `$C14F`, sends UNTALK byte `$5F`, then calls `$C16E` to
  release ATN.
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

The Waterloo module-load path starts at `loadcmd` `$AC20`. `$AC30` is an
internal instruction preparing the mode pointer before the call at `$AC37`
to `openf_` `$B0AE`, which jumps to `openf` `$B23B`. Its call at `$B240`
reaches `asignfcb` `$D412`. The allocator examines RAM pool/list state,
returns zero on exhaustion, and performs no IEEE transaction. A successful
allocation continues through `sysopen` `$C244`, `ieeopen` `$D28C`, and the
normal OPEN sender, then `geterror`. Successful module reads use `loadprog`
`$AC59` and `progread` `$ACFC` through `sysread_` `$B108`. This resolves the
static call path, not the cause of a particular failed runtime load. Displayed
`Loading` or `Program not found` text alone is not compatibility evidence.

The exhaustive address/disposition ledger and reproducible validation command
are documented in [the ROM entry audit](../../docs/dev/PET/ieee-rom-entry-audit.md).
That audit verifies addresses and static calling contracts. It does not claim
that every listed entry has an executed full-ROM compatibility test.

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
