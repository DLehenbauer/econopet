# Host test authoring reference

Start with the [author guide](README.md) for build commands and complete examples.
The [IEEE guide](ieee/README.md) separates wire/FIFO evidence and summarizes ROM
entry coverage. This reference records detailed API contracts and evidence limits.

## Host firmware through production SPI

[`FirmwareSession`](firmware.h) binds one board to the real
host-compiled drive logic in [`ieee_drive.c`](../../fw/src/ieee/ieee_drive.c).
It does not mock the drive or bypass FPGA register decoding. The
[`SPI adapter`](ieee/ieee_drive_spi_adapter.cpp) replaces RP2040 transport
calls with physical mode-0 SPI on the production-top simulator. It mirrors
[`driver.c`](../../fw/src/driver.c)'s command encodings, pipeline return values,
CS boundaries, stall handling, and fill chunking. It does not execute
`driver.c` or model the RP2040 SSP peripheral. This layer proves transport and
session contracts, not complete Commodore DOS compatibility or ROM-specific
initialization.

```cpp
#include "firmware.h"
#include "ieee/controller.h"

System board;
FirmwareSession firmware(board);  // Destroy before the borrowed board.
IeeeController controller(board);
firmware.mount(disk_fixture::D64::empty());
auto rx = firmware.trace_rx();
controller.command(ieee_test::Listen(ieee_test::Device{8}));

// Service the real C firmware under the configured elapsed-cycle deadline.
board.execution().within(10000)
    .every(1, RunConfiguration::MissedService::Coalesce, firmware.service_adapter())
    .until([&](const System&) { return rx.records().size() == 1; })
    .run("firmware RX");
rx.capture().require_count(1, "firmware consumed LISTEN");
firmware.close();
```

Use `RunConfiguration::MissedService::Coalesce` when a service pass may
overrun its nominal period (or include [`scheduler.h`](scheduler.h) for
`Scheduler`). `service_adapter()` checks board identity and retains only a
weak session-lifetime token. Invoking it after close or destruction throws
before clocking, even if another session now owns that same board.
Direct `service()` executes one production cooperative pass. Both forms use
the board's active deadline, including every SPI clock, and reject unusable
boards even when no disk is mounted.

The session cannot be copied or moved. Construct it after the board and keep
the board alive until the session is destroyed. There is one process-global
drive implementation, so concurrent, threaded, and nested sessions are not
supported. Explicit `close()` is idempotent and propagates teardown failure.
After cleanup has begun, it releases its binding and freezes all RX recorders
even if SPI fails. It closes production drive handles before unregistering
only its own writable backing stores. Unrelated registered fixtures survive.
Observation, input-edit, selected-SPI, and scheduled-stimulus contexts reject
mutating session operations before touching C drive state. A rejected close
precondition retains ownership for a later valid explicit close. Destruction
cannot retain ownership for a retry: if cleanup was rejected and the session
is still active after reporting, it prints a diagnostic and terminates.
This prevents dangling process-global bindings without forcing unsafe drive
cleanup inside observation, input-edit, stimulus, or selected-SPI callbacks.

`mount()` copies bytes from a vector or checked D64 into drive slot zero using
the production `/disks/` path helper and mount implementation. Names must be
nonempty, fit `SD_PATH_MAX`, and not already be registered. The shared registry
is checked before the session records ownership, so collisions with another
fixture report a recoverable mount error without replacing or deleting that
fixture. Within a live
session each name is unique, even when a different image replaces slot zero.
An unsupported image reports a mount error and releases its newly registered
file so that name can be retried. If a mount is interrupted after the drive
opens its file, the session retains cleanup ownership until close.
[`FirmwareTest`](framework/firmware_test.h) adds an independent session to a
fresh board and reports teardown errors through GoogleTest.
`FirmwareTestSession` provides the same destructor reporter for local owners.
Both fixture teardown and local session destruction report standard and
non-standard C++ exceptions. Standard exceptions retain their `what()` message,
while other types report `non-standard exception`. A custom
`FirmwareSession::TeardownErrorHandler` receives the original `std::exception_ptr`
and must return without throwing. Explicit `close()` still rethrows the original
exception after releasing ownership. Without a reporter, any destructor cleanup
error prints a diagnostic and terminates the process instead of being silently
discarded.

`trace_rx()` supports multiple simultaneous scoped recorders and retained
captures after close. It records only adjacent successful absolute
production CTRL/STATUS/RX reads with `RxReady`, with no intervening board cycles.
Other physical SPI operations or clock advancement discard pending metadata
evidence. Generic pipelined and bulk reads do not create
semantic RX records. Reads are side-effect-free, so repeated complete
CTRL/STATUS/RX triples can record the same head until firmware explicitly pops it.
These records contain the captured byte, ATN, and EOI. Use
[`WireTrace`](ieee/trace.h) for independent physical EOI evidence. Compare
FIFO and wire records directly, without discarding EOI.

[`firmware_test.cpp`](firmware_test.cpp) independently checks literal MOSI bytes
and CS boundaries, read/write pipelines, bank crossing, buffer-sized fills,
empty operations, full-range rejection, elapsed deadlines, real FIFO
consumption, scoped lifetimes, callback guards, and exceptional cleanup.
Firmware C frames are compiled with exception-unwind support so C++ SPI
failures propagate through the real drive implementation.

### First end-to-end acceptance tests

[`econopet_top_smoke.cpp`](econopet_top_smoke.cpp) round-trips CPU selection,
executes a checked soft-6502 RAM program, captures its result and mounts a D64
through production firmware. [`ieee/drive_test.cpp`](ieee/drive_test.cpp) extends
that path to physical IEEE handshakes, OPEN, load-channel streaming and channel
15 status. Both sources belong to the existing isolated firmware executable.
They need no external ROM media and do not execute a Commodore drive ROM.

PRG cases use 2, 4, 254 and 255 stored bytes (no implicit load address).
They cover the final-sector boundary and the first byte of a second sector.
The CPU receiver and passive wire trace independently match every byte, ATN and
EOI position, and the data FIFO must be empty after the last valid byte.
Status expectations include the leading space in `"00, OK,00,00\r"` and EOI
on its final CR. Simultaneous firmware RX traces check the consumed OPEN
transaction, including the retained EOI metadata. Both paths share an OPEN sender
whose passive wire trace requires EOI on the final filename byte before
UNLISTEN. Native filename collection needs that EOI to queue parsing.
Configured polling `.service(firmware.service_adapter())` drains RX without
introducing a periodic cadence and bounds all SPI clocks with the shared
elapsed-cycle deadline.

Separate one-byte SEQ cases preserve the native initial-EOF bug. For a sole
stored byte `X`, the CPU and passive wire trace must observe `X, 0, 2, X`,
with EOI only on the fourth byte and no queued tail. The receiver reads until
EOI with a four-byte cap and the existing per-byte deadline. Payloads include
zero, CR, `A` and `$FF`. `OR30` overwrites the prefetched EOF with `RDYTLK`,
so the wrapped buffer pointer exposes the sector-link bytes before the
repeated payload reaches EOF normally. The two-byte and 255-byte PRG cases
check the threshold and a final sector containing only one byte.
An operator's BASIC capture corroborated `65, 0, 2, 65` with EOF only on the
last byte, and normal EOF for a two-byte file. Its drive/ROM configuration
was not recorded, so it is not independent identification of a native 4040.

The VIA smoke case checks CPU-written output latches and released IEEE inputs.
It masks unrelated pins, including the running vertical-retrace input on PB5.
Board-only enable readback and ATN/FIFO consumption already have coverage in
[`firmware_test.cpp`](firmware_test.cpp) and
[`ieee/controller_test.cpp`](ieee/controller_test.cpp).

The selected peripheral is the 4040/D64 source model at
[`mist64/cbmsrc` revision `01bd60f`](https://github.com/mist64/cbmsrc/tree/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040).
These narrowly scoped source-derived expectations do not establish complete
DOS compatibility, D80/8050 file streaming, or native PET ROM I/O. The separate
historical-regression checks below cover model-specific status and preserve
the outstanding repeated-EOF defect as an opt-in red test.
Firmware unit tests separately preserve the same
one-byte bug and normal two-byte threshold for D64/4040 and D80/single-sided
8050 DOS 2.7, across SEQ, PRG and USR files. Raw image readers and REL records
(including `.hdd`) retain their existing stored-byte/record semantics.
The existing controller evidence below supplies the PET-side handshake
contract.

| Behavior | Target ROM/model and hardware | Primary evidence | Required result | Code/test | Corroboration |
|---|---|---|---|---|---|
| OPEN filename termination | 4040/D64, production FPGA IEEE fabric | Pinned `DOS_4040/tst2` (`PUT`, `L50`, `L45`) sets `CMDWAT` only on filename EOI, then `idle` (`IDLE`) dispatches `parsex` (`PARSXQ`) | Final filename byte carries EOI before UNLISTEN, verified independently on the wire | Shared `open_file`, PRG and DOS status acceptance cases | PET `OPENI` sends via buffered `CIOUT`, and `UNLSN` flushes the final pending byte with EOI (see the controller evidence below) |
| PRG OPEN and final-sector streaming (at least two stored bytes) | 4040/D64, production FPGA IEEE fabric | Pinned `DOS_4040/master`, `open` (`OP40`), `opchnl` (`OPNRCH`, `OR30`, `GETBYT`, `RDBYT`, `RD4`), `tst4` (`GET`, `SEQGET`), `i_odef` (`EOIOUT`, `RDYTLK`) and `ieee` (`TALK1`, `TLK10`) | Exact stored bytes, EOI only on the last valid byte, no sector padding queued | `DrivePrgTest.StreamsCheckedPrgFixtureOverProductionTop` | [VICE virtual-drive reader](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/vdrive/vdrive-iec.c) corroborates sector-link lengths, not the native one-byte EOF bug or physical IEEE timing |
| Native one-byte initial-EOF overwrite | 4040/D64 wire acceptance, plus single-sided 8050/D80 DOS 2.7 firmware unit tests | Pinned `DOS_4040/opchnl` and `DOS_8250/opchnl` (`OR30`, `GETBYT`), `DOS_4040/opchnl` and `DOS_8250/tst2` (`RDBYT`, `RD0`), ordinary TALK readiness consumption in both `ieee` sources | Sole byte `X` produces `X, 0, 2, X`, EOI only on byte four. Two-byte files terminate normally | `DriveOneByteTest.PreservesNativeEofBugOverProductionTop`, native one-/two-byte firmware tests | Operator BASIC capture confirms the sequence and two-byte threshold, but its model/revision was not identified. VICE high-level virtual-drive EOF is not an oracle for this ROM bug |
| Successful OPEN status | 4040/D64, status channel 15 | Pinned `DOS_4040/parsex` (`PARSXQ`), `erproc` (`ERRTAB`, `OKERR`, `ERRMSG`, `ERMSG2`) and [`tst4` (`GETERC`)](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4#L81), which produces CR with `EOIOUT` | Exact `00, OK,00,00` plus CR, with final-byte EOI | `FirmwareTest.StreamsDosStatusOverProductionTop` | [VICE status formatting](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/vdrive/vdrive-command.c) and [error text](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/cbmdos.c) retain the leading space and CR |

### IEEE regression coverage

The audit of open PRs [#52](https://github.com/DLehenbauer/econopet/pull/52),
[#53](https://github.com/DLehenbauer/econopet/pull/53),
[#54](https://github.com/DLehenbauer/econopet/pull/54) and
[#55](https://github.com/DLehenbauer/econopet/pull/55) adds focused tests in
[`controller_test.cpp`](ieee/controller_test.cpp) and
[`drive_test.cpp`](ieee/drive_test.cpp). These execute CPU-backed PIA/VIA
accesses, production FPGA handshakes and, for drive cases, production firmware
through physical SPI.

The four abort regressions now run by default. The FPGA advances the selected
TX FIFO once when NRFD is asserted after DAV, independently of private `$E820`
reads. DIO and EOI remain latched until NDAC or ATN releases DAV. ATN never
advances the FIFO again. `T_PREFETCH` advances once on NRFD, then enters the
NDAC-wait state `T_ACK`. Both states can finish on NDAC, preserving unread
termination and same-clock NRFD/NDAC handling without a separate prefetch flag:

- `IeeeAbortBeforeNrfdPreservesSampledByte`: X is sampled through `$E820`,
  but NRFD remains released until ATN aborts. The resumed stream must start at
  X. Both data and status FIFOs are exercised.
- `IeeeAbortAfterNrfdPrefetchesUnreadByte`: NRFD is asserted after DAV, but
  the PET does not sample X. Native `TLK20` enters `TLK30` and `GET` on
  asserted NRFD, prefetching Y before `TLK35` waits for NDAC. The FPGA must
  resume at Y. Both FIFOs are exercised.

The sequential terminal-state regressions run by default on both D64/4040 and
D80/single-sided 8050 DOS 2.7:
- `SequentialRetalkAfterEofDoesNotInventData`: a completed two-byte SEQ is
  addressed again without reopening. The channel remains not ready without
  inventing CR+EOI. Reopening restores its original payload and final EOI.
- `SequentialReadErrorDoesNotInventEof`: a malformed successor follows 254
  valid bytes. The prefix has no EOI and no CR filler follows. The failed
  channel is released, so retalking after status acknowledgement neither
  creates data nor regenerates the error. Repairing the mounted backing file
  in place still leaves that channel closed. A subsequent IEEE OPEN restores
  the complete stream, including its repaired successor and genuine final EOI.
  The test retains the emulator's
  existing status-23 mapping. It does not claim that an illegal track is
  native checksum-error 23 or reproduce a drive controller's retries.

The native empty-REL regressions run by default on both D64/4040 and
D80/single-sided 8050 DOS 2.7:
- `EmptyRelativeRecordReturnsNulThenAdvances`: an all-zero first record returns
  one NUL+EOI, followed by record two on the next TALK. Record two crosses a
  physical sector link and must retain every byte and its final EOI.
- `ConsecutiveEmptyRelativeRecordsRemainSeparate`: two consecutive empty
  records each return one NUL+EOI before a marked third record. Trailing zeros
  in that third record are still trimmed, and channel status remains OK.
- `PositionSuffixAndOverflowFollowNativePointers`: P into a nonempty record's
  zero suffix preserves the native modular-buffer overflow, including a
  successor-sector byte with EOI. The first invalid offset reports exact
  error 51 and positions the requested record at its start.
- `DriveRelativeContinuationTest.ResumesAtNativePrefetchBoundary`: both media
  models cross four interruption phases (before DAV, sampled before NRFD,
  prefetched before NDAC, and a partially received record) with status,
  another channel, and another unit. OPEN discards the hardware queue, but
  returning to its owner restores only its unread bytes. The test also arms
  the listener before firmware service to detect cross-owner data leakage.
- `DriveRelativeRomTest.NativeReceivePreservesEmptyRecordsAndEoi`: actual
  corrected PET KERNAL 2.0, KERNAL 4.0, and Waterloo revision-12 input routines
  each receive two NUL/EOI records and a marked third record. CPU captures and
  passive wire traces independently check payload and EOI.

These deliberately edit REL directory metadata over checked data chains, as
the unit fixtures do. They validate the record-data engine, not native
side-sector construction. The original regression failed by returning record
two's `$A0` without EOI instead of record one's NUL+EOI. The original empty-record
controls fail with the old trim predicate at seeds 1 and 17 and pass with
the native correction.

The other abort controls cover the remaining two combinations: released NRFD
without a private sample preserves X, and asserted NRFD with a sample resumes
at Y. All abort cases keep NDAC asserted. ATN preserves the selected NRFD level
rather than changing both lines together. Status resumes after command-free
ATN because a fresh status secondary intentionally flushes TXS. These are
source-derived settled-prefetch expectations, not execution of a drive ROM
or proof of its instruction-level interrupt timing. The #52 proposal to always
preserve a byte until NDAC is released does not implement this native matrix.

Twelve `DriveRomTerminalTest.NativeReceiveTimesOutOnTerminalChannel` cases
execute the installed corrected PET KERNAL 2.0, PET KERNAL 4.0, and Waterloo
revision-12 receive routines against production firmware through physical SPI.
Each revision reads both D64 and D80, at ordinary EOF and after a failed
successor. The native routine first receives every valid byte (two at EOF or
254 before failure), UNTALKs and retalks that channel, then attempts one more
receive. Independent wire records
must contain exactly the valid payload, with EOI only at genuine EOF.

PET cases clear the prior `SATUS` immediately before the extra `ACPTR`. The
normal VIA timer must expire, execution must reach `ERRP1`, and the routine
must return `SATUS=$02` and accumulator CR. That CR is the controller's local
recovery value, not a byte received from the drive. Waterloo cases set the
FCB wait word to one, allowing its native 3200-iteration DAV wait to expire.
Execution must reach `$BF7C`, with FCB status and global error both three.
The error path does not initialize its returned byte, so that value is not
treated as received data. Neither path injects timer faults or fabricates DAV.
These execute controller ROMs, not 4040/8050/8250 drive ROMs.

The separate Waterloo-order control releases NRFD, then NDAC without a DIO
read, waits for DAV release, and only then asserts ATN. It checks the unread
byte on resumed TALK. This reproduces revision-12 register ordering through
the synthetic 6502 controller, not execution of Waterloo ROM or analog timing.
For the unread next byte, NRFD remains released through termination, so the
native `TLK20` NRFD-triggered prefetch has not occurred.

Sixteen enabled model/unit/drive combinations check status on devices 8-11,
drives 0/1, and both D64/4040 and D80/single-sided 8050 DOS 2.7. Independent
errors on two units must survive reading the other unit's status. Reading
status clears its code but retains its drive identity. Unlike #55's blanket
proposal, the 4040 response has no drive suffix and the 8050 response has one.
The model-specific implementation is already present from
[#64](https://github.com/DLehenbauer/econopet/pull/64).
The status matrix is separate from the D64/D80 sequential terminal coverage.
Each combination requires exact native `62, FILE NOT FOUND,...` responses
from both units and `00, OK,...` after status clearing. CPU and wire bytes
must agree, all transfers must have ATN released, and the final CR must carry
EOI. Firmware unit tests also check
that text on both local drives, all four units, and both mixed-model orders.

All Commodore source links below select `mist64/cbmsrc` revision
`01bd60f162ef92212ef0cb67546ae8f42be34168`.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|---|---|---|---|---|---|
| Single-character filename wildcard | 4040 DOS 2 and shared full-size 8050/8250 DOS 2.7 | [4040 `lookup`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/lookup) and [8250 `lookup`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/lookup): `COMPAR`, `CP30` through `CP40`. Respective `open`: `OP20` calls `FFST`, and `OP90` through `OPREAD` use the matched file | `?` consumes exactly one non-`$A0` filename byte. Pattern completion requires name completion or the end of the 16-byte field. `*` immediately accepts the remainder, ignoring subsequent pattern bytes | [`diskimage_find`](../../fw/src/ieee/diskimage.c), [`test_native_single_character_wildcards`](../../fw/test/diskimage_test.c), and [`DriveWildcardRomTest.NativeOpenAndReadResolveSingleCharacterWildcards`](ieee/drive_test.cpp) | [Pinned VICE `cbmdos_parse_wildcard_compare`](https://github.com/VICE-Team/svn-mirror/blob/3723f8a012ec9a24073ee6010e0a2d3342fb63b6/vice/src/cbmdos.c) independently rejects `?` against `$A0`, advances one byte otherwise, and immediately accepts `*` |
| Raw native filename bytes | 4040 DOS 2 and shared DOS 2.7, single-sided 8050 media mode | Same pinned `lookup` sources: `CP30` uses direct `CMP (DIRBUF)Y`, accepts `?` only when the directory byte is not `$A0`, and `CP34` tests `$A0` or the 16-byte boundary | `BAS?C` rejects `BaSIC`. `A?B` accepts `$41,$00,$42,$A0...`, but `A` rejects it. A zero in the first or sixteenth name byte remains consumable by `?` | [`diskimage_find`](../../fw/src/ieee/diskimage.c), [`test_native_single_character_wildcards`](../../fw/test/diskimage_test.c), [`test_native_open_matches_raw_filename_bytes`](../../fw/test/ieee_drive_test.c) | Same pinned VICE comparator compares literals without folding case and treats only `$A0`, not `$00`, as filename padding |
| Native pattern length limit | 4040 DOS 2 and shared DOS 2.7, single-sided 8050 media mode | [4040 `trnsfr`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/trnsfr) and [8250 `trnsfr`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/trnsfr): `FNDLMT` increments `STRSIZ` and exits when `15 < STRSIZ`, leaving `LIMIT` immediately after byte 16. Respective `lookup`: `CP33` branches to `CP34`, which accepts `Y >= 19` | Seventeen question marks and `ABCDEFGHIJKLMNOPX` both select `ABCDEFGHIJKLMNOP`. A mismatch within the first 16 bytes or padding before byte 16 still rejects the name | [`test_native_single_character_wildcards`](../../fw/test/diskimage_test.c) and [`test_native_open_matches_raw_filename_bytes`](../../fw/test/ieee_drive_test.c), on D64/D80 | Same pinned VICE `cbmdos_dir_slot_create` limits the pattern to `CBMDOS_SLOT_NAME_LENGTH`, then `cbmdos_parse_wildcard_compare` compares those 16 bytes |
| Flat HDD fallback isolation | EconoPET extension, not a native Commodore container | Not applicable (extension policy implemented in [`resolve_open`](../../fw/src/ieee/ieee_drive.c)) | An empty requested drive never aliases a peer HDD, regardless of filename spelling or wildcards. A rejected OPEN exposes no REL bytes or writes. Explicitly selecting the mounted HDD still works | [`test_hdd_aliases_preserve_drive_isolation`](../../fw/test/ieee_drive_test.c) and [`test_empty_drive_falls_back_to_native_peer`](../../fw/test/ieee_drive_test.c) | Negative read/write and backing-image checks across all eight device/drive coordinates, positive same-alias controls, and separate D64/D80 fallback controls (extension evidence only) |
| ATN before NRFD-triggered prefetch (#52) | 4040 DOS 2 and shared 8050/8250 DOS 2.7 IEEE handshake | [4040 `ieee`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ieee), [8250 `ieee`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ieee): `ATNIRQ`, `TALK1`, `TLK20` | With NRFD released and NDAC asserted, `TLK20` has not entered `GET`. ATN preserves X, regardless of a private PET read | `ieee.sv`, `IeeeAbortBeforeNrfdPreservesSampledByte` and its unread control | [IEEE handshake description](https://github.com/mist64/cbmbus_doc/blob/master/1%20IEEE-488.md) distinguishes NRFD from NDAC. [MAME RIOT input wiring](https://github.com/mamedev/mame/blob/master/src/devices/bus/ieee488/c2040.cpp) exposes NRFD on bit 7 and NDAC on bit 6, not PET reads |
| ATN after NRFD-triggered prefetch | Same drive ROMs, ordinary nonfinal byte with settled prefetch | Same `ieee` sources: `TLK20` (`BIT PBD2; BPL TLK30`), `TLK30` (`JSR GET`), `TLK35` (NDAC wait) | Asserted NRFD reaches `GET` before NDAC release. After prefetch, ATN resumes at Y, with or without a private PET read | `IeeeAbortAfterNrfdPrefetchesUnreadByte` and its sampled control | Same MAME NRFD/NDAC wiring corroborates the input bits. IEEE bus acceptance alone does not define the ROM's internal prefetch point |
| Counted-read termination control | SuperPET Waterloo `970019-12`, fast FPGA talker | [Revision-12 binary](https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/SuperPET/waterloo-c000-dfff.970019-12.bin), `$C14F-$C16D` | NRFD release precedes NDAC release, DAV release precedes ATN. Unread next byte remains queued | `IeeeWaterlooCountedTerminationPreservesUnreadByte` | [Binary-derived navigation](https://github.com/prachwal/personal-004/blob/main/docs/pet/disassembly/waterloo-c000-dfff.asm) and the same independent IEEE handshake description |
| Exhausted SEQ (#53) | 4040 DOS 2 and shared 8050/8250 DOS 2.7 | [4040 `tst4`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4) and [8250 `tst4`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst4): `GET00`, `GET0`; respective `ieee`: `TALK1`, `NOTLK` | Final EOI leaves an ordinary channel NOTRDY. Retalking must not fabricate CR+EOI. Reopening restores readiness | `ieee_drive.c`, `SequentialRetalkAfterEofDoesNotInventData`, D64/D80 unit cases | [VICE sequential reader](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/vdrive/vdrive-iec.c): EOF leaves read mode, rather than generating a successful CR response |
| Failed SEQ successor (#53) | 4040 DOS 2 and shared 8050/8250 DOS 2.7, with EconoPET backing-read fault stimulus | [4040 `erproc`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/erproc) and [8250 `erproc`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/erproc): `CMDER3`, `TLKERR`, `TLERR`; respective `tst3`: `FRECHN`; `opchnl`: `OPNRCH` | Error recovery releases DAV and the ordinary channel, not an invented final data byte. Status acknowledgement and media repair do not reopen it. OPEN recreates a readable stream | `SequentialReadErrorDoesNotInventEof` on D64/D80, D64/D80 unit cases | Same VICE reader exits normal reading on sector-read failure. Its returned error byte/status is not an IEEE wire-EOI oracle |
| Native PET terminal receive | Corrected `901465-03` and `901465-22` KERNALs, normal external VIA timer | [KERNAL 2.0 `ob1src`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/KERNAL_PET_2.0_REC/ob1src) and [KERNAL 4.0 `ob1src`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/KERNAL_PET_4.0_REC/ob1src): `ACPTR`, `ACP01`, `ERRP1`, `ER001`; selected binaries reach `$F141` or `$F15B` | Valid bytes are received first. With no further DAV, the timer expires, status becomes `$02`, and local CR recovery does not appear on the wire | `DriveRomTerminalTest.NativeReceiveTimesOutOnTerminalChannel`, eight PET cases | Execution of both installed controller ROM revisions plus independent wire and opcode-fetch evidence |
| Native Waterloo terminal receive | SuperPET revision 12, `970018-12` and `970019-12` | [Pinned A000 binary](https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/SuperPET/waterloo-a000-bfff.970018-12.bin): `$BEFB`, finite wait `$BF1A-$BF49`, timeout `$BF7C-$BF83`, global error `$B650`; [C000 binary](https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/SuperPET/waterloo-c000-dfff.970019-12.bin): TALK `$C0DD` | Wait word one expires after 3200 DAV polls, FCB status and global error become three, no extra wire byte | Same native terminal suite, four Waterloo cases | Binary-checked [A000 navigation](https://github.com/prachwal/personal-004/blob/main/docs/pet/disassembly/waterloo-a000-bfff.asm) and actual revision-12 execution with independent wire/opcode observations |
| Empty REL record (#54) | 4040 DOS 2 and shared 8050/8250 DOS 2.7 | [4040 `rel4`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel4) and [8250 `rel4`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel4): `SETLST`, `FNDLST`; respective `rel1`: `NXTREC`, and `rel3`: `RD25`, `RD30`, `RD40` | Scan the whole record. If every byte is zero, retain the valid selected byte as the last character. That NUL carries EOI before advancement | Both `DriveRelativeTest` cases on D64/D80, native unit matrix and flat-extension controls | [VICE `vdrive_rel_read`](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/vdrive/vdrive-rel.c) reads the selected byte before comparing length and moving the record pointer |
| Interrupted REL continuation | Same selected 4040 and shared 8050/8250 sources | Respective `ieee`: `TALK1` sends saved `CHNDAT`, `TLK20`/`TLK30` call `GET` on asserted NRFD before `TLK35` waits for NDAC. Respective `tst4`: `GET` dispatches to `rel3` `RDREL` | Before NRFD, readdressing resumes the same byte, even after a private PET sample. After NRFD, it resumes the prefetched byte. Queueing and FIFO discard never advance a record | `DriveRelativeContinuationTest`, firmware continuation matrix, `ieee_tb` owner/progress controls | Native channel-indexed `CHNDAT`/`CHNRDY` storage, independent CPU and wire observations, and MAME RIOT NRFD/NDAC wiring cited above |
| Queued REL owner selections | Same selected drive sources | Respective `ieee`: `TALK1` selects the addressed channel's saved `CHNDAT` through `LINDX` | Processing an earlier queued selection must never present that channel's data to the currently selected peer | `QueuedSelectionsPauseInitiallyUnboundData` checks exact CPU and wire bytes on D64/D80, starting with no queued REL batch | Same native channel-local data and MAME IEEE unit/handshake wiring cited above |
| Selected talker after UNLISTEN or another peripheral's LISTEN | 4040 DOS 2 and shared full-size 8050/8250 DOS 2.7 | [4040 `ieee`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ieee) and [8250 `ieee`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ieee): `DCDE20`/`DCDE30` clear `ADRSED` for foreign LISTEN without clearing `TLKACT` or `LSNACT`. Only own LISTEN reaches `DCDE40`, which clears `TLKACT`. `DCDE70` changes SA only when `ADRSED` is set | TALK 8, SECOND 2, LISTEN 9, SECOND 15, UNLISTEN retains unit 8's talker and channel 2. A peer's secondary must not flush TXS or select its status FIFO for that talker. The same rule holds for every ordered pair among devices 8-11 and foreign device 7 | [`handle_command`](../../fw/src/ieee/ieee_drive.c), per-unit SA/roles in [`ieee.sv`](../../gw/EconoPET/src/ieee.sv), and `ieee_tb`. `DrivePreservedTalkTest` retains eight original foreign/UNLISTEN cases and adds eight D64/D80 SEQ/REL cases, each covering all twelve ordered virtual-unit pairs and a peer status or different data secondary | [Independent TALK/LISTEN description](https://www.pagetable.com/173) specifies foreign LISTEN as ignored and SECOND as selecting only the last primary-addressed device. Pinned MAME [4040 controller and private RAM](https://github.com/mamedev/mame/blob/7e1ead4a1f40851666d3b428e6624121123f11af/src/devices/bus/ieee488/c2040.cpp) and [8050 counterpart](https://github.com/mamedev/mame/blob/7e1ead4a1f40851666d3b428e6624121123f11af/src/devices/bus/ieee488/c8050.cpp) corroborate independent peripheral state, not execution of this counterexample |
| Independent listener roles and primary selection | Same selected drive ROMs | Same `ieee` sources: foreign LISTEN takes `DCDE30` without changing `LSNACT`. `DCDE60` clears every peripheral's `TLKACT`, but clears `LSNACT` only in the addressed talker. `DCDE50` sets that peripheral's default SA to 32 (channel 0). UNLISTEN clears all listeners, while UNTALK leaves listeners intact | LISTEN A, SECOND 15, LISTEN B, SECOND 15, then binary P for unopened channel 2 with EOI reaches both listeners, changing their prepared error 64 to 70. TALK A instead clears only A's listener, leaving its error 64 while B changes to 70. Repeating own LISTEN resets its data-channel selection to default 0 rather than writing another byte to the previous channel 2 | [`test_virtual_units_retain_independent_listeners`](../../fw/test/ieee_drive_test.c) checks status, write bytes, EOI and own/peer controls across sixteen model/unit/drive combinations. `ieee_tb` checks listener ownership, default SA, foreign secondary rejection and UNTALK/UNLISTEN controls | Same independent TALK/LISTEN description specifies cumulative listeners, global UNLISTEN and replacement of the talker by TALK. The pinned MAME per-device CPU/RAM maps corroborate independent ownership |
| Parking an active REL presentation | Same selected 4040 and shared 8050/8250 DOS 2.7 drive sources, PET KERNAL 4.0 | Respective `ieee`: `TLK20` waits for NRFD, `TLK30` calls `GET`, `TLK35` waits for NDAC, and only `TLKRTN` releases DIO/DAV/EOI. [PET `ob1src`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/KERNAL_PET_4.0_REC/ob1src): `ACPTR` asserts NRFD before sampling EOI and DIO, then releases NDAC | Parking stops new presentations, but preserves the active byte and EOI through NDAC. Firmware must yield without consuming queued commands or reusing the FIFO while that presentation remains active | `DriveRelativeParkingTest.KeepsPresentationThroughNdacWhileDeferringReuse`: eight D64/D80 A-B-A cases before/after NRFD and on nonfinal/final bytes. `ieee_tb` separately checks stable pins, exactly one GET, and active readback | Independent IEEE handshake description and MAME NRFD/NDAC hardware wiring cited above distinguish prefetch from listener acknowledgement |
| REL preparation after EOI | Same selected drive sources | [4040 `rel3`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel3) and [8250 `rel3`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel3): `RD40` calls `NXTREC`, then `RD25`/`RD30` save the next byte and readiness/EOI in `CHNDAT`/`CHNRDY` | GET of an EOI byte prepares the next record before NDAC. A peer write afterward must not replace that prepared data or EOI | `test_relative_eoi_retains_prepared_next_record`, plus the after-NRFD peer-write integration phase on D64/D80 | [VICE REL reader](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/vdrive/vdrive-rel.c) retains per-channel sector buffers and advances `bufptr`/`record_next` at the record boundary. It is not an IEEE prefetch-phase oracle |
| REL writes after physical pointer wrap | Same selected drive sources | [4040 `rel2`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel2) and [8250 `rel2`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel2): `WR40` calls `CLREC`, then `RD40`/`NXTREC`, retaining the physical buffer position | After the 129-byte-record P(1,129) wrapped read, consecutive writes start at payload offsets 383 and 512, not logical offset 258. Earlier data remains intact | `test_relative_wrapped_read_write_continuation` compares the entire backing payload for all sixteen model/unit/drive combinations | Same VICE implementation's `vdrive_rel_listen` advances the buffered `record_next` pointer rather than reconstructing a logical P position |
| P offset overflow | Same selected drive sources | [4040 `record`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/record) and [8250 `record`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/record): `R30`/`R40`, `R60`; respective `parsex`: `ENDCMD`; `rel3`: `RD05` | Encoded offsets 0/1 both select the start. Offset greater than RS records 51, resets the offset to zero, and still positions the requested record. Missing-record error 50 takes precedence | `PositionSuffixAndOverflowFollowNativePointers` and sixteen model/unit/drive boundary cases | [VICE REL positioning](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/vdrive/vdrive-rel.c) corroborates record-overflow handling, not the primary error-precedence oracle |
| P into a nonempty zero suffix | Same selected drive sources | Respective `record`: `POSITN`/`POSBUF`; byte-identical `rel4`: `SETLST`/`FNDLST`; `rel3`: `RD15`/`RD25`/`RD30`; `rel1`: `NRBUF`/`NXTREC` | For a 129-byte first record whose only nonzero byte is its first, P offset 129 returns payload offsets 128..254 (127 bytes), EOI only on offset 254. Selecting an empty suffix already in the successor buffer instead falls back to the selected NUL/EOI | Same system P test and sixteen model/unit/drive physical-pointer cases | VICE's `vdrive_rel_position` documents overflow into subsequent records and the crossed-buffer one-byte exception. Its 1541-derived comments do not override the selected IEEE ROMs |
| Status drive field (#55) | 4040 DOS 2 versus single-sided 8050/shared DOS 2.7 | [4040 `erproc`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/erproc): `ERMSG2` ends after sector; [8250 `erproc`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/erproc): `ERMSG2` appends `DRVNUM AND 1`; `tst4`: `GETERC` supplies final CR/EOI | Exact four-field 4040 or five-field 8050 status, including final-byte EOI and correct local drive | `DriveStatusTest.PreservesModelSpecificStatusAndUnitDriveIdentity` | [MAME ROM-backed 8050/8250 model](https://github.com/mamedev/mame/blob/master/src/devices/bus/ieee488/c8050.cpp). VICE's generic four-field formatter is not used to override the 8050 source |
| Native error-62 token spacing | 4040 DOS 2 and shared 8050/8250 DOS 2.7 | Same `erproc` sources: `ERRTAB` starts error 62 with FILE token `$83`; 4040 `MER7` and 8250 `E60` insert `$20` before expanding each token; `ERRMSG`/`JERRMS` place the message after `62,` | `62, FILE NOT FOUND,00,00` on 4040, or `62, FILE NOT FOUND,00,00,d` on 8050/8250 local drive `d`, followed by CR/EOI | `DriveStatusTest.PreservesModelSpecificStatusAndUnitDriveIdentity`, all sixteen model/unit/drive combinations, and firmware status unit tests | [MAME ROM-backed 4040 model](https://github.com/mamedev/mame/blob/master/src/devices/bus/ieee488/c2040.cpp) executes drive ROMs rather than substituting status text. [VICE's high-level formatter](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/vdrive/vdrive-command.c) and [error table](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/cbmdos.c) omit this space and do not override the selected ROMs |

The sequential correction removes both fabricated CR/EOI paths, without
changing genuine final bytes. In both native `tst4` implementations, `GET00`
routes channel 15 to `GETERC` before testing ordinary-channel EOF. That
status-channel routine supplies CR. For an exhausted ordinary channel,
`GET0` instead writes `NOTRDY`. The respective
[4040 `ieee`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ieee)
and [8250 `ieee`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ieee)
`TALK1` then return through `NOTLK` without offering another byte.
On a sequential talker error, `TLKERR` releases DAV and `TLERR` calls
`FRECHN`. Both [4040 `tst3`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst3)
and [8250 `tst3`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst3)
clear the ordinary channel's `LINTAB` entry. Firmware therefore invalidates
the failed open channel instead of retrying it on a later TALK. These
shared DOS 2.7 control paths do not branch on single/double-sided media.
The tests measure the no-invented-data and terminal-readiness contracts,
not cycle-exact ROM error-interrupt timing. The 254-byte prefix and status
23 describe EconoPET's backing-chain fault stimulus, not a claim that a
native illegal-track fault is checksum error 23.

The sequential channel has one lifecycle enum: closed, readable, or exhausted.
Read failure releases it to closed, while queuing the genuine final byte marks
it exhausted. `streaming` remains separate because UNTALK pauses top-up without
changing that lifecycle. Backpressured firmware unit controls exercise this
readable-but-paused state on all devices 8-11 and both local drives. Readdressing
must resume at the saved cursor with the exact remaining bytes and final EOI.

For empty native REL records, both `rel4` source files are byte-identical.
`SETLST` loads the full record size `RS` as `FNDLST`'s backward-scan limit,
independently of a P command's byte offset. If no nonzero character is found,
`SETL10` falls through to `GETPNT` and stores that selected pointer in
`LSTCHR`. `NXTREC` has already retained the first byte in `DATA`.
Both [4040 `rel3`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel3)
and [8250 `rel3`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel3)
store that byte in `CHNDAT`, compare its pointer with `LSTCHR`, and set
`RNDEOI` through `RD30` on equality. In both models' `i_odef`, `RNDEOI`
includes `EOIOUT=$80`. A subsequent `RD40` invokes `NXTREC`, so an empty
record cannot disappear before its NUL has been offered.

Firmware now scans the entire native record and falls back to the valid
selected byte when the native buffer search is empty. For ordinary record-start
reads this means the whole record is empty. P into a nonempty zero suffix
instead follows the physical `LSTCHR` comparison and buffer-switch exception,
not a logical-record skip. The unit matrix crosses
D64/D80, devices 8-11, and both local drives, at record lengths 1, 2, 128, 129,
and 254. It checks consecutive empty records, embedded/trailing zeros,
advancement to a marked record, and repositioning to an empty record's final
byte. Eight additional unit controls preserve the flat `.hdd` extension's
existing transparent-empty-record behavior. That extension is not a native
Commodore disk format.

The changed native trimming and EOI paths do not branch on 8050 single-sided
versus 8250 double-sided mode. The compatibility conclusion covers the selected
4040 DOS 2 and shared full-size 8050/8250 DOS 2.7 sources, not earlier 8050 DOS
2.5 or 8250LP revisions. D80 system tests execute the EconoPET record engine,
not an 8050/8250 drive ROM or D82 image. They measure exact bytes, EOI, native
controller execution, and interrupted-read advancement, not drive-ROM cycle timing.

#### REL transport progress and ownership

The existing eight-register map is unchanged in size. Reads of data `TX`
now return an unsigned modulo-256 count of NRFD-triggered data prefetches.
Reads of `TXS` return the corresponding status GET count. Reads have no side
effects, including adjacent SPI prefetch. The two counters advance independently.
Data-only flush, automatic OPEN/CLOSE discard, and status-secondary replacement
preserve them. IFC and full control flush reset them.
All supported PET/fabric resets go through the MCU and include
`ieee_drive_reset()`, invalidating DOS channels before fabric progress is
reset. Independently pulling the physical CPU reset pin low is unsupported.
There is deliberately no independent fabric-reset detection or recovery.
The counter records GET in the source-handshake state, independently of the
delayed FIFO pointer update. The collision bench proves that a simultaneous
data discard cannot erase the presented byte's NRFD-triggered progress.
The new firmware requires the matching gateware bitstream for this readback
and ownership contract.

Writes of `SA` bind the data FIFO to `{valid, reserved, unit[1:0], channel[3:0]}`.
The MCU uses valid bindings for production data. A mismatched unit/channel
cannot present data, including before firmware processes the selection.
Status uses its separate FIFO and is unaffected. Zero retains unbound raw
FIFO access for hardware fixtures. Binding data to channel 15 pauses new data
presentations without withdrawing an active byte. That byte's DAV, DIO, and
EOI remain stable until NDAC releases, even if its NRFD-triggered GET has
already occurred. ATN still cancels the presentation through the native abort
path.
CTRL read bit 2 (`DATA_ACTIVE`, mask `$04`) reports an active data DAV, not
status-channel traffic. Before consuming each queued RX command or listener
byte, firmware pauses new presentations and checks that bit. If it is set,
the task returns cooperatively, leaving RX queued and the active byte intact.
After acknowledgement, a later task call can reconcile settled GET progress
and reuse the queue. This never blocks the MCU or a SPI/Wishbone transaction
waiting for the listener, including when the controller CPU is halted.
Firmware also pauses presentation when no REL batch is tracked, because full
flush leaves TX unbound and the fabric may already have processed a later
selection than the firmware command currently being serviced. Only after the
new response is fully restored does firmware bind and release it.
New data-owner bindings remain paused until the task has processed the
ordered RX batch, including its trailing ATN-release entry. Releasing at the
secondary alone could start DAV while that entry still waits in RX, causing
the active-presentation guard to block its own batch completion.
The logical queue binding survives a temporary RX pause independently of the
REL progress pointer and SEQ streaming state. After an RX-service pass,
firmware restores that retained binding, allowing UNLISTEN or foreign LISTEN
to resume the same talker without readdressing. Queue parking and DOS reset
invalidate the retained binding, so resuming cannot resurrect discarded
ownership. The fabric still checks the bound unit/channel against its current
selection, protecting against earlier queued selections.

Each REL channel retains one immutable pending response (at most 254 bytes)
and its consumed offset. The firmware checks progress against that bound,
so modulo-counter wrap is unambiguous even if unrelated sequential traffic
has advanced the counter many times. Retalking the same pending owner leaves
the FIFO intact. Reusing it for another owner parks the unread tail and
restores it without rereading the backing image. GET of the final EOI byte
advances the channel's read/write position even if NDAC has not completed.
For a valid following native record, that reconciliation also saves its
prepared response and EOI state before another channel can overwrite the
backing data. Successful native writes similarly prepare the following
record and advance from the current physical base, not a reconstructed
logical P offset.
P and writes intentionally replace their channel's pending response. CLOSE,
reset, and replacement of its mounted image invalidate the affected channel.
Queue replacement explicitly parks the current REL owner before issuing the
fabric discard. The low-level control-register writer does not synchronize or
advance firmware channels. Reset/unmount instead discard their DOS state
before resetting the fabric.

The error-62 correction changes only its message literal. Both native tables
expand tokens 3 (FILE), 6 (NOT), and 7 (FOUND). Each token expansion emits
`$20` first, producing exactly ` FILE NOT FOUND`, with no trailing space.
`ERRMSG`/`JERRMS` supply the preceding `62,`. The 4040 `ERMSG2` then appends
track and sector, while shared DOS 2.7 unconditionally appends `DRVNUM AND 1`
as a fifth field. That formatter has no single/double-sided mode branch, so
the status-text conclusion applies to both 8050 and 8250 DOS 2.7. Both
`tst4` implementations' `GETERC` supply the final CR with EOI and reset status
to OK. The correction leaves those existing fields and handshake paths alone.
The integration cases exercise D64/4040 and D80/single-sided 8050 through
production firmware and FPGA I/O. The 8250 conclusion is primary-source
evidence for the shared formatter, not execution of an 8250 ROM or D82 media.

Run the focused abort, sequential terminal and native REL controls:

```sh
(cd build/system/board && ./econopet_board_tests \
  --gtest_filter='DataAndStatus/IeeeAbortTest.*')
(cd build/system/board && ./econopet_firmware_tests \
  --gtest_filter='Models/DriveTerminalTest.*:ModelsFailuresRevisions/DriveRomTerminalTest.*')
(cd build/system/board && ./econopet_firmware_tests \
  --gtest_filter='*DriveRelative*:*DrivePreservedTalkTest.*')
```

The working directory supplies the SID model's lookup resources. The ordinary
CTest run includes REL data, continuation, positioning, parking, and native
controller cases.

### Command-channel OPEN dispatch

`OPEN` on secondary 15 supplies a DOS command, not a filename. Previously,
[`resolve_open`](../../fw/src/ieee/ieee_drive.c) discarded that command.
Both OPEN 15 and secondary-address 15 now enter the same counted command
collector and executor. Binary record-number bytes cannot be truncated by
C-string termination or interpreted as filename/type delimiters.
An explicit receive mode distinguishes filename, command, and ordinary data
collection. EOI increments a per-unit pending-command indication (`CMDWAT`),
without executing or resetting the input pointer. Dispatch occurs when ATN
processing returns that unit to IDLE/VIDLE, neither listening nor talking.
Two EOI-terminated P payloads within one continuous LISTEN therefore remain
one counted buffer, and RECORD uses the first P's fields. Incomplete command
bytes remain counted per unit across UNLISTEN and readdressing. A dispatch
consumes the pending count and buffer, so later UNLISTEN cannot repeat it.

The FPGA retains `{eoi, atn, data}` at the accepted DAV edge. CTRL read bit 3
(`RX_EOI`, mask `$08`) exposes the current head's EOI while RX_AVAIL is set.
An additional FIFO entry records ATN release in order, exposed through CTRL
read bit 4 (`RX_ATN_END`, mask `$10`). It carries no bus byte and is filtered
from semantic RX byte traces. It consumes one RX slot and is retained under
full-FIFO backpressure. This avoids inferring past dispatch boundaries from
current bus pins or an empty queue when firmware service is delayed.
Existing addresses and STATUS bit 7 (talker starvation) are unchanged.
Firmware reads CTRL, STATUS, and RX before explicitly popping that same head.
The register mock and physical-SPI observation helpers preserve this metadata
rather than inferring EOI from UNLISTEN or from the live bus after reception.
Deploy the updated firmware and FPGA image together, because older gateware
does not expose the RX-head EOI/ATN-release flags or status GET counter.

All source links below select `mist64/cbmsrc` revision
`01bd60f162ef92212ef0cb67546ae8f42be34168`.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|---|---|---|---|---|---|
| Completed command-channel OPEN | 4040 DOS 2 and shared full-size 8050/8250 DOS 2.7, either hardware side mode | [4040 `tst2`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2) and [8250 `tst2`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2): `PUT`, `L42`, `L50`, `L45` collect command bytes and queue parsing at EOI. Respective [4040 `parsex`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/parsex) and [8250 `parsex`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/parsex): `PARSXQ` sends an OPEN with low nibble `$F` through `PS05`, not `OPEN` | LISTEN, OPEN `$FF`, binary P payload with final-byte EOI, UNLISTEN must position the existing REL channel just as secondary `$6F` does. It must not reopen the REL file or discard its command | [`ch15_begin`](../../fw/src/ieee/ieee_drive.c), [`test_command_open_positions_relative_records`](../../fw/test/ieee_drive_test.c), [`DriveRelativeTest.CommandChannelOpenExecutesBinaryPosition`](ieee/drive_test.cpp) | [Pinned VICE `vdrive_iec_open`](https://github.com/VICE-Team/svn-mirror/blob/3723f8a012ec9a24073ee6010e0a2d3342fb63b6/vice/src/vdrive/vdrive-iec.c) writes command-channel OPEN's counted name bytes through `vdrive_iec_write`. `vdrive_iec_flush` executes the collected count |
| Binary P fields and selected record | Same ROMs, valid open REL channel with two-byte records | Respective `romtbl`: `CMDTBL`/`CJUMPL`/`CJUMPH` map P directly to `RECORD`, without filename parsing. [4040 `record`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/record) and [8250 `record`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/record): `R30` copies bytes 2/3 to `RECL`/`RECH`, position 1 becomes offset 0, and `R40`/`R50`/`R60` position the channel and complete the command | `50 62 02 00 01`, `50 62 0D 00 01`, and `50 62 2C 00 01` select records 2, 13, and 44. In the fixture these return `$03,$04`, `$19,$1A`, and `$57,$58`, with EOI only on the second byte. Status is `00, OK,00,00` plus CR for 4040 or `00, OK,00,00,drive` plus CR for DOS 2.7 | Same unit and integration cases compare literal data, status, and EOI. Unit cases also compare the existing secondary-15 route and split collection across service calls | VICE's counted command OPEN/write/flush path corroborates that embedded zero and interior CR/comma bytes are not C-string or filename boundaries. Existing native REL positioning controls remain enabled |
| Missing versus non-REL P target | Same selected drive ROMs | Respective `record`: `FNDRCH` failure invokes `CMDERR` with `NOCHNL`, while `TYPFIL` mismatch invokes it with `MISTYP`. [4040 `erproc`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/erproc) and [8250 `erproc`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/erproc) define `$70` and `$64`, respectively. `ERMOVE` adds an implied space before the FILE token | Missing channel returns `70,NO CHANNEL,00,00` plus CR. An open sequential channel or channel 15 returns `64, FILE TYPE MISMATCH,00,00` plus CR. DOS 2.7 appends the last selected drive before CR. The valid REL channel is unchanged, and a subsequent valid P succeeds | `test_command_channel_errors_and_recovery`, `CommandChannelErrorsAndRecovery` exercise both entry routes, unchanged data, recovery, and status CR/EOI. Firmware cases cover all sixteen model/unit/drive combinations | Native `FNDRCH` in both `tst2` sources maps SA 15 to the existing error channel before type checking, independently of ordinary file channels |
| EOI pending indication and IDLE dispatch | Same selected drive ROMs | Respective `tst2` `L42` stores counted bytes and retains the pointer. `L50`/`L45` increments `CMDWAT` on EOI and continues LISTEN. Respective `ieee` `ATN50` selects LISTEN, otherwise TALK, otherwise IDLE. [4040 `idle`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/idle) `IDLE` and [8250 `idle`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/idle) `VIDLE` clear CMDWAT and call PARSXQ | P2/EOI followed by P3/EOI within one uninterrupted LISTEN selects record 2 (`$03,$04`) after UNLISTEN and ATN release. Immediate TALK, including UNLISTEN/TALK in one ATN phase, serves existing CHNDAT before pending P dispatch. Missing EOI cannot dispatch. UNTALK and ATN release then dispatch exactly once | `test_command_dispatch_waits_for_idle`, `test_command_completion_requires_eoi`, `PendingCommandsDispatchOnlyAtIdle`, `CommandCompletionRequiresReceivedEoi`, ordered ATN-release FIFO tests | MAME RIOT EOI/DAV wiring and Steil's layered TALK/LISTEN account corroborate distinct receive and role transitions. Neither a host flush nor a generic EOI-termination description overrides native IDLE scheduling |
| Counted command length and CR adjustment | Same selected drive ROMs | Respective `tst2` `L42` retains 41 bytes. Respective `parsex` `CMDSET` strips one final CR, otherwise CR plus the following byte when penultimate is CR, before comparing with 41. Respective `erproc` `LONGLN` is `$32` | Forty non-CR bytes are accepted. Forty-one non-CR bytes return `32,SYNTAX ERROR,00,00` plus CR (DOS 2.7 appends drive) without changing REL position. A retained last or penultimate CR permits the adjusted length. A CR beyond retained byte 41 cannot rescue an overlong command. Subsequent valid P succeeds | `test_command_length_boundaries_and_recovery`, `CommandLengthBoundariesPreservePositionAndRecover` cover lengths 40/41/42, both command entry routes, CR variants, exact status, unchanged position and recovery | Steil's DOS interface corroborates the nominal 40-byte command limit, not the ROM-specific overflow/CR algorithm |
| Partial status and terminal-CR acknowledgement | Same selected drive ROMs | [4040 `tst4`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4) and [8250 `tst4`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst4) `GETERC` retains the error cursor and calls ERROFF/ERRTS0 when preparing terminal CR | Queueing without GET preserves the whole error. Fetching a prefix, selecting peer status, then reselecting the original unit returns the unread suffix with CR/EOI. Fetching terminal CR clears the error even if NDAC never acknowledges that byte. Completing an old queued generation cannot clear a newer error | `status_tx_sync` reuses data's bounded modulo-256 progress accounting. `test_status_progress_survives_reselection`, `PartialStatusSurvivesReselectionAndPeerStatus`, and IEEE RTL counter tests cover partial/zero reads, peer selection, wrap, replacement, reset/remount and terminal prefetch/abort | MAME's separate peripheral RAM corroborates per-unit status ownership. The independently observed DAV/NRFD/NDAC path distinguishes GET from queueing and controller acknowledgement |
| Shared ordinary OPEN/command input lifetime | Same selected drive ROMs | Same pinned `tst2` sources: `PUT` masks `ORGSA` with `$8F`, so both ordinary OPEN and channel 15 use `L42`, `CMDCHN`, and the same `BUFTAB+CBPTR`. Respective `open`: `OPEN` calls `CMDSET`. Respective `parsex`: `CMDRST` resets that pointer, and `ENDCMD`/`CLRCB` clears the consumed bytes. `erproc` `CMDER3` also clears them and the pointer on failure | Partial P selecting record 3, UNLISTEN, completed ordinary OPEN on the same unit, fresh EOI-terminated P selecting record 2 returns `$03,$04`, not `$05,$06`. Ordinary OPEN on a different unit leaves the first unit's partial P intact. An unfinished filename also survives UNLISTEN and can complete on a later OPEN with EOI, then dispatch at IDLE | One counted buffer and EOI completion rule in [`receive_states`](../../fw/src/ieee/ieee_drive.c). [`test_open_consumes_shared_command_input`](../../fw/test/ieee_drive_test.c) and `test_open_input_requires_eoi` each cover sixteen model/unit/drive combinations. `DriveRelativeTest.OrdinaryOpenConsumesPartialCommand` covers D64/D80 through production RTL and firmware, with both command entry routes | Pinned MAME's separate controller CPU/RAM per 4040/8050 peripheral corroborates the per-unit storage boundary. Steil's [DOS interface description](https://www.pagetable.com/174) corroborates counted input and command OPEN as an alternate entry, but is not an oracle for retained prefixes or native EOI gating |
| Binary OPEN 15 from an actual PET ROM | PET KERNAL 2.0/4.0 controller, same selected drive targets | [PET 2 `OPENI`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/KERNAL_PET_2.0_REC/ob2src) and [PET 4 `OPENI`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/KERNAL_PET_4.0_REC/ob2src) send the counted filename as OPEN payload, including embedded zero | ROM OPENI transmits `50 62 03 00 01` on OPEN 15 with final-byte EOI. Native ACPTR returns `$05,$06` with status `$00,$40` | `DriveCommandRomTest.NativeOpen15ExecutesBinaryPosition` runs both PET revisions against D64/D80. Independent wire capture checks both OPEN exchanges, command bytes, selected record, and final EOI | The original CPU-driven PIA/VIA cases remain enabled as an independent controller path |

The original routing repair has sixteen failing-before firmware cases and
two failing-before production-top cases. They exercise both command entry
paths and binary zero/CR/comma fields across both media models, all four
devices, and both local drives. The physical-path cases independently check
OPEN wire bytes, consumed SPI/FIFO bytes, status CR/EOI and the selected record.
The error/EOI follow-up has another 32 failing-before firmware cases for
missing/non-REL channel recovery and incomplete-message handling.

The shared-input review reproduced `$05` instead of `$03` in all sixteen
model/unit/drive combinations and both production-top media cases. It adds
peer-OPEN isolation and incomplete ordinary-OPEN controls. The role review
reproduced talker loss in Icarus and all eight new D64/D80 SEQ/REL integration
cases. Those cases now cover all twelve ordered pairs among devices 8-11,
including peer status/data secondary selection and exact unread bytes/EOI.
Firmware and RTL also check cumulative listeners, own-unit role clearing,
default SA restoration and rejection of an unaddressed secondary.

Final validation totals are consolidated under "Current IEEE validation"
below. The real PET-ROM binary command case now uses the same counted
`KernalFixture::file` helper as ordinary OPEN. Its layout-bound views can
reuse the payload and filename-pointer storage after the first ROM call.
The strict semantic RX trace still rejects missing CTRL or repeated STATUS
metadata, rather than inventing an EOI observation.

Independent source cross-checks expose important limits rather than replacing
the selected ROMs. Pinned VICE
[`vdrive_command_position`](https://github.com/VICE-Team/svn-mirror/blob/3723f8a012ec9a24073ee6010e0a2d3342fb63b6/vice/src/vdrive/vdrive-command.c#L3441)
also returns NO CHANNEL for a missing REL target, but conflates an open non-REL
target with that error. The selected ROMs explicitly return MISTYP instead,
so their 64 result takes precedence. MAME's original-drive models
[`c2040_device::riot1_pa_r`](https://github.com/mamedev/mame/blob/7e1ead4a1f40851666d3b428e6624121123f11af/src/devices/bus/ieee488/c2040.cpp#L300)
and
[`c8050_device::riot1_pa_r`](https://github.com/mamedev/mame/blob/7e1ead4a1f40851666d3b428e6624121123f11af/src/devices/bus/ieee488/c8050.cpp#L385)
independently expose EOI on PA5 and DAV on PA6, corroborating the ROMs' separate
EOI sampling rather than inference from UNLISTEN.
Steil's [DOS interface description](https://www.pagetable.com/?p=1038)
corroborates command-channel OPEN as an alternate command entry and the
40-byte counted buffer, but its general EOI-or-UNLISTEN termination statement
does not override the selected ROMs' EOI-gated CMDWAT path. Likewise, its
[layer-3 description](https://www.pagetable.com/173) calls UNLISTEN the name
terminator. The selected `PUT`/`L50`/`L45` path instead gates parsing on EOI
for ordinary OPEN as well as commands.

The pending-dispatch, length and status review added 48 failing-before firmware
instantiations (sixteen coordinates for each defect). Their production-top
counterparts exercise D64/D80, including same-ATN UNLISTEN/TALK and partial
status across peer selection. Status tests also cross counter wrap, replace an
old response generation, reset/remount partial reads, and interrupt terminal CR
after GET but before NDAC.

This is a source-derived command-entry correction, not a claim that every
DOS command is implemented. Tests establish the observable IDLE dispatch
boundary, retained input and status progress, not exact drive-CPU cycle
scheduling. Ordinary filename and command collection share pending dispatch.
REL write commit retains its existing UNLISTEN handling. General DOS syntax
and real-drive analog timing are not established here. D64 and single-sided D80 are
executed. The cited command-dispatch path is shared by the selected full-size
8050/8250 DOS 2.7 source and does not branch on side mode, but no D82 media,
earlier DOS 2.5 revision, 8250LP, or real-hardware capture is claimed.

The historical RED inventory was measured separately: an opt-in seed-1 run
of all 36 disabled probes passed 30 and failed six Waterloo cases (OPEN,
REL OPEN, SCRATCH, RENAME, input, and status). Their shared legacy helper does
not service firmware while ticking the CPU, so those failures are not
attributed to this repair. That baseline did not enable or count historical
cases as fixed. The later assertion/fixture audit described below explicitly
promotes transport assertions and replaces the unserviced REL position probe.
The original sixteen firmware and two production-top
failing-before/passing-after cases are direct evidence for the command-entry
defect, with separate counterexamples for the follow-up corrections above.

### Native filename wildcard lookup

Both selected drive comparators operate on `$A0`-padded 16-byte directory names.
Matching now retains those raw bytes separately from the display-name decoder.
Native literal comparison is case-sensitive, and `?` consumes `$00` just like
any other non-`$A0` byte. The public entry's display name still ends at its first
`$A0` or NUL, but that lossy view no longer controls file selection.
This intentionally removes the old case-insensitive native lookup behavior.
Native `FNDLMT` also limits the pattern to 16 bytes before comparison. Once
those bytes match, later literal or wildcard bytes are ignored, including a
seventeenth `?`. A mismatch within the first 16 bytes or `$A0` padding reached
by `?` still fails. The flat HDD extension does not acquire native truncation.

The 88 D64/D80 table cases cover leading, middle, trailing, and repeated `?`,
exact length, native pattern truncation, padding rejection, the last filename byte, embedded zeros,
case-sensitive literals, PETSCII bytes, existing prefix handling, and `*`
combinations. Two multi-entry cases check near-match rejection, first-match
selection, and returned metadata. Failed lookups preserve every output field.
Firmware-level raw-name tests exercise eight cases across all sixteen
model/device/drive combinations, asserting exact data, EOI, and DOS status.

The flat HDD container remains an extension. It retains case-insensitive
literal matching for its synthetic `OS9 DRIVE A` filename. Empty-drive fallback
is decided by the peer container's type, never the requested name: an HDD peer
cannot supply a REL channel for an empty requested drive. Eight firmware cases
each exercise six literal/wildcard aliases, rejection status, attempted writes,
unchanged backing bytes, and positive explicit-drive opens. Sixteen D64/D80
controls retain ordinary native-image fallback. These fallback controls
preserve an existing EconoPET policy, not a general native-ROM claim.

Before these corrections, the new checks reproduced both raw-byte
counterexamples on D64/D80 and peer-HDD aliasing at all eight device/drive
coordinates. After correction, the complete firmware suite passes 677 checks.
All fifteen disk-fixture tests and six native wildcard controller tests also
pass. These host runs do not constitute real-drive hardware captures.

The six native controller cases execute corrected PET KERNAL 2.0, PET KERNAL 4.0,
or Waterloo revision-12 OPEN and input routines against each media model.
Independent wire observations require the literal `B?S?C` OPEN, its filename
EOI and UNLISTEN, followed by exactly `$A5,$00` with EOI only on the second
data byte. CPU results additionally require PET status `$00,$40` or Waterloo
EOI flags clear/set with no FCB error. Before the correction, 26 lookup cases
and all six native OPEN/read cases failed. None of the 36 historical disabled
probes was attributed to this wildcard defect. Their later audit is recorded below.

This evidence establishes the changed wildcard comparison rule, not general
filename-parser compatibility or execution of an original drive ROM. D80
execution exercises single-sided 8050 media. The shared DOS 2.7 comparator
also establishes the selected full-size 8250 rule, but no D82, earlier DOS 2.5,
or 8250LP execution is claimed.

### Current IEEE validation

| Validation | Current result |
|---|---|
| Complete affected firmware integration suite | All 215 enabled cases pass at seed 1. No historical disabled probes remain. Fourteen focused pending-dispatch, length, status, EOI and native-ROM cases also pass at seed 17 |
| Native wildcard OPEN/read integration, included above | Six D64/D80 cases pass at seed 1, executing PET KERNAL 2.0/4.0 or Waterloo revision-12 OPEN and input routines with exact CPU bytes, status, and independent wire/EOI checks |
| Preserved-talker integration, included above | Eight original D64/D80 SEQ/REL cases retain exact unread bytes and final EOI through repeated UNLISTEN or foreign LISTEN/UNLISTEN. Eight additional cases check every ordered virtual-unit pair with peer status/data secondary selection, without readdressing the talker |
| Sequential terminal integration, included above | Four D64/D80 EOF and failed-successor cases pass, including status acknowledgement, in-place repair, and reopen recovery |
| Native receive integration, included above | All twelve PET KERNAL 2.0/4.0 and Waterloo revision-12 cases pass at seed 1, including native readdressing, real timeout execution, and exact CPU/wire payload checks |
| Native REL integration, included above | Existing data, continuation, interruption, parking, and native receive cases pass alongside channel errors, pending EOI/IDLE dispatch, exact command-length/CR boundaries, status continuation, and PET-ROM commands. All 26 queued-selection/prefetch-boundary cases pass after deferring data-owner release until the ordered RX batch finishes |
| Complete firmware unit suite | 822 checks pass (775 ordinary and 47 fatal-path), including wildcard, alias, channel lifecycle, and REL controls. Each new pending-dispatch, command-length and status-lifetime family covers sixteen model/unit/drive combinations |
| Complete host unit and contract suite | All 124 cases pass. CTRL readback `$08` is RX-head EOI and `$10` is an ATN-release entry. CTRL writes reject both reserved bits |
| Board IEEE, shared hardware contract, and host encoding | All 379 selected seed-1 cases pass (124 host/contract, 215 firmware integration and 40 board IEEE), including literal `$04`, `$08` and `$10` read masks, semantic trace filtering, and exact drain limits that exclude transport-only entries |
| Gateware IEEE simulations | `ieee_tb` and `ieee_sys_tb` pass under both Icarus and Verilator, including all twelve ordered virtual-unit talker/peer pairs, listener/default-SA controls, RX-head EOI, ordered ATN-release retention under full-FIFO backpressure, independent data/status GET counters, modulo wrap and prefetch/abort controls |
| Complete gateware suite | Not rerun in this follow-up. The prior full run identified two `top_tb` configuration failures from pre-existing temporary CRT/keyboard inversions in `top.sv`, which this change does not edit. All four affected IEEE variants pass |
| Production firmware and FPGA bitstream | Production firmware builds successfully with the `RelWithDebInfo` preset. The FPGA bitstream was not rebuilt because the complete gateware suite remains blocked by those unrelated configuration failures. No new bitstream, timing-closure, or hardware-execution claim is made |

Firmware unit and integration tests use `Debug`, with assertion-enabled
optimized model/runtime code kept separate.
Production firmware uses `RelWithDebInfo`. The isolated integration build
excludes unrelated local gateware edits. Its selected production RTL retains
the NRFD-triggered prefetch states and assertions.

## Commodore ROM integration coverage

[`ieee/commodore_rom_test.cpp`](ieee/commodore_rom_test.cpp) executes installed
PET controller ROM code on the production soft 6502, with the external PIA/VIA
models and production FPGA IEEE fabric. It belongs to the isolated firmware
executable. The session mounts an empty D64 to enable the FPGA peripheral, but
does not service C drive logic while the ROM sender runs. Active physical-SPI
FIFO drains check byte/ATN records independently of passive DAV-edge wire
traces, which additionally check EOI. Neither observation substitutes for
executing a Commodore drive ROM.

Set `ECONOPET_MEDIA_DIR` to a media directory containing `roms/`. The complete
`Upgrade` and `Rom4` manifests in [`roms.h`](roms.h) select the corrected
`901465-03` KERNAL and `901465-22` KERNAL, with editors `901447-24` and
`901447-29`. ROM 4 command transport also uses `901465-21`. The
[PET ROM ledger](../../.github/instructions/ieee.instructions.md) records the
binary identities, entry bytes, reconstructed source and calling prerequisites.
The tests load media only during fixture setup or test bodies, so discovery
requires no ROMs. Execution without required media fails explicitly rather
than skipping or substituting synthetic bytes.
The suites reuse `FirmwareTest` ownership and teardown, including `SystemTest`'s
actual simulation-seed reporting and bounded failure diagnostics. Common setup
mounts the D64 once per test and installs the parameterized revision.

The normal suite has 41 enabled cases (seventeen shared contracts for each
revision, two reset-vector fetch cases and five revision-specific transport cases):

| Enabled contract | Primary evidence and prepared state | Required observation |
|---|---|---|
| `ER001`, `SCATN`, `TKATN` | Selected KERNAL IEEE entry bytes and `ob1src` transport paths, explicit CINT and VIA preconditions | Native NRFD/NDAC receive setup and ATN polarity |
| `ACPTR_timeout` | Native receive-timeout recovery, with an explicit stuck external VIA timer-1 interrupt flag | `SATUS=$02` and accumulator CR |
| `TALK`, `LISTN`, `LIST1`, `UNLSN` | Native addressing/termination paths, with device 8 in `FA` | Exact command bytes, ATN and wire EOI |
| `LISTN_SECND`, `TALK_UNTLK`, `TALK_TKSA` | Dependent native calls in one CPU session | Complete ordered primary/secondary or termination transactions |
| `CLSE1`, `CLOS5` | Native CLOSE sender and logical-file lookup/removal, with secondary/file tables prepared after CINT | Exact LISTEN/CLOSE/UNLISTEN framing, plus file-count removal for `CLOS5` |
| `CLALL` | Public vector, one open file and local keyboard/screen defaults prepared after CINT | File count cleared without IEEE traffic |
| Reset-vector fetch | Installed selected ROM's little-endian `$FFFC-$FFFD` vector | A new soft-6502 fetch at that target, not completion of full-machine boot |
| `CHKIN`, `CHKOUT`, `CLRCHN_output` | Native encoded `$60 OR channel` file tables and prepared default devices | Exact TALK/secondary or LISTEN/secondary selection, and UNLISTEN termination |
| Revision-specific `ISOUR`, buffered `CIOUT`, and ROM 4 RECORD transport | Callable IEEE entries and prepared counted command payload | Byte/ATN/EOI equality in independent FIFO and wire observations, including buffered-output phase boundaries |

The historical transport audit enables both revisions' `ISOUR`, buffered
`CIOUT`, `CLRCHN_output`, `CHKIN` and `CHKOUT`, plus ROM 4 RECORD transport.
The two older OPENI probes are retired in favor of stronger native
OPEN/read and binary OPEN-15 regressions. Seven other prepared `TRANS1` cases
(CONCAT, HEADER, COLECT, BACKUP, COPY, RENAME and SCRTCH) are retired because
their different names all exercised the same prepared transport helper, not
those BASIC parsers or filesystem operations. RECORD retains the native five-byte payload
`50 62 34 00 01` for channel 2, record 52, position 1. In pinned ROM 4
[`ob2src`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/KERNAL_PET_4.0_REC/ob2src),
OPEN's `OP100` stores `$60 OR channel` in SAT.
[`ob1src`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/KERNAL_PET_4.0_REC/ob1src)'s
`JZ100` restores it to SA, which
[`dos.write`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/KERNAL_PET_4.0_REC/dos.write)'s
`BOBREC` copies unchanged before the record-number low/high bytes and position.
[`dos.syntax`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/KERNAL_PET_4.0_REC/dos.syntax)
defaults position to 1 and rejects 0 and 255. The embedded zero is in the
record-number high byte, not an invalid zero position.
There are no disabled historical Commodore probes left. Promoted cases have
exact transport assertions, not merely successful ROM returns.

Two parked fixture assumptions are corrected from primary evidence rather
than accepted as emulator failures. The native OPEN path in pinned
[`ob2src`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/KERNAL_PET_2.0_REC/ob2src)
stores `$60 OR channel` in SAT. Prepared `CLOS5`, `CHKIN` and `CHKOUT` file
tables now contain that encoded value rather than raw channel 2.
[`ob4src`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/KERNAL_PET_2.0_REC/ob4src)
sends it directly through TKSA/SECND. ROM 4
[`dos.write`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/KERNAL_PET_4.0_REC/dos.write)
explicitly sends `$6F` in `TRANS1` and jumps to OPENIB, bypassing OPENI's
`$F0` conversion. The retained RECORD transport case therefore expects
secondary `$6F`, not OPEN `$FF`, and checks its full payload and final-byte EOI.

All ROM calls use revision-bound checked programs. ISOUR and CIOUT explicitly
select phased mode before allocation and retain one CPU/IEEE session across
host FIFO drains. CIOUT pauses after its first call and requires an empty FIFO
and unchanged wire trace before allowing the second call. It then checks the
first byte sent on the second call and UNLISTEN's final-byte EOI flush. The
original budgets remain five million startup/run cycles, 100000 cycles per buffered phase,
500000 cycles per FIFO drain/ISOUR resume, and 20000 cycles for reset fetch.
The old manual completion marker, hard-coded harness start and fixed
post-return ticking are replaced by layout-owned completion and bounded
checked execution. File-table/default-device setup now executes after CINT,
so initialization cannot silently invalidate a test's stated preconditions.

```sh
export ECONOPET_MEDIA_DIR=/local/media
cmake --preset default
cmake --build --preset sys-test --parallel 2
ctest --test-dir build/system --parallel 2 --no-tests=error \
  -R '^firmware\.(Pet/CommodoreRomIeee|Pet/KernalReset)'
```

This layer does not establish full BASIC initialization, LOAD/SAVE, successful
ACPTR data reception, full DOS compatibility or Waterloo behavior.

## Waterloo ROM integration coverage

[`ieee/waterloo_rom_test.cpp`](ieee/waterloo_rom_test.cpp) contains eight enabled
firmware integration cases for the native SuperPET revision-12 IEEE leaves.
The suite derives from `FirmwareTest`, retaining per-test seed reporting,
bounded failure diagnostics and session teardown. Each case mounts an empty D64
and constructs `WaterlooFixture` on its fresh board. ROM loading occurs during
execution, not GoogleTest discovery, and missing media fails explicitly.

The complete selected set contains `970018-12` at `$A000-$BFFF`, `970019-12`
at `$C000-$DFFF` and `970034-12` at `$E000-$FFFF`. The respective MD5 identities
are `84cb402449c6107b7d2b636cb28f0042`, `0f9bd7123d99892ce80f1e7e438c2194`
and `c03098d26dbb1a23737d3286f264bc97`.
The production decoder preserves physical I/O at `$E800-$EFFF`.
The [revision-12 evidence ledger](../../.github/instructions/ieee.instructions.md#superpet-waterloo-6809-controller-behavior)
links the pinned binaries and reachable entry paths. Generated disassemblies
are navigation aids, not original source or independent revision authorities.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Native IEEE initialization | Waterloo `970019-12` | [Pinned C-ROM binary](https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/SuperPET/waterloo-c000-dfff.970019-12.bin), IEEEInit `$C188` | Return with VIA ORB equal to the fixture's idle mask `$06` | [`WaterlooFixture`](ieee/fixture.h), [`waterloo_ieee_init_case`](ieee/waterloo_rom_test.cpp) | [Binary-derived C-ROM navigation](https://github.com/prachwal/personal-004/blob/main/docs/pet/disassembly/waterloo-c000-dfff.asm) |
| ATN assertion and release | Waterloo `970019-12`, PIA2 and VIA | Same pinned binary, ATNDown `$C14F` and ATNUp `$C16E` | ATNDown returns with masked VIA ORB `$02` and PIA2 CA2 mode set. ATNUp releases ATN after the explicit `$02` ORB precondition | [`waterloo_atn_down_case` and `waterloo_atn_up_case`](ieee/waterloo_rom_test.cpp) | Reachable callers and archived labels in the [entry ledger](../../.github/instructions/ieee.instructions.md#revision-12-ieee-routine-map) |
| EOI assertion and release | Waterloo `970019-12`, PIA1 CA2 | Same pinned binary, EOIDown `$C176` clears CRA bit 3 and EOIUp `$C17E` sets it | EOIDown returns with C2 mode cleared. The paired calls preserve the intermediate RAM marker and return with C2 mode set | [`waterloo_eoi_cases`](ieee/waterloo_rom_test.cpp) | [Binary-derived C-ROM navigation](https://github.com/prachwal/personal-004/blob/main/docs/pet/disassembly/waterloo-c000-dfff.asm) and the PIA model's control-bit definitions |
| Absent-device output | Waterloo revision-12 A/C ROMs | [Pinned A-ROM binary](https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/SuperPET/waterloo-a000-bfff.970018-12.bin), `$BE6F` branches through `$BE90`; `$BE96` targets `$BEE4`, setting active-block status 3. `$B653` also sets global status | Both block status and global `$006A` equal 3, with no DAV/wire/FIFO bytes. Fetch `$BE90`, not timeout call `$BEE1` | `Waterloo_BE6F_absent_device` runs serviced native code and checks both statuses, instruction fetches and empty transport | Revision-12 ledger and independently observed physical IEEE wire |
| Serviced native REL position and read | Waterloo revision-12 controller, selected 4040 target | Same A-ROM binary, `$BF98` sends `50 62 03 00 00`, with EOI on final position byte, then UNLISTEN. Selected 4040 `record` treats position 0 as byte 1 | Serviced native OPEN, BF98 and input return exactly `$05,$06`, with EOI only on the second byte and no block error. IDLE dispatch occurs after UNLISTEN's ATN release | `Waterloo_BF98_REL_position` checks actual record effects, per-byte EOI, consumed RX and independent wire framing | Selected drive RECORD source plus independent physical-SPI and DAV-edge observations |

The ATN tests observe final register state, not temporal NRFD/NDAC-before-ATN
ordering. The EOI tests inspect PIA control state and return markers, not an
EOI-marked data transfer. Ordinary setup explicitly clears user I/O hooks at
`$E77A` before IEEEInit. The initializer test calls only IEEEInit itself.
See [revision-bound ROM programs](#revision-bound-rom-programs) for hook
preconditions and the separate active native UNTALK wire/FIFO regression.

The historical audit promotes UNLISTEN and UNTALK with exact transport
assertions, strengthens absent-device output with serviced native execution
and exact block/global status 3 (not a timeout-subroutine claim), and replaces
REL position with serviced native OPEN, binary BF98
positioning, and input returning exactly `$05,$06` with EOI only on the second
byte. Each resumed CPU phase explicitly retains its firmware service adapter.
The twelve remaining return-only probes are retired, including OPEN, REL OPEN,
SCRATCH, RENAME, input and status. No verified filesystem-effect assertions
were removed, and this audit does not establish native SCRATCH/RENAME support.
Across Commodore and Waterloo, the 36 old disabled instantiations become
thirteen promoted transport cases, two strengthened/replaced cases, and
twenty-one retired cases. None remains disabled.

```sh
# Build before selecting the nested cases (the root preset selects only its wrapper).
cmake --preset default
cmake --build --preset sys-test --parallel 2
ctest --test-dir build/system --parallel 2 --output-on-failure --no-tests=error \
  -R '^firmware\.WaterlooRomIeee\.'
```

These checks establish focused native leaf behavior in the production-top
digital simulator, not analog bus fidelity, full Waterloo application startup,
general OPEN/read/write, or complete Commodore DOS compatibility. Existing
`gw-boot` benches separately exercise stock 6502 and SuperPET Waterloo boot.

## IEEE controller and protocol evidence

The board-only IEEE layer requires neither firmware integration nor ROM media.
[`IeeeController`](ieee/controller.h) installs a small RAM mailbox loop and
executes every peripheral access on the production soft 6502. The assembler
owns its mailbox, code, stack, and vectors. Construct it on a fresh board before
enabling the virtual drive, and keep that board alive for its borrowed lifetime.
Startup releases DAV, NDAC and EOI, preloads released DIO/ATN/NRFD latches, and
then enables PIA2 PB0-7 and VIA PB1/PB2 outputs through the CPU mailbox.
PIA2 PA0-7, PIA1 PA6 and VIA PB0/PB6/PB7 remain inputs. The synthetic harness
does not configure unrelated keyboard/cassette pins as the full PET ROM does.
The controller cannot be copied or moved. An interrupted mailbox transfer or
handshake permanently marks it failed, rejecting further active work before
patching or clocking. Do not resume it merely because the old command later
acknowledges. Start with a fresh board and controller instead.
Sending waits for both NRFD released (VIA PB6) and NDAC asserted (VIA PB0)
before asserting DAV, then waits for acceptance and rearming.
Receiving asserts NDAC and releases ATN with NRFD asserted before separately
releasing NRFD, following the blocked `TKATN` role transition. After DAV asserts, it
asserts NRFD before sampling EOI and data, acknowledges through NDAC release,
waits for DAV release, and reasserts NDAC. Both acceptor lines remain asserted
between calls, and consecutive bytes do not toggle ATN. This follows PET
KERNAL 2.0's `ACPTR`, including its per-byte rearm rather than restarting TALK.

[`transactions.h`](ieee/transactions.h) separates device addresses, secondary
addresses, and Commodore channels, validating their domains before encoding.
Only the typed factories construct commands, using shared production protocol
constants after validation. Construction-access tests ensure there is no public arbitrary category/byte
factory (including helpers in `detail`). Fixed termination helpers accept no
caller-selected encoding. Unusual wire commands use `raw_record`/`raw_send`
rather than forging a typed `Command`.
Pure transaction, register-domain, record-matcher,
and diagnostic tests in [`transactions_test.cpp`](ieee/transactions_test.cpp)
run in the host-only target without Verilator. Board integration cases live in
[`controller_test.cpp`](ieee/controller_test.cpp).
Named commands and payloads create exact byte/ATN/EOI records. `raw_send` is the
explicit escape for malformed command bytes, but still requires known EOI.
Sequence comparisons check every field and length without board side effects.

[`fifo.h`](ieee/fifo.h) actively samples and acknowledges RX through physical
SPI. RX stores byte, ATN, and EOI sampled at DAV, plus transport-only ordered
ATN-release entries (CTRL read bit 4). The helper consumes those entries
without emitting fictitious byte records. CTRL read bit 3 reports
the head's EOI, while STATUS bit 1 reports its ATN. **STATUS bit 7 still
means talker starvation, not received EOI.** FIFO and passive wire records
can now be compared directly, including EOI.

[`WireTrace`](ieee/trace.h), obtained through `board.trace_ieee()`, passively
records DAV assertion edges at completed-cycle boundaries. It observes
the active-low bus resolved from fabric device signals, fitted PIA/VIA outputs, and
external DAV (VIA PB7), DIO (PIA2 PA), EOI (PIA1 PA6), and ATN (PIA2 CA1)
inputs, independent of RX consumption. GPIO output latches contribute only
where DDR enables output. Initial attachment uses the same resolved DAV as
capture, so an already-active physical transfer is not invented. Overlapping
drivers cannot create a new DAV edge until every driver releases the bus.
Controller ATN, DIO and DAV come only from the fitted peripherals. The fabric's
raw CPU snoops are not additional drivers (they ignore DDR or can retain stale
controller levels). Device-only DAV/DIO exports preserve the virtual talker's
contribution without reintroducing those unqualified controller latches.
The `Snapshot::ieee_bus_*` fields remain fabric-only observations, not the
complete fitted-bus levels. Resolution is observational only (it neither drives
peripheral inputs nor changes the fabric's CPU-data injection).
Its scoped recorder retains source identity, timestamps, and coverage checks.

`rx_empty_now` is one active status sample, not evidence of future silence.
`require_rx_remains_empty` observes both interval endpoints and every completed
cycle without consuming RX. Drains share one inherited deadline across nested
SPI work and enforce explicit record limits. A failed sequence check may
consume the first mismatch while leaving later records pending.

### Compatibility evidence

The controller is a synthetic test harness, not a replacement PET KERNAL.
Its command bytes and three-wire handshake derive from the primary routines
below. Tests use independent literal encodings and the real production bus.
Simulation wrapper outputs expose existing signals without changing production
RTL or physical timing constraints.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| Primary addressing and termination | PET KERNAL 2.0 and CBM 4040 | [KERNAL `TALK`, `LIST1`, `UNTLK`, `UNLSN`](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_2.0_REC/ob1src) and [4040 `DCDE`](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/ieee) | LISTEN `$20+n`, TALK `$40+n`, UNLISTEN `$3F`, UNTALK `$5F` with ATN asserted | [transactions](ieee/transactions.h), `CheckedCommandDomainsAndEncodings` | [Independent bus arbitration description](https://www.pagetable.com/?p=1031) |
| Secondary and OPEN/CLOSE encoding | PET KERNAL 2.0 and CBM 4040 | [KERNAL `OPENI`](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_2.0_REC/ob2src), [KERNAL `CLSE1`](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_2.0_REC/ob3src), and [4040 `DCDE70`](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/ieee) | Secondary `$60+n`, OPEN `$F0+n`, CLOSE `$E0+n` with category-specific address limits | [transactions](ieee/transactions.h), `CheckedCommandDomainsAndEncodings` | [Independent named-channel description](https://www.pagetable.com/?p=1031) |
| Byte handshake and EOI | PET KERNAL 2.0 and CBM 4040 | [KERNAL `ISOUR`, `ACPTR`](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_2.0_REC/ob1src) and [4040 `ATN30`, `LISTEN`](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/ieee) | Assert DAV with stable inverted DIO, accept through NDAC, release DAV before the next byte. EOI is separate wire evidence | [controller](ieee/controller.h), send/receive transaction tests | [Independent three-wire handshake description](https://www.pagetable.com/?p=1023) and [PET pin map](../../docs/dev/PET/ieee.md) |
| Talk-to-data transition and per-byte acceptor rearm | PET KERNAL 2.0 | [KERNAL `TKATN`, `ER001`, `SCATN`, `ACPTR`, `ACP01`, `ACP03`, `ACP05`](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_2.0_REC/ob1src) | Release ATN with NRFD asserted before advertising readiness. Assert NRFD before sampling EOI/DIO, release NDAC to accept, wait for DAV release, then reassert NDAC with NRFD still asserted. No intervening ATN edge between consecutive bytes | [controller](ieee/controller.h), [FPGA and independently timed source regressions](ieee/controller_test.cpp) | [Independent receiver handshake and rearm description](https://www.pagetable.com/?p=1023) and [NI handshake-line definitions (manual mirror)](https://documentation.help/NI-488.2/gpib0cs3.html) |
| Passive physical-source evidence | PET KERNAL 2.0 and PET PIA/VIA wiring | [KERNAL `LIST4`, `SCATN`, `ACPTR`, `ACP01`, `ACP03`](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_2.0_REC/ob1src) drives ATN and samples DAV, EOI and inverted DIO through the fitted registers | Capture separately driven physical bytes and active-low DAV/ATN/EOI transitions through the PET pin map. Compose open-collector levels, honor GPIO direction and never invent an edge or in-flight byte at attachment or driver handoff | [shared signal sampler](framework/observations.h), [wire recorder](ieee/trace.h), [physical-source, signal-transition, GPIO and overlapping-driver regressions](ieee/controller_test.cpp) | [PET pin map](../../docs/dev/PET/ieee.md) and [independent open-collector bus description](https://www.pagetable.com/?p=1023) |
| Physical GPIO initialization and direction | PET KERNAL 4.0 I/O initialization, PET KERNAL 2.0 handshake and PET wiring | [KERNAL 4.0 `CINT`](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_4.0_REC/oa1src) initializes VIA DDRB and PIA2 DDRB before selecting port access, with [register addresses](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_4.0_REC/ossrc). [KERNAL 2.0 `ISOUR`, `ACPTR`](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_2.0_REC/ob1src) drive DIO/ATN/NRFD and read the separate input pins | Enable the harness's DIO and ATN/NRFD outputs with released latches. Partial/input DDR releases disabled outputs despite retained CPU snoops. Preserve external and device contributions | [controller startup](ieee/controller.h), [physical-pin send/receive, CPU DDR, stale-DAV and virtual-source regressions](ieee/controller_test.cpp) | [PET pin map and I/O initialization disassembly](../../docs/dev/PET/ieee.md) |

FIFO layout, bounded draining, and quiet-interval assertions are EconoPET
hardware/test contracts, not Commodore ROM compatibility claims. The existing
[fabric RX and status definitions](../../gw/EconoPET/src/ieee.sv) establish the
byte/ATN/EOI layout and separate talk-starved flag.

## Building fixture tests

Build production with `RelWithDebInfo`. Build tests, simulator integration,
frameworks, and their dependencies with `Debug`, including production sources
compiled for host tests. Root presets apply this split.

Build Verilator-generated models and runtime separately with optimized
`RelWithDebInfo` flags and SV assertions enabled. Keep linked test, framework,
and firmware sources in `Debug`, without model optimization flags or `NDEBUG`.

Use GoogleTest or Check assertions for test expectations. Use `vet()` for
invariants required in production and `assert()` for test-covered paranoid
checks that are intentionally elided from production. Do not undefine `NDEBUG`
or put required work or side effects inside assertions. Host fatal checks use
[`test_fatal.c`](../support/test_fatal.c).
The host C++ framework requires C++23. Firmware and external dependencies keep
their existing language standards.

The root presets require CMake 3.21 or later and build and run this suite,
including the hardware contract test:

```sh
cmake --preset default
cmake --build --preset sys-test
ctest --preset sys
```

Both `all` presets include this suite, so it runs in CI. Root builds place its
generated files in `build/system`. The standalone commands below use
`build/host-fixtures` instead.
The root `system_tests` wrapper has a 30-minute aggregate timeout for the
complete nested suite. Individual host, board and firmware cases retain their
five-minute timeouts, while hardware-contract checks retain their shorter
limits. A stalled case still fails independently rather than consuming the
whole suite budget.

The shared D64 fixtures, checked framework value types, external PIA/VIA models,
and their C++ tests run
without Verilator, a simulated board, ROM media, or firmware transport.
Standalone configuration defaults to `BUILD_BOARD_TESTS=OFF`. Root presets
enable production-top board simulation and host firmware integration together,
requiring Verilator and firmware headers. Revision-bound ROM fixture tests
also require local media at runtime. The board and
firmware tests remain separate executables, keeping firmware globals and
`fopen` wrapping out of board-only tests.

Standalone builds require CMake 3.20 or later.
Initialize the pinned peripheral model dependency before configuring:

```sh
git submodule update --init test/external/chips
```

The hardware contract consistency test requires Icarus Verilog (`iverilog`
and `vvp`). It elaborates only the production constants, not a simulated board.

```sh
cmake -S test/system -B build/host-fixtures -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host-fixtures
ctest --test-dir build/host-fixtures --output-on-failure
```

To include board simulation in a standalone build, configure with
`-DBUILD_BOARD_TESTS=ON`. This also includes host firmware integration. Initialize
its firmware header dependency before configuring a board-enabled build:

```sh
git submodule update --init fw/external/libyaml
```

The SID lookup table is copied into the generated board-test runtime directory,
so tests do not rely on the source working directory. No Pico SDK, Check
library, or ROM media is needed by the firmware target. Firmware and fixture sources
stay in Debug and do not inherit the optimized model's flags. Firmware tests
have the `firmware.` CTest prefix and the `firmware` label.

The firmware Check tests also use the shared C builder. Build and run those
with `cmake --build --preset fw-test` and `ctest --preset fw`.

Keep regression tests in the suite for the feature they exercise. Register
expected-abort tests with the forked runner, using a separate fatal suite in the
same feature test file when its normal suite runs without forking.

## Production-top simulator foundation

[`system.h`](system.h) owns an independent Verilator context, the production
FPGA model, physical SRAM, and fitted PIA/VIA devices. The simulation-only
[`system.sv`](../../gw/EconoPET/sim/system.sv) wraps production `top.sv`
without replacing SPI, Wishbone, address decoding, CPU selection, or video logic.
[`SystemTest`](framework/system_test.h) gives each test a fresh board.

`tick(Cycles)` advances whole 64 MHz system clocks. It resolves CPU/FPGA data
ownership, previews read data, completes peripheral accesses at falling PHI2,
and commits SRAM data when write enable releases. Fitted devices receive the
physical reset, selects and jiffy input, and feed IRQ, text-mode and audio levels
back to the FPGA. The diagnostic switch also drives fitted PIA1 PA7 without
replacing the other port-A input levels. `RawStimulus::audio_det_i` is active-high
(true means a jack is inserted). The wrapper converts it to the production
active-low `audio_det_n_i` pin, which `top` normalizes back to active-high for `main`.
PIA1 follows its isolated keyboard-scanning clock while PIA2
and the VIA follow CPU PHI2. `peek` and `poke` access physical SRAM for fixture setup,
not CPU-visible addresses or the SPI bridge.
The VIA CA2 level reaches the FPGA and snapshots as `text_mode_i`
(high selects upper/lowercase text, low selects uppercase/graphics).

If a tick rejects an access after the FPGA advances, it suspends at the exact
phase rather than rolling state back or rounding the simulator counter.
`clock_faulted()` identifies this state, `half_ticks()` retains its exact
timestamp, and `time()` remains readable as the last completed whole cycle.
Snapshots, SRAM and peripheral inspection remain available. Further ticks
require external reset assertion. `set_external_reset(true)` followed by
`tick(Cycles{1})` cancels the rejected access and completes the retained cycle
(including only its remaining half when interrupted in the high phase).
Remove invalid stimulus before releasing reset. A zero-cycle tick does not
recover the clock.
Reset does not override normal SRAM or FPGA bus resolution, so SPI memory
inspection remains available while either reset driver holds the CPU.
Only an unsupported VIA shift-register read canceled by reset is treated as
undriven data instead of rejecting recovery.

Use `drive_physical_cpu`, external reset/interrupt setters, and
`raw_stimulus(callback)` for pin-level setup. Raw edits commit only after
successful completion and cannot mutate or clock the board during the callback.
`drive_spi` drives only SPI0 pin levels and cannot steal a scoped transaction's
pins. Changes do not evaluate the FPGA until `tick`.
`snapshot()` and `peek_stimulus()` return editable, assignable detached values
that can outlive their board. Fixture-owned inputs have one access path,
`snapshot().stimulus`. Resolved bus data, device feedback, harness inputs and
FPGA outputs remain separate snapshot members. Editing a snapshot does not
edit the board. The board is noncopyable/nonmovable, and borrowed `io()` devices must
not outlive it.

## Observation, deadlines and failures

`observe(callback)` returns a move-only `ObserverSubscription`. Keep it alive
for the interval to observe. Callbacks receive `const System&` after each full
cycle's FPGA evaluation, peripheral updates, SRAM commits and time advancement.
No callback runs for `tick(0)`. Subscribers run in registration order.
Disconnecting a subscription is idempotent and takes effect during dispatch,
including when a callback disconnects itself or a later subscriber. An executing
callback retains its captures until it returns. A subscription may outlive its
board, but does not keep the board alive.

Observers and completion predicates cannot mutate or clock the board through
captured references, register observers, or mutate retained fitted devices.
Read-only snapshots, SRAM/device inspection and diagnostics remain available.
An observer exception propagates unchanged and stops that cycle's dispatch.
The completed cycle is retained and the board remains clockable after the
throwing subscription is removed. This differs from an interrupted hardware
access, which still requires the external-reset recovery described above.

`run_until(predicate, budget, reason)` checks completion immediately, then
after each cycle through the inclusive final boundary. `service_until` uses
the same predicate contract, but passes a mutable board and the remaining
`Cycles` to an active service callback. If service makes no clock progress,
completion is checked again, then one cycle is clocked if still incomplete.
An already complete zero-budget wait succeeds without work. An incomplete
zero-budget wait times out without service or clocks. Oversized clock requests
are rejected before advancing. Nested waits inherit the tighter deadline and
restore the enclosing deadline on every exit. Predicate and service exceptions
propagate unchanged, and guards are restored. Clock health is checked before
every completion predicate. If active service catches a hardware rejection,
it must complete external-reset recovery before completion can be observed.
Merely asserting reset without completing the retained cycle is not recovery.

```cpp
System board;
const auto start = board.time();
unsigned samples = 0;
auto subscription = board.observe([&](const System& observed) {
    EXPECT_EQ(observed.time(), start + econopet::Cycles{++samples});
});
board.run_until([&](const System& observed) {
    return observed.time() == start + econopet::Cycles{7};
}, econopet::Cycles{7}, "seven observed cycles");
subscription.reset();
```

The default simulation seed is 1, or the decimal value of `ECONOPET_SIM_SEED`.
Valid seeds are in `[1, INT_MAX]`. `System(seed)` supplies a per-board override.
Startup is zero-initialized unless `System(seed, System::InitialState::Random)`
is requested. The seed and policy are installed before model construction and
do not alter a peer board's RNG. These are simulation seeds, not FPGA placement
seeds. `SystemTest` records the actual seed in test properties and assertion
traces, and reports diagnostics when a test fails.

`diagnostic(operation, detail)` formats read-only evidence with the completed
cycle, half-cycle phase, CPU selection, seed, startup policy, reset/fault state,
bus address, SPI levels and up to 16 recent fixture/deadline/SRAM events.
Successful raw-stimulus edits record changed fields and their requested values,
including CPU direction/data and indexed spare pins. Unchanged fields, no-op
edits and discarded callbacks do not add history entries.
Timeouts and board-side access rejections attach this evidence while preserving
their standard exception category. `failure<Exception>(operation, detail)`
constructs an exception with the same evidence for later framework layers.
Diagnostics do not clock, dispatch callbacks or acknowledge peripheral reads.

## External PIA/VIA models

[`io.h`](io.h) and [`io.cpp`](io.cpp) adapt the pinned
[`chips`](../external/chips) digital models. The `econopet_external_io` library
builds in Debug, independently of GoogleTest and the FPGA simulator. Its tests
are in [`io_test.cpp`](io_test.cpp).

The submodule requires the [DLehenbauer/chips fork](https://github.com/DLehenbauer/chips),
pinned to [`264ddc2`](https://github.com/DLehenbauer/chips/commit/264ddc2fe7a5127eeb0e87e44af6ca2314818706),
rather than upstream `floooh/chips`. The adapters depend on its:

- MOS 6520 model and `m6520_peek`/`m6522_peek` functions, which preview
  register reads with current input levels without advancing the original
  model or acknowledging its interrupts or handshakes.
- PIA C2 fixes for manual levels, read-A/write-B handshake strobes, one-cycle
  pulses, and preservation of externally driven control-input levels.
- VIA fixes for independent control-input edge sampling, preservation of
  input levels, IRQ summary gating after enable changes, and ignoring IFR
  summary-bit writes when acknowledging source flags.
- VIA timer-load pipeline restart, underflow detection only on actual count
  edges (including maximum loads), and reset suppression of one-shot
  interrupts until the timers are rearmed.
- T2 pulse counting from resolved PB6 levels across DDR/output changes,
  independently of external/output disagreement or the port input latch.

Dependency updates must preserve them and pass the peripheral regressions.
PIA/VIA control and GPIO pin encodings must also match (checked at compile time).

`Pia6520` models DDR/output-latch isolation, mixed GPIO, both control-input
edge polarities, interrupt flags/enables, and fixed/handshake/pulse C2 outputs.
`Via6522` models unlatched GPIO, T1 one-shot/free-running timers, timed or
PB6-counted T2, IFR/IER, control-input interrupts, and fixed/handshake C2
outputs. VIA shift-register accesses, input latching, PB7 timer output, and C2
pulse-output modes throw before changing device state or write observations.
These are digital models, not analog loading or propagation-delay models.
T2 pulse counting observes the resolved PB6 pin: DDR selects the external
input or output latch. External transitions cannot count while PB6 is driven
as an output, and stable levels never generate repeated pulses. Output-latch
and direction changes count only when they produce a resolved falling edge.

Access fitted devices with `pia1()`, `pia2()`, and `via()`. Use named
`PiaRegister`, `ViaRegister`, control flags, and interrupt sources.
PIA control composites such as `PiaC2High` pass directly to `set_control`.
Use `.bits()` only when encoding a raw bus or assembly byte.
Use `ChipSelect::None`, `ChipSelect::Pia1`, `ChipSelect::Pia2`, and
`ChipSelect::Via` directly when constructing `ChipSelects` bus selections.

```cpp
using namespace econopet;
using namespace econopet::io;

Io devices;
auto& pia = devices.pia1();
const CycleTime at{Cycles{0}};

// Configure external inputs and a mixed-direction port.
pia.inputs([](Inputs& levels) { levels.port_a = 0xa5; });
pia.set_control(PiaRegister::ControlA, PiaDdrAccess, at);
write(pia, PiaRegister::PortA, 0x0f, at + Cycles{1});
// Select port access and a fixed high CA2 output, then drive the output latch.
pia.set_control(PiaRegister::ControlA, PiaC2High, at + Cycles{2});
write(pia, PiaRegister::PortA, 0x03, at + Cycles{3});
// Observe without acknowledgment, then complete a read.
const auto presented = pia.peek(PiaRegister::PortA);
const auto completed = read(pia, PiaRegister::PortA, at + Cycles{4});
```

`read(device, reg, at)` and `write(device, reg, data, at)` each advance that
peripheral by one clock, not the board clock. Reads return the presented value
before acknowledging flags or handshakes. `peek` and `peek_inputs` change
nothing. Keep `clock(access)` for timestamped PHI2 bus integration.

`inputs(callback)` edits detached levels and commits only after successful
callback completion and mutation-guard checks. Callback or guard failures
discard the edit. Nested edits and peripheral mutation during a fitted
device's callback are rejected across all fitted devices.

`Pia6520`, `Via6522`, and `Io` are noncopyable and nonmovable. Device references
must not outlive their `Io` owner. Copy `state()` or use `peek_inputs()` for
detached snapshots. Reset preserves externally driven inputs, injected faults,
and write history.

`Io::sample` captures the preceding stable bus sample once at falling PHI2.
`BusSample::pia1_phi2` optionally supplies the isolated PIA1 clock (otherwise
PIA1 follows CPU PHI2). Isolated PIA1 edges do not advance PIA2, the VIA or the
CPU PHI2 clock count.
Repeated high/low samples do not duplicate accesses, deselected devices still
clock their timers, and reset assertion cancels pending accesses even with
overlapping selects or an invalid register. Held reset does not repeatedly
reset the devices. Multiple physical chip selects are rejected when reset is
inactive. `writes().last()` is optional, so no write
is distinct from a write of zero. Completed-write records carry the caller's
full-cycle timestamp, and explicit observation clearing does not reset devices.
The selected access is validated before any peripheral advances. Unsupported
VIA modes or out-of-range registers leave every device, write history, cycle
count, and pending bus sample unchanged. Retrying the rejected falling edge
rejects again without ticking. Present a corrected high sample before completing
the access, or assert reset to cancel it.

`Via6522::Timer1Fault::StuckInterruptFlag` is an explicit synthetic overlay.
It does not modify normal timer state, survives reset, and raises IRQ only
when the corresponding source is enabled. Removing the fault exposes the
normal underlying interrupt flags again.

Run the standalone peripheral checks with:

```sh
ctest --test-dir build/system \
  -R '^system\.External(Types|Pia|Via|Bus)\.' --output-on-failure
```

Owned write publication and real-CPU peripheral-write capture are described
under [Owned traces](#owned-traces). Scheduled GPIO and native CPU/peripheral
IRQ integration are covered by [`scheduling_test.cpp`](scheduling_test.cpp).
[`program_run_test.cpp`](program_run_test.cpp) additionally checks native
write timestamps, GPIO direction/readback, physical PHI2 timer expiry,
CPU-read acknowledgment and timer-driven 6502 IRQ-handler execution without
a synthetic interrupt flag.

## Checked SPI and CPU lifecycle

[`registers.h`](registers.h) defines named FPGA registers, SPI commands,
status flags, PET peripheral addresses and native reset vectors.
`StatusBit::Text` uses `ECONOPET_REG_STATUS_TEXT_MODE_MASK` for the asserted
video-mode bit (clear means graphics).
`spi().read` and `spi().write` use the production SPI/Wishbone path, not a
memory shortcut. Use `fpga::address(SramAddress)` for direct SRAM access
(including the upper bank), or a named `fpga::Register`. Valid but unmapped
Wishbone addresses reach the real decoder and time out rather than being
rejected as out of range. These operations advance simulation time and work
while the CPU is held in reset.

`spi().command(ByteView)` accepts exactly one complete command, rejects
reserved bits and wrong lengths before clocks or pin changes, and returns
the first received byte. Production reads are pipelined. `spi().read`
seeks with `ReadAt`, then clocks the result with `ReadNext`.
`spi().transaction(callback)` holds CS across complete commands. Its callback
receives only `SpiCommands::command`, not raw shifting or release controls.
The scope drains the controller and releases CS before returning.
Void and value results (including owning move-only values) are supported.
References, raw pointers and
`reference_wrapper` results are rejected. Other returned values must not
retain callback-local or board-borrowed state. These restrictions reject common
borrowed forms, not all borrowing types. Spans, views and reference-capturing
callables still require the caller to ensure every referenced object outlives
its use. The API does not prove that a returned value is detached.

```cpp
System board;
const auto address = fpga::address(econopet::SramAddress{0x12345});
board.spi().write(address, 0x42);
EXPECT_EQ(board.spi().read(address), 0x42);

const std::array<uint8_t, 2> write_next{
    fpga::command_byte(fpga::SpiCommand::WriteNext), 0x5a};
board.spi().transaction([&](auto& commands) {
    commands.command(write_next);
});
```

`spi().raw_transaction(half_period)` explicitly permits partial commands,
bit/byte shifting and custom positive timing. Call `finish()` to drain the
controller before releasing CS, or `abort()` for a deliberate reusable release.
Abort does not roll back an admitted write. Default SCK half-periods are two
system cycles, and release settles for four. Readiness waits accept the
inclusive final cycle and inherit enclosing deadlines. No phase is shortened
to fit a deadline.

An unfinished owner or exception escaping acquisition, work, drain or release
fails closed: CS is raised, SCK lowered and ownership released without cleanup
clocks, FPGA evaluation or observer calls. The original exception and timestamp
are retained. `spi().peek_failed()` then rejects further clocks and active
transport/deadline work permanently, even if a service callback catches the
exception. Discard that board and construct another. Passive snapshots, SRAM,
CPU observations and diagnostics remain usable. This transport policy is
distinct from recoverable hardware clock suspension.

`cpu()` offers shared reset, selection/control reads and passive inspection.
Selection reads decode only the CPU field, independently of the SuperPET I/O
mode flag. Selection, preparation and startup preserve that independent flag.
CPU-control reads mask unspecified upper register bits before constructing
typed flags. Raw SPI register reads still return the complete byte.
`cpu(CPU_SOFT_6502)` or `cpu(CPU_SOFT_6809)` binds core-specific lifecycle
operations. `prepare()` asserts reset, drains admitted activity and selects
the core. `write_vector(entry)` requires that bound core quiescent in reset.
`start(entry)` prepares, installs a native-endian vector, holds reset and
releases it. `start()` instead uses an already installed vector.
Startup does not imply program completion.

6502 reset vectors use little-endian bytes at $FFFC, while 6809 vectors use
big-endian bytes at $FFFE. Reset hold is 1,280 system cycles. Release sets READY
for both physical and soft 6502, clears it for soft 6809, and verifies resolved reset.
External reset cannot be overridden. Selection requires quiescent reset, and
stale bound views cannot release another core or overwrite its vector.
Direct vector installation also requires an idle, unowned SPI bus. It drains
outstanding controller work before patching SRAM, then rechecks CPU
quiescence. Raw CS release settles for four cycles before checking both SPI
and admitted Wishbone/SRAM activity. Calling it inside a checked or raw
transaction is rejected before
clocks or memory changes.
Physical-6502 startup requires explicit fixture vector/memory setup, and its
fetch history is unsupported. `write_control_raw(byte)` is the escape hatch
for deliberately unusual control sequences.

```cpp
auto cpu = board.cpu(CPU_SOFT_6502);
cpu.prepare();
// Install native machine-code bytes in physical SRAM before writing the vector.
board.poke(econopet::SramAddress{0x0300}, 0x4c);
board.poke(econopet::SramAddress{0x0301}, 0x00);
board.poke(econopet::SramAddress{0x0302}, 0x03);
cpu.write_vector(econopet::CpuAddress{0x0300});
const auto checkpoint = cpu.peek_fetch_checkpoint();
cpu.release_reset();
board.run_until([&](const System& observed) {
    return observed.cpu(CPU_SOFT_6502).peek_fetched_at(
        econopet::CpuAddress{0x0300}, checkpoint);
}, econopet::Cycles{10000}, "6502 loop fetch");
```

Native address observations are not necessarily opcode fetches. Fetch history
is per-address, monotonically checkpointed and preserved across reset and
selection changes. Soft 6502 records SYNC opcode consumption at its advancing
falling clock edge,
using that edge's READY and pre-advance address. A core held on SYNC does not
advance checkpoints, even as bus arbitration continues.
Soft 6809 records native SYNC (not exact opcode attribution). Inactive
cores and SPI accesses do not create CPU observations. Snapshots expose these
simulation-only native signals without changing production RTL.

SPI/CPU views borrow a persistent board and must not outlive it. Factories
reject temporary boards. Const-board views expose only passive inspection.
Active operations and captured transaction handles remain subject to observer
and input-edit mutation guards.

## Atomic ROM fixtures

`load_rom(path, SramAddress, size)` and `install_rom_images(images)` install
exact-size binary images into physical SRAM. The whole batch is validated and
read into detached buffers before any bytes or fixture ownership change.
Missing files, incorrect sizes, read failures, invalid mappings and overlaps
throw with board diagnostics. Success and failure both leave simulation time
unchanged.

Installation requires asserted CPU reset, a healthy clock, no active SPI
owner, idle boundary pins and no outstanding SPI/Wishbone/SRAM work. After
manual `drive_spi()` edits, first clock at least four release cycles with CS
high and SCK low, then explicitly drain outstanding work. Installation rejects
unsettled transport without silently advancing, resetting or selecting a CPU.
Changing the external reset pin through `set_external_reset()` or
`raw_stimulus()` also requires a completed tick before installation. The last
evaluated reset output alone is not evidence of quiescence after a pin edit.
No-op edits do not invalidate settlement, and `tick(0)` does not establish it.

The default `FixtureOverlap::Reject` disallows replacing fixture-installed
bytes. `FixtureOverlap::Replace` permits replacing earlier fixtures, but never
permits overlapping images within the incoming batch. Adjacent images are
allowed, and all physical SRAM banks are available.

```cpp
system.load_rom("custom.bin", econopet::SramAddress{0xc000}, 4096);
system.load_rom_set(System::RomSet::Upgrade, "/local/media/roms",
                    System::FixtureOverlap::Replace);
```

Supported complete manifests and revision entry metadata are declared in
[`roms.h`](roms.h). Each immutable descriptor binds metadata to static image
arrays. `images(set)` returns a read-only span with static lifetime rather
than allocating a container. The directory-free `load_rom_set(set)` resolves
`$ECONOPET_MEDIA_DIR/roms` only when called, never during test discovery.
Synthetic loader tests require no ROM media. Pure manifest and complete
routine-entry checks in [`rom_catalog_test.cpp`](rom_catalog_test.cpp) run in
the host target with `BUILD_BOARD_TESTS=OFF`, without Verilator. Both supported
Commodore revisions provide CIOUT, so only the revision-specific TRANS1 entry
is optional. Revision-bound fixture tests execute initialization from installed
local ROMs. Full drive transactions are covered by
[`ieee/drive_test.cpp`](ieee/drive_test.cpp).

`selected_rom()` describes the declared base revision, not a content hash.
Complete-set metadata commits atomically with its bytes. A failed installation
preserves the prior selection. Deliberate `poke()` patches and reset preserve
this base identity. Generic installation outside the selected set also
preserves it, while replacement touching any selected-set byte invalidates it.
The const-lvalue accessor borrows optional storage from its board. The rvalue
accessor returns owned optional storage. Revision entries have static lifetime.

## Revision-bound ROM programs

[`ieee/fixture.h`](ieee/fixture.h) supplies concrete `KernalFixture` and
`WaterlooFixture` board fixtures, plus generic `KernalScenario<Memory>` and
`WaterlooScenario<Memory>` for explicit checked layouts. The runnable fixtures
and `RomProgramBuilder<Cpu>` borrow `System`, just like the underlying
`ProgramBuilder<Cpu>`. A builder does not borrow its fixture and can outlive it,
but never its board.

The KERNAL fixture accepts only `Set::Upgrade` or `Set::Rom4`. Waterloo uses
revision 12. An absent selection loads the requested complete set. An existing
matching selection is adopted without reloading or discarding patches. A
foreign selection is rejected without clocks or replacement.

```cpp
ieee_test::KernalFixture rom(system, test_rom::Set::Upgrade);
auto program = rom.program(test_asm::ProgramMode::Phased);
const auto answer = program.byte("answer");
auto ready = program.within(ieee_test::KernalBudget).pause([&](auto& p) {
    rom.init_ieee(p);
    p.pause("IEEE initialized");
    p.lda(test_asm::imm(0x42)).sta(answer);
});
auto complete = program.resume(ready, ieee_test::ByteBudget);
EXPECT_EQ(program.capture(complete)[answer], 0x42);
```

`program()` defaults to complete execution. Select phased execution before
allocation. Both modes have the same two ordinary result bytes at `$0201-$0202`.
Completion uses `$0200` and phased execution separately reserves `$0203` as
private release storage. Ordinary views and allocations cannot expose it.
KERNAL workspace reservations exclude this byte in phased mode. Each ROM family
has one canonical layout consumed by both the explicit scenario and convenience
builder. Code, stack, vectors, data and workspace are all explicit, not inherited
from unrelated builder defaults. The default ROM code region is `$1000-$17FF`,
separate from editor high RAM at `$03FC`. The Waterloo parameter block
and filename occupy `$0100-$017F`, below its native stack at `$0180-$01FF`.
Waterloo's hook flags and pointers at `$0580-$05FF` are also reserved. Ordinary
data allocations cannot overlap them.
These prepared-state harnesses reuse the PET input buffer at `$0200`. They are
not full BASIC/keyboard-input or arbitrary whole-ROM execution layouts.

Code storage is configurable on the fixture constructor and is then shared
by both execution paths:

```cpp
const test_asm::MemoryRegion code{econopet::CpuAddress{0x2000}, 0x0800};
ieee_test::WaterlooFixture rom(system, code);
auto program = rom.program();
```

Routine selection, allocation, assembly, execution, resume and capture check
the selected base revision. Execution also checks identity through a scoped
board observer, including during configured work. A replaced or invalidated
selection rejects stale fixtures and builders. Identity is a declared
revision, not an installation generation or a hash of patched bytes. Existing
CPU lifecycle guards additionally protect phase handles while the host dwells.
Configured runs, polling services and phase resumes use the underlying single
budget/scheduling owner, not a separate ROM execution engine.

There is no implicit ROM initialization. `init_ieee()` explicitly emits the
selected PET editor's CINT call and the harness's IEEE/status preconditions.
For Waterloo it calls the native hook-clear routine `$E77A`, then IEEEInit
`$C188`. The hook dispatcher `$E76B` must see disabled flags, or transport calls
can escape through uninitialized user pointers. `init_ieee_lines()` retains
the focused single IEEEInit call for intentional custom-hook tests. Neither
setup is implicit on construction or resume. `device()` and `file()` emit CPU
instructions, not host memory writes. PET filenames are counted bytes with a
little-endian pointer. Waterloo supplies a big-endian parameter-block pointer
and filename cursor. Its helper copies 1..64 caller-supplied bytes, without
adding a terminator or implementing OPEN. Supply the delimiter and prepared
state required by the chosen ROM routine. Filename lengths are checked before
emission. The default code region supports the full PET 255-byte and Waterloo
64-byte maxima, including explicit native initialization and phase handshakes.
The existing immediate-store encoders remain unchanged. Custom smaller code
regions can still exhaust capacity and receive the standard bounds exception.
These helpers do not validate arbitrary device/channel bytes as disk commands.
Use `WaterlooBudget` for native setup (one million system cycles). The ROM's
128-byte hook-clear loop alone exceeds the former 100000-cycle fixture budget.

Set `ECONOPET_MEDIA_DIR` to a local directory containing the catalog's exact
`roms/` files before running ROM fixture tests. Media is never read during
GoogleTest discovery. Host tests and firmware-session tests need no ROM media.
The fixture tests execute both supported PET editors, Waterloo's hook clear and
IEEEInit, and native Waterloo UNTALK. The latter verifies native hook dispatch
and exact wire/FIFO command `$5F` with ATN asserted and EOI released. These checks
do not establish full disk-drive compatibility.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Callable revision binding | PET KERNAL `901465-03` and `901465-22`, Waterloo revision 12 | [Exact routine ledger](../../.github/instructions/ieee.instructions.md) and pinned installed binaries | CIOUT is available in both PET revisions, TRANS1 only in ROM 4. Wrong revisions are rejected, byte patches preserve declared identity | [`routine`](ieee/fixture.h), [`ResolvesEntriesAndRejectsUnsupportedRoutines`](ieee/fixture_test.cpp) | [PET symbol map](../../docs/dev/PET/memorymap-gen.csv), archived Waterloo labels cited by the ledger |
| Explicit PET initialization and file workspace | PET editor `901447-24` or `901447-29`, corresponding KERNAL | [CINT source](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_4.0_1979-10-23/oa1src), pinned editor bytes and KERNAL FNLEN/FA/SA/FNADR accesses | CINT executes only on request. Harness then sets VIA ORB `$06`, DIO `$FF`, ST zero and literal file parameters with a little-endian pointer | [`init_ieee` and `file`](ieee/fixture.h), [`ExplicitSetupEmitsRealFileAndDeviceInstructions`](ieee/fixture_test.cpp) | [PET symbol map](../../docs/dev/PET/memorymap-gen.csv) identifies `$03FC` as BOB (IEEE timeout defeat), not an IRQ flag |
| Explicit focused initialization | Waterloo `970019-12`, IEEEInit `$C188-$C1C7` | [Binary-derived navigation](https://github.com/prachwal/personal-004/blob/main/docs/pet/disassembly/waterloo-c000-dfff.asm), checked against pinned ROM bytes | `init_ieee_lines` emits one JSR `$C188`, preserves hooks and is never implicit on construction or resume | [`init_ieee_lines`](ieee/fixture.h), [`WaterlooFixtureFocusedSetupHasOneExplicitInitializerCall`](ieee/fixture_test.cpp), [`WaterlooFixtureFocusedInitializerPreservesHooks`](ieee/fixture_test.cpp) | [Waterloo investigation guide](https://github.com/prachwal/personal-004/blob/main/docs/pet/waterloo-investigation.md), IEEE ledger's `sysioini` call site |
| Native I/O hook dispatch and UNTALK | Waterloo revision 12, `970034-12` and `970019-12` | Pinned `$E76B` dispatcher and `$E77A-$E791` clear loop, `$C13C` UNTALK. [E-ROM navigation](https://github.com/prachwal/personal-004/blob/main/docs/pet/disassembly/waterloo-e000-ffff.asm) checked against MD5 `c03098d26dbb1a23737d3286f264bc97` | Clear exactly `$0580-$05FF` before IEEEInit. Native dispatch returns instead of following `$EAEA`, and UNTALK sends `$5F` under ATN without EOI | [`init_ieee` and `waterloo_layout`](ieee/fixture.h), [`WaterlooFixtureNativeHooksPermitRealUntalkTransport`](ieee/fixture_test.cpp) | [Repository ROM ledger](../../.github/instructions/ieee.instructions.md), Waterloo `sysioini` hook-clear call and investigation guide |
| Prepared Waterloo transport fields | Waterloo revision 12, LISTEN `$C072`, OPEN `$BCD0`, REL OPEN `$BD51`, CLOSE `$BD11` | Pinned binaries and [A-ROM](https://github.com/prachwal/personal-004/blob/main/docs/pet/disassembly/waterloo-a000-bfff.asm)/[C-ROM](https://github.com/prachwal/personal-004/blob/main/docs/pet/disassembly/waterloo-c000-dfff.asm) navigation | Active parameter pointer at `$02`, cursor at `$68`, device/channel at block+9/+10. Block+5 is a record-size word, block+12 uses disk mask `$40` (not record length) | [`WaterlooWorkspace` and `file`](ieee/fixture.h), [`WaterlooFixtureProgramOwnsCpuStackAndExplicitFileSetup`](ieee/fixture_test.cpp) | Ledger's parameter-block versus FCB calling contract and Waterloo investigation guide |

## Owned traces

[`framework/trace.h`](framework/trace.h) provides detached source-bound
windows, captures, shared event storage and weak event publication.
[`framework/observations.h`](framework/observations.h) adds scoped board
recorders. No scheduler, assembler, program execution or IEEE wire decoder
is required.

```cpp
auto writes = system.trace_peripheral_writes();
auto video = system.trace_av(test_observation::AvSampling::EveryCycle);
const auto start = system.time();
system.tick(100);
const auto window = system.trace_window(start, system.time());
const auto captured = writes.capture(window);
captured.require_absent("no physical peripheral writes");
```

Windows use `(start, end]` boundaries and a retained identity unique to each
board, not merely matching timestamps. Foreign, reversed, future, early and
stopped-recorder intervals are rejected. An empty covered interval is valid.
`capture()` without a window preserves all retained records, including the
initial A/V sample. Selecting a window excludes that initial sample at its
start and includes its endpoint.

Recorders subscribe to completed-cycle boundaries, do not clock the board,
and are movable but not copyable. Move assignment stops the destination's
previous subscription. `stop()` is idempotent and preserves past evidence.
Event sources prune expired and stopped registrations when attaching new
recorders as well as when emitting, so idle sources do not accumulate stale
subscription storage across repeated recorder lifetimes.
Recorders and detached captures can outlive their board. Const-lvalue access
borrows records and timestamps. Rvalue recorder access copies shared storage
without disrupting active sampling. Rvalue capture access transfers owned
storage and invalidates the source's coverage, as does moving a capture.
An emptied or moved-from capture cannot prove absence.

Coverage is explicit. A skipped observer boundary, detector exception or
event-publication failure cannot turn incomplete evidence into a passing
zero-count assertion. Failed physical I/O sampling invalidates all active
physical-write subscribers, even if a chip write committed before model
publication threw. Reset recovery does not restore the affected recorder's
coverage. Exceptions propagate without replacing detector payloads.
After a detector failure, stop its recorder before continuing. Assertions
include the requested interval. `require_count()` retains repeated events,
including several events at the same cycle, and `require_order()` compares a
projection of the captured values against an exact sequence.
Selecting a window validates live storage before copying and binary-searches
its timestamp range. Both live recorders and detached captures copy only the
requested records, rather than copying all retained history first.

Available board sources are:

- `trace_peripheral_writes()` for completed physical PIA1/PIA2/VIA bus writes
  in dispatch order (canonical register addresses, including mirrored accesses).
- `trace_pia1_writes()`, `trace_pia2_writes()` and `trace_via_writes()` for all
  completed writes to a fitted chip, including direct host model setup.
  Clearing model counters or resetting a chip does not erase subscribed
  evidence. Direct model writes are not physical CPU-bus events. Supply the
  board's current time for timestamp-explicit setup writes.
- `trace_signal(Signal)` for transitions of reset, IRQ, NMI, READY, video,
  horizontal/vertical drive, jiffy, left/right audio, fitted-device IRQs,
  IEEE DAV/ATN/EOI and IEEE RX readiness.
  The initial level establishes a baseline, not an invented transition.
  CPU reset is active-high assertion state. CPU IRQ/NMI retain their FPGA
  output pin polarity, and fitted-device IRQs are active-high assertions.
  `IeeeDav`, `IeeeAtn` and `IeeeEoi` retain active-low electrical polarity
  (true means released). They resolve the same fitted GPIO, external input
  and device drivers as [`trace_ieee()`](ieee/trace.h), rather than relying
  on CPU output snoops. ATN and EOI transitions are observable even without
  a DAV assertion. `IeeeRxReady` is the active-high FPGA FIFO-ready output,
  not a physical bus line or an acknowledgment.
- `trace_av()` for digital video, horizontal/vertical drive, jiffy and stereo
  audio levels. `ChangesOnly` records an initial sample and changes to any
  bundled level. `EveryCycle` additionally samples each completed cycle.
  These are digital boundary samples, not rendered frames or analog audio.
- `trace_events<Event>(detector)` for an observation-only detector returning
  `optional<Event>` at completed-cycle boundaries. Detectors and trace factory
  creation obey the existing observer and input-edit mutation guards.

`EventTrace<Event>` is a manual timestamped log, not a coverage-bearing
recorder. It only appends events, rejects backwards timestamps and exposes
owned records. Coverage-bearing count, order and absence assertions belong
to `TraceCapture`, not the manual log.
`require_stable()` and `require_absent()` check observation-only predicates
initially and at every completed cycle through the inclusive endpoint.
They do not observe pulses between cycle boundaries.

Pure trace storage and model-publication tests run without Verilator in
[`trace_core_test.cpp`](trace_core_test.cpp). Board sampling and lifetime tests
are in [`trace_board_test.cpp`](trace_board_test.cpp). Phase-bound windows
are covered by [`phase_integration_test.cpp`](phase_integration_test.cpp),
and IEEE signal and wire sources by
[`ieee/controller_test.cpp`](ieee/controller_test.cpp). Configured board runs
are described below.

## Deterministic scheduling and configured runs

[`scheduler.h`](scheduler.h) exposes `Scheduler`, a scoped absolute-time
orchestrator. [`framework/run.h`](framework/run.h) supplies the single-use
configuration returned by `system.execution()`. Neither is a second clock
owner. Both borrow a persistent board, and callback captures must remain alive
through execution. Scheduler and configuration owners cannot be copied or moved.
Both classes are concrete `System` collaborators, not generic board interfaces.
The scheduler's non-template implementation is
[`framework/scheduler.cpp`](framework/scheduler.cpp). Explicit registration-order
overrides are private configuration plumbing. Public authors use `at()` and
`every()`, which assign registration order automatically.

```cpp
const auto start = system.time();
Scheduler schedule(system);
schedule.at(start + econopet::Cycles{2}, [](System& board) {
    board.set_external_interrupts(true, false);
});
schedule.every(start + econopet::Cycles{3}, econopet::Cycles{3},
               MissedService::Reject, [](System& board, econopet::Cycles remaining) {
    // Service may use SPI or tick within the shared remaining budget.
    board.tick(std::min(econopet::Cycles{1}, remaining));
});
schedule.run_until([start](const System& board) {
    return board.time() == start + econopet::Cycles{10};
}, 10, "scheduled board run");
```

One scheduler can attach to a board at a time. Destruction detaches pending
work without clocks. Stimulus events are ordered by absolute timestamp, then
registration order, and execute before evaluation of the next full cycle.
This includes cycles advanced by SPI. The stimulus interval is `[start, end)`:
an event at the endpoint stays pending until the next cycle or explicit
service pass. `tick(0)` does not dispatch it.

Stimulus callbacks may change fixture inputs but cannot tick, run active waits,
start SPI, drive SPI pins, transfer/release a captured transaction, or change
and reenter their schedule. Completed-cycle observers still run after hardware
and external device sampling. A stimulus exception retains its payload and
consumes that event without replay. The scheduler then fails closed until it
is destroyed. A pre-evaluation stimulus failure is not a hardware clock fault.
Hardware sampling failures retain the existing exact half-cycle suspension
and external reset recovery contract. Scheduler work rejects a suspended clock,
and boundary stimulus is not replayed when completing an interrupted high phase.

Periodic service is cooperative active work. It only runs when `step()` or
`run_until()` polls it, never recursively during clocks inside an active
service or SPI. Due services are ordered by due timestamp and registration
order. Zero-clock peers run at their shared due time before fallback ticking.
If no work is due, one fallback cycle advances the board.

`MissedService::Reject` reports late work before invoking it, including when
its prior callback overran the next slot. `Coalesce` invokes once at the current
time and advances to the first original-phase slot strictly after completion.
Service receives the earliest inherited deadline's remaining budget, not its
period. Callback errors propagate and make the scheduler unusable rather than
silently retrying partially completed work.
Health is checked after active callbacks and before accepting completion.
Catching a stimulus exception inside service does not repair the failed
scheduler or allow a successful run. This applies to anchored and after-slice
services, and to checked lifecycle operations that catch service failures.
Exceptions from configured service (including after-slice work and rejected
strict deadlines) leave the schedule failed even when the lifecycle catches
them. No subsequent service, completion check or successful return repairs it.
Periodic work is exhausted after its final representable occurrence instead
of failing when its next timestamp would exceed the simulation clock range.

```cpp
auto trace = system.trace_av(test_observation::AvSampling::EveryCycle);
const auto window = system.execution()
    .within(10)
    .after(2, [](System& board) { board.set_external_interrupts(true, false); })
    .every(3, MissedService::Coalesce, [](System&, econopet::Cycles) {})
    .run("configured interval");
trace.capture(window).require_count(10, "one sample per completed cycle");
```

Configuration does not install callbacks or clock until `run()`. `within()`
is required, and zero budget is valid. Without `until()`, execution advances
exactly the elapsed budget. With an observation-only predicate, success is
checked on entry and after each service pass, including the exact deadline.
False at the deadline throws. Terminal execution consumes the configuration,
including failures during setup. No automatic reset, selection, vector
installation or CPU start occurs.

`after()` and phase-anchored `every()` default to `RunAnchor::RunEntry`, measured
from terminal operation entry (not configuration construction). The first
periodic service is one period after its anchor. `RunAnchor::CpuRelease`
requires an explicitly supplied lifecycle and is rejected by board-only
`run()`, even if a core is selected or already running. The shared `execute()`
plumbing supports a later checked lifecycle's one-shot release hook and
preserves author registration order across anchors. This layer tests the hook
with existing CPU controls, without introducing program builders or phases.
Successful `execute()` requires that the hook was called exactly once when a
release lifecycle is declared, even if no release-anchored work was registered.
Missing release throws instead of discarding pending anchored work. Completion
checks preserve void, owning value and reference result types, and do not
replace exceptions escaping the lifecycle operation. A returned reference must
still outlive its use.

The alternate `every(slice, ServiceTiming::AfterSlice, work)` mode advances
a whole slice and then performs active work. Callback duration shifts the next
slice, unlike anchored periodic service. A partial final slice reaches the
deadline without calling work. A complete final slice may invoke zero-budget
work, but that work cannot clock beyond the inherited deadline. Boundary
stimulus is applied before after-slice work. Predicates observe after the
slice and its work, not at intermediate cycle boundaries. Only one after-slice
service is allowed, and it cannot mix with phase-anchored services.
Before any slice clocks, service validates the owning board and requires an
unowned SPI bus. A release-aware lifecycle must call its release hook before
starting after-slice service. Invalid service calls fail without clocking either
board or invoking pending stimulus/work.

The board-only regression suites are
[`scheduling_test.cpp`](scheduling_test.cpp) and
[`configured_run_test.cpp`](configured_run_test.cpp). Program, phase and IEEE
composition are covered by [`program_run_test.cpp`](program_run_test.cpp),
[`phase_integration_test.cpp`](phase_integration_test.cpp) and the
[`IEEE suites`](ieee/README.md).

## Checked layouts and owned memory

[`framework/asm.h`](framework/asm.h) publishes layout primitives
(`test_asm::Scenario`, `Region` and `RegionKind`) and the explicit CPU-specific
assembly APIs described below. Complete program builders and execution use
[`framework/program.h`](framework/program.h) and
[`framework/execution.h`](framework/execution.h).
[`framework/memory.h`](framework/memory.h) supplies lower-bank views, named
byte/word/buffer handles and detached captures.

```cpp
test_asm::Scenario layout(system);
const auto data = layout.reserve("workspace", test_asm::RegionKind::Workspace,
                                 econopet::CpuAddress{0x400}, 4);
auto view = layout.lower_bank(data);
const auto pointer = view.word("pointer", 0, test_asm::ByteOrder::LittleEndian);
const auto payload = view.buffer("payload", 2, 2);
const std::array<uint8_t, 2> bytes{0x42, 0xff};
view.initialize(pointer, econopet::CpuAddress{0x1234});
view.initialize(payload, bytes);
const auto captured = view.capture();
EXPECT_EQ(captured[pointer], 0x1234);
EXPECT_EQ(captured[payload.byte(1)], 0xff);
```

Reservations have a nonempty unique name, explicit purpose and nonzero CPU
extent. Overlap is rejected for every purpose, including contained and enclosing
ranges. Adjacent reservations are valid. A region may end at `$FFFF`, but
`at()` never returns a one-past address. `slice()` narrows an existing
reservation and retains its identity and purpose (it does not reserve an alias).
Whole-span validation precedes backing writes.
`layout.region(name)` looks up any reservation purpose. It does not impose the
ordinary-data restriction of `lower_bank()`.

Layouts borrow persistent backing memory and cannot copy their identity.
Moving a layout transfers its identity and reservations. All source layout
operations then reject. Previously obtained regions, operands and views remain
valid against the destination. Host-only tests use a small memory adapter with
the same `peek`, `poke` and diagnostic contract as `System`. Views borrow that
backing memory, which must outlive them, but retain their layout identity after
the layout itself is destroyed. Views can be copied. Moved-from views reject
handle construction, reads, writes and capture.

`lower_bank()` accepts only Data, Results and Workspace reservations. Code,
Stack, Mailbox and Vectors remain reserved for their dedicated uses. Host
access maps CPU addresses directly to physical SRAM bank zero, regardless of
CPU selection, banking or I/O decoding. It never uses SPI, clocks or simulated
CPU writes. A view rejects foreign layout owners and bytes outside its extent,
including siblings in the same layout. `Scenario::write()` additionally supports
bounded raw installation in any owned region without adding an assembler.
Empty writes at a region's endpoint are allowed, but nonempty writes must fit.
Actual backing mutation still obeys the board's observation guards.

Named operands own their diagnostic names and CPU addresses. Buffer selection,
sub-buffers and complete words must fit. Words explicitly declare little- or
big-endian storage independently of the host or selected CPU. Initializers
reject negative or oversized integers and validate both word bytes or the whole
buffer before mutation. Buffer input must match its exact length.
Bulk reads, capture copies and initialization validate the extent once and
allocate output storage once where needed. Direct address iteration still calls
the backing object's `peek`/`poke` for each byte, retaining board guards without
per-byte operand/name construction. Valid scalar writes do not format failure
diagnostics.

`view.capture()` snapshots the entire view without clocks. Capture reads require
the same layout identity and bytes present in that snapshot. Same-owner bytes
outside the captured view and foreign operands reject. Captures and operands
can outlive both layout and board, and subsequent backing changes do not modify
captured data. Lvalue `raw_bytes()` borrows immutable storage. Mutable temporary
access transfers bytes, immutable temporary access copies them. Moving or
draining a capture invalidates its source for subsequent reads. View captures
are always nonempty, so MemoryCapture has no `empty()` state query.
Complete program builders also support named allocation and selective capture
(see the checked execution section below).

Pure contract tests are in [`layout_test.cpp`](layout_test.cpp), included even
with `BUILD_BOARD_TESTS=OFF`. Board backing, diagnostic and observation-guard
checks are in [`layout_board_test.cpp`](layout_board_test.cpp). Program
memory cases are in [`memory_test.cpp`](memory_test.cpp). Phase memory cases
are in [`phase_integration_test.cpp`](phase_integration_test.cpp).

## CPU-specific assembly and fluent instructions

Explicit `test_asm::mos6502::Program` and `test_asm::mc6809::Program` assemble
into a previously reserved Code region. They borrow a persistent `Scenario`,
which in turn borrows its backing memory. Both must outlive the program.
Assembly and `install()` do not automatically reset or select a CPU, initialize
its stack or execute code. Checked execution is a separate terminal operation.
Cooperative pause/resume is available through the builder's restricted facade,
not through an explicit standalone assembler.

```cpp
test_asm::Scenario layout(system);
const auto code = layout.reserve("code", test_asm::RegionKind::Code,
                                 econopet::CpuAddress{0x300}, 32);
test_asm::mos6502::Program program(layout, code);
const auto subroutine = program.label("subroutine");
const auto stopped = program.label("stopped");
program.sei().ldx(test_asm::imm(0xff)).txs().jsr(subroutine).jmp(stopped);
program.bind(subroutine);
program.lda(test_asm::imm(0x42)).sta(econopet::CpuAddress{0x200}).rts();
program.bind(stopped);
program.jmp(stopped);
program.install();
```

This example demonstrates assembly and installation only. `install()` does not
start the CPU. Authors may use the existing explicit CPU lifecycle themselves,
or use the complete checked startup/completion API below.

Fluent operands distinguish byte immediates (`imm()`), word immediates
(`imm16()`), CPU addresses, 6502 zero-page (`zp()`) and 6809 direct-page offsets
(`dp()`). Checked constructors reject negative values, narrowing and high
address bits. Ordinary CPU addresses always select absolute/extended encoding,
even below `$100`. Direct-page offsets do not set the live 6809 DP register.
Instructions and labels retain their CPU tag, so cross-ISA emission and calls
are compile-time errors. Low-level encoder functions return fixed byte arrays
tagged with their CPU, and accept their declared byte/word scalar types.
Use the checked immediate helpers before narrowing caller-supplied integers.

6502 fluency includes loads/stores, X initialization, stack-pointer transfer,
interrupt control/return, subroutine return/calls, jumps and equality branches.
6809 fluency includes A/B/D/X/S loads, condition-code flags, A/B extended stores,
direct-page X stores, calls/returns, relative branches and indexed clears.
6502 address relocations are little-endian, 6809 address relocations big-endian.
Literal 6809 branch displacements accept exactly `-128..127`, and X-offset
clears accept exactly `-16..15`. `clr_x()` emits `,X`, not an arbitrary postbyte.
`raw_clr_indexed_postbyte()` is deliberately unchecked and may require trailing
bytes supplied by the author.

`label()` creates a named forward reference. Names must be nonempty and unique
per program. `bind()` assigns its current cursor once, and `mark()` creates a
label already bound at that cursor. Foreign and moved-from label handles reject.
The subroutine returns to the jump over its body and then enters the explicit
stopping loop, rather than falling through into a second return.
Labels may outlive their handle's original scope but do not extend layout or
backing lifetime. All created labels (even unused ones) must be bound before
installation. Relative displacement is calculated from the address following
the branch instruction using modular 16-bit PC arithmetic, including branches
across `$FFFF/$0000`, with signed-range checking before any SRAM mutation.
Invalid or wrong-CPU relocation kinds reject before instruction emission.

Programs own their emitted code. Each instruction and compound word/buffer
store checks the entire requested capacity before appending. Full regions,
including the last CPU byte at `$FFFF`, install successfully, but no cursor or
label may refer to the one-past endpoint. `size()` remains valid at capacity.
`raw_bytes()` deliberately bypasses opcode validation, not reservation bounds.
`install()` resolves all references into detached bytes before writing only the
emitted prefix through the layout's lower-bank `poke` operations. Repeated
installation is allowed. Board mutation guards remain active, and invalid
assembly leaves backing memory and simulation time unchanged.

The common `Program<Cpu, Memory>` directly inherits its CPU-specific fluent
surface. Namespace aliases preserve the author-facing names, and shared append
logic delegates to the checked emitter. Mixin ownership/diagnostic helpers are
private implementation plumbing.

Programs cannot copy. Moving a program transfers code and label ownership, and
all source authoring/installation operations reject. Moving the borrowed
layout invalidates the existing program's layout reference rather than silently
rebinding it. Create a new program against the destination layout if needed.

Owned byte loads/stores check layout identity. `store(word, imm16(value))` and
pointer stores emit two real loads/stores using the word's declared byte order,
clobbering A and load-related flags. `store(buffer, bytes)` requires exact input
length. `store_zp()` explicitly selects 6502 zero-page stores and checks both
addresses before emitting anything. These methods emit CPU operations into code
(they do not initialize host data).

[`assembly_test.cpp`](assembly_test.cpp) checks exact encodings, typed surfaces,
labels, signed limits, moves, code bounds, store validation and installation
without a simulator. [`assembly_board_test.cpp`](assembly_board_test.cpp) checks
clock-free lower-bank installation, backing preservation, board evidence and
observation guards. Layout and assembly suites share the simulator-independent
[`host_memory.h`](host_memory.h) test backing.

## Complete checked program execution

Use lvalue-only `system.program6502()` or `system.program6809()` to create a
single-use `ProgramBuilder<Cpu>` backed by `System`. The underlying layout and
assembler remain generic for simulator-independent memory tests.
Direct builder construction enforces the same active-access guard as the
factories, including heap allocation from observation predicates or callbacks.
Construction, reservations and allocation
are clock-free. The board must outlive the builder and its callbacks.

```cpp
auto test = system.program6502();
const auto answer = test.byte("answer");
const auto pointer = test.word("pointer");
const auto input = test.data("input", econopet::CpuAddress{0x400}, 2,
                             test_asm::Capture::Yes);
// Initialize explicit host backing before execution (not a simulated CPU write).
test.lower_bank("input").initialize(input.word(0, test_asm::ByteOrder::LittleEndian),
                                    0x1234);
// Assembly receives a restricted CPU-specific fluent surface.
const auto result = test.within(20000).run([&](auto& p) {
    p.lda(test_asm::imm(0x42)).sta(answer).store(pointer, input.address());
});
EXPECT_EQ(result[answer], 0x42);
EXPECT_EQ(result[pointer], input.address().value());
EXPECT_EQ(result[input], (std::vector<uint8_t>{0x34, 0x12}));
```

`byte()`, `word()` and `buffer()` allocate uniquely named owned operands in the
Results arena, captured by default. `word()` selects the CPU's native byte order.
`data()` reserves external ordinary data, uncaptured by default. Both allocation
surfaces accept `Capture::Yes` or `Capture::No`. `capture()` opts an existing byte,
word or sub-buffer into the result without creating another reservation.
Adjacent, overlapping and duplicate selections are unioned, so a word or buffer
is readable if its complete extent was selected. Holes and foreign owners
reject. Captures sample each selected address once without additional clocks.

`Layout::Pet` defaults to a stack at `$0100` (256 bytes), Results at `$0200`
(255 bytes), a dedicated private completion byte at `$02FF`, Code at `$0300`
(256 bytes), and Vectors at `$FFF0` (16 bytes). `LayoutOverrides` replaces these
extents and adds named Workspace or ordinary Data reservations through
`workspace` and `data`. Predeclared Data is accessed with `lower_bank(name)`,
preserves explicit initialization, and is captured only when selected.
Every extent is nonempty,
nonwrapping and disjoint. Vectors must contain the selected native reset vector.
The 6502 hardware stack must be exactly `$0100-$01FF`. The 6809 stack uses a
representable one-past descending-stack pointer. `lower_bank(name)` admits only
ordinary Data, Results or Workspace, never the private completion mailbox.

`run()` consumes the builder before invoking assembly. The assembly callback
cannot install, run, finish or allocate data through its receiver. Recursive
runs and subsequent builder mutation reject, including after callback failure.
Assembly may explicitly borrow an ordinary workspace for operands. All labels,
relocations, ownership, terminal capacity and vector ownership validate before
the runner changes memory or clocks. Explicit host writes made by user callbacks
are still user-controlled mutation, not rollback transactions.

The runner masks interrupts and initializes the stack (6502 SEI/LDX/TXS,
6809 ORCC/LDS), appends a private completion store and terminal park, then enters
one elapsed deadline covering reset, CPU selection, installation, vector setup,
release and completion polling. It establishes reset quiescence before writing
code, freshly zeros Results, preserves Data initialization and resets the marker
to pending on every start. The native vector is written through the checked CPU
lifecycle, and release is reported exactly once. Zero budget does not clock or
install anything. Completion at the final allowed observation boundary succeeds.
Timeouts, callback errors and model/transport failures propagate without returning
a success-shaped result. Existing board/SPI reset recovery rules still apply.

`after()`, `every()` and `until()` share the standalone configured-run vocabulary,
including RunEntry and CpuRelease anchors and after-slice active work. Startup
counts against RunEntry schedules, whereas CpuRelease schedules and slices start
only after release. `until()` is an additional condition, never a replacement for
mandatory fresh completion. An independent `system.execution()` configuration
can be passed to `run(assemble, configuration)`, but cannot replace a builder's
authored schedule. `run(assemble, service)` adapts polling work through the same
`RunConfiguration::service()` and terminal execution path, with no separate
budget owner. Per-pass service may share stimulus and `until()` conditions, but
cannot be mixed with periodic or after-slice active work. Invalid service
configuration rejects before assembly. Inherited caller deadlines always win.

Both builders and explicit Scenario execution return `ExecutionResult`, which
owns detached selected bytes, `started()`, `completed()` and a
source-bound `window()` covering the complete run. Operands and captured results
can outlive both board and builder. Missing or foreign operands throw, and
`empty()` identifies a valid run with no selected data. Moving invalidates source
data access. Use `trace.capture(result.window())` to assert CPU I/O during the run.
All captures use one sorted, disjoint, adjacency-merged span representation.
`MemoryCapture::raw_bytes()` deliberately preserves contiguous view capture
behavior (borrow from lvalues, transfer from mutable temporaries, copy from const
temporaries). It also accepts a contiguous execution capture or an empty result,
but rejects disjoint selected spans instead of flattening away address gaps.
The CPU remains released in a terminal park loop after successful completion.

Explicit layouts may issue a one-byte Mailbox `completion()` marker, call
`program.finish(marker)`, and execute with `scenario.run(program, [results,]
budget, reason[, service])` or a `RunConfiguration`. Results may be a Results or
Data reservation, or omitted entirely. Explicit authors supply their own
stack/interrupt prologue if needed. `finish()` checks its whole terminal sequence
before appending, and further instruction emission rejects. `install()` alone
remains clock-free and does not execute even a finished program.

[`execution_core_test.cpp`](execution_core_test.cpp) verifies marker ownership,
terminal encoding, exact capacity, moves and post-finish rejection without
Verilator. [`program_test.cpp`](program_test.cpp), [`memory_test.cpp`](memory_test.cpp)
and [`program_run_test.cpp`](program_run_test.cpp) cover both CPUs, private startup,
captured data, deadlines, configured anchors, actual repeated I/O and failures.
ROM-specific runners remain deferred.

## Cooperative pause/resume

Use `builder.pause(assemble)` instead of `run(assemble)` to preassemble a
cooperative program. The same fluent callback may call `p.pause("name")` between
instructions. Every inline pause, its continuation and the terminal `"complete"`
pause is assembled and validated before clocks or runner memory mutation.
Names must be nonempty, unique and different from `"complete"`. Each boundary
uses one unique nonzero byte token, so at most 254 inline pauses plus the terminal
pause are supported.

```cpp
auto test = system.program6809(test_asm::ProgramMode::Phased);
const auto answer = test.word("answer");
// Assemble both continuations before starting the CPU.
auto first = test.within(20000).pause([&](auto& p) {
    p.store(answer, test_asm::imm16(0x1234)).pause("ready");
    p.store(answer, test_asm::imm16(0x5678));
});
const auto initial = test.capture(first);
EXPECT_EQ(initial[answer], 0x1234);
// Service or observe the board while the CPU polls its closed private gate.
system.tick(500);
auto last = test.resume(first, 20000);
EXPECT_TRUE(last.complete());
EXPECT_EQ(test.capture(last)[answer], 0x5678);
EXPECT_EQ(initial[answer], 0x1234);
```

`pause()` uses the complete runner's reset/selection, result initialization,
code/vector installation and native stack prologue. Its one RunConfiguration
owns the full startup-through-acknowledgment budget. The phase emitter saves A
and flags on the native stack, stores its token in the private acknowledgment
mailbox, then repeatedly loads and compares the private release byte. Opening
the gate restores A and flags before the preassembled continuation. Other
registers are untouched and the stack pointer is restored. A stack must have
space for both saved bytes.

Select `ProgramMode::Phased` at construction, before allocating or binding data.
Default phased Results contain 254 bytes (`$0200-$02FD`), with a separate private
release Mailbox at `$02FE` and acknowledgment Mailbox at `$02FF`.
`LayoutOverrides::results` always denotes ordinary data only. Use
`LayoutOverrides::release` to relocate the separate one-byte release mailbox.
Reservations reject overlapping overrides, workspace aliases and data aliases.
Neither host nor assembly `lower_bank()` views expose either mailbox, and data
allocation/capture need no special gate exclusions. Complete builders retain
255 default Results bytes and reject `pause()` without consuming the builder.
The framework resets Results and both mailboxes before release, and never clears
user data between phases. The terminal handshake itself is the permanent park
(no unreachable ordinary completion sequence is appended).

Acknowledgment requires a fresh real selected-CPU SRAM write of the current
token, followed by a real lower-bank read of the still-closed release gate.
That read must follow a fresh opcode fetch of this phase's preassembled gate-load
after the token write, not arbitrary user-code mailbox traffic or a data read of
the handshake's code. The 6502 uses its opcode history. A simulation-only witness
qualifies the pinned 6809 core's first-byte fetch state. Both are exposed through
`CpuView::peek_opcode_fetched_at(address, checkpoint)`, using the existing
monotonic `peek_fetch_checkpoint()`. The existing `peek_fetched_at()` retains
native 6809 SYNC history (SYNC is an instruction wait, not an opcode-fetch marker).
A mailbox value, opcode-fetch guess or SPI read alone cannot issue a capability.
Skipped phase tokens, stale RAM and host-spoofed acknowledgments cannot replace
the expected bus sequence. After successful acknowledgment, the CPU remains
released and polls its gate while the host may advance clocks.

`PhaseHandle` is move-only and bound to the issuing builder and current phase.
`resume(handle, budget)` opens only that phase, consumes its capability and waits
for the next verified closed gate. It does not reset/select the CPU, patch code,
replace vectors or reinitialize ordinary data. The automatically appended terminal
phase cannot be resumed. Zero resume budget does not open the gate or clock.
Exact final-boundary acknowledgment succeeds, and inherited deadlines always win.
Timeout or escaping callback/transport/model failure invalidates the phase run,
so it cannot subsequently yield successful capture or resume.
Shared phase invariants retain the first reset, selection, transport, clock or
mailbox failure cause. Capture and resume report that cause even if the external
state is subsequently restored.
Escaping clock and SPI exceptions sample board faults before falling back to a
generic execution failure, even during startup before CPU monitoring begins.
The original exception still escapes unchanged. Subsequent capture/resume
reports the retained failure rather than masking it with a consumed-handle error.
Data and acknowledgment timestamps freeze on the observer cycle that verifies
the closed gate, not when `pause()` or `resume()` returns. A multi-cycle service
may finish later, and `until()` may deliberately wait longer. Neither extends
the result/phase windows or resamples data modified after acknowledgment.

`pause(assemble, configuration)` and `resume(handle, configuration)` accept fresh
board-owned RunConfigurations. Startup RunEntry anchors precede reset work,
whereas startup CpuRelease anchors begin at checked CPU release. On resume,
RunEntry is resume entry and CpuRelease is the gate opening (not a new CPU reset).
`until()` is an extra observation condition, never a replacement for verified
park. Periodic, after-slice and per-pass services retain their configured-run
semantics. `pause(assemble, service)` and `resume(handle, budget, service)` adapt
polling work through the same configuration path. Invalid foreign, missing-budget
or conflicting configurations reject before releasing a valid capability.
Owned/nonidle SPI transport also rejects before gate opening.

Reset or CPU-selection changes invalidate the monitored lifecycle, even if later
restored. Unclocked external reset changes, failed clocks/transport, altered
acknowledgments and externally opened gates also reject. Detected lifecycle/gate
failure is sticky. Resume and builder construction remain active operations
forbidden from observation callbacks, while frozen phase capture is read-only.
Destroying the builder unsubscribes its monitor without opening the gate.
As with complete execution, builders borrow a persistent board.

`capture(handle)` copies the selected `ExecutionResult` sampled exactly at the
verified acknowledgment (no clocks or later resampling). The result's timestamps
and window cover original startup through that boundary, while the handle's
`window()` covers only the current startup/resume interval, excluding host dwell.
Use `trace.capture(handle)` for per-phase I/O assertions, or
`trace.capture(test.capture(handle))` for cumulative execution evidence.
Detached captures and copied windows outlive the builder and board. Consumed
handles retain their diagnostic names/windows but cannot authorize capture or
another resume.

[`phase_core_test.cpp`](phase_core_test.cpp) checks exact handshake encoding,
native-endian gate addresses and atomic capacity without Verilator.
[`phase_test.cpp`](phase_test.cpp) executes both CPU handshakes and exercises
register/stack preservation, tokens, stale/spoofed acknowledgment rejection,
capability lifetimes, reset invalidation and exact deadlines.
[`phase_integration_test.cpp`](phase_integration_test.cpp) covers owned capture,
configured startup/resume, actual peripheral traces, closed-gate host intervals
and cross-layer failure contracts. ROM fixtures and ROM-specific phases remain
deferred to their consuming layers.

## Shared hardware contract

[`hardware_contract.h`](../../fw/src/hardware_contract.h) owns the
SDK-independent C/C++ hardware encodings used by production firmware.
Corresponding definitions in
[`common_pkg.sv`](../../gw/EconoPET/src/common_pkg.sv) have the same names
without the `ECONOPET_` C namespace prefix and are grouped between
`BEGIN SHARED HARDWARE CONTRACT` and `END SHARED HARDWARE CONTRACT`.
`_WIDTH` counts bits, `_BIT` is a bit index, `_MASK` is a bit mask, and
`_ADDR` is a full byte address. `REG_*` values are indices, and
`WB_*_DECODE_PREFIX` values are decode prefixes (not full addresses).
`WB_*_BASE_ADDR` values are full window addresses.

The C header exports values used by firmware and the host framework, not every
RTL definition. Decode prefixes and other RTL-only construction details stay
in `common_pkg.sv`. The contract covers data/address widths, Wishbone windows,
register addresses, control/status masks, CPU selections, configuration pin levels,
SPI encodings, IEEE register bounds and addresses, and the
IEEE TX burst capacity guaranteed by the room indication. Control-read and
control-write masks are distinct. IEEE status bit 7 means talk starvation,
not RX EOI. Firmware policy such as `CPU_AUTO` is not a hardware value.
Firmware display and keyboard model enums use the shared configuration pin
encodings. The driver's `read_pet_model` stores the decoded status bits directly,
while RTL uses the named CRT level for fixed timings and output polarity.
Keyboard status forwards the raw pin level without interpreting it.

USB keymap files use the same business-first, graphics-second model order,
so firmware indexes them directly with the keyboard model enum. Install the
regenerated keymaps together with the aligned firmware. Older graphics-first
files are incompatible. Startup defaults remain Graphics keyboard and Fixed
display.

Use unsigned literals for every C contract definition, including addresses,
masks, and counts. Keep derivations in RTL, where applicable. The consistency
test checks the compiled C values against elaborated RTL, keeping macro expansion
simple and hardware values easy to inspect without duplicating construction logic.

The video RAM field mask derives from both RTL bit boundaries and is used by
firmware to reject values that would spill outside the field before encoding.
This checks the complete field layout without exporting an unused high-bit index.

`system.HardwareContract.MatchesCommonPackage` compares compiled C values
with independently elaborated SystemVerilog values using a CMake runner.
Discovery uses the GNU/Clang C preprocessor, accepting legal whitespace,
comments, and line continuations. Every exported `ECONOPET_*` macro must be
object-like, use an uppercase name, and contain an unsigned decimal or hexadecimal
literal with a `u` suffix. Unsupported exports fail configuration instead of
escaping coverage. A missing RTL counterpart fails compilation and a changed
value fails CTest. `system.HardwareContract.Discovery` tests these discovery rules.
Changing either file rebuilds the relevant evaluator.

Run through CTest so the CMake runner checks evaluator execution and compares
their complete named output:

```sh
cmake --build build/host-fixtures
ctest --test-dir build/host-fixtures \
  -R '^system\.HardwareContract\.' --output-on-failure
```

The generated `build/host-fixtures/hardware_contract_values.c` and
`build/host-fixtures/hardware_contract_tb.sv` files expose each evaluated
constant for review. The comparison itself is in
[`test.cmake`](hardware_contract/test.cmake).

## Checked framework value types

Include [`types.h`](types.h) for the standalone C++23 types in `econopet`.
Construction and arithmetic checks throw standard exceptions independently
of `NDEBUG`.
They do not depend on `assert`, board state, or a
firmware fatal handler. [`types_test.cpp`](types_test.cpp) tests these contracts
as the independently runnable `BoardTypes` suite:

```sh
ctest --test-dir build/system -R '^system\.BoardTypes\.' --output-on-failure
```

Use `build/host-fixtures` instead for the standalone build. Both suites remain
part of `ctest --preset sys`. Use standard facilities such as `std::span` and
`std::to_underlying` rather than equivalent custom utilities.
Hardware encodings come from the production contract, with independent numeric
expectations alongside the C/RTL consistency tests. Durations and borrowed byte
views are framework utilities, not hardware encodings.

### Address domains

`CpuAddress`, `SramAddress`, and `WishboneAddress` are distinct types with capacities
of 64 KiB, 128 KiB, and 1 MiB respectively. Integer construction is explicit,
rejects negatives and values outside the domain before narrowing, and permits
no implicit conversion between domains or back to integers. Use `value()` at
an encoding boundary.

Adding an integer offset checks both endpoints, including extreme signed and
unsigned inputs. Subtracting two addresses in the same domain returns a signed
displacement. Invalid construction or offsets throw `std::out_of_range` with
the domain name, address, and (for offsets) operand.

The Wishbone domain is the FPGA's byte-addressed bus space, accessed over SPI,
not a separate SPI address space. [`registers.h`](registers.h) defines checked
SPI and peripheral register mappings.

Fixture code can explicitly choose lower-bank SRAM backing with
`SramAddress{cpu_address.value()}`. This copies the CPU address bits, not a
live CPU bank translation or I/O decoder.

### Durations and timestamps

`Cycles` measures full system-clock cycles, not CPU PHI2 cycles. Integer
durations are checked and may convert implicitly for concise duration
arguments. `Maximum` leaves room for two simulation half ticks per cycle.
`half_ticks()` converts without overflow, and `from_half_ticks()` rejects an
incomplete cycle with `std::logic_error`.

Duration addition and multiplication throw `std::overflow_error` on overflow.
Multiplication accepts only integral operands and rejects signed negatives with
`std::out_of_range`, even for a zero duration. Floating-point multipliers do not
compile.
Subtraction throws `std::out_of_range` on underflow. `CycleTime` is an explicit
timestamp constructed from elapsed `Cycles`, not an interchangeable duration
or integer. Add a duration to obtain a deadline, or subtract ordered timestamps
to obtain elapsed cycles. Stream diagnostics include units.

### Named flags and encodings

Use the production `cpu_type_t` from [`driver.h`](../../fw/src/driver.h) and
`pet_video_type_t` and `pet_keyboard_model_t` from
[`system_state.h`](../../fw/src/system_state.h) directly. Hardware CPU-selection
APIs must reject `CPU_AUTO`, which is firmware policy, and reserved values.
USB keymap files use
business-first, graphics-second model order and firmware indexes them directly
with the keyboard model enum.
`std::to_underlying(enum_value)` from `<utility>`
extracts an enum's underlying integer at a hardware boundary, but does not
validate arbitrary enum casts. The production enums have non-fixed underlying
types, so casting integers outside their representable enum ranges is undefined
behavior in C++.

`Flags<Bit>` represents a byte-sized flag domain whose enum declares `All`.
`CpuControl` uses `CpuControlBit::{Ready, Reset, Nmi}`. Combine named bits with
`|`, query individual or composite masks with `contains`, and use `bits()` at a
wire boundary. `from_bits()` and enum construction reject unknown bits and
values exceeding byte capacity with `std::invalid_argument` before narrowing,
even when a wider enum's `All` mask includes high bits. Different flag domains
cannot mix.

### Borrowed bytes

`ByteView` is a checked wrapper around `std::span<const uint8_t>`. It borrows a
byte C array, `std::array`, `std::vector`, or mutable/immutable byte span with
its extent. C arrays, standard arrays, and vectors must be mutable or const
lvalues, including empty standard containers. Both mutable and const rvalue
arrays and vectors are rejected to prevent borrowing storage that expires with
the construction expression. Fixed-extent spans and span subranges are supported, including
temporary span objects whose backing storage remains alive. It deliberately
has no direct raw pointer/count constructor. A span input supplies its own
extent, whose validity and backing storage lifetime remain the caller's
responsibility.

`size()` reports the borrowed extent and indexed reads throw
`std::out_of_range` outside it, including for empty spans and containers.
These checks do not depend on `NDEBUG` (C++23's plain `std::span`
indexing does not provide this exception contract). Keep the source storage
alive and do not resize or otherwise invalidate it while the view is in use.
The view does not own or freeze its bytes, and accepting a span does not extend
the source lifetime.

## Reusable disk-image fixtures

[`d64.h`](../support/d64.h) owns a checked 35-track D64 image independently of
the board, firmware transport, filesystem mocks, and test framework.

Geometry comes from the pure production helpers in
[`diskimage.c`](../../fw/src/ieee/diskimage.c). Image sizes,
sector layout, directory slot sizes, and file types use
[`diskimage.h`](../../fw/src/ieee/diskimage.h). Production geometry is tested
directly against independent ROM-derived zones and fixed byte offsets in
[`diskimage_test.c`](../../fw/test/diskimage_test.c), not only through fixture
round trips. The 8050 D80 checks use the single-sided `NTRK80`/`NSEC80` tables in
[DOS 2.7 ROMTBL](https://github.com/mist64/cbmsrc/blob/master/DOS_8250/romtbl).
Formatting, BAM allocation, file installation, and corruption remain test-only.

Fixture internal invariants use `vet` for consistent fatal diagnostics,
independent of `NDEBUG`. These framework-independent checks reuse the
SDK-independent host fatal implementation in
[`test_fatal.c`](../support/test_fatal.c), which checks expected diagnostics and aborts.

```cpp
#include "d64.h"

using disk_fixture::D64;

auto disk = D64::empty();
disk.prg("BASIC", {0x42, 0x43, 0x44, 0x45});
const auto& image = disk.bytes();
```

Construct `D64{}` or the explicitly named `D64::empty()`. Both produce a
formatted 174848-byte image with BAM, disk name `TEST DISK`, ID `00`, and an
empty directory.

Add files with `prg(name, bytes)`. Each call encodes a closed PRG directory
entry, block count, chain links, exact final-sector length, and BAM allocation.
Payload bytes are preserved exactly (no implicit PRG load address). Zero-byte
payloads have one empty terminal sector, a synthetic fixture representation
rather than the CR that native DOS writes when closing an unwritten file.
`seq(name, bytes)` and `usr(name, bytes)` share this contract, with closed
directory type bytes `$81` and `$83` respectively.

Read immutable storage through `bytes()` for independent assertions or a
consumer that copies the image. On a named lvalue fixture, `bytes()` borrows
storage for the fixture's lifetime. On a mutable rvalue it returns an owned
vector and invalidates the fixture, and on a const rvalue it returns an owned
copy. Fluent file and corruption methods preserve rvalue ownership, so
`D64::empty().prg("HELLO", payload).bytes()` also returns an owned vector.
Keep that vector in a named variable before passing it to `ByteView`.

Copies own independent images and allocation state.
Move construction and assignment explicitly invalidate the source and
empty its bytes. Moved-from fixtures reject further construction or corruption,
including after copying or moving that invalid state. Assigning a valid fixture
restores usability. Self-move assignment leaves the fixture unchanged.

Names contain 1..16 printable ASCII characters, excluding DOS delimiters
`:,=*?"`. The existing pure PETSCII encoder folds letters into uppercase and
maps punctuation (for example `_` becomes `$A4`). Names are padded with `$A0`.
This is an ASCII authoring interface, not the production lookup encoding.
`diskimage_find()` accepts PETSCII names received from IEEE traffic and does not
transcode ASCII punctuation. To query a fixture using its authoring name, call
`d64_fixture_name()`, copy the encoded bytes before the first `$A0` padding byte
into a NUL-terminated query, and pass that PETSCII query to production lookup.
Duplicate encoded names, directory exhaustion (144 files), data exhaustion
(664 sectors), and oversize payloads reject before changing bytes or ownership.
Directory storage uses track 18. Data allocation is deterministic (tracks
17 down to 1, then 19 through 35) but does not emulate drive interleave.
The 21/19/18/17-sector zones follow 4040 DOS 2
[`TRKTBL` and `SECTRK`](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/romtbl).

For deliberately malformed media, build valid files first, then call
`corrupt(byte_offset, bytes)` to replace a checked byte range.
`D64::offset(track, sector)` validates coordinates. Further checked file
installation rejects after corruption, because the metadata can no longer
serve as allocation authority. Empty corruption ranges are no-ops, including
at image end. Consumers may still read the raw bytes, with no implicit repair.

Firmware C unit tests use the same
[`d64_fixture.h`](../support/d64_fixture.h) construction core and diagnostics.
Check every returned diagnostic (`NULL` means success). The C API also permits
explicit checked sector lists for sparse-chain tests. Nonempty payloads that
overlap image storage reject before mutation. Zero-length payloads may alias.
Build ordinary files before applying direct
malformed-media patches. The specialized REL fixture builds a checked sparse
USR chain, then explicitly patches its type and record length for whitebox
tests (no REL side sectors).

D80 and extended/error-table D64 construction are not supported by this builder.

The fixture format rules use these sources (no production protocol change):

| Behavior | Target peripheral/ROM | Primary source and routine | Required fixture result | EconoPET code and test |
|----------|-----------------------|----------------------------|-------------------------|-----------------------|
| Empty directory and BAM | 4040 DOS 2 | [NEW, N110/NEWMAP/USEDTS](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/new), [FRETS](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/frets) | Directory 18/1 ends with `0,$FF`, 18/0 and 18/1 are allocated, free counts agree with bitmaps | [Shared core](../support/d64_fixture.c), `DiskFixture.EmptyIsFormattedWithoutDirectoryEntries` |
| Directory name and closed PRG metadata | 4040 DOS 2 | [ADDFIL AF20/AF25](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/addfil), [TRNAME TN10](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/trnsfr), [CLSDIR CLSD5/CLSD6](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/close) | Type `$82`, 16-byte `$A0` padding, start T/S and little-endian sector count | [Fixture tests](disk_fixture_test.cpp), `PrgOwnsNamePayloadAndDirectoryMetadata` |
| Nonempty final-sector length | 4040 DOS 2 | [CLSWRT CLSW20](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/close) | Final track zero, byte 1 is payload length plus one (255 for 254 bytes) | [Parser round trip](../../fw/test/diskimage_test.c), `test_shared_d64_builder_round_trips_files` |
