# REL creation, growth and CLOSE

This larger dependency batch follows the existing-record core in #128.
It joins native storage jobs through internal owners, without enabling
public REL OPEN, RECORD, queued LISTEN input or REL COPY/SCRATCH/VALIDATE.

## Ownership and boundaries

`dos_rel_channel_create` accepts a fresh claim and an already-allocated first
data sector. It schedules side, optional 8250 super-side, data and BAM writes.
The three pooled data/data/side pages remain owned throughout. Directory
ADDFIL publication and public OPEN acceptance belong to the integration batch.
Raw record sizes are retained, including the native RS=0 NULBUF loop, implemented
as bounded pending work rather than an MCU-wide hang.

`dos_rel_channel_grow` accepts a completed missing-record result without
accepting an input byte. SSEND/POSBUF precede native allocation preflight.
ADDREL/NEWSS preserve byte arithmetic, allocation order, side-table updates,
8250 super-group publication and final repositioning. Completed allocations,
header coordinates and descriptor swaps survive explicit retry. Growth does
not perform the additional host backing-store flush.

`dos_rel_channel_close` completes CLSREL, not CLREC. It SCRUBs the active
dirty page, swaps and awaits the alternate, computes the native SSEND block
count and performs CLSDIR when required. 4040 always publishes the directory.
DOS 2.7 requires DYFILE. Replacement bit $20 suppresses the normal type edit,
otherwise CLSD5 retains the low nibble and sets bit $80.

`dos_close` and its cooperative continuation retain native descending-SA
CLOSE/CLSALL ordering. Explicit CLOSE 0 uses FRECHN, while a CLSALL visit to
SA 0 still uses CLSCHN. CLOSE 0/1 retain status, higher secondaries use ENDCMD.
Existing ordinary/direct CLOSE and COPY lifecycle behavior is preserved.
Legacy clean internal readers without directory coordinates remain cancellable.
Actual CLSREL requires the caller's saved directory coordinates.

Failed jobs stop polling and retain owners, pages and unfinalized peers.
Explicit creation, growth and CLOSE retries resume only the retained job.
Failed directory writes restart from READ, preserving entries committed by
peers after the failure. Host finalization drains only its current storage
owner and rejects an overlapping command owner.

Every shared sector publisher also retires matching peers' saved CLSDIR
snapshots (including partial host WRITE failures). The affected REL rereads
its directory sector in a separate pass before applying its type/count edit.
This covers ordinary CLOSE and OPEN, directory growth and command publishers
without a new lock or page. Other media identities and sector coordinates
do not match. A failed or replaced-media reread retains the owner for the
existing explicit-retry/lifetime policy.

CLSALL's reader-cancellation shortcut excludes all retained creation, growth
and CLOSE jobs, including failed creation and bounded RS=0 work. Reopening
the same secondary through direct, directory or ordinary OPEN also rejects
a retained storage job with the internal-core syntax boundary before lookup,
AUTOI or page reuse. Native GETRCH normally FRECHNs the old owner, but it has
no counterpart for these suspended host storage jobs. Ordinary owners and
completed clean internal readers retain their existing OPEN/CLOSE behavior.

Captured media identity guards every storage boundary. Replacing either slot
cannot redirect old allocations or writes onto replacement media. A dirty
backing unit is flushed before release (additional firmware durability policy,
not native MAPOUT). A clean DOS 2.7 close need not issue a host flush.

Directory phase remains independent of the REL cache discriminator.
`record.cached_group` stays outside the shared lookup/growth cursor union.
New tags append to the published enums without renumbering existing phases.
No private page, file-sized index or firmware heap allocation is added.
An owner advances cooperatively, although native allocation can traverse
multiple existing BAM parts in one pass. Core read/write and CLOSE fixtures
retain their one-READ/WRITE-per-pass assertions.

## Selected ROM evidence

Targets are 4040 DOS 2.0 revision 2 (901468-14/-15/-16), full-size MPI 8050
single-sided DOS 2.7 and MPI 8250 double-sided DOS 2.7 (901887-01/901888-01).
The [core evidence](drive-rel-position-index.md) remains the dependency's
positioning/read/write authority.

The selected 4040 revision-2 OW20 tail calls MAPOUT after WRTOUT/WATJOB.
The pinned archived 4040 `opchnl` omits that call, unlike the selected bytes.
The mapped selected-image SHA-256 used for this check is
`08bff8efbf73a1a9b03f87696e96377903c918a1560c66ca269cf597d43fe166`.
DOS 2.7 `opchnl:OW21` explicitly contains MAPOUT. Private ROM images and
generated assembly are not part of this batch.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Initial storage construction | Selected 4040, 8050 and 8250 | [4040 opchnl][op40] OPNWCH/OW20 (selected tail above), [DOS 2.7 opchnl][op27] OW20/OW21/OWBREL | Side header `[0,17,0,RS]`, first data pointer at 16/17, optional `[track,sector,254]` super root, then data and BAM WRITE | `rel_create_work`, `test_pipeline_create_native_pages` | [VICE][vice] allocation structure, [MAME 4040][m40] and [8050/8250][m27] mode/ROM selection |
| Native empty-record bytes | All selected models | [4040 NULBUF][nul40] and [DOS 2.7 NULBUF][nul27] NB20/ADDNR, opchnl OW21/NULLNK | Retain byte-wide NR and link adjustment, seed $FF markers and NULLNK's NR-1. RS=0 stays pending | `dos_rel_fill`, creation-size cases and `test_storage_zero_size_creation_remains_bounded` | VICE corroborates empty records, not the exact native NR loop |
| Growth preflight and persistence | 4040/8050 small REL, 8250 big REL | [4040 ADDREL][add40], [DOS 2.7 ADDREL][add27] ADDREL/ADRELS/ADDR1 | Complete SSEND/POSBUF before preflight, preserve native wrapping and side-boundary undercount. Reject capacity with 52 before allocation | `rel_growth_step`, retained `test_growth_native_preflight`, capacity and native-page cases | VICE uses higher-level capacity/cache logic, not this arithmetic authority |
| Side and super-group expansion | All selected models, 8250 big-REL branch | [4040 NEWSS][new40], [DOS 2.7 NEWSS][new27] NS20/NS40/NS50/NEWSS4, [SYSTEM][sys27] RDSSSA/WRTSSS/RDSS1D | Publish new side, repair old side tables, and publish/read 8250 super-group links without losing pooled descriptors | Native growth pages and ten retained persistence-phase failure variants | VICE's data/side/super graph, MAME's hardware-mode distinction |
| CLSREL and directory publication | 4040 versus DOS 2.7 DYFILE | [4040 CLOSE][cls40] CLSREL/CLSDIR, [DOS 2.7 CLOSE][cls27] CLSREL/CLSBIG/CLSRE1/CLSD5 | SCRUB without padding, await alternate, publish native low-word block count. DOS 2.7 skips clean CLSDIR. Preserve replacement-bit handling | `rel_close_work`, native close, failure, peer-directory and replaced-media cases | VICE corroborates commit/release, but its `vdrive_rel_close` pads through `fillrecord` (not native CLSREL) |
| Ordered close and failure retention | All selected models | CLOSE/CLSALL/CLSCHN/FRECHN in both CLOSE units | Explicit 0 bypasses CLSREL, CLSALL orders descending SA including 0, SA 0/1 retain status. Failed owners and later peers survive | Storage CLOSE/CLSALL and finalize-owner/peer cases, full ordinary regression suite | MAME executes the selected ROMs. Host retries/flush are explicitly additional policies |
| Fresh directory publication across host yields | All selected models | Both CLOSE:CLSDIR -> TST4:DRTRD -> JOBS:DOIT/WATJOB (4040 WATJOB is in TST2), then CLSD5/CLSD6 -> DOIT | A type/count edit must not restore a sector older than a completed peer publication | `invalidate_rel_directory`, staggered REL/ordinary CLOSE, partial WRITE and invalidated-reread regressions | VICE REL commit/release and MAME selected models, host snapshot retirement is an explicit adaptation |
| Suspended creation ownership | All selected models | Both OPCHNL:OW20/OW21 await side/data jobs before OROW. CLOSE:CLSALL/CLS20 order descending SA. TST3:GETRCH normally calls FRECHN | Host CLOSE/OPEN cannot mistake a suspended creator for a disposable reader or lose its allocation/retry state | `close_work`, `dos_open`, pending/failed/RS=0 CLSALL and three OPEN-path regressions | VICE completes construction/commit before releasing buffers, explicit host retention is not a native OPEN error policy |

## Qualification

The isolated candidate passes 1,619 REL cases in 138 groups and 6,036 firmware
cases (5,989 ordinary plus 47 fatal/assert). It adds 303 cases in 21 groups.
All 115 unaffected core groups remain token-identical. Two existing groups
change only where full CLOSE intentionally replaces the core's temporary fence.
All 29 `dos_rel.c` leaf helpers remain byte-identical to #128.
The reviewed #123/#125/#126 GET, NXTREC and required-prefetch engine seams
and final #128's retry, cold-version-cache and ownership fixes are preserved.

Debug tests/frameworks/dependencies and RP2040 RelWithDebInfo production pass.
clang-format and whitespace checks pass. Actual compiler measurements remain
record/channel/unit 36/68/4580 bytes on the Debug host and 32/64/4544 bytes
on ARM, below the 4608-byte unit bound.

This is storage/core qualification, not fresh full-ROM, public bus-transaction,
board or electrical acceptance. Those public integration and acceptance
contracts remain the next two larger batches.

[op40]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/opchnl
[op27]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/opchnl
[nul40]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/nulbuf
[nul27]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/nulbuf
[add40]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/addrel
[add27]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/addrel
[new40]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/newss
[new27]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/newss
[sys27]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/system
[cls40]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/close
[cls27]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/close
[vice]: https://github.com/VICE-Team/svn-mirror/blob/a0e14df25846cd7f06043f24b48677bf45c02149/vice/src/vdrive/vdrive-rel.c
[m40]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c2040.cpp
[m27]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c8050.cpp
