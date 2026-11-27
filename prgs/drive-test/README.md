# Drive Test program

`drive-test.prg` is a self-contained PET program built by the `prgs` target and
run independently in VICE. The PRG target alone does not install an SD-card
asset or register a firmware diagnostic.

With the separate SD-card packaging and firmware Drive Test integration,
the program is installed at `/test/drive-test.prg` and launched from the
EconoPET serial terminal CLI via `test drive [count]`. That integration
creates temporary zero-filled D64/D80/D82 scratch images in `/test/tmp`,
loads the `pet-80xx-60hz` (8032) profile, mounts all eight slots on devices 8
through 11 (drives 0 and 1), runs the program while monitoring its `$7FF0`
target mailbox on the HDMI diagnostic display, and removes `/test/tmp` on pass
or fail.

The current executable full-formats all eight
images, then creates and reads slot-unique 16-byte and 512-byte sequential
files (with distinct 256-byte regions to detect replay) and short binary PRG
and USR streams on every slot. It checks the
complete empty and single-file directory streams, every formatted BAM sector,
the affected BAM sector and the data-sector link and payload after the first
file, rename, copy, collision,
scratch, missing-file and consumed status, counted direct-block writes and
reads and a buffer-pointer reposition on track 1, sector 0, a 256-byte U2/U1
round-trip on track 1, sector 2, a UB/UA round-trip on sector 3 once per
geometry and a U2/U1 round-trip on the last sector of each D82, short REL
create/reopen/position/read checks on every slot, alternating reads between
both drives on each unit and between units 8 and 11, cross-drive copies in both
directions, concatenation and self-append on each unit, multi-record REL growth
across a sector and the first side-sector pointer boundary once per geometry,
disk DUPLICATE in both directions on representative units 8 through 10 with
a seeded free sector and source-retention check,
SEQ/PRG/USR boundary and sequential append and replace cases once per
representative geometry,
wildcard scratch, block allocation, successor and release once per geometry,
initialization and validate
with unreferenced-block recovery and a retained-file read, quick format and
free-sector retention once per geometry, and representative geometry-zone
boundary writes and retention reads. Empty and one-byte files check the native
four-byte EOF quirk rather
than assuming EOI on the first byte. It stops on the first mismatch, releases
its bus outputs without sending cleanup commands, and leaves the failing media
state intact.

This is not yet a comprehensive compatibility suite. Additional directory
states and block variants, extended REL, capacity, other cross-drive
operations, many command variants, and complete data-sector assertions after
mutations remain unimplemented. The unused tail of the first file's sector
depends on prior drive RAM and is not compared. Intentionally excluded 6502
drive-memory and utility-execution commands (`M-*`, `B-E`, `U3..U8`, `&`) are
omitted from this runner (firmware unit tests verify their `30`/`31` policy
responses). The program reports incomplete rather than a comprehensive pass.

The 2-, 129-, and 254-byte REL records round-trip once per representative
geometry after the smoke phase; the 254-byte case also reads from offset 254,
checks overflow at 255, and checks status 50 on positioning to missing records
2 and 257, then verifies CR/EOI and status 50 again on GET. It also checks
record-number zero and offset zero aliases against the first record byte.
The 255-byte write case expects native status 51 at the record boundary and
closes the channel without attempting readback.

For that case, the selected ROMs' `NULBUF:ADDNR` wraps the next-record pointer
from data offset 2 to 3 when adding length 255. `RELPUT` then sets overflow
after the first byte, and `WRTREL:WR30` reports status 51 on the final EOI.

All 468 checks in the current `drive-test.prg` runner have been vetted
end-to-end in headless VICE (`xpet -console -warp`, standard 32K PET 8032 with
ROM 4 and unmodified true-drive 4040 DOS 2.0r2, 8050 DOS 2.7, and 8250 DOS 2.7
ROMs across units 8-11) via `python3 prgs/drive-test/vet_vice.py --download-roms`.
On-board EconoPET hardware qualification remains the primary target test pass.

Standalone `LOAD`/`RUN` rejects stale mailbox attachment flags and returns to
BASIC after one pass. A fresh MCU launch is identified by cleared magic bytes,
`HOST_ATTACHED`, and `STATE_RUNNING` armed before CPU release. Publishing the
target magic consumes that launch, while subsequent MCU cycles wake the
existing program without re-entering startup. VICE vetting seeds a prior
attached pass and requires the standalone run to return to BASIC READY.

## Command coverage

The selected ROMs dispatch `I`, `V`, `D`, `M`, `B`, `U`, `P`, `C`, `R`, `S`
and `N`. DOS 2.7 also dispatches `&`. A partial case is not comprehensive
coverage of that command.

| Commands | Implemented cases | Still missing |
| --- | --- | --- |
| `N` | Full format with an explicit ID and quick format retaining ID and free-sector bytes | Other quick-format cases and invalid forms |
| `R`, `S`, `C` | Rename, scratch including `?` and `*`, cross-drive copy and SEQ/PRG concatenation, SEQ self-append, missing-source precedence and collision/type errors | Other wildcard forms, copy-all, REL/USR concatenation and other error paths |
| Command parsing | Unknown command `X` returns status 31 with the ROM's `SYNTAX ERROR` text | Length limits, malformed syntax and termination variants |
| `B` | `B-R`, `B-W`, `B-P`, `B-A` and `B-F` selected cases | Other offsets, aliases, limits and errors |
| `U` | `U1` raw BAM reads, `U2`/`U1` round-trips per slot, representative geometry boundaries and the D82 last sector, and `UB`/`UA` on one slot per geometry | Other sectors, aliases, resets and safe user vectors |
| `P` | Position/read for 2-, 127-, 129-, and 254-byte records; zero aliases, 254/255 offset boundary, missing records 2/257, 129-byte sector crossing, and growth from the first to second side sector | Other record lengths, later side-sector and capacity boundaries, and error paths |
| `M`, `&` | Intentionally excluded by EconoPET policy (no 6502 drive RAM/code execution; firmware returns `30`, or `31` for `&` on 4040) | Excluded by design |
| `I`, `V` | Initialize and recover an unreferenced block while retaining a file | Channel lifecycle, invalid media and exact repaired BAM |
| `D` | `D0=1` and `D1=0` on units 8-10 copy a seeded free sector and retain each source | Whole-image sector comparison and duplicate error paths |

Drive memory and downloaded-code commands (`M-*`, `B-E`, `U3..U8`, `&`) are
excluded by design.

## Implemented case evidence

These are partial cases, not a complete command inventory. Text status checks
include the complete record and EOI, including the DOS 2.7 drive field.
Original-drive execution and hardware qualification are still outstanding.

| Phase | Setup and expected result | Selected source routines |
| --- | --- | --- |
| Full format | `N<drive>:<slot>,<id>` on each slot, followed by OK | `new`, `erproc` |
| Empty and populated directory | Full BASIC directory bytes, footer free count and final EOI before and after the first closed SEQ file | `lstdir:STDIR/DIR1/DIR3`, `trnsfr:GETNAM/NEWDIR/MSGFRE` |
| Format and file BAM | Full formatted BAM parts, then the affected BAM sector after the first SEQ file (comparing bytes `0..179` on 4040 drive 0 after directory listing, since 4040 `NAMBUF` shares `$41B4..$41D7` inside `BAM0`) | `new`, `map`, `ramvar:NAMBUF`, `tstfnd:INTTS/JINTTS`, `close:CLSWRT/MAPOUT` |
| First file sector | Exact native link and 16 payload bytes at 17/0 for D64 or 38/1 for D80/D82, with full-transfer EOI | `tstfnd:INTTS/JINTTS`, `close:CLSWRT` |
| Short sequential files | Close and reopen lengths 0 and 1, checking the four-byte stream and final EOI | `close:CLSWRT`, `opchnl:OR30`, `tst2:RDBYT` |
| REL record lengths and missing-record timing | Every slot round-trips 127 bytes; representative geometries round-trip 2, 129 and 254 bytes, verify the final byte at offset 254, expect status 51 at offset 255 for length 254 and when writing length 255, and check status 50 at missing-record positioning and GET | `open:CKTM/OP60`, `record:R40/POSITN`, `rel2:WRTREL/RELPUT`, `rel3:RD05`, `parsex:ENDCMD`, `erproc:NOREC/ERRTAB`, `rel1:NXTREC`, `tst2:RDBYT` |
| REL growth and sector crossing | A 129-byte REL file receives a short first record. Positioning to record 2 returns 50 at command and actual missing GET. Writing after positioning at offset 123 grows the file and starts at offset 1. A later write at offset 123 crosses the 254-byte boundary. After CLOSE/reopen, read both records, the initialized empty third record, and the missing fourth record, checking bytes, padding, status and EOI | Both `addrel:ADDREL/AR65`, `record:RECORD/POSITN`, `rel2:WRTREL/CLREC`, `rel3:RDREL/RD05`, `rel4:SETLST/FNDLST`, `nulbuf:NULBUF/ADDNR`, `close:CLSREL` |
| REL side-sector boundary | Grow the existing 254-byte `T...` file from record 1 to record 121, crossing the 120 data pointers in the first side sector. After CLOSE/reopen, compare record 121, the initialized record 120 and all 254 bytes of record 1, then check missing record 122. The focused D64 fixture begins with 120 initialized records to exercise the boundary without simulating the long setup | Both `fndrel:FNDREL/DIV120`, `addrel:ADDREL/AR45/AR60`, `ssutil:SSPOS`, `rel2:WRTREL`, `rel4:SETLST` |
| Cross-drive concatenation | On every unit, `C1:JOIN=0:R...,1:R...` concatenates two 16-byte SEQ streams and `C1:JOIN=1:JOIN,0:R...` self-appends the first. Mixed SEQ/PRG inputs run in both orders, checking that the first source supplies the output type. Exact 32- and 48-byte streams and final EOI are checked. A missing second source wins over an existing destination (62), an existing destination returns 63, and PRG self-append returns 64. Re-read the sources and existing target after errors | Both `rename:COPY/CHKIN/CHKIO/CY10/CY20`, `open:APPEND`, `lookup:LOOKUP`, `erproc:ERRTAB` |
| Ordinary-file boundaries, append and replace | SEQ/PRG/USR round-trips at selected 253 to 509-byte lengths, append two bytes to a 253-byte SEQ file and replace it with a short SEQ file | `open:CKTM/APPEND/OP75/OP82`, `close:CLSWRT/CLSDIR`, `tst2:RDBYT/WRTBYT` |
| Wildcard scratch | `P?` removes eight two-character PRG names while retaining a longer name, then `P*` removes the longer name and retains an unrelated USR file | `lookup:COMPAR`, `scrtch:SCRTCH` |
| Unknown command | `X` returns the complete status `31,SYNTAX ERROR` with model-specific drive fields and EOI | `parsex:PARSXQ`, `erproc:BADCMD/ERRTAB` |
| Direct and allocation | Counted B-W/B-R, B-P reposition, full 256-byte U2/U1 and UB/UA round-trips including D82 154/22, and B-A/B-F successor and recovery cases | `block:USER/USREXC/BLKWT/BLKRD/UBLKWT/UBLKRD/BLKPTR/BLKALC/BLKFRE`, `vector:UBLOCK`, `romtbl:NTRK80/NSEC80`, `tst2:PUTBYT`, `tst4:RNDGET` |
| Geometry boundaries (partial) | After quick format, write and reread all 256 bytes and final-byte EOI on the last sector of selected zone-boundary tracks. Reread all seeded sectors after all writes, including across both D82 sides | `romtbl:TRKTBL/SECTRK` (4040), `romtbl:NTRK80/NSEC80` (8050/8250), `block:UBLKRD/UBLKWT/BKOTST` |
| Initialize and validate | `I` and `V` recover a deliberate unreferenced claim and retain the smoke file | `tst2:INTDRV/INITDR`, `verdir:VALDAT` |
| Quick format | `N<drive>:<label>` retains the ID and a seeded free sector, replaces the directory and rebuilds all BAM parts | `new:N108/N110`, `tst2:INITDR`, `verdir:NEWMAP` (4040), `map:NEWMAP` (8050/8250) |
| Disk duplicate (partial) | Both directions per unit, with a seeded free-sector payload checked on destination and source after each operation. DOS 2.7 toggles `DRVNUM` to the destination before `ENDCMD`, and `ERMSG2` emits that drive in the status suffix | `duplct:DUPLCT/DUP1/CPYD1/CPYTRK`, `setdrv:TOGDRV`, `parsex:SCREND`, `erproc:ERMSG2` |

The concatenation and REL-growth cases target 4040 DOS 2.0 revision 2 and
8050/8250 DOS 2.7 in the selected single- and double-sided modes. Their primary
sources are the pinned 4040 and DOS 2.7
[`rename`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rename)
and [`rename`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rename),
[`addrel`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/addrel)
and [`addrel`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/addrel),
[`rel2`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel2)
and [`rel2`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel2),
and [`nulbuf`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/nulbuf)
and [`nulbuf`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/nulbuf)
routines, plus the 4040 and DOS 2.7
[`fndrel`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/fndrel)
and [`fndrel`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/fndrel)
side-index calculations. [VICE COPY](https://github.com/VICE-Team/svn-mirror/blob/a0e14df25846cd7f06043f24b48677bf45c02149/vice/src/vdrive/vdrive-command.c#L815-L1010)
corroborates source-before-destination checking and ordered concatenation, but
not the native self-append rule. [VICE REL growth](https://github.com/VICE-Team/svn-mirror/blob/a0e14df25846cd7f06043f24b48677bf45c02149/vice/src/vdrive/vdrive-rel.c#L310-L630)
corroborates initialized records and zero padding, not the native offset reset.
MAME's [4040](https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c2040.cpp)
and [8050/8250](https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c8050.cpp)
models corroborate the selected hardware/ROM modes, not execution of these cases.
The linked-PRG D64 concatenation fixture passed the SEQ and mixed-type cases
and a corrupt-source fail-stop control. Formatted D64, D80 and D82
production-path fixtures passed cross-drive COPY in both directions and the
concatenation cases. The REL-growth fixture passed on D64, D80 and D82, and
the prepared D64 side-sector-boundary fixture passed. Those focused
production-path fixtures do not cover organic growth from record 1 to 121
on all three geometries. The full VICE run covers that growth and the first
side-sector boundary on D64, D80 and D82 using the selected original ROMs.
Physical original-drive and EconoPET hardware qualification remain open.

The geometry cases use D64 17/20, 19/18, 24/18, 25/17, 30/17,
31/16, and 35/16; D80 39/28, 40/26, 53/26, 54/24, 64/24, 65/22,
and 77/22; and, on D82, additionally 78/28, 116/28, 117/26, 130/26,
131/24, 141/24, 142/22, and 154/22. Quick format clears the files
before these writes. D64 track 18 and D80/D82 track 38 are avoided because
they contain filesystem metadata. D80/D82 39/28 is not the header or first
directory sector and is unused after quick format. These tests are not a sweep
of every sector, nor do they check invalid geometry or every BAM part. The
primary sources are the pinned
[4040 table](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/romtbl)
and [8250 table](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/romtbl).
MAME's [4040](https://github.com/mamedev/mame/blob/master/src/lib/formats/c4040_dsk.cpp)
and its [D64 base](https://github.com/mamedev/mame/blob/master/src/lib/formats/d64_dsk.cpp),
and [8050](https://github.com/mamedev/mame/blob/master/src/lib/formats/d80_dsk.cpp)
formats corroborate first-side geometry, and its
[8250 format](https://github.com/mamedev/mame/blob/master/src/lib/formats/d82_dsk.cpp)
uses two heads. The full VICE run covers the selected geometry boundaries,
including both D82 sides, using the selected original ROMs. Second-side
physical hardware evidence remains outstanding. The complete production-path
geometry fixtures have not yet completed: a three-geometry run was stopped
after more than 20 minutes. Those dedicated simulator cases are temporarily
disabled, while the geometry phase remains enabled in the PRG and its VICE
run. The separate D64 raw-block and alias regression passed.

The partial PRG uses the CC65 PET target (`-Os -Cl`) as an ordinary 32K
base-RAM program (no `$FFF0` banking), with assembly stream send/receive/EOI
helpers and in-place screen progress updates, and is checked to load entirely
below `$7FF0` (reserving `$7FF0-$7FFF` for the target diagnostic mailbox).

IEEE handshakes have finite software-loop deadlines (~0.8s default per edge at
1 MHz, with a longer deadline around format, validate, duplicate and side-sector
growth). Their real-machine timing still requires hardware qualification.

## Evidence boundary

The status layout, scratch count, block commands, command dispatch, and
buffer pointer follow the selected `DOS_4040` and `DOS_8250` source units
`erproc`, `scrtch`, `block`, `record`, `rel1`, `parsex`, `romtbl`, `tst2`,
`tst4`, and `verdir`.
Directory and BAM expectations follow `lstdir`, `trnsfr`, `new`, `map`,
`verdir`, and `tstfnd`. The 8050/8250 BAM expectations also follow `begin`,
`romtbl`, and `frets`. Short-file EOF and append expectations follow `close`,
`opchnl`, `tst2`, and `open` in both selected ROM trees. Cross-drive COPY follows
`copysetup`, `setdrv`, `lookup`,
and `rename`, while typed file opens follow `open:CKTM` and `romtbl:TPLST` in
both selected DOS source trees. The 4040 revision overlays are in
`tools/drive-roms/drive-emulation-roms.json`. The transport follows the PET
ROM 4 IEEE handshake sequence. The full VICE run executes these implemented
checks against the selected original drive ROMs. Execution on supported
EconoPET hardware remains outstanding.
VICE's block-command handlers corroborate the counted and raw operations
without establishing exact cursor behavior (see `docs/dev/drive-direct-block-reset.md`).

Production FPGA/firmware simulations exercise the production virtual-tape
callback and `LD210` buffer restore (`tape_callback_test.cpp`), and focused
linked-PRG IEEE transport/stream EOI, `UNTALK` pending-byte cancellation,
missing-talker timeout release, and initial format startup
(`drive_prg_transport_test.cpp`). Full DOS command/media matrix vetting of
`drive-test.prg` runs in seconds under `xpet -console -warp` (`vet_vice.py`)
prior to execution on physical EconoPET hardware. Physical write protection,
damaged media, power-loss durability, and electrical-edge timing require
separate qualification.

## Build and VICE vetting

`cmake --build --preset prgs` builds `build/prgs/drive-test/bin/drive-test.prg`.
The separate SD-card integration installs it at `/test/drive-test.prg`.
Scratch disk images are generated in a temporary directory during VICE vetting
and, with the firmware diagnostic integration, on the MCU during `test drive`
(under `/test/tmp`).

To vet `drive-test.prg` against the pinned PET ROM 4 and 4040/8050/8250 drive
ROMs in headless VICE:

```sh
python3 prgs/drive-test/vet_vice.py --download-roms
```
