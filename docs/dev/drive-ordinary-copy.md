# Ordinary COPY foundation

This dependency slice extends the direct/block/reset foundation in #116.
It adds ordinary-file COPY to the complete-request `dos_command` API, not
the production facade. The legacy engine and flat HDD support stay active.

## Included behavior

Named COPY, concatenation, SEQ/USR self-append, colonless whole-drive file
copy (`C0=1`), and patterned directory copy (`C0:*=1:pattern`) share the
existing channel/page pool. The first source supplies the output type.
Internal reads copy payload bytes directly, including PRG load-address bytes,
without ordinary IEEE GET's prepared-byte/singleton EOF behavior.

COPY retains five filename descriptors (one target and up to four sources),
with bounded names and a saved directory iterator. Each worker services a
directory sector, a read/append continuation, or at most 64 payload bytes.
There is no heap allocation, extra sector pool or whole-file chain cache.
Callers finish `dos_work_pending` before submitting another request.

## Native evidence

The selected targets are 4040 DOS 2.0 revision 2 (`901468-14/-15/-16`),
8050 MPI single-sided and 8250 MPI double-sided DOS 2.7
(`901887-01/901888-01`). Both immutable [4040 master][4040-master] and
[DOS 2.7 master][8250-master] compositions were consulted. These are not
claims about DOS 2.5, 8250LP or arbitrary controller ROMs.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Dispatch and parsed groups | All selected targets | Both [4040 copysetup][4040-setup]/[DOS 2.7 copysetup][8250-setup], `DSKCPY/PRSEQ/DX0000/DX0005/DX0010`, respective `parsex:PARSE/TAGCMD` and `setdrv:ALLDRS/SETDRV` | A colon selects named/pattern parsing. Comma before that colon returns 34. Colonless selectors immediately surround `=` and must be distinct ASCII 0/1 | `copy_start`, parser, retained-tail, drive/default, shifted-CR and table-capacity tests | [VICE command COPY][vice] implements a different general/CMD-HD parser (including six sources). Its limits and wildcard policy are not native authority |
| Lookup and output precedence | All selected targets | Both [4040 rename][4040-copy]/[DOS 2.7 rename][8250-copy], `COPY/CHKIN/CHKIO/CY`, respective `lookup:OPTSCH/SCHTBL/LOOKUP` | Check all sources before an existing destination returns 63. A missing source returns 62. Select output type from the first source | `copy_search/copy_lookup_finish`, missing-source, duplicate, type, dual-drive and fallback tests | VICE independently checks sources before output and retains first-source type. [MAME 4040][mame-4040]/[8050][mame-8050] corroborate selected ROM/hardware identities |
| Self-concatenation | All selected targets | Both rename sources, `COPY/COP05/CY/CY10/CY15/CY20`, respective `open:APPEND/GCBYTE/AP30` | Same resolved target and first-source entry plus further inputs appends SEQ/USR. PRG append returns 64. No new directory entry. Native read/write block-count history is retained | `copy_lookup_finish/copy_work`, self-append payload, type and block-count tests | VICE corroborates ordinary concatenation, not these native self-append rules. MAME confirms the ROMs executed by the original-hardware models |
| Source data and track decoding | 4040 versus selected DOS 2.7 modes | Both rename sources, `OPIRFL/GIBYTE/GCBYTE`, [4040 open][4040-open]/[DOS 2.7 open][8250-open], `OPREAD/OP130`, respective `opchnl:OPNRCH`, `tst2:STRDBL/RDBUF` | Internal source pointer starts at payload offset 2. Preserve empty/singleton/linked bytes without stripping a PRG header. 4040 masks the directory start track with $3F only at OPREAD. DOS 2.7 uses the full track byte | `copy_read_start/copy_work`, payload, residue, linked/singleton and model-specific high-track tests | VICE corroborates ordinary payload copying. MAME separately corroborates model selection, not the $3F rule (established by both ROM listings) |
| Whole-drive file iteration | All selected targets | Both [4040 copyall][4040-all]/[DOS 2.7 copyall][8250-all], `MOVLP1/MOVLP2/PUPS1/FFST/FFRE/ENDIT` | `C0=1` copies files in directory order, not raw disk sectors. Preserve the source iterator across each file. Run ENDCMD only after the iteration. Keep already completed files after a later failure | `copy_all_next/copy_work`, multi-sector iteration, empty source, completion-prefix and recovery tests | VICE confirms file-level COPY generally, but does not establish native colonless iteration. MAME corroborates the exact dual-drive models |
| Pattern retention and collision | 4040 versus selected DOS 2.7 modes | Both copyall sources, `CPYDTD/FIXIT/TRFNME/EXLP0/EXLP2`, respective `lookup:LK25/LK26/COMPAR/CMPCHK/CC15` | Save the source pattern while replacing per-file target names. Literal $A0 remains part of a parsed pattern. 4040 existing target returns 63. DOS 2.7 retains the pattern-type flag and repeats the matching entry | `copy_start/copy_all_next/copy_lookup_finish`, patterned names, literal-padding-byte, per-model collision and pending-stall tests | VICE rejects wildcard targets, so it disagrees and is not authority for CPYDTD. MAME corroborates the selected native ROM sets, not an independent replay of the stall |
| Completion and failed ownership | All selected targets | Both rename sources, `CY20`, respective `close:CLSWRT/CLSDIR/MAPOUT` and `tst4:FREICH` | Success closes output and persists its data/directory/map. Failure frees only internal COPY channels without CLOSE, leaving partial output unclosed and completed earlier files intact | `copy_finish/copy_work`, multi-channel, allocation, read/write/flush, stale-identity and recovery tests | VICE corroborates ordinary file-copy completion. Storage flush, stale generations and retained host namespace intents are firmware policies |
| Short names and transferred names | All selected targets | Both [4040 lookup][4040-lookup]/[DOS 2.7 lookup][8250-lookup], `COMPAR/CP34`, respective copyall `TRFNME` | A supplied short name requires one following $A0, not padding through byte 16. A transferred copy-all name retains all 16 directory bytes | `copy_search/copy_all_next`, padded source, destination collision and full-field transferred-name tests | Native lookup and transfer routines are authoritative. Host namespace reservations are separately described below |
| Read-before-write boundary | All selected targets | Both rename sources, `CY20/GIBYTE/GCBYTE/CY15`, respective [4040 tst4][4040-tst4]/[DOS 2.7 tst4][8250-tst4], `GBYTE/GET/SEQGET`, and [4040 tst2][4040-tst2]/[DOS 2.7 tst2][8250-tst2], `STRDBL/RDBYT/DBLBUF` | Cache speculative file-data read failures until selection. Wait for the successor before sending the linked page's last byte to PIBYTE. A third-page failure leaves only one full output block persisted | `copy_read_start/copy_work`, second/third-page fault tests inspect persisted partial chains and untouched successor sectors | Synchronous backing I/O is a host adaptation, not controller scheduling or retry equivalence |
| 4040 negative track match | 4040 DOS 2.0 revision 2 | 4040 lookup `CP40/CMPCHK/CC15/LK26` | A named source or destination match with raw track bit 7 set stays at the same unresolved entry. Bit 6 alone still reaches OPREAD's $3F mask | `copy_search`, $81/$C1 source/target pending, no-I/O and reset-cleanup tests | DOS 2.7 CC15 uses the pattern flag instead. Its raw track follows the separate full-byte read path |
| Discarded and selected directory jobs | All selected targets | Both rename `OPIRFL`, open `OPREAD`, respective [4040 tst3][4040-tst3]/[DOS 2.7 tst3][8250-tst3], `GETRCH/FRECHN/GETBUF`, lookup `SRRE/SEARCH/SR40`, and copyall `FFST/FFRE/FIXIT/CY` | Reopening IRSA discards the old directory prefetch result. Complete current-page matches before observing a required successor failure. A new FFRE owns a new job, not an earlier discarded error | `copy_read_start/copy_all_next`, self-append unused-successor faults, persisted current-page prefix and transient-versus-persistent read-fault tests | 4040 GETBUF clears JOBS directly. DOS 2.7 waits for job completion then clears JOBS without interpreting its old error. No replay or controller-retry claim |
| Source-group boundaries | All selected targets | Both copysetup `DSKCPY`, respective [4040 parsex][4040-parsex]/[DOS 2.7 parsex][8250-parsex], `TC50/PARSE`, and [4040 trnsfr][4040-trnsfr]/[DOS 2.7 trnsfr][8250-trnsfr], `FNDLMT` | A later '=' ends its filename, not the comma-separated source group. Later missing inputs still return 62 before output creation | `copy_start`, second '=' in first/later source plus following absent-source regressions | Filename truncation and group parsing have distinct native routines |
| Operation-local drive selection | All selected targets | Both rename `CY20/GIBYTE/PIBYTE`, respective tst2 `WRT0/SETDRN`, and open `APPEND` | Reselect the writer during yielded APPEND and before output allocation. Reselect the reader before subsequent linked-read checks. DOS 2.7 status suffix identifies the failing operation's disk | `copy_work`, interleaved peer/read-error and bidirectional read-versus-allocation status regressions | Cooperative yielding is a host adaptation, not native processor or controller timing |
| Zero-track lookup match | All selected targets | Both lookup `CP40/CMPCHK/CC15/LK25/LK26` | A matched zero FILTRK stays unresolved and repeats the same entry, for source or destination. Do not convert it to missing-source or file-exists completion | `copy_search`, per-model zero-track source/target pending, no-I/O and reset-cleanup tests | This LOOKUP rule is not mechanically imposed on FFST's separately selected copy-all source |

[4040-master]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/master
[8250-master]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/master
[4040-setup]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/copysetup
[8250-setup]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/copysetup
[4040-copy]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rename
[8250-copy]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rename
[4040-all]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/copyall
[8250-all]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/copyall
[4040-open]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/open
[8250-open]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/open
[4040-lookup]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/lookup
[8250-lookup]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/lookup
[4040-tst4]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4
[8250-tst4]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst4
[4040-tst2]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
[8250-tst2]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
[4040-tst3]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst3
[8250-tst3]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst3
[4040-parsex]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/parsex
[8250-parsex]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/parsex
[4040-trnsfr]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/trnsfr
[8250-trnsfr]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/trnsfr
[vice]: https://github.com/VICE-Team/svn-mirror/blob/a0e14df25846cd7f06043f24b48677bf45c02149/vice/src/vdrive/vdrive-command.c#L815-L1010
[mame-4040]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c2040.cpp
[mame-8050]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c8050.cpp

## Host adaptations and exclusions

Bounded cooperative jobs, synchronous image reads, captured media identities,
storage-error translation, corruption limits and backing flush are host
mechanisms, not native controller timing. COPY honors the existing OPEN/RENAME
reservation for an unpublished writer's canonical filename. It remembers a
conflict before yielding and checks again before writing, returning host policy
25 even if CLOSE publishes and releases that owner during lookup. Missing-source
precedence is retained. This policy is not a native ROM namespace algorithm.
The same retained-owner policy protects deleted directory coordinates and both
ends of an incomplete directory-growth link. Ordinary OPEN and COPY share those
guards. Whole-drive and patterned COPY also check the source directory iterator
before reading an incompletely published growth sector. A later CLOSE may
recover the original writer without overwriting COPY's persisted entry.
COPY also shares OPEN's retained `DOS_CLOSE_RECLAIM` entry exclusion. A failed
replacement CLOSE can free the old chain without publishing its new link, so
the visible old entry must not be trusted after those sectors are reassigned.
Check all resolved entries before creating output or starting self-append, then
recheck each deferred source when opening it. Preserve the retry owner and
return host policy 25 rather than reading or overwriting another file's data.
A successful peer replacement CLOSE can instead release that owner while a
deferred COPY descriptor still names the old chain. Before opening a source,
compare its captured start link, type and name against the directory sector
already read for OPIRFL. Reject a changed binding with host policy 25 rather
than copying reassigned sectors or silently selecting a different source.
This opening check adds no page, state field or backing read.

An already-open COPY reader or self-append writer must also stop if a peer
replacement CLOSE retires its chain between worker calls. Before CLSDIR releases
that chain, record the affected live descriptor in one byte of COPY state.
The next continuation aborts with host policy 25 before any further read or
write. Match media identity, unit, slot and exact entry coordinates. Exclude
COPY's own writer, completed sources, and unrelated entries. The copy-all cursor
becomes live when each new entry is selected, not only after destination lookup.
This avoids repeated directory reads and does not block the peer's CLOSE.

Self-append judges completion from its writer's allocation and failure state,
not the unit's shared status. An unrelated reader may report an error while the
append traversal yields, without abandoning an otherwise healthy COPY writer.

Directory prefetch still performs the backing read and retains partial buffer
contents. OPREAD discards its unused result when reopening IRSA for file data.
The copy-all iterator observes a successor error only when it exhausts the
current directory page without handing another match to CY. After each file,
native SRRE reopens the directory and starts a new job. Consequently an old
discarded error must not be latched across per-file work. A transient first
prefetch error can disappear on re-entry, while a persistently unreadable
successor stops enumeration after already completed current-page files.

Malformed empty or overlapping CPYDTD source-text relocation returns explicit
host policy 30 instead of reproducing command-RAM corruption. Parsed source
starts outside retained command storage are also rejected. A DOS 2.7 patterned
collision is represented as pending work without spinning the MCU. Reset can
release it, but no timeout converts it into success or error 63.
The 4040 negative-track named match uses the same cooperative wait representation.

All previously landed read/write, metadata and reset behavior is retained.
COPY failure does not bypass finalization's orphaned-BAM persistence or flush
and stale-identity failures. Exhausted ordinary allocation remains 72 on 4040
and 67 on DOS 2.7.

REL sources (including later concatenation inputs) return host policy 30 before
creating that file's output. REL state, indexing and COPY will be extracted
later. FORMAT/DUPLICATE, arbitrary memory execution, transport collection,
production cutover and HDD removal are outside this slice. An all-files command
can have completed earlier ordinary files before encountering excluded REL.

Historical full-engine original-ROM captures are not fresh evidence for this
isolated slice. There is no new claim of complete ROM equivalence, board
acceptance, electrical handshake behavior, controller retries, physical head
movement or service latency.

## Validation

The isolated candidate passes 3162 focused DOS cases and 4145 total firmware
Check cases (4098 ordinary plus 47 fatal/assert cases). Tests and their
dependencies use Debug. The RP2040 production RelWithDebInfo build,
clang-format dry run and whitespace checks pass.

Every final merged #116 test group and registration is retained, including
direct-channel MAPOUT, alternate-page RAM and stale-generation fixes. Its excluded-command
boundary now uses FORMAT (`N`) rather than newly supported COPY (`C`).
Focused additions cover unpublished-name ownership across a yield/CLOSE,
literal $A0 patterns, excluded REL sources and model-specific track decoding.
Feedback regressions additionally cover retained directory slots/growth,
source-directory iterator ownership, padded short-name lookup versus transferred
full fields, native linked-read failure ordering, cooperative negative-track
waits, and peer read-error status during self-append.
Second-review regressions cover unused directory errors during self-append and
whole-drive/patterned iteration with persistent or discarded transient failures.
Third-review regressions cover later sources after '=', failed replacement
publication with original-sector reuse, a CLOSE between lookup and deferred
source opening, and full cross-drive status strings for APPEND/read/allocation
failures.
Fourth-review regressions use successful replacement CLOSE followed by an
already-open peer writer's PUT/CLOSE to reassign the original sector before
first/later named, whole-drive or patterned source opening. They verify stale
COPY rejection, untouched peer data and successful fresh-command recovery.
Fifth-review regressions retire active reader/self-append bindings before all
old sectors are reassigned, allow replacement CLOSE after a copy-all source
already completed, and preserve both ROM families' zero-track lookup waits.
No fresh original-ROM replay or board acceptance was performed.
