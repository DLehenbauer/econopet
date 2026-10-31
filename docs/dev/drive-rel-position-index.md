# REL existing-record core

This expanded slice follows clean POSBUF in #126. It joins resident
side-index positioning, existing-record writes, dirty-page writeback,
padding/EOI completion, read/write transitions and explicit storage retries.
These are internal owner APIs. Public REL routing remains disabled.

## Included contracts

The caller retires this owner's old FIFO sources before requesting a position.
READY/MISSING readers and READY/WRITING/FULL/MISSING retained writers can
position without I/O. Busy, failed and unsupported owners are rejected
without mutation. Raw record identity and normalized
position replace the request, but pooled data/data/side pages, data pointers,
media identity and the side-cache discriminator survive. Only the 4040
GETFLG is cleared. Successful internal work retains status and its unread
cursor. Public RECORD's immediate RD05/error-50 and overflow-status policy
remain the command caller's responsibility.

SSTEST first accepts an equal side header, even a wrapped number outside the
six-side range. Otherwise a present side-table track requests one READ.
SSPOS tests again, but its second branch checks N only. A surviving V does
not request another READ. Malformed zero data tracks can therefore reach a
required data READ (host range error), rather than becoming missing records.

8250 uses one pooled side page, not a second super-sector cache. RDSSS marks
the tag NONE before replacement and SUPER only after success. RDSS1 similarly
publishes a group only after its READ. OPEN initially retains the super root
without reading it. Cold preparation captures that root and publishes tags
from completed leaf index reads. Request replacement and cached data
preparation retain the tag.

Out-of-range lookup runs SSEND. 8250 RDLG directly reuses cached group 90,
but reloads/scans the super table from other groups, including group 91.
It scans 90 down to zero, independently of arithmetic's possible group 91.
SSEND selects the last nonzero side pointer, reads that side only if its
header differs, requires a terminal link, and preserves byte-wide
`byte[1] - 1` wrap. Empty small side tables and nonterminal final links
report native BREAK error 67 at zero coordinates.

Missing preparation retains the old data pointer/NR and prepares CR/EOI.
Preview does not report 50. Actual GET owns the existing missing-byte event.
The public RECORD status wrapper is not implemented here. Malformed empty
8250 super-table underflow can inspect adjacent physical ROM-machine RAM.
That unsupported boundary remains the internal NO_INDEX/syntax diagnostic,
not fabricated native missing completion.

Each cooperative pass performs one decision or at most one media READ/WRITE.
Required index/data failures stop with retained ownership and attempted
coordinates under the host READ ERROR 23 policy, without automatic retry.
Stale media stops before reading replacements and reports zero coordinates.
CLOSE cancels pending work and releases clean READ ownership. Internal MODIFY
owners remain allocated (including CLOSE 0 and CLOSE ALL). This explicit host
boundary returns syntax rather than pretending to implement CLSREL or letting
ordinary metadata cleanup discard dirty pages. It does not model native CLOSE.
Other channels and their retained output sources remain independent.

The ten side-index tags and four dirty/retry tags are appended without
renumbering the #126 tags.
`dos_rel_record_t.cached_group` occupies existing record padding and does not
alias `dos_channel_t.directory`, which also admits REL directory filters.
Compiler layout remains 36-byte record, 68-byte channel and 4580-byte unit
in Debug host tests, and 32/64/4544 bytes respectively in RP2040 production.
Both remain under the existing 4608-byte unit bound. No heap, queue or
private page is added.

## Existing-record writer contracts

`dos_rel_channel_begin_write` retires readiness without replacing the native
GET cursor or raw record counter. A completed writer can return to reads and
reenter writes at the actually consumed cursor, preserving dirty bits and
DOS 2.7 DYFILE. A pending or unexposed completion cannot accept another byte.
`dos_rel_channel_put` returns true only for an actual cached store. The caller
owns transport byte retention and EOI collection. There is no queued LISTEN
facade in this slice and no admission for missing-record growth.

RELPUT dirties the active physical page. WRTOUT/SCRUB writes that page's saved
track/sector, not lookup coordinates or its forward link. Success clears only
that page's dirty bit and retains status. READ/WRITE history is stored in the
existing pool masks. Dirty NRBUF writes the outgoing page before exposing the
new active page. A matching completed READ can be reused, but TSTWRT requires
a READ after WRITE. Dirty NRBUF does not issue the clean branch's prefetch.
Dirty POSBUF scrubs before rotation when required and never overwrites an
owned dirty target. Explicit failed-wait phases prevent a retry rotating twice.

`dos_rel_channel_pad` runs bounded CLREC without declaring EOI or advancing
the record. `dos_rel_channel_finish_write` queues CLREC followed by RD40/NXTREC,
retaining EOI across failed WRITE/READ jobs. The raw counter advances once.
Filling the record does not itself report overflow. An excess EOI-marked byte
reports 51 only after completion prepares the next byte. Otherwise 4040 retains
the previous status and DOS 2.7 uses OKERR, not ENDCMD. `finish_cached` provides
the same-page fast path without I/O and requires an explicit `resume_read`.

Resumed reads stage only a preview. Actual GET commits the consumed prefix,
then queues dirty readback/NRBUF at a page seam. SETLST may scan retained old
WRITE bytes rather than a speculative forward-link cache. Failed prefetch
remains deferred until the native read window needs it. Retained jobs stop
polling after host failure, and retries repeat the I/O continuation without
replaying consumed bytes or raw-record advancement. Generation guards prevent
replacement-media callbacks even when a retained WRITE is explicitly rearmed.
Cached lifetime failures cannot revive a retired writer. No sector WRITE is
a backing-store flush or a complete file-metadata CLOSE acknowledgement.

## Native evidence

Targets remain 4040 DOS 2.0 revision 2 (`901468-14/-15/-16`) and full-size
8050 MPI single-sided / 8250 MPI double-sided DOS 2.7
(`901887-01/901888-01`). Both pinned master compositions, equates, RECORD,
SSTEST, SSUTIL, SSEND, error tables and DOS 2.7 SYSTEM were consulted.
Both REL1/REL2/REL3/REL4 and TSUTIL/TST2 were also consulted for the writer join.
The approved 4040 revision-2 POSBUF correspondence is retained from #126,
not substituted with the older archive's P80 sequencing. No private ROM
binary or generated assembly is published.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Resident range and second-test flags | All selected targets | [4040 SSTEST][4040-sstest], [DOS 2.7 SSTEST][8250-sstest], both [4040 SSUTIL][4040-ssutil] / [DOS 2.7 SSUTIL][8250-ssutil], SSTEST/ST20, SSPOS/SSP20, SETSSP | Equal header precedes six-side bound. First ER1 reads the selected side. Second nonnegative result accepts its data pointer even if V survives | `rel_position_index_work`, resident-index and wrapped-header cases, including zero data-track host READ failure | [VICE positioning][vice-position] corroborates indexed positioning and data-window reuse, but its high-level implementation does not establish these native N/V rules |
| Super/group cache and RDLG asymmetry | Selected double-sided 8250 MPI DOS 2.7 only | [SYSTEM][8250-system], BIGREL/RDSSS/RDSS1/RDLG, [DOS 2.7 SSTEST][8250-sstest] | NONE before replacement, SUPER/group only after successful READ. RDLG directly accepts cached 90, otherwise scans the super table from 90 downward, not 91 | Cold/OPEN cache lifetime, four last-group variants, group READ failures, high-group directory discrimination | [MAME MPI modes][mame-mpi] identifies 8050/8250 hardware selection. VICE uses its own larger index cache, not the native single-page tag lifetime |
| Missing endpoint and corruption | All selected targets | [4040 SSEND][4040-ssend] / [DOS 2.7 SSEND][8250-ssend], SE10/SE20/SE30/BREAK, both SSUTIL SSP10 | Choose the last nonzero side pointer, optionally READ it, require terminal track zero, subtract one with byte wrap. Empty side table/nonterminal last side reports 67 | Resident missing/corrupt and six endpoint-wrap cases preserve old data pointers and CR/EOI | VICE's high-level record count is corroboration of missing records, not proof of SSEND's byte-wrap or malformed-index rules |
| Flags, raw identity and status boundary | All selected targets | Both [4040 RECORD][4040-record] / [DOS 2.7 RECORD][8250-record], R30/R40/R50/RD05, both ERPROC error-50 table entries | Native RECORD captures raw number and clears model-specific flags. Native out-of-range RECORD enters RD05/error 50. This internal primitive defers command status to its caller and only actual GET reports the missing-byte event | Resident raw-zero/flag/status-cursor tests and actual GET string checks. Public command dispatch deliberately remains disabled | VICE normalizes through its own record indexing. [MAME revision-2 chips][mame-4040] establishes the selected 4040 revision, not this host status adaptation |
| Cooperative lifetime and unsupported underflow | Explicit host adaptation supporting selected targets | No direct ROM entry for C phases, generation tokens or empty-super-table bounds. Native SYSTEM/RDSS1C and SSEND establish the jobs being adapted | One READ/WRITE per pass, no implicit flush or replacement-media callback, preserve failed ownership/peer sources, cancel clean work. Empty big-table underflow remains unsupported, not native 50 | Failure/remount/CLOSE, peer-prefix, cache-lifetime and empty-table cases, plus unchanged prior lifecycle/layout tests | MAME model declarations do not establish host error-23 mapping, cooperative scheduling or generation guards |
| Stores, page dirtiness and CLREC | All selected targets, DYFILE only on DOS 2.7 | Both [4040 REL2][4040-rel2] / [DOS 2.7 REL2][8250-rel2], RELPUT/RELP07/SDIRTY/CLREC | Store only in the existing record, dirty the selected page, retain model flags, zero the remaining bytes until FULL without advancing the raw number | Owned padding, readonly/version failures, physical-role reversal and write reentry cases | [VICE writes][vice-write] corroborates cached stores and sector dirtiness, but reports overflow at the attempted write rather than native WRTREL's EOI boundary |
| Saved headers, dirty NRBUF and POSBUF | All selected targets with the approved 4040 revision-2 POSBUF overlay | Both [4040 TSUTIL][4040-tsutil] / [DOS 2.7 TSUTIL][8250-tsutil], GETHDR/WRTOUT/SCRUB/SJ20, both REL1 NRBUF and TST2 DBLBUF | Write saved active header, clear only acknowledged page bit. Dirty NRBUF omits clean prefetch. Reused WRITE requires READ. POSBUF scrubs before required rotation | Saved-header/mask, dirty handoff/refill, dirty indexed positioning, failed alternate/cold wait retry | VICE writes corroborate two data roles, but its higher-level cache is not proof of native TSTWRT or failed host-wait retry |
| EOI, delayed overflow and model status | All selected targets | Both REL2 WRTREL/WR30/WR40, REL3 RD40 and REL1 NXTREC. DOS 2.7 WR45/OKERR differs from 4040 WR51 | CLREC precedes RD40, increment raw number once, report 51 only for excess EOI. Normal 4040 preserves status, DOS 2.7 publishes OK without ENDCMD | 54 full EOI variants, cached completions, retained EOI WRITE failure and peer isolation | VICE's earlier overflow publication is explicitly not the revision authority |
| Read return and retained WRITE windows | All selected targets | Both [4040 REL3][4040-rel3] / [DOS 2.7 REL3][8250-rel3], RDREL/RD40, both [4040 REL4][4040-rel4] / [DOS 2.7 REL4][8250-rel4], SETLST/FNDLST, REL1 NRBUF | Actual GET owns progress and missing 50. Scan retained native pages, write the outgoing dirty page before exposure, retain raw counter across job retry | Short-prefix, missing CR/EOI, retained WRITE-window, dirty readback/refill and lifetime cases | VICE corroborates record reads, but its larger cache and high-level indexing do not establish native byte-wide windows |
| Host failure retention and CLOSE fence | Explicit internal host adaptation, not full native CLOSE | No direct ROM entry for explicit C retries or the core-only fence. Both TSUTIL SCRUB and TST2 WATJOB establish the native jobs being adapted | Retain dirty owners after failure, do not poll or write replacement media. Until CLSREL joins later, refuse internal writer CLOSE without cleanup | WRITE/refill retries, remounts, peers and 36 CLOSE-shortcut variants | MAME model declarations establish target selection, not host recovery or the deliberately excluded CLSREL implementation |

## Validation and exclusions

The isolated candidate passes 1208 REL cases in 110 groups and all 5625
firmware cases (5578 ordinary plus 47 fatal/assert). This includes all 716
previously staged cases plus 492 writer/dirty/retry/CLOSE-boundary cases.
All 74 previously staged groups remain unchanged, and final #126 adds its
12 required-prefetch error cases. All merged registrations remain unchanged, including
final #125's resident-terminal fix and final #123's stale-batch safeguards.
The cooperative fixture now checks the combined READ/WRITE bound.
All 29 leaf helpers remain byte-identical. Debug tests/frameworks/dependencies,
RP2040 RelWithDebInfo production, clang-format and whitespace pass.

Public RECORD/OPEN/LISTEN routing, queued transport byte ownership,
creation/growth, full CLSREL, cold-leaf SSEND fallback, REL COPY/SCRATCH/VALIDATE
and production cutover remain excluded. This is not fresh full-ROM execution,
board acceptance or an electrical/timing claim.

[4040-sstest]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/sstest
[8250-sstest]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/sstest
[4040-ssutil]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ssutil
[8250-ssutil]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ssutil
[4040-ssend]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ssend
[8250-ssend]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ssend
[8250-system]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/system
[4040-record]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/record
[8250-record]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/record
[vice-position]: https://github.com/VICE-Team/svn-mirror/blob/a0e14df25846cd7f06043f24b48677bf45c02149/vice/src/vdrive/vdrive-rel.c#L1055-L1324
[mame-4040]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c2040.cpp#L142-L150
[mame-mpi]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c8050.cpp#L79-L80
[4040-rel2]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel2
[8250-rel2]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel2
[4040-tsutil]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tsutil
[8250-tsutil]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tsutil
[4040-rel3]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel3
[8250-rel3]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel3
[4040-rel4]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel4
[8250-rel4]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel4
[vice-write]: https://github.com/VICE-Team/svn-mirror/blob/a0e14df25846cd7f06043f24b48677bf45c02149/vice/src/vdrive/vdrive-rel.c#L1383-L1509
