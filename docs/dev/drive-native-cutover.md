# Native facade, presence and acceptance join

This larger dependency batch builds on final reviewed and merged #130
(`e999fe62d6ca428e824692a9007096f7fa25bcf5`).
The native DOS engine becomes the only production IEEE drive engine. Its
qualified ordinary and REL operations are routed through the production
SPI/fabric interface, with no legacy fallback.

The firmware and FPGA ABI change together. CTRL bits 4-7 select devices 8-11.
CTRL bit 0 is reserved and ignored (it is no longer an enable bit).
Every CTRL write replaces that selection, including FIFO flush requests.
Readback bit 0 reports whether any unit is selected. With no selected units,
physical IEEE I/O is transparent. With any selected unit, interception is
global (this is not per-address physical/emulated passthrough).

## Ownership and lifecycle

DOS owns channels, collected input, sector pages, status and retry state.
The facade owns FILE handles, presence, bus roles and the global data/status
staging suffixes. GET counters commit only the actual consumed prefix.
Pause, reconcile and discard preserve interrupted cursors and peer owners.
Queued commands cannot overtake cooperative DOS work or an active DAV.

Mount validates the complete proposed pair before replacing either slot.
Failed finalization retains the old media and retry state. Successful unmount
first detaches media, closes its FILEs and resets DOS to `DOS_MODEL_NONE`
before transport reset. This avoids finalizing cached BAM references belonging
to a detached generation. Configuration loading halts the CPU and requires
successful unmount before applying setup actions in their declared order.

Presence withdrawal retires old roles before a coincident accepted LISTEN or
TALK can select a retained peer. Secondary/OPEN/CLOSE commands require the
addressed unit to remain present, so withdrawal cannot flush a peer's response.
Completed transport events retain their historical owners.

Qualification also exposed a narrow inherited DOS error-path defect. Named
COPY selected the output drive before rejecting an existing target. Native
`rename:CY` calls `CHKIO` before that selection, so error 63 must retain
LOOKUP's final drive. The batch restores that drive only in the collision arm
and adds six firmware cases (both copy directions across all selected models).
All reviewed #130 DOS tests and other production tokens are retained, including
the command-input ownership, retained REL publication and COPY fixes added
during review.

## Simulator and reference migration

The system tests compile the real DOS engine and facade in Debug and drive
the production FPGA top through SPI and PIA/VIA handshakes. Controller and
passive wire observations remain independent. Firmware service runs during
cooperative command settlement and between received bytes.

If teardown encounters an observer/deadline exception, host-only non-SPI
cleanup discards the failed simulator's dirty DOS/media state and closes its
FILE borrowers before unregistering fixture storage. The original exception
is rethrown. Rejected teardown preconditions remain retryable and production
hardware still requires durable unmount. An unpublished mount candidate uses
GCC cleanup ownership, including C exception unwinding in host builds.

Legacy whole-file FIFO assumptions are replaced with native bounded GET
progress. Public REL acceptance constructs valid side/super-sector metadata
through native creation, then checks sparse positioning, retained peers,
overflow, COPY, CLOSE-all, remount, SCRATCH and VALIDATE against physical
backing bytes. Invalid REL fixtures are not accepted merely because their
directory entry says REL.

Reference tools cover disposable-image ROM scenarios and retain exact
observed bytes, EOI, status, media and trace prefixes. Replay validates the
entire input before mounting, services firmware during receive progress and
supports a separately owned source disk. Tool unit tests are not substitutes
for fresh ROM comparisons or board measurements.

## ROM evidence

Targets remain selected 4040 DOS 2.0 revision 2 (901468-14/-15/-16, approved
overlays), full-size single-sided 8050 and double-sided 8250 DOS 2.7
(901887-01/901888-01). Controller evidence uses corrected PET KERNAL 2.0
901465-03 and PET KERNAL 4 901465-22, not uncorrected early ROM 2.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| Address selection and roles | Both selected DOS families | [4040 IEEE], [8250 IEEE]: DCDE20/DCDE40/DCDE60/DCDE70, ATN50/ATN60 | LISTEN/TALK selects only its unit. Secondary commands require a valid primary. UNTALK does not free a channel | `command`, unit-mask RTL and both IEEE benches, multi-unit production-top tests | [MAME 4040], [MAME 8050] device/address wiring, [VICE RIOT] drivenumberjumper and handshake |
| GET versus presentation | Both selected DOS families | [4040 IEEE], [8250 IEEE]: TLK10/TLK20/TLK30/TLK35 | GET occurs at NRFD progress, before NDAC completion. TLK20 can return to IDLE without GET | `sync_queue`, `fill_tx`, paused-source and interruption tests | [MAME 4040], [MAME 8050] open-collector composition and [VICE RIOT] RFDO/DACO |
| LISTEN and collected EOI | Both selected DOS families | [4040 IEEE], [8250 IEEE]: LSN30/LSN40, [4040 PUT], [8250 PUT]: PUT/L45 | Capture DATA and sampled EOI. Deliver each actual byte once. Dispatch follows ordered collection | `ieee_drive_task`, `dispatch`, binary RECORD and public REL wire tests | [VICE RIOT] samples EOI/DAV and logical DIO, [MAME 4040] port mapping |
| Host teardown boundary | Both selected DOS families | [4040 CLOSE], [8250 CLOSE]: CLSCHN/CLSREL/CLSDIR | Native CLOSE persists owned metadata. Host unmount must not leave callbacks or retry state referring to detached generations | `ieee_drive_unmount_all`, failed-write lifecycle and remount regressions | Host FILE ownership/generation policy is an explicit adaptation, not a ROM operation |
| Controller read and termination | Corrected KERNAL 2.0, KERNAL 4 | [PET2 IEEE], [PET4 IEEE]: TKSA/TKATN/ACPTR, CIOUT/UNLSN/UNTLK, GETIN/BN30 | Complement DIO, sample EOI, preserve each ROM's termination order and status behavior. GETIN clears ST before input | migrated controller/ROM tests and GETIN fixture entry | [MAME 4040], [VICE RIOT] independently corroborate bus polarity |
| Public REL acceptance | Selected 4040, 8050/8250 | [public REL evidence](drive-rel-integration.md), [storage evidence](drive-rel-storage.md) | Native create/RECORD/LISTEN/COPY and CLOSE persist valid REL structures and model-specific status | `NativeRelPublicCreatePositionCopyAndRemount`, `NativeRelOverflowCloseAllAndPersistedBytes` | Pinned VICE REL/MAME sources in the dependency evidence, not fixture encoding as oracle |
| Named COPY collision drive | Both selected DOS families | [4040 COPY], [8250 COPY]: COPY/CY/CHKIO, [4040 lookup], [8250 lookup]: OPTSCH/LOOKUP, [8250 status]: ERMSG2 | Error 63 retains the final lookup drive, before CY selects the output drive. DOS 2.7 exposes that drive in its status suffix | `copy_lookup_finish`, six `test_copy_cross_drive_collision_retains_lookup_drive` cases and production `PatternCopyRetainsPeersAndPersistsOnlySelectedFiles` | Source-derived dual-drive evidence and actual SPI/fabric observations, not a new full-drive ROM comparison |
| Emulation presence | Host extension supporting both families | [4040 IEEE], [8250 IEEE]: DCDE compares native hardware addresses | Only configured virtual units answer. Zero selection restores global physical transparency. Flushes preserve selected peers | CTRL contract, `control`, presence/flush tests | Native address comparison is primary. The mask/register policy itself has no original ROM entry |

[4040 IEEE]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ieee
[8250 IEEE]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ieee
[4040 PUT]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
[8250 PUT]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
[4040 CLOSE]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/close
[8250 CLOSE]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/close
[8250 COPY]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rename
[4040 COPY]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rename
[4040 lookup]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/lookup
[8250 lookup]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/lookup
[8250 status]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/erproc
[PET2 IEEE]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/KERNAL_PET_2.0_REC/ob1src
[PET4 IEEE]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/KERNAL_PET_4.0_REC/ob1src
[MAME 4040]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c2040.cpp
[MAME 8050]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c8050.cpp
[VICE RIOT]: https://github.com/VICE-Team/svn-mirror/blob/a0e14df25846cd7f06043f24b48677bf45c02149/vice/src/drive/ieee/riot2d.c

## Qualification boundary

Firmware/framework/system sources retain Debug assertions. Production ARM
firmware uses RelWithDebInfo. Verilated models and runtime remain separately
optimized with assertions preserved. Formatting preserves literals (including
C++ digit separators) and executable tokens.

Both IEEE benches run under Icarus and Verilator. The actual system build's
CTest registry is authoritative, not a successful run that discovers no tests.
Detailed command logs, counts and exact reconstruction receipts accompany the
staged snapshot privately.

The original publication baseline (before PR feedback fixes) was:

| Check | Result |
| --- | --- |
| Configured Debug firmware | 5,756 cases (5,703 ordinary and 53 fatal/assert), all pass over final reviewed #130 |
| Actual system registry | All 717 cases pass in a fresh complete run over final reviewed #130, including all 18 COPY cases |
| Reference tooling | 165 tests pass |
| IEEE benches | Four Icarus/Verilator variants and their setup fixture pass |
| Production ARM and ROM | Fresh builds pass over final reviewed #130 |
| Production FPGA | Prior qualified C3 map/interface/PNR/programming/export results reused only after proving all tracked gateware and timing-tool inputs identical |
| C3 timed paths | Worst reported setup +1.021 ns and hold +0.086 ns |

The earlier immutable-publication qualification (5,588 firmware cases and a
717-case system run followed by the COPY correction rerun) remains archived
separately. It is not substituted for the complete final-main run above.

The PR feedback fixes were qualified separately:

| Check | Result |
| --- | --- |
| Debug firmware | All 5,756 cases pass |
| Actual system registry | All 720 cases pass across disjoint 601-case and 119-case batches, including three added lifecycle regressions |
| Gateware | All 64 entries pass across the full run and corrected IEEE reruns, including 144 coincident-withdrawal cases |
| Production ARM and ROM | Fresh builds pass, with simulator-only discard absent from the ARM object |
| Production FPGA | Fresh C3 map/interface/PNR/programming/export pass with unchanged SDC and flow inputs |
| C3 timed paths | Worst reported setup +0.299 ns and hold +0.086 ns, timing checker passes |

The original half-hour root system wrapper timed out locally after 601
passing cases and on original-head CI after 607 passing cases, without test
assertion failures. The wrapper now allows one hour for native cooperative
SPI workloads, retaining its two-worker resource bound and individual test
deadlines. The generated wrapper properties were checked. A complete run
through the new wrapper remains for new-head CI, rather than being inferred
from the split local results. Reference tooling was not rerun for these fixes.

STA coverage is not complete. The unchanged MC6809 paths retain two
unclocked pins and two unconstrained NMI-latch endpoints. The unchanged
`pia1_clock_o` output lacks an output delay. Eight CPU data inputs appear in
the no-input-delay list but retain the SDC's explicit one-period max-delay
bound. These CPU/PIA gaps are recorded, not silently waived or repaired in
this batch. Positive timed-path slack does not establish complete STA.

No future DOS rewrites, legacy diskimage cleanup, timing/SDC edits, Interface
Designer changes, test-only top pin inversions, unrelated display/CPU work,
private acceptance packaging or private ROM bytes are included. The reviewed
`block_start` still rejects genuine REL block editing. This is software/fabric
qualification, not fresh board/electrical acceptance or complete ROM parity.
