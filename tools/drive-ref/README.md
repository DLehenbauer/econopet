# Original-drive reference tools

These tools execute approved Commodore 4040, 8050, and 8250 ROMs in MAME,
capture their observable IEEE-488 behavior, and replay the captured transactions
against EconoPET's host-compiled firmware and production FPGA simulation.
Comparisons retain byte, ATN, EOI, ordering, and final disk-image differences.

This is behavioral reference testing. The sibling [drive-roms](../drive-roms/)
toolkit reconstructs assembly source and verifies ROM bytes instead.

## Components

| File | Role |
| --- | --- |
| `build_drive_reference.py` | Build pinned Debug MAME and record its binary and patch identities |
| `drive_reference_mame.patch` | Instrument composed IEEE nets and enforce fine-grained CPU scheduling |
| `drive_reference.py` | Prepare disposable images, execute original ROMs, and verify captures and persisted writes |
| `drive_reference.lua` | Observe PET I/O, execution phases, completion, and active ROMs inside MAME |
| `drive_wire.py` | Decode accepted transfers, distinguish aborted DAV pulses, and export replay stimulus |
| `compare_drive_reference.py` | Run the simulator replay and compare accepted transfers and final media |
| `drive_vice.py` | Provide supplementary VICE ROM-order, completion-monitor, and c1541 inspection helpers |
| `test_*.py` | Test the toolkit with synthetic inputs, without private ROMs |

The main workflow is build, capture, replay, and compare. VICE helpers support
additional corroboration, but are not a separate required stage in every run.
The C++ replay executable remains in
[`test/system/ieee/reference_replay.cpp`](../../test/system/ieee/reference_replay.cpp)
because it links against the simulator and firmware.

## Prerequisites

Run the commands below from the repository root. Supply:

- Python 3 and Git
- A checkout of the pinned MAME revision and its native build dependencies
- VICE's `c1541` and `petcat` fixture tools
- Approved external drive and PET ROMs matching
  [`drive-emulation-roms.json`](../drive-roms/drive-emulation-roms.json)
- The separate `901467` hardware GCR lookup ROM
- The repository's CMake, C++ and Verilator dependencies for the Debug replay

## Build instrumented MAME

Prepare the pinned source checkout, then build the PET-only Debug executable:

```sh
git clone --filter=blob:none --no-checkout \
  https://github.com/mamedev/mame.git build/drive-mame
git -C build/drive-mame checkout --detach \
  f379cd060ac3b2bd567ebbd0957aed059d6895ff
python3 tools/drive-ref/build_drive_reference.py \
  --source-dir build/drive-mame --jobs 4
```

For an existing checkout, omit the clone. The builder checks the revision,
applies or verifies the exact trace patch, and records the build identity in
`build/drive-mame/petd.json` beside `petd`. It rejects unrelated tracked source
changes, whether staged or unstaged. The approved patch can be unstaged, staged,
or partly staged when rerunning the builder.

## Capture an original-ROM baseline

Replace the external input paths with your approved ROM directories:

```sh
python3 tools/drive-ref/drive_reference.py \
  --model 4040 \
  --manifest tools/drive-roms/drive-emulation-roms.json \
  --drive-rom-dir /path/to/drive-roms \
  --controller-rom-dir /path/to/pet-roms \
  --gcr-rom /path/to/901467.bin \
  --mame build/drive-mame/petd \
  --trace-build-info build/drive-mame/petd.json \
  --output-dir build/drive-reference/4040
```

Use `--model 8050` or `--model 8250` with a fresh output directory for each.
The baseline checks startup and consumed status, ordinary and boundary reads,
interposed status TALK, writes/CLOSE/readback, and reset status.

Outputs include initial and final disk images, bounded PET observations,
controller I/O, composed-net traces, `replay.txt`, and hash-bound `result.json`
evidence. The capture verifies active drive ROMs, exact bytes and EOF status,
complete observations, and the persisted write payload.

Accepted-wire replay requires the instrumented build. Controller-register
observations from a supported stock build alone cannot generate replay stimulus.

## Replay and compare against EconoPET

Configure production and build the Debug simulator using the root presets:

```sh
cmake --preset default
cmake --build --preset sys-test
python3 tools/drive-ref/compare_drive_reference.py \
  --reference-dir build/drive-reference/4040 \
  --output-dir build/drive-comparison/4040 \
  --read-only
```

The default replay executable is
`build/system/board/econopet_drive_reference`. Use `--replay-tool` to select
another build explicitly.

`--read-only` selects the exact captured prefix before the first write.
Use `--ordinary-files` instead to include write/CLOSE/readback without UJ,
or omit both flags for the full baseline. Each run needs a fresh output
directory.

The comparator retains wire observations, received bytes, replay coverage,
final media, logs, and `comparison.json`. CPU receive records must match the
selected replay's receive count, action identity, byte, and EOI independently
of composed-wire agreement. Receive mismatches are retained in
`received_differences` and fail the comparison. Mismatches, incomplete replay, and
invalid or aborted wire observations fail the comparison. A legacy firmware
limitation is not treated as an equivalence exemption.

## Test the toolkit

Run all four synthetic test modules without external ROMs:

```sh
python3 -m unittest discover -s tools/drive-ref -p 'test_*.py' -v
```

The build, capture, and comparison entry points also accept `--help`.

## Evidence limits

Passing comparisons establish only the selected observations and coverage.
They do not establish physical timing, storage-fault, or complete drive parity.
Reported handshake-phase times are unadjusted simulation diagnostics.
Host filesystem calls omit physical SD/RP2040 latency, completed-cycle FPGA
samples do not resolve delta-cycle edges, and emulator scheduling is not
hardware acceptance.

VICE's C-based FDC provides supplementary checks, not original controller-ROM
execution. See the [IEEE integration guide](../../test/system/ieee/README.md)
for the simulator workflow and links to its contracts.
