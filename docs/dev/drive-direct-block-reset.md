# Direct/block and reset foundation

This dependency slice extends the metadata foundation in #115. It is not a
production facade cutover. The legacy production engine and flat HDD support
remain unchanged.

## Included behavior

The direct APIs now support `OPEN "#"`/`"#n"`, shared circular GET/PUT,
`B-P`, `B-R`, `B-W`, `B-A`, `B-F`, raw `U1`/`UA` and `U2`/`UB` aliases,
public INITIALIZE, fixed user-table restoration, and factory warm, cold and
interrupt vectors. Commands still enter through `dos_command`, not an
OPEN-15/LISTEN collector. Callers complete `dos_work_pending` before the next
request and commit only actually consumed staged bytes through `dos_get`.

Direct pages share the existing twelve-page pool. Their prepared byte,
last-byte marker and EOI alias ordinary/directory cursor storage. Block
workers borrow the channel's active page and retain the captured media
identity. No heap, extra sector pool, per-file chain cache or REL state is
introduced.

Ordinary readers retain their sequential GET and EOF behavior after block
operations. B-P prepares the selected byte without incrementing, so ordinary
GET normally repeats it. Raw U1 on an ordinary reader emits the duplicated
first byte followed by the raw page (257 bytes total), then becomes not ready.
Direct channels remain circular after EOI. LOAD secondary 0 retains its
distinct sequential-before-EOI direct behavior.

## Native evidence

The targets are 4040 DOS 2.0 revision 2 (`901468-14/-15/-16`), 8050 MPI
single-sided and 8250 MPI double-sided DOS 2.7 (`901887-01/901888-01`).
The source revision below is immutable. Consult both `master` compositions,
not an earlier DOS 2.5 or 8250LP listing. The approved 4040 revision-2
receive limit and DOS 2.7 BW20 logical-index reload remain the established
overlays documented by the ROM provenance tooling.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Direct allocation and RAM cursor | All selected targets | Both [4040 block][4040-block]/[DOS 2.7 block][8250-block], `OPNBLK/OB10/OB30/BP05/ASCHEX`, respective `tst3:GETRCH/GETBUF/FNDLNX` and `tst4:GET/RNDGET` | Highest free page for bare #, explicit reservation before channel search, binary page number, pointer 1, sticky EOI and circular reads | `open_direct`, direct parser/ownership/wrap tests | [VICE IEC][vice-iec] confirms memory pages, initial pointer 1 and wrap to 1. Its explicit-page FIXME is not an allocation authority. [MAME 4040][mame-4040]/[8050][mame-8050] corroborate physical shared RAM and ROM selection |
| Ordinary/direct B-P | All selected targets | Both block sources, `BLKPTR/BUFTST`, respective `tst2:GETPRE/GETBYT/RDBYT/DBLBUF` and `tst4:RNGET2` | SA 2..13, no increment during positioning, preserve pending EOI, revive ordinary readers only at LSTCHR equality | `block_start`, `materialize_reader`, `read_pointer_work`, offset/singleton/boundary tests | [VICE commands][vice-command] independently supplies B-P but is not authoritative for native ordinary-reader or sticky-EOI behavior. MAME confirms shared-page architecture |
| Counted versus raw transfers | All selected targets | Both block sources, `BLKRD3/BLKRD/UBLKRD/BLKWT/UBLKWT/BW20`, respective `tst4:DRTRD/DRTWRT` | B-R uses byte 0 as count. U1 exposes raw bytes. B-W writes max(1,pointer-1) at byte 0 and sets pointer 1 before I/O. Raw writes preserve cursor/count | `block_read_work`, `block_write_work`, direct/ordinary boundary, drive, version, failure and retry tests | VICE command handlers independently corroborate counted/raw sector operations, not ordinary-channel eligibility or exact U2 cursor retention |
| Map claims and suggestions | All selected targets | Both block sources, `BLKFRE/BLKALC/BLKTST`, respective `frets:FRETS/USEDTS`, DOS 2.7 `map:SETMAP` | B-F changes cached allocation only. B-A claims only the exact requested free bit, otherwise reports 65 with an unclaimed forward suggestion, or 00,00 on exhaustion | `block_map_work`, claim/free/persistence, search, geometry and failure tests | VICE B-A/B-F handlers corroborate image allocation operations. Selected ROMs control search order and delayed map persistence |
| Direct CLOSE and initialization | All selected targets | Respective `close:CLSCHN/CLSC30/MAPOUT`, `tst2:INTDRV/INITDR`, `tst3:CLDCHN/FRECHN` | CLOSE enters MAPOUT without file finalization. INITIALIZE abandons only the selected active-job drive, reloads metadata and never CLOSEs unfinished writers | Direct `finish_channel`, existing `initialize_work/free_drive_channels`, selected-drive and retry cases | MAME confirms retained page/drive hardware. Host flush and stale-generation rejection are explicit firmware policies |
| Model-specific direct MAPOUT | 4040 revision 2 versus selected DOS 2.7 modes | [4040 close][4040-close], `MAPOUT/MAPCHK`, `tstfnd:AVCK`, `jobs:SETJOB`, [DOS 2.7 map][8250-map], `MO10/JSCRBM`, both `parsex:ENDCMD` | 4040 validates and writes even a clean BAM, exposing 26 for write protection, 71 for count/bitmap inconsistency and 73 for wrong DOS version. It retains DRVNUM across successful direct MAPOUT. DOS 2.7 sets DRVNUM from LSTJOB and skips clean-map writes | `prepare_mapout_4040/finish_channel`, `test_direct_close_clean_map_native_errors_and_retry`, `test_direct_close_remembered_drive_is_model_specific` cover selected/all CLOSE, lazy/cached maps and failure/retry | MAME corroborates model selection. Host callbacks and statuses test logical behavior, not controller retries or physical media |
| Explicit CLOSE 0 versus CLOSE 15 | All selected targets | [4040 close][4040-close]/[DOS 2.7 close][8250-close], `CLOSE/CLSALL/CLSCHN` | Explicit CLOSE 0 frees its direct owner without MAPOUT or ENDCMD. CLOSE 15 still visits SA 0 through CLSCHN and persists its dirty map | `dos_close`, `test_direct_close_zero_bypasses_mapout_only_when_explicit` covers writable/read-only backing and later persistence | MAME corroborates model selection. Host callbacks establish the absence of writes/flushes, not physical controller behavior |
| Direct OPEN error retirement | 4040 versus selected DOS 2.7 modes | Both block sources, `OB05/OB30`, [4040 erproc][4040-erproc]/[DOS 2.7 erproc][8250-erproc], `CMDER3/TLERR`, both `equate:DIRTYP/RELTYP` | DOS 2.7 retires the selected ordinary non-REL owner on direct OPEN failure, including numeric-policy rejection. DIRTYP preserves direct owners, including both overlength paths. Idle-role 4040 retains either owner | `command_error/open_direct`, `test_direct_open_errors_retire_only_model_eligible_owners`, `test_direct_load_save_and_overlong_request` | MAME corroborates shared channels/RAM. Malformed scratch-dependent numeric status 30 remains an explicit host policy |
| Replacing direct owners | All selected targets | Both `open:OP02/OP04/OPREAD/LOADIR`, `opchnl:OPNRCH`, `lstdir:STDIR`, `tst3:GETRCH/GETR2/FRECHN` | Reopen as ordinary data, raw directory, formatted listing or retained LOAD '*' releases the old direct page without entering CLOSE/MAPOUT | `open_directory/dos_work/dos_close`, `test_reopening_direct_owner_does_not_finalize_map` checks admission/header or data on writable/read-only backing | Native GETRCH distinguishes replacement from CLOSE. Later metadata paging can independently need MAPOUT, outside this admission check |
| User/reset vector selection | All selected targets | Both block sources, `USER/USRINT/USREXC`, [4040 vector][4040-vector]/[DOS 2.7 vector][8250-vector], `UBLOCK`, respective `ieee:ATNIRQ` | U0 preserves command RAM and owners. Index 8 enters warm NMI, 9 cold DSKINT, 10 ATNIRQ. IRQ retains channels/maps and short U repeats the selector | `dos_command/warm_reset/dos_reset`, alias/state snapshots and cold-vector tests | MAME independently corroborates the exact ROM sets and CPU/RAM mapping. Historical full-engine original-ROM captures support warm/IRQ observations, not this isolated slice |
| Deferred DOS 2.7 warm reload | 4040 versus selected DOS 2.7 modes | [4040 dskint][4040-dskint], `DIAGOK/INTTAB`, [DOS 2.7 begin][8250-begin], `BEGIN/CONFIG/CONF10`, `romtbl:MSCCOM`, `lookup:JAUTOI`, `tst2:INITDR`, [DOS 2.7 trnsfr][8250-trnsfr], `NEWDIR` | Preserve RAM without flush. 4040 retains dirty maps. DOS 2.7 marks both maps for deferred discard at direct OPEN, lookup or each formatted-directory header, not B-A/B-F or raw-directory OPEN | `auto_initialize/open_directory/prepare_directory_line`, warm entry-point and per-drive/raw-exemption matrices check both header orders and emitted free-block totals | MAME ROM/hardware identities and historical native warm captures corroborate model selection. Host tests establish only logical cache handling |
| Captured generation on page/drive switches | Host storage adaptation for all selected targets | `dos_media.h` unit-pair reference contract, not native ROM RAM | Keep the captured generation when switching alternate pages or entering a peer directory header. Cached old bytes do not authorize a refill from replacement media | `pointer_next_buffer/prepare_directory_line`, alternate-switch and directory-peer generation regressions observe zero replacement reads and eventual host 23 after cached data | Shared page/job-drive behavior is ROM-evidenced above. Host generations and rejection policy have no native controller equivalent |

[4040-block]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/block
[8250-block]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/block
[4040-vector]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/vector
[8250-vector]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/vector
[4040-dskint]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/dskint
[8250-begin]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/begin
[4040-close]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/close
[8250-close]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/close
[4040-erproc]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/erproc
[8250-erproc]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/erproc
[8250-trnsfr]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/trnsfr
[8250-map]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/map
[vice-iec]: https://github.com/VICE-Team/svn-mirror/blob/a0e14df25846cd7f06043f24b48677bf45c02149/vice/src/vdrive/vdrive-iec.c
[vice-command]: https://github.com/VICE-Team/svn-mirror/blob/a0e14df25846cd7f06043f24b48677bf45c02149/vice/src/vdrive/vdrive-command.c
[mame-4040]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c2040.cpp
[mame-8050]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c8050.cpp

## Adaptations and exclusions

Native RAM/controller jobs are represented by bounded cooperative state and
synchronous image callbacks. Host identity checks, generic storage-error
translation, corruption bounds, backing flush and failed-owner retention
are firmware policies, not controller/electrical emulation. AUTOI only
invalidates the selected lazy cache here, not a physical trial or INITDR
channel sweep.

This extraction retains all merged read/write fixes. A failed successor read
remains cached: a failed B-P/block normalization preserves the channel cursor
and readable old-page prefix, but does not silently retry or erase the failed
job when normal GET reaches it. DOS 2.7 CMDERR retires ordinary non-REL owners
but exempts direct owners, including on overlong OPEN. The host's explicit
numeric-syntax rejection uses the same model-aware retirement path.

Direct persistence never skips a dirty selected BAM or backing slot. A failed
block-write flush therefore remains retryable with its owner intact, even
though no allocation bit changed. DOS 2.7 native CLOSE and RAM-only host
finalization can avoid I/O when both are clean. Native 4040 CLOSE instead
prepares its unconditional MAPOUT validation/write, including lazy BAM load.
Explicit CLOSE 0 and direct-owner replacement still bypass that native CLOSE
entry. The captured owner
generation is checked before MAPOUT as well as before backing flush, so an
old RAM page cannot persist a newer generation's cached map. A retry also
includes B-A/B-F edits made after an earlier failed backing flush, rather than
treating the previous MAPOUT phase as proof that the current map is clean.
The durability and generation guards are host policies, not additional
native status claims. Switching a page's job drive or a listing's peer drive
changes only the slot within the captured pair generation, never silently
issues a new lifetime token.

COPY, FORMAT/DUPLICATE, REL, arbitrary memory/buffer execution, custom
USRJMP/VNMI, and user indices outside the factory table remain excluded.
Recognized excluded commands report explicit host policy 30. B-E is likewise
excluded. Formatted LOAD listings are not block-transfer targets. Scratch-RAM
dependent malformed numeric parsing is rejected explicitly instead of
inventing native scratch values.

Historical full-engine captures in the development worktree are not fresh
replay or board evidence for this isolated foundation. This slice makes no
new claim of complete ROM equivalence, hardware acceptance, electrical
behavior, controller retries, physical head movement or service latency.

## Validation

The feedback candidate adds 201 model/scenario variants and passes 2744
focused DOS cases and 3727 total firmware Check cases (3680 ordinary plus 47
fatal/assert cases). The same final fixtures linked against the original
PR implementation reproduce 129 failures (48 against the first feedback
commit). Firmware tests and their dependencies
use Debug. The RP2040 production RelWithDebInfo build,
clang-format dry run and whitespace checks pass.

All #115 test groups remain, with its exclusion-boundary case now using
still-excluded COPY instead of the newly supported block-command family.
The extracted tests additionally retain the final merged #115 metadata,
#114 writer and #113 read/status regression coverage. No fresh board acceptance or
original-ROM replay was performed for this dependency slice.
