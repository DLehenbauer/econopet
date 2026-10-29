# DOS read/status foundation

This is the fourth slice of the native-drive replacement series. It builds on
the [media/lifecycle foundation](drive-media.md), introduced in #112. It adds
directly testable read/status code without selecting it in `ieee_drive.c`.
The existing production engine and flat HDD support remain active. Writers,
commands, direct channels, REL and the eventual facade cutover belong to later
slices.

## Included behavior

- Bounded per-unit state with eight logical indices (six ordinary), twelve
  shared sector pages and two separately bounded BAM caches. Compile-time
  checks enforce the 4 KiB native-state budget plus those two BAM sectors.
- OPEN parsing with the revision-2/DOS-2.7 58-byte limit, drive prefixes,
  PETSCII names, wildcard matching, type/mode fields and peer-drive lookup.
  Parsing an `L` parameter does not enable REL operations.
- Ordinary readers with two owned pages, initial prefetch, sector links,
  terminal-byte EOI, the native initial-singleton EOF sequence and retained
  last-program LOAD selection.
- BASIC directory headers, entries and free-block footers, bounded patterns,
  type filters, dual-drive continuation and raw directory/header-chain reads.
- Retained model-specific status. Preview does not consume output. GET commits
  only actual progress, and status clears only after its CR is consumed.
- Resource release and reuse through CLOSE, with cooperative lookup yielding
  between directory sectors.

Writes, append, explicit REL OPEN and direct-channel OPEN are rejected in this
slice rather than being silently accepted. This temporary extraction boundary
is not a native-ROM error-policy claim. The formatter includes status spellings
that later slices can use, but their operations are not enabled.

## Native targets and evidence

The selected targets are 4040 DOS 2.0 revision 2 (901468-14/-15/-16), 8050 MPI
single-sided mode and 8250 MPI double-sided mode using shared DOS 2.7
(901887-01/901888-01). Earlier DOS 2.5 and 8250LP are not interchangeable targets.
The [reconstruction tools](../../tools/drive-roms/README.md) identify the selected
source overlays and byte-verified ROM regions. No ROM binaries are included here.

Primary source is `mist64/cbmsrc` at
[`01bd60f162ef92212ef0cb67546ae8f42be34168`](https://github.com/mist64/cbmsrc/tree/01bd60f162ef92212ef0cb67546ae8f42be34168).
Each implementation function names its relevant source units and routines:

| Contract | 4040 | Shared DOS 2.7 |
|---|---|---|
| OPEN and parameters | [open](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/open), [parsex](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/parsex) | [open](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/open), [parsex](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/parsex) |
| Channels, pages and bytes | [tst2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2), [tst3](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst3), [opchnl](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/opchnl) | [tst2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2), [tst3](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst3), [opchnl](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/opchnl) |
| Directory and free counts | [lstdir](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/lstdir), [trnsfr](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/trnsfr) | [lstdir](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/lstdir), [trnsfr](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/trnsfr), [map](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/map) |
| Status and CR/EOI | [erproc](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/erproc), [tst4](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4) | [erproc](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/erproc), [tst4](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst4) |

`OPNRCH/OR30` unconditionally restores `RDYTLK` after its first `RDBYT`.
For a first sector linked as `0/2`, that loses the initially prepared EOI.
The reader deliberately retains the resulting payload, zero, two, payload
sequence rather than treating it as a conventional one-byte file.

## Host adaptations and limits

The media layer owns generation checks and callback I/O. DOS owns no host file
handles and allocates no image-sized state. Callback failure mapping is bounded,
not the complete controller's retry/error translation. In particular, absent
4040 media uses a host-policy 74 response, not an entry in its native ERRTAB.

Cooperative lookup, immutable FIFO preview and consumed-prefix GET are host
scheduling/transport boundaries. The 64-byte preview bound keeps the unconsumed
suffix resident in the two owned data pages. Directory preview retains one row
until GET retires it. Corrupt-chain traversal bounds are host safeguards, not
native mechanical timeouts.

Callers finish pending work before starting another OPEN or coordinating
CLOSE/reset. Media rebinds must coordinate DOS/channel teardown before an
eventual production facade publishes new mounts. Cached BAM/free-count records
are identity-bound, and stale deferred lookup cannot access replacement media.
`dos_reset()` clears DOS resources but does not detach media. It is not a full
native reset/user-command implementation.

The direct Check tests cover parsing, exact status, cancellation/partial GET,
sector seams, native singleton EOF, finite resources, stale lookup, directory
bytes/patterns/raw chains, prefetch failure and read-only operation. Their
passing results do not establish electrical handshake timing or complete ROM
compatibility. Hardware/reference acceptance remains a later integration gate.
