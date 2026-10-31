# Clean REL read scheduling and output foundation

This dependency slice follows the three-page ownership foundation in #122.
It connects the existing clean REL reader to internal channel preparation,
cooperative work and preview/actual-GET output. Public REL OPEN and RECORD
remain disabled. Production facade routing, the legacy engine and flat HDD
support remain unchanged.

## Included contracts

`dos_rel_channel_prepare` restarts an already claimed clean read owner using
the caller's validated record size, side/super-sector root, record and
position. Retire that owner's FIFO copies and borrowed writes first. This
cold restart invalidates its two data pages, captures the model and performs
no media I/O or successful-status replacement. It cannot abandon owned writes.
It is not native OPEN or resident/dirty POSBUF reuse.

`dos_work` services the lowest eligible REL owner only after existing command,
initialization and OPEN work. Each REL step performs at most one sector read,
using the owner's existing three pages rather than the command-work union.
Saved page coordinates, last-job drive and READ bits describe the attempted
job. Failed or partially filled pages do not become resident. An unused failed
prefetch remains deferred until the reader actually needs that page.

`dos_stage` previews a bounded prefix. `dos_get` commits only its consumed
bytes, including record-end counter changes, page handoff and missing-record
events. Preview and the final good record's EOI-to-missing handoff do not
publish error 50. An actual GET of an already missing CR/EOI does.
Media identity/generation failures use the captured model and owner drive
with the existing host generic READ ERROR 23 and zero track/sector.
A valid staged GET batch stops at terminal owner failure, retaining its
status and borrowed pages rather than replaying the remaining count.
Later acknowledgments of that staged prefix also skip reader replay, including
when another preview detected the failure before the first acknowledgment.

`dos_output_stage/get` retains either the resolved logical channel or the
status source across later TALK selections. It adds no lifetime/generation
token. Retire captured sources before channel reuse, repositioning or status
replacement. Staging status does not clear it. Consuming its final CR does.
These wrappers are not yet wired into the production transport.

Listings always use ordinary metadata. Their `file_type` field is a filter
and can equal REL, so scheduler admission also requires no directory state.
Listing dispatch precedes REL dispatch. Existing zero-capacity preview,
ordinary/direct output and status behavior are preserved. The captured model
uses existing channel padding and the 4608-byte per-unit bound still passes.

## Explicit incomplete boundaries

Missing index traversal (`NO_INDEX`) and terminal unbuffered NRBUF
(`NEEDS_LINK`) stop with a retained failed owner and host syntax 30 at zero
coordinates. This is an internal incomplete-path diagnostic, not native
SSEND, missing-record behavior or an enabled public REL result. No automatic
retry, private page, length cache, write queue or new REL result tag is added.

Native initial OPEN ordering, resident/dirty RECORD/POSBUF reuse, terminal
unbuffered NRBUF, full SSEND fallback, binary RECORD dispatch, writes,
creation/growth, dirty retry, full CLSREL and production cutover remain
separate slices. Clean resource release is not full native REL CLOSE.

## Native evidence

Targets remain 4040 DOS 2.0 revision 2 (`901468-14/-15/-16`) and full-size
8050 MPI single-sided / 8250 MPI double-sided DOS 2.7
(`901887-01/901888-01`). Both pinned master compositions were consulted.
The approved 4040 overlays change POSBUF's buffer sequencing and GETBUF's
previous-job wait. This cold host path does not claim unchanged POSBUF or
native controller scheduling parity.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Cold read admission | All selected targets | [4040 record][4040-record] / [DOS 2.7 record][8250-record], POSITN/P30, and [4040 rel3][4040-rel3] / [DOS 2.7 rel3][8250-rel3], RD15 | Preserve owner identity and distinguish read preparation from actual GET. This cold restart does not implement native resident/dirty POSBUF ordering | `dos_rel_channel_prepare`, peer-reposition and clean-reprepare cases | [VICE REL read][vice-read] has separate current/next data ownership, but its high-level positioning is not native job-order evidence |
| One-job continuation and deferred prefetch | All selected targets | [4040 rel1][4040-rel1] / [DOS 2.7 rel1][8250-rel1], NRBUF, [4040 tst2][4040-tst2] / [DOS 2.7 tst2][8250-tst2] STRDBL/DBLBUF | Preserve current bytes while a following page is pending or failed. Do not report an unused failed prefetch as a current read failure | `rel_work`, pending-refill, generation/I/O and job-header cases, `finish_pipeline` bounds every REL step to one read | VICE's two-page read/swap path corroborates data ownership, not the host cooperative schedule or exact eager prefetch timing |
| Missing-byte error timing | All selected targets | [4040 rel3][4040-rel3] / [DOS 2.7 rel3][8250-rel3], RDREL/RD05, [4040 erproc][4040-erproc] / [DOS 2.7 erproc][8250-erproc] NOREC | Prepare CR/EOI without error 50. Actual GET of the already missing byte publishes 50 with zero coordinates, not preview or the last good EOI handoff | REL `dos_stage/get`, prefix/status/missing-GET cases | VICE explicitly returns CR and sets NO RECORD past the last record, but has no speculative FIFO preview |
| Record end and model flags | Selected 4040 versus DOS 2.7 targets | [4040 rel1][4040-rel1] / [DOS 2.7 rel1][8250-rel1], NXTREC/NXTR15, and [4040 rel3][4040-rel3] / [DOS 2.7 rel3][8250-rel3], RD10/RD40 | Advance the raw record counter at actual EOI, retain the selected model's flags and schedule clean page handoff without consuming a preview suffix | Existing reader leaf helpers unchanged, pipeline prefix/refill tests | VICE independently tracks record progress and page swaps, not the ROM's model-specific flag storage |
| Saved job identity and host failures | All targets, labeled host media safeguard | [4040 tsutil][4040-tsutil] / [DOS 2.7 tsutil][8250-tsutil] RDAB/SJ10/RDLNK and both tst2 STRDBL, [4040 parsex][4040-parsex] / [DOS 2.7 parsex][8250-parsex] CMDERR/JCMDER | Preserve each attempted page's track/sector and job drive/READ bit. Stale host identity never publishes failed bytes. Stop staged GET replay at terminal failure without losing pages or status. Generic 23 and unsupported-boundary 30 are host policies, not native controller decoding | `rel_work/rel_read_state`, command-priority/job-header, generation/removal, staged-batch removal/remount through both GET APIs and explicit-boundary cases | VICE tracks per-page coordinates. [MAME 4040][mame-4040] / [8050/8250][mame-8050] establish selected ROM/hardware modes, not execution of these host policies |
| Preview/commit ownership | All selected targets, host FIFO adaptation | [4040 ieee][4040-ieee] / [DOS 2.7 ieee][8250-ieee] TLK10/TLK30 and [4040 tst4][4040-tst4] / [DOS 2.7 tst4][8250-tst4] GET/GETERC | Commit the captured channel or status source even after another TALK selection. Clear status only after its final CR is consumed | `dos_output_stage/get`, peer/status, ordinary-source and interrupted-prefix cases | VICE corroborates separate data/error channels but does not model this hardware FIFO retirement contract |
| Directory and ordinary isolation | All selected targets, host union discriminator | [4040 lstdir][4040-lstdir] / [DOS 2.7 lstdir][8250-lstdir], STDIR/GETDIR | A REL type filter still produces directory bytes and final EOI, never REL record state. Zero-capacity preview changes no reader/listing state | Directory routing guard, existing relative-filter/reopen/zero-capacity regressions and ordinary-source cases | Native listing source is authoritative. VICE/MAME model corroboration is not evidence for the C union layout |

These are source-derived leaf contracts plus explicitly labeled internal host
adaptations. No new ROM binaries or disassembly are included. No fresh
full-ROM execution, physical acceptance, timing or electrical claim is made.

## Validation

The isolated candidate passes 525 focused REL cases (55 test groups) and
4942 total firmware cases (4895 ordinary plus 47 fatal/assert). Tests and
dependencies use Debug. The RP2040 production RelWithDebInfo build,
clang-format and whitespace checks pass. All 29 REL helpers remain unchanged.
Existing engine helpers are unchanged except the four scheduler/output entry
points. All 47 prior REL groups and registrations remain, with one synthetic
peer explicitly parked as failed so ordinary OPEN does not schedule its
artificial pending cursor. No owned-page/peer assertions are removed.

[4040-record]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/record
[8250-record]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/record
[4040-rel1]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel1
[8250-rel1]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel1
[4040-rel3]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel3
[8250-rel3]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel3
[4040-parsex]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/parsex
[8250-parsex]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/parsex
[4040-tst4]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4
[8250-tst4]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst4
[4040-tst2]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
[8250-tst2]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
[4040-tsutil]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tsutil
[8250-tsutil]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tsutil
[4040-erproc]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/erproc
[8250-erproc]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/erproc
[4040-ieee]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ieee
[8250-ieee]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ieee
[4040-lstdir]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/lstdir
[8250-lstdir]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/lstdir
[vice-read]: https://github.com/VICE-Team/svn-mirror/blob/a0e14df25846cd7f06043f24b48677bf45c02149/vice/src/vdrive/vdrive-rel.c#L1229-L1324
[mame-4040]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c2040.cpp
[mame-8050]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c8050.cpp
