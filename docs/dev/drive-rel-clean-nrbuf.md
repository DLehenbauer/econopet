# Clean REL NRBUF continuation foundation

This slice depends on the initial-read ordering in #124. It joins clean
NRBUF to internal actual GET and NXTREC without enabling public REL OPEN,
RECORD or writes. Legacy/HDD routing and production facade selection remain
unchanged.

## Included contracts

REL preview stops at the current page seam as well as capacity/record EOI.
Preview remains side-effect free. Actual GET captures the outgoing link and
queues the clean continuation before any dependent bytes can be staged.
Six appended phase tags distinguish GET from already-counted NXTREC, using
existing per-channel cursor and pooled pages. No field, page or write queue
is added. All 29 leaf helper definitions and the 4608-byte unit bound remain.

The clean phase uses an already-read alternate without another read. A
NONE-job page retains native bytes even at a terminal link. A reused WRITE
header requires READ of the saved outgoing link, and a failed required READ
stops the owner without retry. The following phase reads ahead only for a
nonzero forward track. Each pass performs at most one read, with no WRITE or
flush. Attempted coordinates, drive bits and READ headers remain observable.

Unused prefetch I/O/range failures stay deferred while valid current data
remains readable. NXTREC resumes SETLST/FNDLST against completed window bytes
without repeating actual GET or the raw record increment. Successful work
does not replace retained status. Missing CR/EOI still reports error 50 only
on actual GET of the already-missing byte.

Host generation checks stop queued work after media replacement without
reading the replacement. Final #123's stale staged-batch safeguards remain:
the first terminal failure stops acknowledgments, and later acknowledgments
cannot consume retained failed-owner state. Existing clean CLOSE cancels a
queued continuation and returns its resources. That host cancellation is
not complete native CLSREL.

The preceding terminal NEEDS_LINK diagnostic is intentionally superseded
only for these clean GET/NXTREC joins. Cold SSEND/NO_INDEX still reports the
explicit host incomplete-path syntax diagnostic, not a native missing record.

## Native evidence

Targets remain 4040 DOS 2.0 revision 2 (`901468-14/-15/-16`) and full-size
8050 MPI single-sided / 8250 MPI double-sided DOS 2.7
(`901887-01/901888-01`). Both pinned master compositions and relevant
NRBUF, RDREL, SETLST and job helpers were consulted. Approved 4040 RECORD/
GETBUF overlays do not justify unchanged older-source POSBUF parity.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Clean alternate selection and terminal NONE | All selected targets | [4040 rel1][4040-rel1] / [DOS 2.7 rel1][8250-rel1], NRBU50/NRBU70, [4040 tstflg][4040-tstflg] / [DOS 2.7 tstflg][8250-tstflg], TSTWRT | Toggle alternate and refill only a WRITE-job page. A NONE job is not itself a missing record. Preserve required-job failure | `rel_queue_nrbuf/rel_nrbuf_work`, three terminal cases and nine refill/failure/CLOSE cases | [VICE REL read/position][vice-read] corroborates dual-buffer ownership, but its file-level record limits do not override native low-byte/NONE-job behavior |
| Read-ahead and saved headers | All selected targets | Both rel1 NRBU70, [4040 tsutil][4040-tsutil] / [DOS 2.7 tsutil][8250-tsutil], RDLNK/RDAB/SETHDR/LSTJOB | Read the selected page's forward link, submit inactive READ only for nonzero track, preserve drive/coordinates and deferred unused failure | Nine imported clean OPEN/NRBUF cases cover success, deferred failure and replacement for all models. Refill cases check READ bit/header retirement and zero writes/flushes | VICE sometimes delays next-page reads, supporting separate ownership rather than proving native eager-job timing |
| GET versus NXTREC and cached tail scan | All selected targets | [4040 rel3][4040-rel3] / [DOS 2.7 rel3][8250-rel3], RD10/RD40/RD05, both rel1 NXTR15/NXTR20/NXTR35, [4040 rel4][4040-rel4] / [DOS 2.7 rel4][8250-rel4], SETLST/FNDLST | Retire actual GET before work, resume an already-counted NXTREC without recounting, preserve last-byte/missing timing | Page-bounded `dos_stage`, `dos_get`, `rel_next_cached`, unchanged leaf/counter/error-50 groups, initial-window continuation tests | VICE scans record tails but is not evidence for native raw wrapping counters |
| Host FIFO/lifetime and cooperative work | Host adaptation supporting all targets | No direct ROM entry for C source tokens, phase tags or generations. Native NRBUF/DBLBUF/WATJOB establish the required read/byte order | Preview does not submit jobs. One read per pass. Stale queued work never reads replacement. Stop first failed staged acknowledgment. CLOSE releases only owned pages | Existing 36 final #123 stale-batch cases retained unchanged, remount and pending-CLOSE cases, static unit bound | [MAME 4040][mame-4040] / [8050/8250][mame-8050] establish selected chips/MPI modes, not C scheduling or hardware acceptance |

## Validation and exclusions

The isolated slice passes 588 focused REL cases in 62 groups and all 5005
firmware cases (4958 ordinary plus 47 fatal/assert). Tests/frameworks and
dependencies use Debug. RP2040 production RelWithDebInfo, clang-format,
whitespace and the unit bound pass. All 59 dependency groups/registrations
remain, with only the three terminal-NRBUF expectations intentionally
adapted. Nine future clean-NRBUF cases use the existing memory-backed fixture
without future dirty-state fields. Nine added cases cover reused WRITE
refill, required READ failure and pending CLOSE.
Three additional terminal NXTREC cases verify that NR is selected before
cached SETLST, without repeating the record increment.

Public REL admission/dispatch, resident/dirty RECORD/POSBUF, full SSEND,
dirty NRBUF/writeback/retry, PUT integration, creation/growth, full CLSREL
and production cutover remain later slices. No fresh full-ROM execution,
board acceptance, electrical or timing claim is made.

[4040-rel1]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel1
[8250-rel1]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel1
[4040-rel3]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel3
[8250-rel3]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel3
[4040-rel4]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel4
[8250-rel4]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel4
[4040-tsutil]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tsutil
[8250-tsutil]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tsutil
[4040-tstflg]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tstflg
[8250-tstflg]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tstflg
[vice-read]: https://github.com/VICE-Team/svn-mirror/blob/a0e14df25846cd7f06043f24b48677bf45c02149/vice/src/vdrive/vdrive-rel.c#L1115-L1324
[mame-4040]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c2040.cpp
[mame-8050]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c8050.cpp
