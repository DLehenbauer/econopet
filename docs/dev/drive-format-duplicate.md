# FORMAT and DUPLICATE foundation

This dependency slice extends ordinary COPY in #117. It adds NEW/FORMAT and
sector DUPLICATE to the complete-request `dos_command` API. It does not change
the production facade, legacy engine, flat HDD support or transport collection.
REL lookup/read is the next dependency slice.

## Included behavior

`N0:name` rebuilds metadata while retaining the ID and unrelated data.
`N0:name,AB` first zeroes all raw sector data, then rebuilds the native
directory, BAM and header. `D0=1` formats the destination and copies every
source sector, including free data. Both DUPLICATE directions are supported.

The workers use the existing command union, BAM cache and shared data pages.
DUPLICATE temporarily reserves pages 0..10. NEW checks its fixed formatter
scratch against retained owners. Sector erasure/copy yields between operations,
and shared INITDR uses its existing bounded continuation. No heap, extra sector
pool, whole-image buffer or whole-file chain cache is introduced.

## Native evidence

The selected targets are 4040 DOS 2.0 revision 2 (`901468-14/-15/-16`),
8050 MPI single-sided and 8250 MPI double-sided DOS 2.7
(`901887-01/901888-01`). Both [4040 master][4040-master] and
[DOS 2.7 master][8250-master] compositions were consulted. These rules are not
claims about DOS 2.5, 8250LP or arbitrary controller ROMs.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Parsing and ID retention | All selected targets | Both [4040 new][4040-new]/[DOS 2.7 new][8250-new], `NEW/N101/N108`, respective `parsex:TAGCMD/PARSE`, `setdrv:ONEDRV/SETDRV`, `trnsfr:TRNAME/FNDLMT/TRCMBF` | Missing colon or comma before it returns 34. Reject wildcard/equal groups with 30. A trailing comma without an ID remains quick mode. A one-byte ID consumes the retained following command byte | `format_start`, parser rejection and inherited-drive/ID-boundary tests | [VICE formatter][vice] corroborates retained ID without an explicit new ID, but its generalized parser is not native authority |
| Empty labels | All selected targets | Both `trnsfr:TRNAME/FNDLMT/TRCMBF` and NEW's `TRNAME` call with size 27 | Empty name before comma still copies that delimiter once, then 27 padding bytes. ID and DOS-version fields overwrite their native positions | `format_start/format_label`, empty-label and padding-spill boundary cases | VICE's general label handling does not establish this native do/while edge. Both selected primary listings agree |
| Quick NEW and native metadata | All selected targets | Both new sources, `N108/N110`, [4040 verdir][4040-map], `NEWMAP`, [DOS 2.7 map][8250-map], `NEWMAP/NEWMPV/BAMOUT`, DOS 2.7 `new:TRNHDR/N150/N160` | Initialize selected disk without CLOSE. Reject a foreign BAM version with 73 before destructive metadata writes. Rebuild empty directory/BAM/header while retaining unrelated raw sectors and ID | `format_map/format_label/format_work`, complete-image, version and storage-failure tests | VICE independently constructs empty directory/BAM metadata. Its unconditional channel close and generalized version policy differ, so the selected ROMs control those details |
| Full NEW raw-sector data | Selected 4040 revision 2 and DOS 2.7 MPI modes | Both new sources, `NEW/FORMAT`, respective [4040 duplct][4040-duplicate]/[DOS 2.7 duplct][8250-duplicate], `FORMAT/FMT105`, approved formatter payloads in the [ROM manifest][manifest] | An ID formats all sector data, then writes native metadata with the new ID. 4040's decoded $064B loop writes zero sector data. DOS 2.7's $063B loop uses zero FMTDAT installed by FORMAT | `format_work`, quick/full complete-image and destructive-prefix failure cases | VICE independently erases sector data when given an ID (with its own FIXME). [MAME 4040][mame-4040]/[8050][mame-8050] corroborate selected ROM/hardware identities. No fresh healthy original-controller full-FORMAT result is claimed |
| Channel scope and scratch | 4040 versus selected DOS 2.7 modes | Both new sources, `N108/NEW`, respective `tst3:CLRCHN/CLDCHN`, `tst2:INITDR`, `getact:GETACT`, and both duplct sources, `DUP1` | Quick INITDR abandons selected active-job-drive owners. Full 4040 NEW retains channels. DOS 2.7 full NEW releases SA14..1 across both disks but retains SA0. DUPLICATE initializes source then abandons target owners without CLOSE | `format_initialize/format_work`, channel-lifecycle and busy-scratch tests, shared landed `free_drive_channels` | MAME corroborates physical shared RAM and dual-drive models. Native listings control channel scope. Host scratch-conflict rejection is explicitly not native scratch corruption |
| Raw-sector DUPLICATE | All selected targets with matching native geometry | Both duplct sources, `DUPLCT/DUP1/CPYD1/CPYTRK/SETRH/SETR3/READS/WRITES`, respective `copysetup:PRSEQ` | Adjacent distinct ASCII 0/1 selectors surround `=`. Initialize/version-check source, format target, copy every sector, then initialize target. Tracks ascend, eleven-page groups descend, sectors within each group ascend. Source remains unchanged | `format_copy_group/format_work`, both-direction whole-image/source-isolation and read/write failure tests | VICE's virtual-drive dispatcher leaves DUPLICATE unused, so it is not corroboration for the native algorithm. MAME confirms the two-drive ROM execution models, not a fresh successful formatter run |
| Failure and retained initialization result | All selected targets, with explicit host persistence policy | Both new/duplct sources, `INITDR`, `VNERR`, `ERROR`, `ENDCMD` | Stop after failed initialization or media I/O, retain any destructive prefix, release only command-owned scratch. Consuming the status line cannot turn failed initialization into success | `format_work`, initial/final INITDR status-consumption, stale-generation, storage-failure and recovery tests | Selected ROMs establish failure exits. Captured generations, asynchronous status consumption and backing-file flush are firmware mechanisms, not electrical/controller equivalence |
| Prefix pattern and equals validation | All selected targets | Both `parsex:PRSCLN/PARSE/TAGCMD/TC30/PR28` and `romtbl:STRUCT` NEW mask, `equate:MXFILS=5` | Retain prefix '*'/'?' for syntax 30 before media access. TAGCMD decrements the post-colon pointer twice before scanning for '=', so `N=:NEW` is rejected while `N=0:OK` is allowed. Stop suffix validation at the fourth comma: following patterns/equals are ignored | `format_start`, quick/full prefix-pattern and adjacent-equals rejection, plus earlier-prefix-equals and before/after-comma-limit controls | Both pinned PARSE routines retain PATFLG across the colon but stop at their bounded comma table |
| Operation-local drive selection | All selected targets | Both `new:N101/N108`, `duplct:DUPLCT/DUP1` and `parsex:ENDCMD/SCREND` | Successful NEW records its selected disk in LSTDRV and DOS 2.7's status drive. Version rejection identifies NEW's disk or DUPLICATE's source, regardless of a yielded peer read error | `format_work`, bidirectional peer-error, inherited-drive and NEW/DUPLICATE version-rejection regressions | Cooperative peer interleaving is host behavior. Explicit reselection preserves the ROM's DRVNUM ownership |
| Borrowed BAM failure recovery | All selected targets, host cache policy | Both `new:N110` and DOS 2.7 `new:N160`, alongside landed BAM load/write contracts | Failed scratch read/write/reload must not expose partial or unbound bytes as a resident BAM. Subsequent B-W and B-A/MAPOUT reload real metadata. Rejection before scratch retains dirty allocation state | `format_work` failure exit, partial-read/untouched-write recovery and pre-scratch rejection controls | Cache invalidation is host lifetime policy, not a native rollback. Already written sectors remain changed |
| Late internal reader and metadata reads | Selected DOS 2.7 modes, not 4040 | DOS 2.7 `new:NEW3/N150/N160 -> frets:SETMAP/RDNBAM/USEDTS`, `tst4:OPNIRD/NXTBUF -> opchnl:OPNRCH -> tst3:GETRCH/FNDLNX/GETBUF`, `tst2:STRDBL/NF05/NF10` | After the metadata prefix, refill 38/0 and reserve the header before allocating OPNIRD's logical channel and two pooled pages. Follow the written header/BAM/directory links, then reserve the remaining metadata and count NF05 ranges. An earlier BAM read error precedes exhausted-channel status 70 | `format_work`, exhausted index/page cases with and without a combined BAM read fault, plus quick/full late metadata/NF05 failures and cached-first-map controls | Serial one-sector jobs preserve selected-page failure observation, not physical controller concurrency, retries or latency. No synthetic read success substitutes for backing I/O |
| Surviving listing and formatter BAM ownership | Selected DOS 2.7 modes, host cooperative scheduling | DOS 2.7 `new:NEW/CLRCHN` retains SA0, `new:N160/NF05` owns the BAM count, and `lstdir:MSGFRE` emits the directory footer | Preparing a retained footer must not evict FORMAT's map or overwrite its partial count. Yield footer preparation while its drive is formatter-owned, preserving ready state and resuming after success or failure | `prepare_directory_line`, surviving-SA0 tests immediately before and after the first NF05 count, with successful and failed completion | A host scheduling safeguard, not a physical timing claim. Already staged rows remain readable and unrelated-drive footers are not blocked |
| 4040 completion without reread | Selected 4040 revision 2 | [4040 new][4040-new], final `DRTWRT -> ENDCMD` | After the final metadata write, retain the generated BAM and calculate the host free count without another read. A newly armed read failure cannot turn completed NEW into status 23 | `format_work`, quick/full final-write read-fault controls and adjusted failed-format recovery/finalization controls | The selected listing has no final BAM reload. This does not claim controller retry or timing equivalence |
| Quick NEW without an extra header read | All selected targets | Both `new:N108/N110` and `tst2:INITDR`, then `new:TRNHDR` on DOS 2.7 | After successful INITDR/version acceptance, begin metadata generation without rereading the header. Reuse initialization's retained ID | `initialize_work/format_work`, both-drive quick-NEW tests arming a read failure between INITDR and the first metadata write, checking zero extra reads and retained ID | Native DSKID comes from controller headers. Raw images retain the existing host convention of reading the ID from the directory header, now using the already-required initialization read |
| DUPLICATE backing-store alias rejection | Host adaptation, no physical-drive counterpart | Native duplct's `DUP1/FORMAT` assumes distinct physical disks. Host descriptors may alias despite different selectors | Identical read callback/context is rejected with host policy 25 before INITDR or erase, in either direction, including a read-only source | `format_start`, alias/source-immutability regressions | Descriptor equality cannot prove that different contexts refer to distinct files. The facade must ensure distinct underlying backing stores |
| Failed formatter persistence ownership | Host adaptation, no controller-flush counterpart | Host `dos_finalize`/`dos_media_flush_unit`, retaining the existing native destructive prefix | Discard scratch validity, not its captured media reference. Failed backing flush remains observable and retryable without another OPEN. Remounted media cannot acquire old finalization | `format_work`, direct finalization failure/retry and replacement-generation controls | No rollback, automatic command-time flush or mount-policy change is introduced |
| Failed reinitialization after dirty NEW | All selected targets, host backing persistence policy | Both `tst2:INITDR` discard native map state. Shared host `initialize_work` must independently retain backing flush ownership | A completed, unflushed NEW followed by failed NEW/I/VALIDATE/DUPLICATE initialization still fails and retries finalization when backing flush fails | `initialize_work`, sequential dirty-format then first-refill failure tests for all shared callers | Discarding dirty cached allocations is not permission to forget already issued backing writes |

[4040-master]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/master
[8250-master]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/master
[4040-new]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/new
[8250-new]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/new
[4040-map]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/verdir
[8250-map]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/map
[4040-duplicate]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/duplct
[8250-duplicate]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/duplct
[manifest]: ../../tools/drive-roms/drive-emulation-roms.json
[vice]: https://github.com/VICE-Team/svn-mirror/blob/a0e14df25846cd7f06043f24b48677bf45c02149/vice/src/vdrive/vdrive-command.c#L3371-L3438
[mame-4040]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c2040.cpp
[mame-8050]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c8050.cpp

The privately decoded formatter inputs match the approved manifest SHA-256
identities: `8b7dd86b69fa0d0f7a95feffcd88acfabb62a8db4add9245822cb1b4094ff471`
(4040, 672 bytes) and
`ad205636a7ab5641fc8014eb379e8d7e98e2019776ab346aa714dbe7b11dc421`
(DOS 2.7, 880 bytes). Those controller payloads establish raw-data intent,
not a completed physical-formatting acceptance run. No ROM binaries or decoded
payload listings are included here.

## Adaptations and exclusions

Raw image writes model sector data, not magnetic headers, gaps, retry timing,
parallel controller-job error ordering or formatter-code echoes into RAM.
Fixed live/orphan scratch conflicts return host policy 70 instead of overwriting
retained owners. Captured media generations prevent old commands from accessing
replacement images. Failures are not transactions and do not roll back a
destructive prefix.

Shared INITDR retains its explicit `succeeded` result independently of readable
status. NEW/DUPLICATE, not the INITDR prefix, owns ENDCMD. Existing landed
read/write, metadata, direct-channel MAPOUT, alternate-page RAM, COPY namespace
and reset behavior remains intact.

Every formatter continuation reselects its operation's disk before publishing
status or ENDCMD. The shared version-rejection path separately selects the
initialized disk (DUPLICATE's source). Failed generated-map and required
metadata-read phases discard borrowed BAM residency and its cached free-block total,
while retaining the captured reference needed to finalize dirty backing writes.
Full-format scratch rejection happens before this borrowing and retains the
previous cache, including dirty allocations. No rollback or extra sector page
is introduced.

DOS 2.7 NEW refills the first BAM and reserves the header before attempting
its late internal reader allocation. Thus a 38/0 read failure is reported
before exhausted logical indexes or pages can return 70. It retains the reader's
logical index and two existing
pooled pages while rereading the written metadata chain, then through map
reservation and NF05's ascending map scan. The first map is already cached
after reservation, later map ranges are read from their links, and the final
range remains resident. Each late worker turn performs at most one synchronous
read/write, with a bounded metadata-chain guard. It releases the reader on
success or failure.
4040 NEW does not gain that resource requirement or a final BAM reread after
its last metadata write. It retains the generated map instead.
DUPLICATE rejects a source
and destination with the same read callback/context before initialization,
without forbidding harmless alias mounts globally or pretending to detect
aliases hidden behind distinct contexts.

A surviving listing yields only when preparing a footer for a drive currently
owned by NEW/DUPLICATE. This protects both the shared BAM binding and NF05's
partial count without adding another cache or counter. Ready state and already
staged rows remain intact. Success and failure both release this scheduling
gate when the command becomes idle.

Quick NEW captures the raw-image ID during INITDR's existing header read and
reuses that result after successful version acceptance. It does not reread the
header before beginning NEWMAP. The shared initializer retains only two ID bytes,
not another sector. Full NEW keeps its explicit command ID, and DUPLICATE retains
its existing independent source/target initialization.

Shared INITDR also retains its captured media reference before the first
refill. Failed initialization after an earlier dirty NEW cannot silently
drop that disk's backing flush obligation, whether entered through another
NEW, ordinary I, VALIDATE or DUPLICATE.

Command completion is not a backing-file flush. Existing facade finalization
or remount owns that persistence boundary. REL, arbitrary memory execution,
transport collection and atomic production cutover remain excluded.

Historical full-engine quick-NEW captures are not fresh evidence for this
isolated slice. Healthy original-controller full FORMAT/DUPLICATE execution
and destructive physical acceptance remain open. No SD-card files or physical
media were changed, and no new latency or IEEE electrical claim is made.

## Validation

The isolated candidate passes 3434 focused DOS cases and 4417 total firmware
Check cases (4370 ordinary plus 47 fatal/assert cases). Tests and dependencies
use Debug. The RP2040 production RelWithDebInfo build, clang-format dry run
and whitespace checks pass.

Every final merged #117 test group and registration remains, including deferred
directory errors, retained entry ownership and COPY binding invalidation. The excluded-command boundary
now uses memory command `M` rather than newly supported `N`. New tests cover
complete images, both DUPLICATE directions, destructive failures, stale
generations, channel/scratch ownership, retained ID/empty-label boundaries,
parser rejection and status consumption around failed INITDR.
First-round feedback regressions add 48 model/scenario variants. Those fixtures linked
against initial PR production reproduce 38 failures with zero errors. They
cover prefix patterns and prefix-equals controls, peer errors before completion
or version rejection, subsequent drive-less NEW, partial HEADER/final reload,
first-write failures, native-write/allocation/finalization recovery, and
preserved dirty cache on pre-scratch rejection.
The second feedback round adds 39 further variants for colon-adjacent equals,
backing aliases, direct failed-format finalization/retry/remount, and native
internal-reader capacity/cleanup. Final fixtures reproduce 34 failures against
the first feedback fix and 60 against initial PR production, with zero errors.
The third round adds 54 variants for sequential dirty-NEW initialization
failures, native comma-table boundaries and actual late metadata/NF05 reads.
Final fixtures reproduce 35 failures against the second feedback fix and 101
against initial PR production, with zero errors. Late-read tests also assert
the one-operation-per-turn bound and recovery without leaked formatter pages.
The fourth round adds 14 variants for surviving-SA0 footer boundaries and
failure cleanup, BAM-read precedence over exhausted indexes/pages, and quick/full
4040 completion without a reread. Two earlier 4040 late-read fixtures now
correctly expect success. Final fixtures reproduce 16 failures against the
third feedback fix and 116 against initial PR production, with zero errors.
The fifth round adds six variants for reusing the initialized ID on both disks
of each model without a pre-metadata header reread. The three historical
extra-header recovery cases now inject failure at the required initial header
read instead. Final fixtures reproduce six failures against the fourth feedback
fix and 119 against initial PR production, with zero errors.
