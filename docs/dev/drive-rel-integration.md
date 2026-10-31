# Public REL engine joins

This dependency batch extends the qualified REL storage publication in #129.
Its reviewed publication base is
`ec7cc28c14847273bc0aee58307e2bc650c6f8cb`. Original extraction was qualified
over `4e9a276ca250f5e6ca5d94e22147a33f8692caeb`. It joins public DOS OPEN,
binary RECORD, collected requests, retained LISTEN bytes and REL COPY to
the existing clean-read, positioning, writeback, creation, growth and CLOSE
owners.

The production IEEE facade is deliberately unchanged. Switching that facade
requires the remaining presence/configuration/acceptance batch: existing system
REL fixtures omit side sectors, and some controller reads assume that the old
facade queues a whole file without cooperative service. Enabling the replacement
facade before migrating those contracts failed production-wire qualification.
The attempted cutover and its evidence are saved privately, not included here.

## Ownership and admission

The existing 59-byte command backing shares storage with the input collector.
EOI completes a request but does not execute it. Ordered dispatch copies the
bounded request before native command retirement can clear its backing bytes.
Each genuine REL owner retains one input byte, its EOI and a four-phase
PUT/EOI/FINISH continuation. An ordinary or direct channel keeps its existing
PUT path.

Collection mode follows the current routed secondary, not an interrupted
OPEN/command. A new command collection clears the preceding EOI readiness.
Ordinary/direct reselection therefore cannot dispatch data as an old request.
Every replacement selection also discards a completed receive record, including
TALK and CLOSE. CLRCB retires only 58 logical bytes, retaining the extra receive
byte for short native parses. Dispatch snapshots and consumes the current
receive ownership before executing it. A subsequent collection can therefore
survive an older worker's retirement, while its own dispatch still waits for
that worker. Parsed worker arguments do not borrow this shared receive RAM.

Pending input rejects CLOSE, finalization and public positioning without
consuming that byte, including the resolved-pointer POSBUF entry. Missing-record
growth and failed page stores retain the byte until explicit recovery. Entering
WRTREL restores only the synchronous writer-readiness prerequisite that capture
hid from GET. It performs no I/O
before that readiness is consumed.

RECORD resolves FNDRCH and type before record and position fields. Its ordered
command owner prevents overlapping CLOSE/finalization. Raw little-endian record
bytes, retained short-command tails, model-specific GET flags and error
50/51 precedence use the already-qualified positioning workers.

Public OPEN retains ordinary namespace exclusions, post-retirement relookup and
native type/access parsing. REL construction uses three pooled pages, then
ADDFIL publishes a closed REL entry with count zero, side/super root and record
size. Failed construction/publication retains its filename, pages and phase.
Competing OPEN/COPY cannot replace that context. A read-only initial data failure
instead frees the owner and clears publication context so a fresh OPEN can run.
Reconciliation retains #129's failed-owner OPEN rejection (status 30) and
directory snapshot invalidation after successful or potentially partial peer
writes. The added failed-CLOSE/reopen case follows that reviewed status policy.
Actual channel release also retires its matching host ADDFIL continuation.
INIT's intentional abandonment cannot leave OPEN/COPY/finalization referring
to a freed owner. Releasing another drive preserves the live continuation.
Raw/BASIC directory re-OPEN uses native GETRCH abandonment for a healthy REL
writer, not ordinary writable finalization. Existing failed-owner admission
still protects retained retry. The 4040's exact `$0`/`$1` LOAD shorthand keeps
its separate native drive-wide initialization.
Retained REL creation also reserves its padded filename in the shared namespace
check. A failed ADDFIL cannot let RENAME claim that name before publication retry.
If a partial WRITE makes the planned slot visible, RENAME also excludes that
source slot until publication commits or the owner is explicitly released.
SCRATCH's active-entry test likewise uses that planned slot during retained
publication, so partial directory visibility cannot free still-owned REL chains.
Creation does not bind lookup's unmatched directory coordinates to its channel.
Only successful ADDFIL installs committed coordinates, so a failed creation
cannot protect an unrelated entry from SCRATCH. Unrelated RENAME remains allowed.
Terminal REL OPEN failures retire logical command RAM, including early channel,
buffer and initial-allocation failures, without losing retained creation intent.
A subsequent short RECORD cannot inherit the failed name and reposition a peer.
As with successful retirement, a newer complete or partial collection keeps its
receive RAM.

COPY retains reviewed stale-entry, namespace, ordinary prefetch and reclamation
checks. The first source supplies output type. REL input uses GIBYTE semantics,
omits the terminal EOF CR and shares retained WRTREL delivery. DOS 2.7 performs
CYEXT endpoint preallocation, while 4040 does not. FREICH abandons only internal
owners, including their ADDFIL continuation. Public peers survive failed jobs.
Existing native SCRATCH/VALIDATE side/super/data walkers are unchanged.

Ordinary COPY sources share successor loading and the traversal bound regardless
of output type. GIBYTE's failed successor is observed before the seam byte can
enter retained REL PUT. Successful REL CLSREL already frees its writer, so
copy-all's next close pass releases only an owner still allocated. Buffer zero
and every surviving peer remain owned across later source/writer reuse.
REL COPY completion uses the writer's retained EOI/overflow flags, not the
mutable public status line. A peer read error cannot abandon healthy output,
and consuming status cannot mask an actual overflow. Storage failure remains
the writer's retained failure condition.
If cached REL source preview rejects a replaced media generation, COPY reports
READ error before abandoning its partial output, even without a new READ job.

Finalization retains main's durability policy: persist retained dirty BAMs and
backing-store state even after an abandoned internal writer. It never replays a
freed REL page. RS=0 creation remains cooperatively pending rather than spinning
inside host finalization. Rejecting an existing RS=0 directory entry is a host
corruption safeguard, not evidence of the ROM's behavior on malformed media.

## Evidence

Targets are 4040 DOS 2.0 revision 2 (901468-14/-15/-16, approved overlays),
full-size single-sided 8050 and double-sided 8250 DOS 2.7
(901887-01/901888-01). Primary source is pinned at
`01bd60f162ef92212ef0cb67546ae8f42be34168`. The selected 4040 OW20 composition
includes MAPOUT, unlike the older archived `opchnl` listing. Preserve that
approved composition rather than inferring absence from the archive.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| REL OPEN and publication | Selected 4040, 8050/8250 | [4040 open], [8250 open]: OP60/OPREAD/OPWRIT. [4040 opchnl], [8250 opchnl]: OPNRCH/OPNWCH/OROW. [4040 addfil], [8250 addfil]: AF20/AF30/AF50 | Record-size byte selects missing-file creation. Two data pages plus index ownership precede closed REL publication and count zero | `dos_open`, `public_rel_open_work`. Public create/remount, publication faults, directory growth and read-failure/reopen groups | [VICE REL]: `vdrive_rel_open` corroborates existing/new REL selection and dual data buffers, not exact ROM failure ordering |
| Command collection | Both selected DOS families | [4040 tst2], [8250 tst2]: PUT/L42/L50/L45. [4040 ieee], [8250 ieee]: LSN30/LSN40 | Retain DATA and sampled EOI. Capacity handling still notices final EOI. Execution follows ordered collection | `dos_input_put`, `dos_input_dispatch`. Three input-collection groups | [MAME 4040], [MAME 8050] corroborate port-sampled IEEE inputs, not the host FIFO dispatcher |
| RECORD | Both selected DOS families | [4040 record], [8250 record]: R20/R30/R40/R50/R60, POSITN/POSBUF | Resolve channel/type first. Preserve raw record bytes. Positions zero/one agree. Missing record takes RD05 before successful ENDCMD | `dos_command`, `record_work`. Completion, stale input, failed retry and wrong-channel/type groups | [VICE REL]: `vdrive_rel_position_internal` corroborates numbering and addressing. Its immediate overflow return is not the ROM error-priority oracle |
| Retained LISTEN | Both selected DOS families | [4040 tst2], [8250 tst2]: PUT/L41/PIBYTE. [4040 rel2], [8250 rel2]: WRTREL/WR30/WR40/WR50/WR60 | Store or drop each actual byte once, retain EOI through ADDREL/CLREC/RD40, report overflow after native continuation | `dos_listen_put`, `rel_input_work`. Seven LISTEN/RECORD groups compare retained owners, primitive bytes and physical pages | [VICE REL]: `vdrive_rel_listen` corroborates a record-boundary operation, not cooperative retry ownership |
| REL COPY | 4040 versus shared DOS 2.7 | [4040 rename], [8250 rename]: COPY/OPIRFL/GIBYTE. DOS 2.7 CYEXT/SSEND/ADRELS/ADDR1 | Exclude EOF CR from output. Preserve output record size. Only DOS 2.7 performs CYEXT preallocation | `copy_work`, `copy_put`, `copy_finish`. Short records, pressure and exhaustive READ/WRITE/flush fault groups | VICE is not used as an oracle for CYEXT or FREICH. The model distinction is established by the two primary units |
| GET and teardown boundary | Both selected DOS families | [4040 ieee], [8250 ieee]: TLK10/TLK30/TLK35. [4040 close], [8250 close]: CLSREL/CLSALL | Preview does not commit GET. CLOSE retains failed ownership. Captured input cannot be discarded by another host operation | Existing `dos_output_stage/get` and storage workers. Source preview/progress, failed REL CLOSE and input-retention groups | [MAME 4040], [MAME 8050]: port and open-collector composition corroborate the hardware boundary, not host FILE lifecycle |
| Current-secondary input routing | Both selected DOS families | Both IEEE:DCDE70 retain current ORGSA. Both TST2:PUT choose L40/L42 from that byte on every received byte | An interrupted or completed OPEN/command cannot run under replacement ordinary/TALK/CLOSE context. New command readiness requires its own EOI | `dos_input_address`, ordinary/direct and completed-request reselection regressions | Both MAME selected hardware models corroborate sampled input/addressing, not host collection lifetime |
| Receive RAM retirement | Both selected DOS families | [4040 idle]/[8250 idle] clear CMDWAT before PARSXQ. [4040 parsex]/[8250 parsex]:CLRCB clear CMDLEN bytes, not the extra receive byte | Retain overflow CR for a subsequent short REL OPEN. Older workers cannot erase a newer host collection | `clear_command`, `dos_input_dispatch`, cross-OPEN size-13 creation and pending RECORD/OPEN/COPY/CLOSE followed by new OPEN/command dispatch | Shared receive RAM across cooperative yields is a host adaptation. No second buffer or added ROM concurrency is inferred |
| Directory re-OPEN after REL PUT | Both selected DOS families | Both OPEN:OP04 -> OPNRCH -> TST3:GETRCH/FRECHN | Abandon only the healthy previous REL owner, not through ordinary CLSWRT. Preserve unrelated peers and failed-owner admission | `open_directory`, raw/BASIC directory re-OPEN after actual REL PUT | MAME selected ROM/hardware families. Exact 4040 LOAD shorthand remains separately native drive-wide |
| Mixed-source COPY handoff | Both selected DOS families | Both RENAME:CY20 -> GIBYTE -> [4040 tst4]/[8250 tst4]:GBYTE/GET -> TST2:RDBYT/DBLBUF/WATJOB, then CY15:PIBYTE | Complete or reject the ordinary source successor before retaining its offset-255 byte for REL output | `copy_work`, valid 254-byte REL/ordinary COPY, terminal control, overflow completion and failed-successor ordering cases | MAME selected models corroborate ROM/hardware selection. VICE is not an oracle for CYEXT or native seam ordering |
| Retire only released storage ownership | Both selected DOS families | Both CLOSE:CLSREL -> [4040 tst3]/[8250 tst3]:FRECHN/RELBUF. INITDR -> CLDCHN -> FRECHN | Successful CLSREL releases once. Native abandonment retires matching host publication intent, while another drive and live retry retain theirs | `free_channel`, `copy_work`, copy-all buffer-zero peer, failed construction/publication INITIALIZE and fresh OPEN/finalization regressions | VICE REL commit/release and both MAME selected models, host ADDFIL retention is an explicit adaptation |
| Private COPY completion and pending namespace | Both selected DOS families | Both RENAME:CY15/PIBYTE -> REL2:WR30/WR40 retain EOI/overflow through CLREC/RD40. RENAME:CHKIO/TRNAME and ADDFIL publish resolved names/slots | Public status consumption/errors cannot replace the retained output outcome. Failed host ADDFIL reserves its name and any partially visible planned source slot | `copy_work`, `unpublished_writer`, `rename_work`, real peer-read/status-GET interleavings and failed/partial publication RENAME plus successful retry cases | VICE corroborates REL commit ownership, not host yielded ADDFIL retry or private COPY completion |
| Failed OPEN retirement and binding | Both selected DOS families | Both ADDFIL:AF50 install DIRENT or DSEC/DIND after DRTWRT. Both [4040 erproc]/[8250 erproc]:CMDER3 call CLRCB | Failed creation has no committed lookup binding. Retire command RAM without changing retry intent or a newer host collection | `dos_work`, `public_rel_open_work`, unrelated SCRATCH/RENAME, short RECORD after early/late OPEN failure and newer collection regressions | Selected 8250 OR21 defers super-root READ, so its side-read probe is a success control |

[4040 open]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/open
[8250 open]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/open
[4040 opchnl]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/opchnl
[8250 opchnl]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/opchnl
[4040 addfil]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/addfil
[8250 addfil]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/addfil
[4040 tst2]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
[8250 tst2]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
[4040 ieee]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ieee
[8250 ieee]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ieee
[4040 record]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/record
[8250 record]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/record
[4040 rel2]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel2
[8250 rel2]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel2
[4040 rename]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rename
[8250 rename]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rename
[4040 close]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/close
[8250 close]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/close
[4040 tst3]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst3
[8250 tst3]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst3
[4040 tst4]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4
[8250 tst4]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst4
[4040 idle]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/idle
[8250 idle]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/idle
[4040 parsex]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/parsex
[8250 parsex]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/parsex
[4040 erproc]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/erproc
[8250 erproc]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/erproc
[VICE REL]: https://github.com/VICE-Team/svn-mirror/blob/a0e14df25846cd7f06043f24b48677bf45c02149/vice/src/vdrive/vdrive-rel.c
[MAME 4040]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c2040.cpp
[MAME 8050]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c8050.cpp

## Qualification and boundary

All 138 published REL groups and 248 retained ordinary DOS groups have unchanged
executable tokens. The sole retired DOS group enforced the superseded blanket
REL COPY exclusion. Seven new REL groups and thirty new DOS groups qualify the
joined behavior. The 29 REL leaf helpers and 139 other engine helpers are
unchanged, including reviewed NRBUF, cold BAM preparation, SCRATCH/VALIDATE and
native block admission. The COPY joins retain reviewed ordinary namespace,
prefetch and stale-binding checks.

Reconciled Debug firmware qualification passes 6,372 cases (6,325 ordinary and 47
fatal/assert), including 1,691 REL cases and 3,698 DOS cases. The 159 additional
feedback cases include 45 reproduced failures against the original publication
and 51 against the first fix publication, plus twelve third-cycle reproductions
(including a related partial-publication source-rename defect), and three
partial-publication SCRATCH reproductions. Cached source replacement reproduces
false success on 4040, while DOS 2.7 preallocation already detects the stale
generation on the same scheduling probe. Entry-point, terminal-page and
stale-media controls remain covered. Six unrelated-entry SCRATCH and twenty
stale short-RECORD cases reproduce the final OPEN failure-path findings.
Unrelated RENAME, 8250's deferred super-sector read and newer receive ownership
remain controls. Production firmware
builds with `RelWithDebInfo`, and the ROM build passes. Actual compiler layouts
for record/channel/unit are 36/68/4588 bytes in Debug and 32/64/4552 bytes on ARM,
within the 4608-byte unit bound. Formatter and whitespace checks pass.
The original extracted build passed 159 targeted system cases (124 host/contract
and 35 production-wire/firmware cases) through the system build's CTest registry.
The reviewed storage changes and integration are requalified together by the
complete firmware suite, including all reviewed storage regression cases.

The production facade remains the published implementation, preserving all its
existing transport regression groups. Per-unit fabric presence, facade
replacement, configuration lifecycle changes, expanded system acceptance and
reference tooling remain together in the final batch. Native REL block editing
stays rejected by the reviewed `block_start` guard. This slice does not claim
fresh board, electrical, FPGA timing or complete ROM acceptance.
