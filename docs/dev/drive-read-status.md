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
  The 59-byte receive window is checked separately from the logical limit,
  which follows CMDSET's CR trimming. Omitted drives start from the last
  successful command's drive. Equals ends the filename without hiding later
  comma-separated parameters, and `D` resets an earlier type for inference.
  Explicit comparison lengths survive literal `$A0` in both OPEN and directory
  patterns, without changing padding or wildcard rules.
  Parsing `L` does not enable REL operations.
- Ordinary readers with two owned pages, initial prefetch, sector links,
  terminal-byte EOI, the native initial-singleton EOF sequence and retained
  last-program LOAD selection. Initial `LOAD "*"` starts at drive zero and
  releases channels whose active page's last job selected drive zero, before
  parsing or lookup. It filters for PRG entries before checking OPEN parameters.
  A retained-program reload uses its saved drive/link without this teardown or
  parsing the ignored name suffix.
- BASIC directory headers, entries and free-block footers, bounded patterns,
  type filters (`R` for REL, not OPEN's `L`), dual-drive continuation and raw
  directory/header-chain reads.
- Retained model-specific status. Preview does not consume output. GET commits
  only actual progress, and status clears only after its CR is consumed.
- Resource release and reuse through CLOSE, with cooperative lookup yielding
  between directory sectors. Failed OPEN also releases the failing secondary's
  non-REL ordinary reader on DOS 2.7. The inactive-bus 4040 path retains it.

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
until GET retires it. Failed data-page jobs retain their target and result, so
repeated or partially consumed previews cannot silently retry a one-shot failure.
The error becomes visible after the valid prefix is consumed and identifies the
channel's drive, even when other readers or pending OPEN work are interleaved.
Corrupt-chain traversal bounds are host safeguards, not native mechanical timeouts.
Directory bounds count sector visits independently of cache misses and emitted
rows, so even a resident self-link eventually reports READ ERROR with the
listing's drive and current coordinates. Zero-capacity previews return no bytes
and no EOI without preparing a row, issuing I/O or changing DOS state.

The read-path regression evidence distinguishes native rules from those host
boundaries. All native rows below apply to the selected 4040 revision 2 and
shared DOS 2.7 hardware modes:

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|---|---|---|---|---|---|
| Filename delimiter | Selected native targets | Both `lookup:COMPAR` call `trnsfr:FNDLMT`, which stops at comma or equals. `parsex:PARSE` independently collects parameters. | `0:FILE=IGNORED,S,R` reads FILE. A later U still produces 64 for SEQ. | `dos_open_parse`, `test_open_name_ends_at_equals_before_parameters` | [Commodore DOS](https://www.pagetable.com/174), File Access API, excludes equals from filenames. |
| Type reset | Selected native targets | Both `open:CKTM` search `romtbl:TPLST` (`DSPUL`), including index zero. | `FILE,P,D` infers the matched type, while `FILE,D,P` still requires PRG. | `dos_open_parse`, `test_open_d_resets_type_in_parameter_order` | Same article, Channels 0 and 1, distinguishes their PRG default from ordinary channels. It does not document the D reset. |
| Directory REL filter | Selected native targets | Both `setdrv:FS1SET` search `romtbl:TYPLST` (`DSPUR`). | `=R` lists only REL. Unrecognized `=L` leaves the listing unrestricted. | `open_directory`, `test_directory_relative_filter_uses_r_not_l` | Same article, Directory Listing, explicitly distinguishes R from OPEN's L. |
| Initial and retained LOAD | Selected native targets | Both `open:OP0415` reset DRVNUM/LSTDRV and set PRG TYPFLG. `lookup:FF30` filters matches. `open:OP02` precedes parsing for retained loads. | Initial `*` starts at zero and skips SEQ entries. A retained reload uses the saved link even with an otherwise invalid suffix. | `dos_open`/`dos_work`, `test_initial_load_star_resets_drive_and_filters_programs`, `test_failed_initial_load_star_resets_drive_before_parsing`, `test_retained_load_star_bypasses_ignored_parameters` | Same article, Channels 0 and 1, corroborates LOAD/read and PRG defaults, not the shortcut details. |
| Failed OPEN owner | DOS 2.7 versus 4040 revision 2, outside active bus roles | DOS 2.7 `erproc:CMDER3` restores ORGSA, finds its owner even without LSNACT/TLKACT, then `TLERR` calls `tst3:FRECHN` for types below REL. 4040 skips that idle-role teardown. | Missing name, type mismatch, overlength and lookup-I/O errors retire only the failing DOS 2.7 secondary. The peer, last successful drive and 4040 owner survive. Listing reservations are released even with a REL filter. Types at/above REL remain exempt. | `command_error`, `test_failed_reopen_releases_only_dos27_ordinary_owner`, `test_failed_reopen_releases_dos27_listing_reservations` | Both ROMs' `tst3:FRECHN/RELBUF` independently establish the channel/page release mechanism. The generic article does not specify this model distinction. |
| Explicit pattern extent | Selected native targets | Both `lookup:COMPAR` compare bytes through `trnsfr:FNDLMT`'s LIMIT. Only `CP34`, after exhausting the supplied bytes, tests directory padding. | `A,$A0,X` does not match `A,$A0,Y`. Ordinary OPEN and every directory pattern preserve explicit lengths, suffix wildcards, question-mark padding rejection and the 16-byte cap. | `name_matches`/`dos_open_parse`/`open_directory`, `test_open_preserves_explicit_pattern_length`, `test_directory_preserves_explicit_pattern_lengths` | The separate legacy `diskimage.c` matcher already consumes every supplied byte. The article labels `$A0` illegal for creating filenames, which does not define raw lookup on an existing directory. |
| Initial LOAD owner teardown | Selected native targets | Both `open:OP0415` call `tst2:INITDR`, beginning with `tst3:CLDCHN`. `getact:GETACT` selects the page whose LSTJOB drive bit determines ownership. | Before initial `*` parsing, release only active-page drive-zero owners (including on syntax/not-found failure). Media slot and inactive-page jobs must not substitute for that bit. Retained-program reload does not perform this teardown. | `dos_open`, `test_initial_load_star_releases_active_drive_owners` | Both ROMs' `getact:GETACT` independently corroborate the BUF0/BUF1 selector. This row covers channel ownership, not the remaining controller/media initialization. |
| Corrupt directory bound | Host adaptation, not native behavior | Both `lookup:SEARCH` and `lstdir:GETDIR` traverse entries. Neither defines this host image-size bound. | Cached self-links, unmatched rows and multi-sector cycles terminate with retained status 23 and the listing's drive/coordinates. | `prepare_directory_line`, `test_directory_cycles_publish_bounded_read_error` | The documented host safeguard above, not an original-drive timeout claim. |
| Empty preview | Host transport boundary, not a native API | Both `tst4:GET` advance after prepared data is transferred. There is no ROM capacity-zero preview operation. | Ordinary, raw and BASIC-directory previews with capacity zero change no state or I/O and return zero with EOI false. | `dos_stage`, `test_zero_capacity_preview_preserves_reader_and_directory_state` | Same article, Sequential Files, corroborates EOI on actual file completion, not a zero-size observation. |

Routine names refer to the pinned source units linked above. Delimiter/type
tables are in
[4040 trnsfr](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/trnsfr),
[DOS 2.7 trnsfr](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/trnsfr),
[4040 setdrv](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/setdrv),
[DOS 2.7 setdrv](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/setdrv),
[4040 romtbl](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/romtbl)
and [DOS 2.7 romtbl](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/romtbl).
The corroborating article's generic default-drive/type descriptions do not
define dual-drive LSTDRV behavior or ordinary-channel type inference. Those
follow the selected ROM instructions above.

Successful OPEN completion and command-level errors clear only the first 58
command bytes, following SCREND/CMDER3 and CLRCB. The extra receive byte survives,
and pending parsing retains its input until completion. This prevents a shorter
OPEN with an empty parameter from reusing a preceding command's type or mode.

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
