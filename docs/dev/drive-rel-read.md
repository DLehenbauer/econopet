# REL index and clean-read foundation

This dependency slice follows FORMAT/DUPLICATE in #118. It adds reusable REL
arithmetic, side/super-side lookup, last-index traversal, clean-record cursors
and bounded byte/EOI preview with separate actual-GET accounting. It does not
enable REL OPEN or P commands, connect REL channels to IEEE transport, or
change the production facade, legacy engine or flat HDD support.

## Scope and ownership

`dos_rel.h` exposes independent cursors. Each preparation step reads at most
one sector. The caller lends a side page and two independent data pages and
keeps their contents stable between preparation and consumption. No heap,
private sector buffer, whole-file index or record-length cache is introduced.
Compile-time bounds limit lookup/end/record/reader cursors to 24/32/36/40 bytes.

Lookup `FOUND` exposes coordinates, not ready file data. `NO_INDEX` requires
the future owner's SSEND/POSBUF fallback, not an immediate error 50.
`NEEDS_LINK` explicitly hands unprepared NRBUF work back to the caller.
These results are boundaries between independently usable primitives, not
successful public REL requests. The standalone end cursor does not itself
complete a RECORD request.

The clean-record cursor follows a current sector's forward link for read-ahead.
A failed read-ahead is retained until SETLST, GET or NXTREC actually needs it.
Preview copies the cursor and stops at EOI, the staging limit or an unprepared
page. Only actual GET changes the raw record counter and GET history.
An already-missing record previews CR with EOI. Only actual consumption of
that missing byte sets `no_record`, the future owner's error-50 event. Good
EOI transitioning to missing state does not set that event.

Captured media generations protect reads and cached bytes against replacement.
Cancel/restart only discard cursor state. They neither release borrowed pages
nor flush storage or close channels. These APIs require serialized ownership
and do not add concurrent media-change synchronization.

## Selected evidence

The targets are 4040 DOS 2.0 revision 2 (`901468-14/-15/-16`) and full-size
8050 MPI single-sided / 8250 MPI double-sided DOS 2.7
(`901887-01/901888-01`). Both [4040 master][4040-master] and
[DOS 2.7 master][8250-master] were consulted at the pinned source revision.
The [approved ROM manifest][manifest] supplies the 4040 revision-2 `record`
overlays. Resident/dirty POSBUF reuse and that overlay's active/inactive-page
sequencing belong to future channel integration, not this cold clean cursor.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Record addressing | All selected targets | [4040 record][4040-record] / [DOS 2.7 record][8250-record], R30/R40, and [4040 fndrel][4040-fndrel] / [DOS 2.7 fndrel][8250-fndrel], MULPLY/DIV254/DIV120/FNDRE1/DIV006 | Record/position zero alias one for addressing. Excessive nonzero position restarts at the record beginning with overflow retained. Skip two data-link bytes and index 120 data sectors per side | `dos_rel_address`, `test_record_address_boundaries`, wrapped-index tests | [VICE position][vice] corroborates zero aliases and 254-byte payload addressing, but returns overflow before native restart and does not model 4040 DIV120 folding |
| Model-specific arithmetic | 4040 revision 2 versus DOS 2.7 modes | Respective fndrel DIV150..DIV700 versus DIVID1/DIVID2, DOS 2.7 system BIGREL | Preserve 4040's folded remainder, including index 12 for record 65534/size 127, versus DOS 2.7 index 28. Only 8250 splits sides into super groups | `dos_rel_address`, boundary/wrapped-index/last-group tests | [MAME 4040][mame-4040] and [8050/8250][mame-8050] corroborate selected ROM identities/modes, not a mathematical correction to native arithmetic |
| Side/super-side traversal | All selected targets, super groups only for 8250 | [4040 sstest][4040-sstest] / [DOS 2.7 sstest][8250-sstest], ST20/ST40/ST50, [4040 ssutil][4040-ssutil] / [DOS 2.7 ssutil][8250-ssutil], SSSET/SSPOS, [DOS 2.7 system][8250-system] RDSS1C/RDSS1B | Use byte 3 super-group table and byte 4 side table. Header equality precedes six-side rejection. SSPOS repeats SSTEST after the selected read: negative ER2/ER3 requires SSEND, while a second ER1 is accepted without another read. ER1 checks only the side-table track, so even a zero data track reaches subsequent data-read geometry validation. Sector zero remains valid | `dos_rel_lookup_step`, side-table/super-group, post-read ER0/ER1/ER2/ER3, record fallback/geometry, failure and cancellation tests | VICE's REL implementation independently uses side/super-side pointer tables, but caches whole tables and is not authority for native malformed-header behavior |
| Last-index traversal | All selected targets, RDLG only for 8250 | [4040 ssend][4040-ssend] / [DOS 2.7 ssend][8250-ssend], SE10/SE20/SE30, system RDLG/RDSS1 | Scan side table backwards. Reuse matching resident header, otherwise read selected last side. Reject nonzero terminal track. Preserve endpoint DEY wrapping. 8250 end search starts at group 90 even though arithmetic can address 91 | `dos_rel_end_step`, last-side/group, corrupt-index, empty-super, storage/generation and restart tests | VICE independently derives last data location from side-sector endpoint, but computes/caches file length and does not establish RDLG's starting group. MAME corroborates model selection |
| Clean record and EOI | All selected targets | Both record POSITN/P2/P30, respective rel1 NXOUT/NXTR45/NXTR50, [4040 rel4][4040-rel4] / [DOS 2.7 rel4][8250-rel4], SETLST/FNDLST | Scan trailing zeroes with byte-wide counters and low-byte LSTCHR equality. Preserve cross-page and positioned-past-trimmed-data behavior. Exact final-page endpoints remain readable, truncated records become missing | `record_prepare`, preparation/actual-GET, incomplete-boundary and native-pool-page tests | VICE position/read corroborates trailing-zero trimming and documents the positioned-read overflow family, but its widened-pointer algorithm does not establish each selected-ROM byte edge |
| Read-ahead and record continuation | All selected targets | Both tst2 STRDBL/DBLBUF and rel1 NRBUF/NRBU50/NRBU70/NXTREC, respective rel4 SETLST | Prepare alternate data without observing unused job failure. Selecting it for end scan/GET/NXTREC observes the retained failure. Next-record handoff retains the native low-byte NR comparison | `dos_rel_record_step/get/next`, deferred-prefetch, next-stream, generation and peer-isolation tests | VICE independently uses current/next data buffers but reads on demand. Native job-selection/error ordering comes from the selected ROMs, with MAME's shared-RAM hardware model as context |
| Preview versus actual GET | 4040 GETFLG versus DOS 2.7, all selected missing-record paths | Both ieee TLK10/TLK30, [4040 rel3][4040-rel3] / [DOS 2.7 rel3][8250-rel3], RD10/RD30/RD40/RD05, both rel1 NXTREC | Preview does not commit counters, GET history or error 50. Actual good EOI increments the raw 16-bit record number with wrap. Actual missing GET retains CR/EOI and requests error 50 repeatedly | `dos_rel_reader_stage/get`, preview-prefix/missing/reposition, raw-counter/wrap, interrupted-prefix and deferred-error tests | VICE read independently returns CR with missing-record error. Its high-level API does not separate FPGA preview from GET and is not authority for that adaptation |

The independent sources corroborate structure and model selection, not complete
native equivalence. In particular, VICE's high-level vdrive implementation is
not the selected drive's ROM-level execution and differs in overflow policy,
buffering, arithmetic edges and error observation.

## Adaptations and exclusions

Cooperative one-read steps, captured generations and bounded FIFO preview are
host mechanisms. Empty 8250 super tables return explicit `END_NO_GROUP` rather
than emulating malformed RDLG underflow into unrelated RAM. The caller must
decide how to surface that safeguard. Terminal helper results retain their
captured outcome and do not silently retry storage errors.

Selected-side lookup repeats the native range decision without following a
second side-table link. A mismatched selected header is not by itself invalid:
SSPOS accepts a second ER1. When that branch carries a zero data track, lookup
returns its coordinates and the record cursor reports the later data read's
geometry failure, not `NO_INDEX` or a fabricated missing-record byte.
SSEND's separate selected-last-side path still uses its native link/endpoint
checks rather than inheriting SSPOS's second SSTEST.

Excluded: channel allocation/teardown, public OPEN/P dispatch, SSEND-to-POSBUF
owner continuation, cached/dirty-page reuse, unbuffered NRBUF jobs, record
creation/growth, LISTEN/write/padding, completion/CLOSE, REL COPY, status-line
publication and transport integration. No new gateware, board reset, electrical
timing or latency behavior is claimed. No ROM binaries are included.

## Validation

The isolated candidate passes 290 focused REL cases (31 test groups) and 4707
total firmware cases (4660 ordinary plus 47 fatal/assert). Tests and their
dependencies use Debug. The RP2040 production RelWithDebInfo build,
clang-format and whitespace checks pass. Existing DOS sources, headers,
tests and registrations remain unchanged from final merged #118, including
FORMAT failure/cache ownership, native metadata ordering and backing-image
alias rejection.

Feedback regressions add 21 model/scenario variants for post-read range
decisions and the record cursor's SSEND/geometry handoff. The final fixtures
linked against initial PR production reproduce 12 failures with zero errors.
Controls retain acceptance of a second ER1 without another read and header
equality before the side-table decision.

These are source-derived host contracts, not fresh full-ROM controller
comparisons, physical acceptance, or a claim that public REL reads are enabled.

[4040-master]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/master
[8250-master]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/master
[4040-record]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/record
[8250-record]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/record
[4040-fndrel]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/fndrel
[8250-fndrel]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/fndrel
[4040-sstest]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/sstest
[8250-sstest]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/sstest
[4040-ssutil]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ssutil
[8250-ssutil]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ssutil
[4040-ssend]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ssend
[8250-ssend]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ssend
[8250-system]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/system
[4040-rel3]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel3
[8250-rel3]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel3
[4040-rel4]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel4
[8250-rel4]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel4
[manifest]: ../../tools/drive-roms/drive-emulation-roms.json
[vice]: https://github.com/VICE-Team/svn-mirror/blob/a0e14df25846cd7f06043f24b48677bf45c02149/vice/src/vdrive/vdrive-rel.c#L955-L1355
[mame-4040]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c2040.cpp
[mame-8050]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c8050.cpp
