# Super-OS/9

## Graphics Keyboard Investigation

The Waterloo ROM graphics-keyboard patch does not affect Super-OS/9's console
keyboard. Super-OS/9 scans the PIA itself and uses its own translation tables.
See the [Waterloo ROM patch](PET/roms.md#graphics-keyboard-patch-waterloo-revision-12)
and the [keyboard matrices](PET/keyboard.md) for comparison.

The following findings apply to the `os9` program on the downloaded
[system disk](https://mikenaberezny.com/wp-content/uploads/2009/11/os9-systemdisk.d80).
The D80 has MD5 `a772ba7ad5d2f6a635a092f80f80d45c`, and its extracted `os9`
PRG has MD5 `8a94e77a89203146a2e5903af060e84b`. These offsets must not be
assumed to apply to other versions or boot disks.

The [Super-OS/9 reconfiguration disk](https://www.zimmers.net/anonftp/pub/cbm/pet/SuperPET/os9/os9-relo-bin.d80.gz)
contains the source file `matrix.table`, the linker map `os9.map`, and an
`os9.d8d8` PRG that is byte-for-byte identical to the downloaded `os9` PRG.
All 240 translation-table bytes match `matrix.table`. The reconfiguration
disk's README identifies `jiffy.irq` as the keyboard and clock code used by
`CbmCon`, but says its source is not included. The disk does include its
relocatable binary, `jiffy.irq.b09`.

### Scanner and Tables

The Waterloo-loaded `os9` PRG contains the external processor code. Its
keyboard scanner begins at `$7672`, writes KSCAN values to PIA1 port A
(`$E810`), and reads active-low KIN bits from port B (`$E812`). It reads the
input twice until stable. The addresses below are addresses in this loaded
program, not offsets in the OS-9 RBF filesystem.

| Table | Loaded address | Offset in extracted PRG | Size |
| ----- | -------------- | ----------------------- | ---- |
| Unshifted | `$774B-$779A` | `$0970-$09BF` | 80 bytes |
| Shifted | `$779B-$77EA` | `$09C0-$0A0F` | 80 bytes |
| Control | `$77EB-$783A` | `$0A10-$0A5F` | 80 bytes |

The PRG offsets include the six-byte Waterloo program header. Each table is
indexed by `8 * KSCAN + KIN`, with no sentinel entry. The table values are
OS-9 console codes, not PETSCII or the Waterloo ROM table indices. For
example, business `A` at KSCAN 3/KIN 0 translates to `$61`, `$41`, and `$01`
in the three tables. Control takes precedence over Shift when both are held.

### Modifiers

The business keyboard has no dedicated Control key. Super-OS/9 uses its RVS
key at KSCAN 8/KIN 0 as a held Control modifier. The scanner removes that
matrix bit from ordinary key processing and selects the Control table. Its
business Shift keys are at KSCAN 6/KIN 0 and KSCAN 6/KIN 6.

On the graphics keyboard, Left Shift occupies the business RVS position
(KSCAN 8/KIN 0), while Right Shift is at KSCAN 8/KIN 5. Graphics RVS is at
KSCAN 9/KIN 0. A graphics-keyboard patch can preserve the original physical
key choice by using graphics RVS as Control.

The scanner tests modifiers separately from the tables, so remapping only the
tables cannot fix Shift and Control. For the identified PRG, the following
in-place byte changes move the modifier tests and removal operations:

| PRG offset | Loaded address | Original | Patched | Purpose |
| ---------- | -------------- | -------- | -------- | ------- |
| `$08AE` | `$7689` | `$41` | `$21` | Shift bits 0 and 5 |
| `$08B0` | `$768B` | `$66` | `$68` | Test Shift in KSCAN 8 |
| `$08B4` | `$768F` | `$66` | `$68` | Remove Shift bits from KSCAN 8 |
| `$08B6` | `$7691` | `$66` | `$68` | Store the modified KSCAN 8 row |
| `$08BF` | `$769A` | `$68` | `$69` | Test Control in KSCAN 9 |
| `$08C3` | `$769E` | `$68` | `$69` | Remove Control from KSCAN 9 |
| `$08C5` | `$76A0` | `$68` | `$69` | Store the modified KSCAN 9 row |

The Control mask remains `$01`. These offsets are within the extracted PRG,
not the D80 image. A disk-image patcher must follow the Commodore file's
sector chain rather than treating the PRG as a contiguous D80 byte range.

### Generated Disk

[download.sh](../../.devcontainer/download.sh) retains the original disk and
uses [patch-os9-keyboard.py](../../.devcontainer/patch-os9-keyboard.py) to
create `os9-systemdisk-graphics.d80`. The patcher checks both the source D80
and loader identity, follows the PRG sector chain, and changes only its
payload. It preserves the original OS-9 REL container and disk metadata.
The SuperPET configurations mount the graphics disk for graphics keyboards and
the original disk for business keyboards. Since these are separate images,
changes made within OS-9 on one disk do not appear on the other.

The three tables use OS-9's original codes at graphics-keyboard positions.
For example, graphics `A` is at KSCAN 4/KIN 0 and translates to `$61`, `$41`,
and `$01`. Punctuation follows the existing Waterloo graphics patch, reversing
the business unshifted/shifted pair when needed to match a graphics keycap.
Graphics keypad digits retain the business keypad's shifted function codes.
RVS acts as Control, preserving the business keyboard's physical key choice.

The graphics keyboard lacks physical ESC, TAB, and REPEAT keys. As with the
Waterloo patch, ESC is not reassigned, and the unwired TAB and REPEAT positions
remain available to injected matrix input. OS-9 maps the business REPEAT
position to rubout (`$7F`).

System tests exercise the patched 6809 scanner and translator through the
simulated PIA and check both keyboard-specific mount selections. Full OS-9
boot, application behavior, key repeat, and physical-keyboard testing remain
to be verified.
