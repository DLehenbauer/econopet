# Native drive media foundation

`fw/src/ieee/dos_media.*` provides bounded backing storage for a future native
DOS engine. It supports four dual-drive units, exact-size raw D64/D80/D82
descriptors, one shared model per unit, generation-bound references, and
single-sector I/O. It borrows callbacks and contexts without opening or closing
files, allocating image-sized buffers, or interpreting filesystem contents.

The existing production DOS remains active. Its D64/D80 and flat `.hdd` support,
directory helpers, and stream/chain APIs remain available. Although the shared
image API now recognizes D82, the legacy drive facade explicitly rejects it.
The new media API is built and tested independently, not connected to production
mounts, DOS commands, channels, resets, or the FPGA unit-mask protocol.

## Storage and lifetime contract

Zero-initialize a bank once and preserve its generation across unmounts and DOS
resets. A successful nonempty rebind consumes a bank-wide identity and invalidates
both slots' old references in that unit, including an unchanged partner. Other
units remain usable. Empty slots cannot alias their populated partners.
Generation exhaustion rejects before I/O rather than wrapping.

Validate a complete pair before touching old storage. Nonempty partners must
have the same raw format, a reader, and (if writable) a flush callback. A dirty
old pair must flush successfully before publishing its replacement. A write
marks its slot dirty before invoking the callback because even a failed callback
can have changed backing bytes. Failed flushes retain dirty state and bindings.

Bulk unmount flushes every unit before detaching any. On failure, all bindings
remain, although earlier successful flushes remain progress. Callback contexts
remain the caller's responsibility. The caller must serialize access, quiesce
transfers, and finalize pending DOS work before rebinding. These operations are
not lock-free publication, rollback, native CLOSE acknowledgement, or a claim of
power-loss durability.

## Compatibility evidence

The target modes are 4040 revision 2, 8050 MPI single-sided, and 8250 MPI
double-sided, identified in the [ROM manifest][manifest].
Geometry and BAM locations follow the selected Commodore sources at
[`01bd60f162ef92212ef0cb67546ae8f42be34168`][cbmsrc].
MAME format sources at
[`f379cd060ac3b2bd567ebbd0957aed059d6895ff`][mame]
corroborate the raw image representation. Host lifecycle policy is deliberately
distinguished from ROM behavior.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|---|---|---|---|---|---|
| D82 geometry | 8250 MPI double-sided, DOS 2.7 | [`CONFI1`/`CONFI2`][config] and [`NTRK80`/`NSEC80`][zones] | 154 tracks, two consecutive 77-track sides, 4,166 sectors, 1,066,496 raw bytes. Track 78 starts at byte 533,248. Reject invalid coordinates without changing outputs. | `diskimage_track_sectors/sector_offset/open`, `test_d82_geometry_covers_two_exact_native_sides`, `test_disk_geometry_rejects_invalid_inputs` | [MAME D82 format][d82] and [D80 sector zones][d80] |
| D82 directory and second-side reads | 8250 MPI double-sided, DOS 2.7 | [`CONFIG` directory track][config] and [BAM/directory tables][bam] | Start the directory at 39/1, not 18/1. A chain may read through the final sector at 154/22. | `dir_start`, `diskstream_next`, `test_d82_directory_and_second_side_stream` | [MAME D80 disk-ID/header placement][d80] corroborates track 39, not the directory chain itself |
| Raw 4040 BAM access | 4040 revision 2 | [`INITSU`/`INITDR`][init4040] | Part zero reads one 256-byte sector at 18/0. Reject higher parts before I/O. | `dos_media_read_bam`, `test_model_bam_access_is_bounded_and_media_bound` | [MAME D64 disk-ID placement][d64] corroborates 18/0. The host test independently checks byte offset 357 * 256, callback counts, and stale references. |
| Raw DOS 2.7 BAM access | 8050 MPI single-sided and 8250 MPI double-sided | [`CONFI1`/`CONFI2`][config] copying [`BMT80S`/`BMS80S` and `BMT80D`/`BMS80D`][bam] | 8050 parts are 38/0 and 38/3. 8250 adds 38/6 and 38/9. The final 39/1 table entry is not a BAM part. Read exactly one sector per valid part. | `dos_media_read_bam`, `test_model_bam_access_is_bounded_and_media_bound`, `test_bam_rejects_unconfigured_and_invalid_units` | Independent offsets from 37 * 29 * 256 and callback counts, not full native initialization |
| Model selection and media identity | EconoPET host policy, not ROM media autodetection | Storage/lifetime contract above | Exact raw sizes, homogeneous nonempty partners, either single-slot placement. Rebinding invalidates both old references in only the changed unit. Reject identity wrap before I/O. | `dos_media_validate_pair/set_unit/set_slot/reference`, `test_open_recognizes_exact_d82_size`, `test_complete_pair_validation_is_side_effect_free`, `test_remount_invalidates_unit_without_cross_unit_reset`, `test_identity_exhaustion_preserves_old_mount` | Callback counts and unchanged peer-unit reads, no native-parity claim |
| Write/flush ownership | EconoPET host policy, not native error mapping | Storage/lifetime contract above | Mark dirty before writes, including partial failures. Failed rebind or bulk teardown retains all old bindings and unflushed state. | `dos_media_write/flush_unit/unmount_all`, `test_partial_write_and_flush_failure_retain_old_media`, `test_bulk_teardown_retains_bindings_then_invalidates_jobs` | Independently fault-injected callbacks, no power-loss atomicity claim |
| Production activation boundary | EconoPET incremental integration policy | Existing legacy facade and unchanged mount contract | Shared D82 detection does not enable incomplete DOS support. Reject a D82 replacement while keeping the old mounted D64 usable. | `mount_image`, `test_d82_mount_rejection_preserves_legacy_source` | Existing D64/D80 and flat-container regression suites |

BAM access reads fixed-location sectors only. It does not initialize media,
extract disk IDs, cache or repair a BAM, allocate blocks, or emulate controller
retry/timing behavior. These host tests do not establish native DOS command,
electrical handshake, latency, throughput, or physical-board acceptance.

## Validation

Use the root presets so production is `RelWithDebInfo` and the firmware test
runner, its production sources, and dependencies are `Debug`:

```sh
cmake --preset default
cmake --build --preset fw-test
ctest --preset fw --parallel
cmake --build --preset fw
```

The firmware runner includes the new `dos_media` suite and retains existing
`diskimage` and `ieee_drive` tests, including flat-container coverage.

[cbmsrc]: https://github.com/mist64/cbmsrc/tree/01bd60f162ef92212ef0cb67546ae8f42be34168
[manifest]: ../../tools/drive-roms/drive-emulation-roms.json
[mame]: https://github.com/mamedev/mame/tree/f379cd060ac3b2bd567ebbd0957aed059d6895ff
[config]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/begin#L84-L164
[zones]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/romtbl#L137-L141
[bam]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/romtbl#L144-L151
[init4040]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2#L218-L247
[d82]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/lib/formats/d82_dsk.cpp#L32-L36
[d80]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/lib/formats/d80_dsk.cpp#L55-L100
[d64]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/lib/formats/d64_dsk.cpp#L121-L132
