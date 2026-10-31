# Cached REL write and completion primitives

This dependency slice follows the REL allocation primitives in #120. It adds
RELPUT/CLREC leaves and cached-only completion joins over the existing borrowed
data pages. It does not enable public REL writes, creation, growth, CLOSE or
COPY. Production facade routing, the legacy engine and flat HDD support
remain unchanged.

## Included contracts

`dos_rel_record_put` accepts an already-prepared existing record. It stores
before advancing and ORs the physical page's bit into the owner's dirty mask.
WRITING retires read readiness, FULL represents OVRFLO rather than status 51,
and NEEDS_WRITEBACK retains the stored byte, dirty bit and wrapped pointer zero
before native NRBUF work. Even a resident following page does not authorize
the clean scheduler to consume this dirty boundary.

`dos_rel_record_pad` implements only CLREC's zero-fill loop. It stops at FULL,
a writeback boundary or failure within one page. No leaf performs I/O,
allocation, file-flag updates, EOI/status publication or dirty-bit clearing.
The owner must first retire FIFO previews and retain dirty pages across
failure, cancellation and repositioning.

`dos_rel_record_finish_write` prepares only a fully cached post-CLREC
NXTREC/SETLST transition. It refuses an NRBUF boundary or an unknown page
without committing speculative changes to FULL. A stale mount instead retires
the cursor. `dos_rel_record_prepare_cached` joins an owner's completed native
buffer window. SETLST may scan a completed inactive page without proving
that it is the forward-link target for GET. These host scheduling entries
leave counters, flags, status, persistence and page ownership with the caller.

## Native evidence

The targets are 4040 DOS 2.0 revision 2 (`901468-14/-15/-16`) and full-size
8050 MPI single-sided / 8250 MPI double-sided DOS 2.7
(`901887-01/901888-01`). Both pinned master compositions were consulted.
The approved 4040 overlays alter POSBUF sequencing, not the selected
RELPUT/CLREC, NXTREC or SETLST routines. The existing read scheduler's
documented clean-only boundary remains unchanged.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Store, dirty ownership and full record | All selected targets | [4040 rel2][4040-rel2] / [DOS 2.7 rel2][8250-rel2], RELPUT/SDIRTY/RELP05/RELP07/RELP10 | Dirty and store before advance. At NR equality retain the last accepted pointer and set overflow. Further full-state leaf calls accept no bytes. DOS 2.7 DYFILE and both models' actual buffer flags remain owner work | `dos_rel_record_put`, borrowed-page and prepared-existing-record tests | [VICE write][vice] independently stores into dirty current pages and bounds writes to a record, but uses widened pointers and different overflow publication |
| Byte wrap and NRBUF boundary | All selected targets | Both rel2 RELP06/RELP07/RELP10, [4040 rel1][4040-rel1] / [DOS 2.7 rel1][8250-rel1], NRBUF/NRBU20 | Pointer 255 with NR 0 or 2 becomes FULL without page selection. Other wraps store first, then retain pointer zero and stop before dirty writeback, even with cached or failed read-ahead | `dos_rel_record_put`, native-wrap/deferred-prefetch tests for all models | VICE's buffer swap/commit corroborates two-page dirty ownership, not native byte-sized wrap or job scheduling. [MAME 4040][mame-4040] and [8050/8250][mame-8050] establish hardware/ROM selection |
| Zero padding and input guards | All selected targets | Both rel2 CLREC/CLR10 and WRTREL/WR30/WR40/WR60 | Fill only the remaining record suffix, stopping at OVRFLO or a cooperative NRBUF handoff. MISSING requires owner ADDREL, not an accepted store. WRTREL's EOI, overflow status and DOS 2.7 OKERR remain excluded | `dos_rel_record_pad`, borrowed-page, missing/pending/failed and generation-retention tests | VICE fillrecord/listen independently zero-fill the written record, but do not establish native status timing |
| Cached completion and refusal | All selected targets | Both rel2 WR40, [4040 rel3][4040-rel3] / [DOS 2.7 rel3][8250-rel3], RD40, both rel1 NXTR15/NXTR20/NXTR45 | Prepare an in-page next record or sticky missing CR/EOI. A dirty page seam or unknown end-scan page retains FULL unchanged. The owner still increments the record number and clears revision-specific flags | `dos_rel_record_finish_write`, cached-boundary tests over both physical page selections | VICE listen advances and scans the next record, but uses its own I/O/cache policy rather than this host-only no-I/O join |
| Completed inactive-page scan | All selected targets | Both rel1 NXTR20, [4040 rel4][4040-rel4] / [DOS 2.7 rel4][8250-rel4], SETLST/SETL01/SETL05/FNDLST | Scan the completed native window with byte-wide remaining count. Scanning the inactive page does not establish forward-link residency for a subsequent GET. Wrong state/phase is a no-op, deferred failure prevents readiness | `dos_rel_record_prepare_cached`, completed-window tests, including GET's later NEEDS_LINK | VICE listen independently scans trailing zeros over current/next data, but its cache metadata does not prove native retained-WRITE-window behavior. Both selected primary rel4 routines agree |
| Cached access after media retirement | Host lifetime safeguard for all targets | No direct ROM entry. Both rel2 SDIRTY and rel1 NRBUF establish the dirty-buffer obligation this guard protects | Reject cached stores/completion on an old generation without overwriting bytes or clearing dirty ownership. Another unit's pages/cursor remain usable | Existing generation check plus write/completion-generation and peer-isolation tests | VICE/MAME model different image/job lifetimes. They corroborate the device distinction, not EconoPET's generation-token mechanism |

These are source-derived leaf contracts, not controller or public-channel
acceptance. Independent sources corroborate structure and model selection,
not every arithmetic, status or retained-window detail. No ROM binaries or
disassembly are included.

## Validation and remaining work

The isolated candidate passes 423 focused REL cases (44 test groups) and
4840 total firmware cases (4793 ordinary plus 47 fatal/assert). Tests and
dependencies use Debug. The RP2040 production RelWithDebInfo build,
clang-format and whitespace checks pass. All 25 dependency helpers and
36 dependency test groups retain their executable tokens and registrations,
including final merged #119's selected-side retest and geometry handoff.

Stateful reservation/publication, BAM ownership, creation/growth continuation,
dirty NRBUF jobs and retry, public channel integration, LISTEN/write status,
CLOSE and REL COPY remain separate. No fresh full-ROM controller comparison,
physical acceptance, timing or electrical claim is made.

[4040-rel1]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel1
[8250-rel1]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel1
[4040-rel2]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel2
[8250-rel2]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel2
[4040-rel3]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel3
[8250-rel3]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel3
[4040-rel4]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel4
[8250-rel4]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel4
[vice]: https://github.com/VICE-Team/svn-mirror/blob/a0e14df25846cd7f06043f24b48677bf45c02149/vice/src/vdrive/vdrive-rel.c#L1383-L1600
[mame-4040]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c2040.cpp
[mame-8050]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c8050.cpp
