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

## Review regression evidence

The following source references use the pinned revision above. Tests cover
4040, 8050 MPI and 8250 MPI unless a narrower target is stated.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|-----------------------|---------------|
| RENAME destination ownership, including a retained CLOSE during lookup | All three models, host publication policy | Both `rename:RENAME/CHKIO` establish source-before-collision ordering. Unpublished host intent has no ROM counterpart. | Preserve source error 62 and visible collision 63. Reject conflicting unpublished intent with host 25, even if CLOSE retires it after lookup passes its sector. | `unpublished_writer`, `rename_start`, `rename_work`; `test_rename_respects_unpublished_writer_namespace` | Same-disk, cross-drive, nonparticipating-peer and linked-directory interleavings observe persisted payloads. |
| Failed source publication retains one canonical name | All three models, host publication policy | Both `rename:RENAME` rewrite the directory name. Host partial-write ownership is an adapter boundary. | Reject RENAME of a still-unpublished source with host 25, preserving its original intent and bytes. A separate destination remains safe, and normal RENAME works after successful CLOSE. | `rename_work`; `test_rename_preserves_failed_source_publication_intent` | Injected partial directory writes, exact entry comparisons and later independent CLOSE/readback. |
| SCRATCH completion cannot act on replacement media | All three models, host generation policy | Both `scrtch:SCRTCH` establish command completion. Host pair identities and backing flushes have no ROM counterpart. | A stale completion returns host 25 without replacement read, write or flush callbacks, including no-match completion and peer-only rebinds. | `scratch_work`; `test_scratch_stale_completion_never_flushes_replacement` | Dirty replacement created through the media API remains dirty. Callback counters remain unchanged. |
| Two-byte selector precedes single-colon syntax | All three models | Both [setdrv](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/setdrv) / [setdrv](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/setdrv), `SETDRV/SD40` | `::SOURCE` consumes both colons. `:::SOURCE` leaves a literal leading colon. Apply the same rule to later SCRATCH patterns. | `rename_start`, `scratch_start`; existing RENAME and SCRATCH syntax tables | Success and missing-name controls verify both parser branches on each model. |
| Default SCRATCH patterns with an absent peer | 8050/8250 DOS 2.7 native behavior. 4040 missing host binding adaptation. | `DOS_8250/lookup:OPTSCH/OS50/COMPAR` reduces `DRVCNT` for an inactive peer. The 4040 source has no identical inactive-peer branch. | With the starting disk present and peer absent, include its defaulted patterns on the only scan rather than skipping them. | `scratch_start`; `test_scratch_mixed_defaults_require_third_drive_search` | Mixed explicit/default patterns run with peer present and absent. Do not infer native 4040 inactive-drive behavior from absent host bindings. |
| VALIDATE owns its delegated initialization outcome | All three models, cooperative host scheduling | Both `tst2:INITDR` and `verdir:VALDAT`, plus error recovery through `INITDR` | A peer's status 23 cannot abort successful initialization or replace the saved recovery error 25. An actual recovery read failure retains its own 23 and coordinates. | `initialize_work`, `validate_work`; `test_validate_initialization_ignores_interleaved_peer_status` | Orphan-map repair, peer ownership, successful recovery and actual recovery failure are independently observed. |
| Saved data starts remain pending after side-chain traversal | All three models | Both `verdir:VD15/VD17` unconditionally restore and process the saved data start | Reject an invalid saved data start before map publication, including track zero. Do not confuse zero with absence of pending work. | `validate_work`; `test_validate_invalid_data_start_after_side_chain` | Side-chain and no-side-chain controls verify exact status, unchanged image and zero writes. |
| Invalid VALIDATE geometry uses the native command error | All three models | `DOS_8250/verdir:VMKBAM`, both [4040 jobs:SETJOB/TSCHK](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/jobs) / [DOS 2.7 jobs:SETJOB/TSCHK](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/jobs), `BADTS/CMDER2` | Return 66 with the invalid track/sector, without controller-error initialization recovery. The 4040 unchecked `MRKBAM` is not emulated as unsafe host indexing. | `validate_used`, `validate_work`; invalid-start controls and `test_validate_corruption_and_side_links` | Zero track, excessive track and excessive sector cases distinguish geometry errors from backing-read failures. Both `erproc` sources define `BADTS=$66`, distinct from `SYSTS=$67`, although the message is shared. |
| SCRATCH releases side chains before attempting invalid data starts | All three models | Both `scrtch:SC15/DELFIL/MAPOUT/SC17`. DOS 2.7 [frets:SETMAP](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/frets) and both `jobs:SETJOB` paths reject track zero with `BADTS/CMDER2`. | Persist side-chain allocation changes before rejecting a closed entry's invalid data start with 66. Unclosed entries skip data traversal. Side read/write failures retain precedence. | `scratch_work`, `data_pending`; `test_scratch_side_chain_precedes_invalid_data_start` | Exact whole-image comparisons cover closed and unclosed entries, missing side chains, side BAM write failure and side read failure. Bounded geometry checks avoid unsafe 4040 indexing while retaining its eventual native status. |
| SCRATCH counts and shared status fields use model-specific decimal widths | All three models | Both [erproc:HEXDEC/BCDDEC](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/erproc) / [erproc:HEXDEC/BCDDEC](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/erproc). `scrtch:SC20/SC30` retains an eight-bit count. | 4040 formats the low two decimal digits. DOS 2.7 preserves the optional hundreds digit. After 256 deletions, both report count 00. Apply the same conversion to error track and sector fields. | `dos_status`; `test_scratch_native_count_digit_boundaries`, `test_status_native_decimal_field_boundaries` | Real OPEN/CLOSE/SCRATCH sequences remove 99, 100, 199, 200, 255 and 256 files across two mounted disks. Independent directory checks verify deletion. Exact status records cover both numeric fields. |
| SCRATCH protects actual file bindings, not listing cursors | All three models | Both [4040 open:OPREAD](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/open) / [DOS 2.7 open:OPREAD](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/open), `tstflg:TSTCHN` | An unopened entry remains scratchable even when a BASIC listing cursor happens to point at it. A real retained reader still protects its bound file. Preserve listing ownership and peer-drive isolation. | `scratch_active`; `test_scratch_listing_cursors_do_not_protect_unopened_files` | Fresh and advanced listing cursors, a concurrent real file reader and a different-drive control verify count, persisted deletion, retained payload and cleanup. Other entry predicates already require writable or reclamation owners. |

The related namespace fixes deliberately reject unsafe unpublished intent
rather than introducing an alias journal or mutating a retained writer's
publication name. Successfully published, still-open writers remain eligible
for RENAME. The delegated success flag similarly avoids using a mutable
unit-wide status as a scheduler result. These are host ownership choices,
not claims about the ROM's buffer scheduling.

## Extraction validation

The reviewed candidate passes 1368 direct DOS cases and 2351 total firmware
Check cases (2304 ordinary plus 47 fatal/assert cases), with
tests and dependencies built in Debug. The RP2040 production build passes
in RelWithDebInfo. Formatting and whitespace checks pass.

The feedback adds 117 model/scenario variants and corrects existing
invalid-track expectations. The same final fixtures linked against
the original PR-head production implementation reproduce the defects.
That baseline has 85 failures and zero errors, versus zero failures/errors
with the fixes. This is a paired host regression check, not an original-ROM
replay. The third review cycle's final fixtures also reproduce 33 failures
against the second fix commit, demonstrating the later fixes separately.

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
