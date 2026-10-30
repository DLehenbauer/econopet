# Metadata command foundation

This is the sixth extraction slice, dependent on the ordinary-file write
foundation in #114. It adds directly testable RENAME, SCRATCH and VALIDATE.
The legacy production engine and flat HDD support remain active. This is
not the production facade cutover.

## Entry and ownership boundary

`dos_command(unit, media, index, request, length)` accepts an already-collected
request. Complete preceding work before submitting another command or OPEN.
It preserves the bounded retained command page, applies CMDSET's CR trimming
before the logical length limit, and retires CLRCB's bytes after retaining
worker-specific names and patterns. `dos_work()` advances the selected job
and `dos_work_pending()` reports OPEN or metadata work.

This API does not collect IEEE command-channel bytes, interpret transport EOI
or implement OPEN-15 versus secondary-15 dispatch. Those integrations remain
deferred. INITIALIZE is included only as VALIDATE's internal prerequisite and
error recovery, not as a public `I` command. Warm reset, user vectors, direct
channels, block commands, COPY, FORMAT/DUPLICATE and REL OPEN/data APIs remain
excluded. Recognized excluded commands return explicit host policy error 30,
not a claim about their native result.

The command tag owns a shared transient union. RENAME and SCRATCH borrow two
existing pool pages. VALIDATE borrows one internal reader and DOS 2.7's fixed
map pages, protecting live peers with host policy 70. There is no new sector
pool, heap allocation or whole-file chain cache. The existing per-unit
4608-byte compile-time ceiling remains enforced.

## Included operations

| Operation | Behavior |
|-----------|----------|
| RENAME | Native colon/equals syntax, bounded names, source-before-collision errors, default/explicit drive selection, cross-drive collision search without moving the source, empty-target spill, commit-time directory reread and unfinished-writer name retention |
| SCRATCH | Five bounded patterns, native wildcard/type/group selection and drive search order, active/locked protection, type-byte deletion before linked allocation release, byte-sized deletion count and nontransactional persistence |
| VALIDATE | Selected-drive abandonment without CLOSE, free-map reconstruction, closed-chain and directory-chain traversal, unclosed-entry deletion, preserved block counts, 4040 duplicate tolerance versus DOS 2.7 NO BLOCK, ordered map writes and delegated recovery |

Metadata matching retains explicit pattern lengths, including literal `$a0`
bytes, instead of treating padding as a host string terminator. VALIDATE
reuses the landed read foundation's active-page last-job drive selection
when abandoning channels. The #113 parser, directory, read-selection,
cache-identity and failed-refill fixes remain intact.

Generic native side-chain links are traversed without enabling REL channels
or record operations. This is not a claim of complete REL acceptance.
Host traversal bounds, generation guards, backing-store flushes, missing-media
responses, resource protection and generic I/O error mappings remain explicit
adaptations. Published deletions or completed map writes are not rolled back
after a later failure.

## Native evidence

Targets remain 4040 DOS 2.0 revision 2 (901468-14/-15/-16), 8050 MPI
single-sided and 8250 MPI double-sided using shared DOS 2.7
(901887-01/901888-01). Earlier DOS 2.5 and 8250LP are not substitutes.
The [ROM reconstruction tools](../../tools/drive-roms/README.md) identify
composition and approved overlays.

Primary source is `mist64/cbmsrc` at
[`01bd60f162ef92212ef0cb67546ae8f42be34168`](https://github.com/mist64/cbmsrc/tree/01bd60f162ef92212ef0cb67546ae8f42be34168).
Both model masters and the following routines were consulted. Each worker
also retains its pinned source block beside the implementation.

| Contract and routines | 4040 | Shared DOS 2.7 |
|-----------|----------|----------|
| Command structure and completion (`CMDSET`, `SIMPRS`, `TAGCMD`, `ENDCMD`) | [parsex](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/parsex) | [parsex](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/parsex) |
| Native name bounds and transfer (`FNDLMT`, `TRCMBF`, `TRNAME`) | [trnsfr](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/trnsfr) | [trnsfr](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/trnsfr) |
| Rename and error precedence (`RENAME`, `CHKIN`, `CHKIO`) | [rename](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rename) | [rename](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rename) |
| Matching and allocation release (`COMPAR`, `SCRTCH`, `DELDIR`, `DELFIL`) | [lookup](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/lookup), [scrtch](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/scrtch) | [lookup](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/lookup), [scrtch](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/scrtch) |
| Initialization and map reconstruction (`INITDR`, `VALDAT`, `NEWMPV`, `MRKBAM`/`VMKBAM`, `VUSED`, `WTMAPS`) | [tst2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2), [verdir](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/verdir) | [tst2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2), [verdir](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/verdir), [map](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/map) |

The [reference tools](../../tools/drive-ref/README.md) contain independent
original-ROM rename, scratch and validate scenarios. Retained earlier
full-engine captures corroborate normal status/data and resulting images
on all three models. They are historical evidence, not fresh replay results
for this isolated foundation or a complete dual-drive/fault/resource matrix.
The pinned [MAME 8050/8250 model](https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c8050.cpp)
also corroborates DOS 2.7's mirrored fixed map pages.

## Extraction validation

The isolated writer-based candidate passes 1251 direct DOS cases and 2234
total firmware Check cases (2187 ordinary plus 47 fatal/assert cases), with
tests and dependencies built in Debug. The RP2040 production build passes
in RelWithDebInfo. Formatting and whitespace checks pass.

The metadata additions cover parser boundaries, explicit name lengths,
status/error precedence, drive selection, retained peers, stale identities,
resource pressure, storage failures and recovery, exact persisted metadata
and allocation maps. Existing read/status and ordinary-write tests remain
enabled, including all regression tests from the final merged #114.
COPY-dependent follow-on tests and public REL-owner tests are not
silently treated as metadata-only coverage.

Passing these checks does not establish full ROM equivalence, fresh board
acceptance or IEEE electrical/timing compatibility. Production integration
and its hardware/reference acceptance remain later gates.
