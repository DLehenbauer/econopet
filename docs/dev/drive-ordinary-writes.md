# Ordinary-file write foundation

This is the fifth extraction slice, dependent on the
[read/status foundation](drive-read-status.md) in #113. It adds directly
testable ordinary-file writers, not the production facade cutover.
`ieee_drive.c` still selects the legacy engine and preserves flat HDD support.
Commands, direct channels and REL execution remain outside this slice.

## Included behavior

- Ordinary SEQ/PRG/USR creation, APPEND and `@` replacement, including peer-drive
  lookup and LOAD/SAVE secondary overrides.
- Two-page streaming writes with native allocation order and interleave,
  shared per-drive dirty BAM caches, bitmap/count checks and cached-version
  write gating. Zero or the selected model's version is accepted. DOS 2.7
  uses the currently cached BAM part, not the directory header's version.
- Unclosed directory publication, deleted-slot reservation, directory growth
  and read-modify-write updates that preserve other open files' entries.
- CLOSE termination, replacement-chain deletion, closed directory metadata,
  BAM persistence and host backing-store flush. Empty files contain CR.
  At an exact 254-byte payload boundary, DOS 2.7 frees the speculative extra
  block while the selected 4040 retains that allocation.
- Retained write phases after host I/O failure, including reserved successor
  sectors and directory-growth links. DISK FULL releases an ordinary owner,
  while host I/O failure retains it. CLOSE 15 visits secondaries downward and
  stops on failure without discarding lower owners.

`dos_put(unit, media, secondary, byte)` accepts already-decoded ordinary data.
It does not collect OPEN names, dispatch commands or interpret transport EOI.
EOI is not CLOSE. Callers complete lookup/APPEND before delivering data.
An error can occur after the byte filling a page was stored. The failed owner
then rejects further PUT bytes, and CLOSE/finalization retries its persistence
phase instead of blindly resending that byte.

`dos_close()` now takes the media bank and returns success only after the
writer's persistence boundaries complete. Readers still release without
writing. `dos_finalize()` supports coordinated media lifecycle operations:
successful ordinary descriptors remain allocated, the first failure retains
its phase and unfinalized peers, and a pending APPEND traversal is cancelled
without writing its read cursor. Finalization also persists unit-owned BAM
and backing-store changes after DISK FULL has released the last writer.
Failed BAM writes and backing-store flushes remain retryable without a channel.
The caller must quiesce transport and finish
pending lookup first. Reset/rebind belongs after successful finalization.
These APIs are not wired into the production facade in this slice.

## Native evidence

Targets remain 4040 DOS 2.0 revision 2 (901468-14/-15/-16), 8050 MPI single-sided
and 8250 MPI double-sided using shared DOS 2.7 (901887-01/901888-01). Consult the
[reconstruction tools](../../tools/drive-roms/README.md) for composition,
approved overlays and byte-verified regions. Earlier DOS 2.5 and 8250LP are not
substitutes. No ROM binaries or private media are included.

Primary source is `mist64/cbmsrc` at
[`01bd60f162ef92212ef0cb67546ae8f42be34168`](https://github.com/mist64/cbmsrc/tree/01bd60f162ef92212ef0cb67546ae8f42be34168).
The implementation retains pinned source blocks beside each function.

| Contract and routines | 4040 | Shared DOS 2.7 |
|---|---|---|
| OPEN/replacement and APPEND (`OP77`, `OP82`, `OP120`, `APPEND`) | [open](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/open) | [open](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/open) |
| Allocation (`INTTS/JINTTS`, `NXTTS/JNXTTS`, `FNDNXT`, `AVCK`) | [tstfnd](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tstfnd) | [tstfnd](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tstfnd) |
| Streaming (`PUT`, `WRTBYT`, `WRT0`) | [tst2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2) | [tst2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2) |
| Directory publication and growth (`ADDFIL`, `NXDRBK`) | [addfil](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/addfil), [tst4](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4) | [addfil](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/addfil), [tst4](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst4) |
| Termination and allocation persistence (`CLSWRT`, `CLSDIR`, `MAPOUT`, `SCRBAM`) | [close](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/close) | [close](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/close), [map](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/map), [frets](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/frets) |
| Cached-version gate (`SETJOB`, `SJB3`, `VNERR`) | [jobs](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/jobs) | [jobs](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/jobs) |

### Write-feedback evidence

The targets below are the exact revisions and hardware modes selected above.
All test names refer to `fw/test/dos_test.c`, and implementation names refer
to `fw/src/ieee/dos.c`. "Host" explicitly identifies a lifecycle or identity
adaptation, not behavior claimed for native controller hardware.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|---|---|---|---|---|---|
| Unit persistence after writer release | Host, all three targets | Native BAM persistence: [4040 close: `MAPOUT`][40-close], [DOS 2.7 close: `CLSWRT`][27-close]. Channel-independent finalization and backing-store flush are host policy. | Finalization persists dirty BAM and backing-store state even without a writer. Failed writes/flushes retain retryable state. | `dos_finalize`, `test_finalize_released_writer_persists_unit_state` | Injected BAM/flush faults and independently inspected backing BAM. No native flush equivalence claimed. |
| Terminal command retirement | All three targets, with retained host I/O owners | [4040 parsex: `CLRCB`][40-parse], [DOS 2.7 parsex: `CLRCB`][27-parse], [4040 erproc: `CMDER3`][40-errors], [DOS 2.7 erproc: `CMDER3`][27-errors] | Clear the first 58 command bytes, preserve the extra receive byte, failure status and last successful drive. Host failures must not discard retryable writers. Yielding APPEND retains its command. | `clear_command`, OPEN/APPEND failure exits, `test_failed_write_open_retires_command_bytes` | Direct failed-OPEN then `NEW,` reproduction. General command/status interface: [pagetable][dos-interface]. No physical failure capture. |
| Initial allocation exhaustion | 4040 revision 2, 8050 MPI, 8250 MPI | [4040 tstfnd: `INTTS`][40-allocation], [DOS 2.7 tstfnd: `JINTTS/ITS2`][27-allocation], [begin: `CONFIG`][27-config] | Initial full-image allocation gives 4040 status 72 at 0,0. DOS 2.7 gives 67 at MAXTRK (78 or 155), retaining the directory lookup sector. Subsequent `NXTTS` exhaustion remains 72 at 0,0. | `allocate_sector`, `open_write`, `test_initial_full_allocation_preserves_native_status`, existing full-write cases | Exact status strings tested at directory sector 1. [MAME target inventory][mame-targets] corroborates ROM/model selection, not the status quirk. |
| PUT drive selection | All three targets | [4040 tst2: `WRT0/SETDRN`][40-stream], [DOS 2.7 tst2: `WRT0/SETDRN`][27-stream] | Allocation warnings/errors select the writer's drive even after peer OPEN, without changing the last successful command's drive. | `dos_put`, `test_put_reselects_writer_drive` | Interleaved drive-1 OPEN and drive-0 allocation warning/full tests. No physical trace. |
| Yielded APPEND drive selection | All three targets, cooperative scheduling is host-only | [4040 open: `APPEND`][40-open], [DOS 2.7 open: `APPEND`][27-open], both `tst2` `SETDRN` paths above | Resumed APPEND restores its own drive before completion, even if a peer reader reported an error while it yielded. | `dos_work`, `test_append_resume_reselects_drive` | Peer read-failure interleaving reproduces the stale selection. Native reference is the synchronous ROM path, not host scheduling. |
| Directory ownership | Host generation references, all three targets | Native selected-drive allocation: both `tstfnd` sources above. Unit/slot/generation references are host policy. | Shared pair generations must not alias deleted-entry reservations or pending directory growth across slots. | Both `dos_work` ownership guards, `test_directory_writer_ownership_includes_slot` | Equal-generation dual-slot fixtures independently check peer directory links and entry coordinates. |
| Wildcards beyond stored name | All three targets | Both `parsex: PARSE` sources above, [4040 open: `OP75/OP815/OP115`][40-open], [DOS 2.7 open: `OP75/OP815/OP115`][27-open] | Retain filename wildcard presence past byte 16. Ordinary wildcard WRITE rejects with 33, unmatched wildcard replacement with 64, without writes. | `dos_open_parse`, `dos_work`, `test_write_rejects_truncated_wildcard_suffix` | `*` and `?`, with and without `@`, tested on all three targets. [pagetable][dos-interface] corroborates general syntax, not the truncation edge. |
| OPEN after peer-writer retirement | All three targets, retained host CLOSE phases | Both `open` and `tst2` sources above | Successful old-writer retirement must not redirect the replacement OPEN's status or last successful drive. A failed retirement retains its own error and owner. | `open_directory`, `dos_open`, `dos_work`, `test_reused_writer_secondary_restores_requested_drive`, terminal-failure cases | Same-secondary ordinary WRITE, raw directory and ordinary READ replacements tested across drives. |
| APPEND block count | All three targets | Both `tst2: STRDBL/STRTIT` sources above | Count the initial read, subsequent traversed read jobs and final write. Appending one byte to lengths 22, 254 and 300 publishes counts 2, 3 and 3. | Initial APPEND `blocks`, `test_append_directory_count_includes_initial_read` | Independent directory-byte inspection plus complete payload readback. No physical reference capture. |
| Cancelled APPEND retirement (related finding) | Host finalization, all three targets | No native cancellation equivalent. Logical buffer clearing follows both `parsex: CLRCB` sources above. | Cancelling unfinished APPEND releases only its read cursor and retires command bytes, with no clean-media write/flush. | `dos_finalize`, `test_finalize_cancels_unfinished_append_without_writes` | Existing cancellation cases strengthened with direct command-RAM checks. |

[40-close]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/close
[27-close]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/close
[40-parse]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/parsex
[27-parse]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/parsex
[40-errors]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/erproc
[27-errors]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/erproc
[40-allocation]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tstfnd
[27-allocation]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tstfnd
[27-config]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/begin
[40-stream]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
[27-stream]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
[40-open]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/open
[27-open]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/open
[dos-interface]: https://www.pagetable.com/174
[mame-targets]: https://github.com/mamedev/mame/blob/master/src/devices/bus/ieee488/c8050.cpp

## Adaptations and validation boundary

Synchronous sector callbacks, retained retry phases, backing-store flush,
generation guards, bounded corrupt-chain traversal and cooperative APPEND
scheduling are host adaptations, not reproduced controller jobs or mechanics.
The cache identity and failed-refill guards from #113 remain intact.
`dos_reset()` still clears state without implementing native reset/user commands.

Direct Check tests exercise payload boundaries, create/append/replace,
peer-directory isolation, directory growth, read-only media, near-full/full
allocation, malformed BAM counts, write-version checks, dual drives, storage
fault recovery, finalization and CLOSE-all ordering. The existing parser,
read/status, directory and cache-rebind tests remain enabled.

Passing local tests and compilation do not establish full ROM equivalence or
IEEE electrical/timing compatibility. Fresh board/reference acceptance remains
a later integration gate.
