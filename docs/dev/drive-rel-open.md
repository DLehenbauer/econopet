# REL initial-read ordering foundation

This dependency slice follows clean REL scheduling/output in #123. It adds
internal initial-read preparation and three bounded OPNRCH job phases.
Public REL OPEN and RECORD remain disabled. Production facade routing,
the legacy engine and flat HDD support remain unchanged.

## Included contracts

`dos_rel_channel_prepare_open` accepts a fresh claimed clean reader and
validated directory data/side coordinates and nonzero record size. It
captures those values without I/O or successful-status replacement. Listings
are excluded even when their file-type filter is REL.

`rel_open_work` reads the directory-linked data page first, then its linked
following page when present. It does not obtain the initial data coordinates
from a side-sector pointer. For 4040/8050 it subsequently reads the side page,
without interpreting its index. For 8250 it retains the super-root coordinates
without reading the super page or publishing a resident group.

The final phase enters the existing RD40/NXTREC leaf from raw record zero,
next pointer two and data pointer zero. It prepares record one at byte two
using the directory-linked window. Terminal sizes 254/255 retain native
exact-wrap/missing behavior. Preview of the initial missing CR/EOI does not
report 50. Only actual GET of that already missing byte does.

Each initial-work pass performs at most one media read and does no writes or
flushes. Saved page coordinates, last-job drive and READ bits identify each
attempt. Failed current/side jobs retain an unreadable failed owner. A failed
unused following-page read is deferred, and its partial bytes never become
resident. Host generation checks reject replacement media rather than reading
it or publishing cached old bytes. Success leaves retained status unchanged.
Existing clean CLOSE cancels pending jobs and returns only owned resources.
That cancellation is not full native CLSREL completion.

Existing first-track/sector bytes have REL root aliases without changing
channel size. All ordinary fields, directory routing and pool teardown remain.
The 4608-byte unit bound passes. Three phase tags are appended, with no new
result tag, private page, length cache, heap or write queue. All 29 REL helper
definitions and all 55 merged REL test groups/registrations are unchanged.

## Native evidence

Targets remain 4040 DOS 2.0 revision 2 (`901468-14/-15/-16`) and full-size
8050 MPI single-sided / 8250 MPI double-sided DOS 2.7
(`901887-01/901888-01`). Both pinned master compositions were consulted.
The selected 4040 GETBUF overlay waits for the previous job. This host
primitive reserves all three pages before any job, unlike native OPNRCH's
third-page allocation after STRDBL. It claims initial read/byte ordering,
not complete native admission, controller concurrency or POSBUF parity.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Directory-linked initial window | All selected targets | [4040 opchnl][4040-opchnl] / [DOS 2.7 opchnl][8250-opchnl], OPNRCH/INTPNT/OR10, [4040 tst2][4040-tst2] / [DOS 2.7 tst2][8250-tst2], STRDBL/STR1 | Read directory data before side data. Prefetch its following link only when track is nonzero. Keep page headers and owner drive. Side-index coordinates must not replace the initial stream | `dos_rel_channel_prepare_open/rel_open_work`, directory-link/initial-record and per-step order cases | [VICE REL open][vice-open] allocates dual buffers then positions through its cached side index. It corroborates separate ownership, but differs from native initial job order and cannot override OPNRCH |
| Small side versus big super root | Selected 4040/8050 versus 8250 targets | Both opchnl OR20/OROW and DOS 2.7 OR21, [system][8250-system] BIGREL/BIGRE1 | Read 4040/8050 side after data jobs. Retain 8250 super coordinates without an initial super read or resident-group claim | Initial-record cases include an alternate side/root sector and model-specific residency/read counts, per-step order cases distinguish the zero-read 8250 final phase | [MAME 4040][mame-4040] / [8050/8250][mame-8050] establish revision and MPI hardware modes, not execution of this host path. VICE eagerly loads its index representation instead |
| First record and missing-byte timing | All selected targets | Both opchnl OROW -> [4040 rel3][4040-rel3] / [DOS 2.7 rel3][8250-rel3] RD40 -> [4040 rel1][4040-rel1] / [DOS 2.7 rel1][8250-rel1] NXTREC/NXTR20/NXTR45/NXTR50 | Start raw record zero, NR two and pointer zero, then prepare byte two of record one. Preserve exact terminal size 254/255 behavior. Missing preview is CR/EOI without error 50, actual missing GET reports 50 | Existing leaf unchanged, 18 initial-window cases across six size/link/root variants | VICE's first-record setup corroborates record ownership and missing CR behavior, but its high-level maximum-record scan is not native OROW ordering |
| Deferred prefetch and host lifetime safeguards | All targets, labeled host adaptation | Both tst2 STRDBL submit the alternate job without WATJOB, both rel1 NRBUF selects/waits for needed pages | Failed current or small-side jobs stop unreadable. An unused failed prefetch retains valid current bytes. Replacement media receives no reads. Retain owner/status/failed headers instead of retrying automatically | Failed-job/lifetime and peer-isolation cases, existing generation/prefix suites | VICE sometimes postpones next-page reads, corroborating ownership rather than exact eager-job timing. Host generation tokens and generic READ ERROR 23 at zero coordinates have no direct ROM entry |
| Bounded storage and clean cancellation | Host implementation supporting all targets | No direct ROM entry for phase tags or C aliases. [4040 tst3][4040-tst3] / [DOS 2.7 tst3][8250-tst3], FRECHN/RELBUF/RELINX establish resource separation | One read per work pass, no WRITE/flush, no added page, unchanged channel size. CLOSE during either data phase cancels all later jobs and releases clean ownership | `rel_work`, instrumented memory-backed fixture, nine order/cancellation cases and the existing static unit bound | VICE owns distinct REL buffers and release state. MAME model declarations do not prove the C layout or cooperative cancellation |

No ROM binaries or disassembly are included. No new full-ROM execution,
physical acceptance, timing or electrical claim is made.

## Validation and exclusions

The isolated candidate passes 567 focused REL cases (59 groups) and
4984 total firmware cases (4937 ordinary plus 47 fatal/assert). Tests and
dependencies use Debug. RP2040 production RelWithDebInfo, clang-format,
whitespace and the unit bound pass. Three existing future OPEN groups are
imported with only a test-fixture name/signature adaptation. Nine additional
cases verify job-by-job ordering, drive/READ metadata and pending cancellation.

Complete public OPEN allocation/admission and read/write flags, resident/dirty
RECORD/POSBUF, terminal unbuffered NRBUF, full SSEND fallback, binary RECORD
dispatch, writes, creation/growth, dirty retry, full CLSREL and production
cutover remain later slices. NO_INDEX/NEEDS_LINK retain the preceding slice's
explicit host incomplete-path diagnostic, not a fabricated native result.

[4040-opchnl]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/opchnl
[8250-opchnl]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/opchnl
[8250-system]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/system
[4040-tst2]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
[8250-tst2]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
[4040-rel3]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel3
[8250-rel3]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel3
[4040-rel1]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel1
[8250-rel1]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel1
[4040-tst3]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst3
[8250-tst3]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst3
[vice-open]: https://github.com/VICE-Team/svn-mirror/blob/a0e14df25846cd7f06043f24b48677bf45c02149/vice/src/vdrive/vdrive-rel.c#L853-L914
[mame-4040]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c2040.cpp
[mame-8050]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c8050.cpp
