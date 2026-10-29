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
without writing its read cursor. The caller must quiesce transport and finish
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
