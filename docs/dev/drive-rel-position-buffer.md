# Clean REL POSBUF data-window foundation

This slice follows clean NRBUF in #125. It adds internal data-window
positioning after the caller has resolved SSPOS, not public RECORD or side/
super/group lookup. It does not change production facade or legacy routing.

## Included contracts

`dos_rel_channel_position_buffer` accepts an owned non-directory clean reader
in READY or MISSING state. The caller must retire its old FIFO sources and
resolve the requested track/sector first. Busy, failed and non-READ owners
are rejected without mutation. The primitive retains pooled data/data/side
bytes and headers, raw record identity, model and media generation. It clears
the missing event and 4040 GETFLG without adding a DOS 2.7 GETFLG operation.
Command-level overflow/missing status and complete RECORD admission remain
outside this primitive.

The worker first performs BHERE against the current saved track/sector.
A matching header retains current bytes, including completed NONE/WRITE-job
bytes (not dirty REL ownership). Otherwise clean SCRUB has nothing to write.
A nonterminal outgoing link allows alternate BHERE. A terminal outgoing link
skips that match, but the selected revision-2 P80 still rotates before STRDBL.
Cold refill and alternate reuse preserve that rotation and the selected
page's forward link.

Each pass makes one decision or at most one READ. Required READ and selected
alternate failures stop without automatic retry, preserving owned pages,
failed headers, phase and raw counter. They report the host READ ERROR 23
policy with attempted track/sector and the owner's drive. Unused prefetch
I/O/range failure remains deferred. A generation mismatch stops before any
replacement read and reports the existing host error at zero coordinates.

POSITN/P2/P30/NXOUT and cached SETLST/FNDLST prepare the requested byte without
an actual GET or extra record increment. Successful work preserves retained
status and its unread cursor. The side page is not changed or read. Clean
CLOSE cancels pending work and releases owned resources, but is not full
native CLSREL.

Seven phase tags are appended after clean NRBUF. No field, private page,
heap or queue is added. All 29 leaf helpers, all 63 merged dependency test groups,
the stale-batch safeguards and the 4608-byte unit bound remain unchanged.

## Native evidence

Targets remain 4040 DOS 2.0 revision 2 (`901468-14/-15/-16`) and full-size
8050 MPI single-sided / 8250 MPI double-sided DOS 2.7
(`901887-01/901888-01`). Both pinned master compositions were consulted.
The selected private 4040 mapped POSBUF was checked against the approved
revision-2 correspondence. Its original-source overlay ranges are
`record:124-126,135,149`, documented in the
[revision-2 mapping](drive-emulation.md#revision-2-differences-that-affect-implementation).
The older unmodified 4040 source has different P80 sequencing and is not
claimed equivalent. No private generated assembly or binary is published.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Current/alternate BHERE and cold rotation | All selected targets, with approved 4040 revision-2 overlay | [4040 record][4040-record] / [DOS 2.7 record][8250-record], POSBUF/BHERE/SCRUB/P80/STRDBL | Current header match reuses bytes without READ. Otherwise try alternate only for a nonzero outgoing link. P80 rotates before cold refill | `rel_position_work`, 24 reuse cases cover current, alternate, both cold rotations, terminal alternate, raw zero and completed WRITE-header bytes | [VICE position][vice-position] corroborates header-based dual-buffer reuse but omits native P80/SCRUB sequencing. [MAME 4040][mame-4040] identifies revision-2 chips, not execution of this C path |
| Required page wait, prefetch and headers | All selected targets | Both record POSBUF and [4040 tst2][4040-tst2] / [DOS 2.7 tst2][8250-tst2], DBLBUF/WATJOB/STRDBL, [4040 tsutil][4040-tsutil] / [DOS 2.7 tsutil][8250-tsutil], RDAB/GETHDR/SETHDR | Stop selected failed pages. Refill current target before prefetch, preserve READ drive/coordinates, defer only unused prefetch failure | Failure/cancellation cases assert current target error, failed alternate, deferred following data and zero WRITE/flush | VICE postpones some next-page reads and therefore corroborates ownership, not exact eager ordering. [MAME MPI][mame-mpi] establishes selected 8050/8250 hardware modes |
| Raw identity, flags and byte readiness | All selected targets | Both record R30/R40/POSITN/P2/P30, [4040 rel4][4040-rel4] / [DOS 2.7 rel4][8250-rel4], SETLST/FNDLST, both rel3 RD15/RD05 | Retain raw record zero while addressing record one. Normalize position, clear 4040 GETFLG only, prepare the requested byte/EOI without GET. Success retains command status | Reuse cases check raw zero, position two, flags, selected byte, side-page identity and status cursor. Existing counter/missing-byte leaf cases unchanged | VICE normalizes through its own high-level record indexing and scans zero tails. It does not establish native raw wrapping or the C scheduling boundary |
| Bounded host scheduling, lifetime and source ownership | Explicit host adaptation supporting all targets | No direct ROM entry for generation/source tokens or C phase tags. Native POSBUF/DBLBUF establish required page/byte ordering | One READ per pass, no replacement reads, no writes/flushes, cancel pending work, preserve another owner's retained prefix | Three failure/remount/CLOSE cases per relevant variant, three peer-prefix cases and unchanged scheduler/layout assertions | MAME model declarations do not prove host generation guards, generic status 23 mapping or cooperative cancellation |

## Validation and exclusions

The isolated candidate passes 642 focused REL cases in 66 groups and all
5059 firmware cases (5012 ordinary plus 47 fatal/assert). Tests/frameworks
and dependencies use Debug. RP2040 production RelWithDebInfo, clang-format,
whitespace and the unit bound pass. The 42 new cases cover only this clean
data-window primitive. No prior expectation or registration is changed.

Full resident SSTEST/SSPOS/SSEND and 8250 group traversal, dirty positioning/
writeback, explicit retry, public RECORD/OPEN, WRITE/PUT integration, creation/
growth, full CLSREL and production cutover remain later slices. This is not
fresh full-ROM execution, board acceptance or an electrical/timing claim.

[4040-record]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/record
[8250-record]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/record
[4040-tst2]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
[8250-tst2]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
[4040-tsutil]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tsutil
[8250-tsutil]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tsutil
[4040-rel4]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel4
[8250-rel4]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel4
[vice-position]: https://github.com/VICE-Team/svn-mirror/blob/a0e14df25846cd7f06043f24b48677bf45c02149/vice/src/vdrive/vdrive-rel.c#L1115-L1324
[mame-4040]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c2040.cpp#L142-L150
[mame-mpi]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c8050.cpp#L79-L80
