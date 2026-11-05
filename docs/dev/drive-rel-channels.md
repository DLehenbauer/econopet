# REL channel storage and ownership foundation

This dependency slice follows the cached REL write primitives in #121. It
adds compact per-channel REL storage and an internal three-page claim helper.
It does not enable public REL OPEN, positioned reads, writes or CLOSE.
Production facade routing, the legacy engine and flat HDD support remain
unchanged.

## Included contracts

`dos_rel_channel_claim` reserves two data pages and one side/index page from
the existing twelve-page pool. It reuses the lowest available file-channel
index and highest free pages, excluding the existing internal directory
reservation. An exhausted logical table or a failed partial page claim returns
DOS_UNUSED without publishing a channel or changing status. GETBUF may still
invalidate the unowned pages' old cache/job metadata during a failed attempt.

Successful reservation tags the channel REL, keeps readiness false and
initializes its compact reader. It captures the caller's media reference
without reading sectors or checking disk contents. Supply an unowned decoded
data secondary. The helper is an admission primitive, not complete native
OPNRCH: native initial reads, read/write channel flags, record-size setup,
index jobs and record readiness remain later owner work.

Ordinary channel metadata and `dos_rel_reader_t` share file_type-tagged
storage. All existing ordinary fields, including `directory_searches`, remain.
Media identity, pool indices, access, directory routing, allocation, readiness,
failure and teardown fields stay outside the union. A populated reader must
not masquerade as an ordinary listing or writer when released. The existing
4608-byte per-unit bound passes in both host and RP2040 builds.

Use the existing REL leaf APIs with the claimed pages, not ordinary
`dos_stage/dos_get/dos_put`. This slice supports only clean read ownership.
The existing clean release path is resource cleanup, not native CLSREL's
SCRUB/SSEND/directory publication. Dirty pages and full REL CLOSE remain
excluded. No scheduler, public dispatch, page pool, heap or length cache is
added.

## Native evidence

The targets remain 4040 DOS 2.0 revision 2 (`901468-14/-15/-16`) and full-size
8050 MPI single-sided / 8250 MPI double-sided DOS 2.7
(`901887-01/901888-01`). Both pinned master compositions were consulted.
The approved 4040 overlays make GETBUF wait for the previous job and change
POSBUF sequencing. Existing synchronous cache invalidation is retained here,
not presented as controller concurrency.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Three-page REL ownership | All selected targets | [4040 opchnl][4040-opchnl] / [DOS 2.7 opchnl][8250-opchnl], OPNRCH/OR10/OR20 | Reserve two data pages and one SS page. Claim alone publishes no readable byte, performs no index/data I/O and does not reproduce complete OPNRCH ordering | `dos_rel_channel_claim`, owned-reader and pool/peer tests | [VICE REL open][vice] independently owns dual data buffers and side-sector storage, but uses heap/cached side arrays rather than the bounded native pool |
| Selection, exhaustion and rollback | All selected targets | [4040 tst3][4040-tst3] / [DOS 2.7 tst3][8250-tst3], GETRCH/FNDLNX/GETBUF or JGETBF/GBERR/RELBUF | Reuse the lowest eligible logical index and highest free data pages. A shortage of zero, one or two pages returns all partial claims without publishing ownership. The host primitive returns DOS_UNUSED rather than invoking CMDERR/70 itself | Existing `allocate_channel/get_buffer`, new partial-pool/logical-exhaustion tests and directory-reservation checks | VICE's separate buffer ownership corroborates resource separation, not native allocation order or error timing. [MAME 4040][mame-4040] / [8050/8250][mame-8050] establish the selected ROM/hardware modes |
| Drive/job identity and page reuse | All selected targets | Both tst3 GETBU4/GETBU5, OPNRCH/INTPNT and respective jobs/tsutil | Clear old residency/failure/write-job state for claimed pages and retain the selected media drive independently of the caller's previous drive. Existing bytes are not scrubbed on read admission | Existing allocation policy, both-drive pool tests | VICE independently tracks current/next page track/sector metadata but does not model this synchronous host pool or native wait jobs |
| Tagged storage and clean teardown | Host layout adaptation protecting all targets | No direct ROM entry for the C union. [4040 ramvar][4040-ramvar] / [DOS 2.7 ramvar][8250-ramvar] retain per-logical-index file/record state, both tst3 FRECHN/RELBUF/RELINX release data and SS ownership | A populated REL reader cannot alter access, directory routing or pool teardown. Return only this owner's three pages, preserve a peer and the directory reservation, and safely reuse the descriptor for an ordinary direct page | `dos_channel_t`, owned-reader teardown and peer/ordinary-reuse tests | VICE's distinct REL buffer mode and teardown corroborate type-specific ownership, not the C overlay. MAME confirms model selection, not execution of this host layout |
| Excluded native REL CLOSE | All selected targets, DOS 2.7 DYFILE qualification | [4040 close][4040-close] / [DOS 2.7 close][8250-close], CLSREL/CLSBIG/CLSRE1 | Full native completion must SCRUB, find the index endpoint and apply model-specific directory handling before release. This reservation slice does not claim that completion | Clean-only claim contract, no changes to `dos_close` or dirty scheduling | VICE separately fills/commits REL data on close. Neither its high-level path nor clean resource-release tests establish native CLSREL parity |

These are source-derived resource contracts plus explicitly labeled host
adaptations. No new public drive behavior, full-ROM controller comparison,
physical acceptance, timing or electrical claim is made. No ROM binaries or
disassembly are included.

## Validation and remaining work

The isolated candidate passes 444 focused REL cases (47 test groups) and
4861 total firmware cases (4814 ordinary plus 47 fatal/assert). Tests and
dependencies use Debug. The RP2040 production RelWithDebInfo build,
clang-format and whitespace checks pass. Existing engine function tokens,
all 29 REL helpers and all 44 dependency test groups/registrations are
preserved. The imported claim and owned-reader test retain their future
implementation tokens.

Cold owner scheduling/output, native OPEN and resident RECORD/POSBUF reuse,
dirty NRBUF/writeback/retry, reservation/BAM publication, creation/growth,
LISTEN/status, full CLOSE and REL COPY remain separate dependency slices.

[4040-opchnl]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/opchnl
[8250-opchnl]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/opchnl
[4040-tst3]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst3
[8250-tst3]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst3
[4040-ramvar]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ramvar
[8250-ramvar]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ramvar
[4040-close]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/close
[8250-close]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/close
[vice]: https://github.com/VICE-Team/svn-mirror/blob/a0e14df25846cd7f06043f24b48677bf45c02149/vice/src/vdrive/vdrive-rel.c#L853-L914
[mame-4040]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c2040.cpp
[mame-8050]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c8050.cpp
