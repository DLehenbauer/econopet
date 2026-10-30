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

Command completion is not a backing-file flush. Existing facade finalization
or remount owns that persistence boundary. REL, arbitrary memory execution,
transport collection and atomic production cutover remain excluded.

Historical full-engine quick-NEW captures are not fresh evidence for this
isolated slice. Healthy original-controller full FORMAT/DUPLICATE execution
and destructive physical acceptance remain open. No SD-card files or physical
media were changed, and no new latency or IEEE electrical claim is made.

## Validation

The isolated candidate passes 3273 focused DOS cases and 4256 total firmware
Check cases (4209 ordinary plus 47 fatal/assert cases). Tests and dependencies
use Debug. The RP2040 production RelWithDebInfo build, clang-format dry run
and whitespace checks pass.

Every final merged #117 test group and registration remains, including deferred
directory errors, retained entry ownership and COPY binding invalidation. The excluded-command boundary
now uses memory command `M` rather than newly supported `N`. New tests cover
complete images, both DUPLICATE directions, destructive failures, stale
generations, channel/scratch ownership, retained ID/empty-label boundaries,
parser rejection and status consumption around failed INITDR.
