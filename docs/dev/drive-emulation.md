# ROM-structured Commodore drive DOS replacement plan

## Objective and agent execution rules

EconoPET needs faithful 4040, 8050, and 8250 behavior beyond its current
loader-oriented disk emulation subset.

Replace the firmware DOS implementation incrementally with C structured around
the selected Commodore ROMs' state and algorithms. Use an independent host-side
reference executing original drive ROMs to establish and compare behavior.
Do not perform a literal instruction-by-instruction translation or add a
production RP2040 6502 emulator.

A C port does not itself guarantee correctness. Acceptance requires comparison
with the selected ROMs, end-to-end tests, and hardware checks. The FPGA transport
remains a separate correctness boundary.

This document is an implementation plan, not a claim of completed compatibility.

Implement one milestone at a time. Do not treat this document as permission to
complete the entire replacement unattended.

- Read [repository guidance](../../AGENTS.md), scoped instructions, and the
  [IEEE compatibility requirements](../../.github/instructions/ieee.instructions.md)
  before changing implementation. This plan adds scope and acceptance gates,
  not an alternative compatibility standard.
- Start with M0. Propose exact executable drive ROM sets and obtain operator
  approval before replacement implementation. Pinned source links alone do not
  identify the ROM binaries.
- At every milestone, supply executable hardware-test instructions and pause.
  Incorporate operator results, fix failures, and repeat the relevant host and
  hardware checks until the milestone passes. Do not start its successor while
  approval or hardware results are pending.
- Stop the affected work when primary evidence, distribution terms, oracle
  capability, or a required design decision is unresolved. Report the specific
  blocker rather than substituting plausible DOS behavior.
- Preserve unrelated user changes and read-only input snapshots. Never run
  destructive image tests against the operator's only copy of a disk.

### Workspace baseline

The initial workspace is branch `ieee2` in `/workspaces/econopet.ieee` at
`c843b975847e2b34449e5ac085a6633f32ec3935`. These are starting facts, not
instructions to reset or recreate the worktree. Record the actual revision and
dirty state at each milestone. Do not import changes from another worktree or
modify the pre-existing `ieee` branch without approval.

## Confirmed MVP scope

- Target the 4040, 8050, and 8250.
- Include full native disk-image DOS operations: sequential reads/writes,
  SAVE, creation, append/replace where supported, directories, scratch, rename,
  copy, validate, format, REL creation/growth, and block commands.
- Support D64, D80, and D82 images.
- Require matching disk types within each unit: D64/D64 uses 4040, D80/D80 uses
  8050, and D82/D82 uses 8250. A single mounted disk in either slot selects that
  unit's model. Different units may use different models simultaneously.
- Reject mixed-type pairs in the MVP. Hybrid units are a next-iteration feature,
  not an optional fallback or hidden development mode.
- Preserve device addresses 8-11 and two media slots per unit.
- Exclude uploaded custom drive code and memory commands, including M-R/M-W.
- Preserve Waterloo revision 12 and Super-OS/9 workflows on native supported
  images, including native REL support.

### Mount and model-selection contract

These host-driven rules are EconoPET policy, not claims about native media
changes:

| Situation | Required result |
|-----------|-----------------|
| First mount into an empty unit | Select the model from the validated image type, regardless of slot number. |
| Mount or replace with a peer disk present | Require the candidate's type to match the peer. Reject a mismatch before changing mounts, DOS/channel state, or persisted configuration. |
| Request explicitly targets an empty slot | Use the unit model's native missing-media behavior. Never redirect the operation to the populated slot. |
| Both slots empty | The virtual unit is absent from IEEE addressing, including when another virtual unit is populated. |
| Omitted drive number | Follow the selected ROM's command-specific rules, not a universal disk-0 fallback. |
| Successful mount, replacement, or removal | Quiesce the affected unit, finalize pending writes, change media, then reset that unit's DOS/channels and recalculate its model. Preserve other units' state. |
| Pending-write finalization or flush fails | Keep the old mount and necessary pending state, report failure, and do not publish the candidate or report a successful media change. |

Validate a complete configured pair before applying it, so configuration order
does not decide which half of an invalid pair survives. Cover direct mount,
configuration loading, menu actions, unmount, and bulk teardown. Distinguish
host-driven media changes from native INITIALIZE, DOS reset, PET reset, and
IFC. Derive native reset effects from ROM/hardware evidence rather than applying
the host media-change flush policy to every reset.

### Future hybrid boundary (not MVP work)

Retain explicit backing-slot/profile ownership for channels and pending jobs.
Keep model-specific algorithms separate from pair validation and routing.
The next iteration is intended to use independent per-disk buffer pools with
one unit-wide channel namespace. Do not implement those pools, mixed-model
copy rules, or hybrid status composition now. Homogeneous MVP units must retain
their selected ROM's native shared resource limits, not double them by
allocating a full drive's buffers to each slot.

## Compatibility boundaries and evidence

Propose one exact ROM set and hardware mode for each target in M0. The
8050/8250 candidate is shared DOS 2.7 with single-/double-sided hardware modes.
Earlier 8050 DOS 2.5 and 8250LP are outside scope unless explicitly approved.
Identify both the main DOS and disk-controller ROMs required by the oracle.

Target externally observable DOS and IEEE behavior over sector images.
D64/D80/D82 do not capture arbitrary flux, rotational layout, or every physical
media fault. This is not complete physical-drive emulation or a promise of
cycle-exact mechanics. Preserve native handshake ordering, readiness,
cancellation, and EOI, and measure latency against controller polling limits.

Preserve native quirks within scope, including EOF, resource limits, status
consumption, and parser behavior. Define and test handling of excluded commands
rather than silently accepting them. Inventory U-family commands, aliases, and
block-execute paths individually so native reset/utility operations remain in
scope without enabling arbitrary uploaded code.

Separate three evidence classes: native ROM-derived behavior, EconoPET policy
(such as pair validation), and sector-container limitations. Do not label
extension rejection behavior as native ROM parity. Resolve container variants
(including extended tracks and optional error data), corrupt-image handling,
and unsupported physical faults in M0. Reject unsupported formats explicitly.

## Current implementation evidence

These observations describe the initial revision above, not required behavior
for the replacement. Recheck them before implementation.

- [ieee_drive.c](../../fw/src/ieee/ieee_drive.c) has one global ordinary-file
  stream, described as sufficient for the Waterloo loader, and a separate
  limited REL channel pool.
- `ch15_execute` implements P positioning and simplified I/V handling.
  Other collected commands are logged as ignored, not fully implemented.
- `get_drive_model` infers 4040/8050 behavior from mounted images. It has no
  explicit 8250 model.
- [diskimage.h](../../fw/src/ieee/diskimage.h) supports D64 and D80, not D82.
  Native REL ignores side sectors and writes only within an existing chain
  without changing sector links.
- [ieee.sv](../../gw/EconoPET/src/ieee.sv) owns the byte handshake and firmware
  FIFOs. A DOS replacement cannot fix every gateware/FIFO lifecycle error.
- The [system fixtures](../../test/system/reference.md) execute production C
  through SPI into the production FPGA simulator. PET/Waterloo ROM execution
  supplies controller-side evidence, not original drive-ROM execution.
- The [native one-byte EOF case](PET/ieee.md) illustrates why ROM state
  transitions matter more than merely producing sensible output.

### Integration map

| Boundary | Existing entry points | Required implementation follow-through |
|----------|-----------------------|----------------------------------------|
| Public lifecycle | [ieee_drive.h](../../fw/src/ieee/ieee_drive.h), `ieee_drive_init/reset/mount/unmount_all/task` | Preserve the facade where possible. Propagate new failure results through every caller when signatures must change. |
| Storage | [diskimage.h](../../fw/src/ieee/diskimage.h), [diskimage.c](../../fw/src/ieee/diskimage.c), file callbacks in `ieee_drive.c` | Reuse verified sector geometry and I/O, not incompatible high-level shortcuts. Add explicit write/flush failure handling. |
| Configuration/menu | [config.c](../../fw/src/config/config.c), [menu.c](../../fw/src/menu/menu.c), [menu_config.c](../../fw/src/menu/menu_config.c) | Validate homogeneous pairs and supported image formats before destructive changes. Current `load_config` unmounts first and `action_mount` makes a failed mount fatal. Reconcile these with the new lifecycle contract. |
| PET reset | [pet.c](../../fw/src/pet.c) | Keep firmware and fabric reset effects coordinated without conflating reset with successful CLOSE. |
| Transport | [ieee.sv](../../gw/EconoPET/src/ieee.sv), [hardware_contract.h](../../fw/src/hardware_contract.h) | Preserve wire acceptance versus FIFO staging, ownership, and cancellation. Review unit-local changes against shared FIFOs. |
| Unit tests | [ieee_drive_test.c](../../fw/test/ieee_drive_test.c), [diskimage_test.c](../../fw/test/diskimage_test.c) | Add exact state/output and storage-fault cases. Existing expected values are not the oracle. |
| System tests | [IEEE fixtures](../../test/system/ieee/README.md), [host contracts](../../test/system/reference.md) | Exercise production C through production SPI/FPGA. Extend fixture mounting beyond its current slot-zero convenience API as needed. |

## Implementation constraints

1. Keep the public drive service and mount lifecycle as the integration facade.
2. Give each unit shared IEEE addressing, command collection, channel namespace,
   status, and native resource accounting. Record the validated model/revision
   profile and bind channels and pending jobs to their backing slot and media
   identity. A remount must not redirect stale work onto the replacement image.
3. Represent ROM-equivalent DOS transitions and algorithms directly. Preserve
   observable pointer wrapping, readiness flags, shared-buffer ownership,
   deferred operations, and explicit byte-width arithmetic.
4. Put sector reads/writes and native job results behind a small media boundary.
   Reuse file/SD callbacks and verified geometry helpers where suitable.
   Replace high-level image helpers that encode incompatible shortcuts.
5. Preserve the FPGA handshake/SPI boundary initially. Change it only for
   demonstrated readiness, ownership, ATN, reset, or EOI gaps.
6. Separate 4040 and DOS 2.7 differences explicitly. Share logic only after
   primary evidence establishes equivalent semantics.
7. Retain status provenance through deferred execution and partial channel-15
   reads. Selection of another disk/unit must not reformat pending output.
   Derive overwrite/consumption behavior from the ROM, rather than inventing
   an unlimited status queue.
8. Keep service work bounded so disk operations cannot starve IEEE, input, or
   other main-loop work. Measure resource use for four active units and eight
   mounted disks, not only one open file.

For every stateful slice, specify ownership, trigger, state transition,
completion observation, failure retention, and reset/remount invalidation.
Distinguish data received over IEEE, staged in RAM, written to the image,
flushed to storage, and acknowledged by DOS. Do not claim power-loss atomicity
from a successful buffered write. Define the persistence boundary and SD error
mapping in M0, then test partial writes and flush failures independently.
Preserve required error state without inventing successful rollback or retrying
a partially completed operation as though no writes occurred.

### Boundary for 6502-style helpers

Use ordinary C for loads, stores, comparisons, branches, and argument passing.
Allow a small set of typed `static inline` helpers for semantics the ROM
actually depends on, such as 8-bit carry/borrow, rotate-through-carry, explicit
wrapping, and little-endian address construction.

Helpers take explicit inputs and return explicit results. Do not maintain a
virtual accumulator, X/Y registers, or implicit processor flags across DOS
routines. Avoid instruction-shaped macro sequences such as `LDA`, `SBC`, and
`BCC`, generic emulated memory access, or literal stack tricks.

The goal is recognizable algorithms and verifiable semantics, not recognizable
instruction sequences. Inline helpers are not inherently slow, but RP2040
performance and memory use must be measured rather than assumed.

## Source-to-C traceability

Make side-by-side comparison with the original ROM sources a design requirement.

Document source adaptation and distribution terms before incorporating
third-party material. Prefer original explanations and pinned citations.
Include brief attributed excerpts only where permitted. Permission to quote
does not itself license ROM binary distribution or relicense adapted code.
Do not apply the repository's CC0 header to adapted material without establishing
that it is appropriate.

- Organize DOS modules around original source units and responsibilities, such
  as OPEN, CLOSE, PARSEX, OPCHNL, and REL. Avoid both a monolith and a function
  or file for every assembly branch label.
- Use recognizable original routine names adapted to repository conventions,
  qualified by model where needed. Preserve exact assembly labels in provenance
  comments and prefer direct, reviewable call relationships.
- Beside each adapted routine or coherent group, identify target model/hardware
  mode, source revision, source unit, entry labels, and commit-pinned links with
  line anchors where available. Cite all relevant ranges for combined, split,
  or shared adaptations.
- Where permitted, include concise attributed assembly excerpts beside
  non-obvious adaptations, with links and explanations of the corresponding C
  behavior. Otherwise use original summaries. Do not paste whole ROM routines.
- Preserve meaningful algorithm ordering, state transitions, buffer layouts,
  byte arithmetic, and error exits. Use recognizable names for corresponding
  DOS state and constants.
- Explain flag/register values replaced by C state or results, carry/borrow,
  pointer wrap, interrupt/deferred boundaries, and non-local error exits.
- Mark intentional departures explicitly: sector jobs instead of mechanics,
  FPGA transport, excluded memory commands, and host mount policy.
- Compare corresponding routines before sharing model logic and cite both
  sources. Keep meaningful model differences visible.
- Maintain a compact source-unit/label-to-C-symbol-and-test mapping, including
  unmapped/excluded routines and reasons.
- Name or annotate tests with original routines and reference cases. Similar
  names, quotations, and source structure alone do not prove equivalence.

For each adapted slice, reviewers must be able to locate its pinned source,
identify corresponding C entry points and state, understand material departures,
and find independent ROM-derived or ROM-execution evidence. Missing provenance
or unexplained transformations make a slice incomplete even if local tests pass.

## Milestones and dependencies

Execute M0 -> M1 -> M2 -> M3 -> M4 -> M5 -> M6. Each arrow requires the
predecessor's operator acceptance, not merely a passing host suite. Integrate
each slice through the facade as it is implemented, not in a final big-bang
integration. Split an oversized milestone into smaller operator-gated slices
without weakening its exit criteria.

### M0: Approve the compatibility contract and baseline

Deliver:

- A proposed ROM manifest with model, hardware mode, DOS/controller ROM part
  numbers, byte sizes, SHA-256 hashes, acquisition source, source commit, and
  verified source-to-binary correspondence or explicitly unresolved differences.
  Pin the PET and Waterloo controller ROMs separately.
- A per-model command inventory including aliases, OPEN modes, directories,
  REL, direct/block channels, finite resources, reset/utility commands, and
  exclusions. Each row has a behavior/evidence entry or a named evidence gap.
- Concrete contracts for homogeneous pairing, empty slots, media changes,
  pending writes, excluded commands, and container variants.
  Resolve native values from evidence. Ask for approval of extension choices
  not fixed by this plan.
- Baseline build/test results, firmware RAM/flash use, and an operator runbook
  for existing native D64/D80 read, status, reset, and Waterloo/OS-9 smoke cases.
  Distinguish known baseline defects from new regressions.
- Numeric resource/latency acceptance budgets derived from actual RP2040
  headroom and controller polling limits, with a measurement procedure. Do not
  invent a budget from host execution speed.
- A source-to-C/test map convention and third-party licensing/distribution
  assessment. Keep ROM binaries out of commits unless redistribution is allowed.

Exit: the operator approves exact ROM sets and contract decisions, runs and
accepts the documented baseline, and approves the measurement budgets. Known
baseline limitations are recorded, not relabeled as successful compatibility.
No replacement implementation begins before this gate.

### M1: Establish an independent original-ROM reference

Evaluate a pinned full-drive emulator such as MAME or VICE. Select it only after
demonstrating all three approved drive models, deterministic IEEE automation,
and writable image export. Execute the actual drive CPUs, ROMs, peripherals,
shared memory, and controller behavior. A bare CPU core, high-level virtual
DOS mode, always-successful job stub, or EconoPET DOS reuse is not an oracle.

Deliver a reproducible invocation and a case format containing:

| Field | Required content |
|-------|------------------|
| Identity | Case ID, emulator version/commit and settings, drive/controller ROM hashes, hardware mode, EconoPET revision and engine. |
| Initial state | Independent copies of identical image bytes with hashes, mounts, write protection, reset state, and deterministic setup/seed. |
| Stimulus | Byte-exact controller transactions, ATN/EOI boundaries, interruptions, and timeout limits. |
| Observations | Returned bytes and EOI, status reads and consumption, handshake event order, completion/failure, and elapsed time with units. |
| Media result | Exported final image and sector differences after defined completion/flush boundaries, including failed operations. |
| Evidence | Primary-source routines, independent corroboration, and any justified comparison exclusions. |

Compare bytes and defined sector contents exactly. Do not normalize away BAM,
links, metadata, EOI, or status differences. If physical formatting produces
undefined bytes or image conversion changes representation, identify the
excluded bytes from evidence and validate the conversion independently.
Compare required event ordering and latency bounds, not cycle-identical timing.

Prove reset/status, OPEN/read/CLOSE, one-byte and sector-boundary EOF,
interrupted/resumed TALK, and at least one write/CLOSE/export case on each
reference model. Use the current EconoPET path to establish differential
plumbing and record expected baseline mismatches, not to redefine the oracle.
Document which storage faults the reference can reproduce. Firmware-only
injection is not original-ROM evidence for an equivalent physical fault.

Exit: reproducible independent reference cases and operator-run board
comparison of the available baseline slice. Report discrepancies with primary
evidence. If reference automation fails, stop and seek a revised approach
before a wholesale rewrite.

### M2: Build the lifecycle, resources, and sector foundation

Implement isolated per-unit state, slot/media ownership, native finite
resources, D82 geometry, and model-specific sector/BAM access. Add an opt-in
replacement read/status vertical slice through the real facade and SPI/FPGA
path. Implement only the parser/channel subset needed for that slice, then
extend it in M3 rather than creating a throwaway second path.

Implement pair validation and the approved mount/remount/removal failure
contract across all entry points. Reject unsupported image formats on both
selectable paths. Do not delete user media or silently convert unsupported
images.

Keep development engine selection explicit and stable for an active unit.
Never fall back to legacy for an unsupported replacement operation or D82.
Keep unfinished features opt-in and fail explicitly according to the staged
contract. Do not claim native parity for temporary unsupported-feature errors.

Exit: exact read/status comparisons, native resource ownership tests, rejected
mixed pairs and unsupported images, both single-slot mount positions, all three
homogeneous pairs, absent units, and multi-unit isolation. Demonstrate on
hardware that the vertical slice meets the M0 memory/service budgets and that
media changes do not deliver stale FIFO data or reset another unit.

### M3: Implement ordinary files and directories

Implement parsing, lookup, wildcards, modes, directory streams, sequential
reads/writes, SAVE, creation, append/replace, and CLOSE finalization. Follow
selected-ROM allocation, links, BAM updates, and disk-full algorithms.

Exit: differential transactions and image comparisons for each model, plus
operator hardware cases covering empty/one-byte files, sector boundaries,
repeated EOF, interrupted TALK, simultaneous channels on both disks, resource
exhaustion, and writable/read-only/full media. Inject write and flush failures
at allocation, data, link, and directory-update boundaries. Verify retained
errors and subsequent recovery, not just the first failed return.

### M4: Implement native DOS commands

Implement scratch, rename, copy, applicable same-model dual-drive operations,
initialize, validate, format, direct/block channels, and approved utility/reset
commands. Match parsing, omitted-drive selection, deferred execution, status
fields/text, and recovery. Unsupported arbitrary code execution remains excluded.

Exit: non-REL command-inventory coverage with exact status and resulting-media
checks, malformed input, empty slots, invalid sectors, read-only/full media,
and storage faults. Record REL-dependent command cases for M5 rather than
marking them passed. Include multi-command sequences and partial status reads.
The operator runs destructive format/validate/copy tests only on disposable
images and verifies them after remount. Reconfirm mixed-type pairs cannot be
installed.

### M5: Implement complete native REL behavior

Implement creation, positioning, reads/writes, growth, side sectors, and
model-specific indexing structures. Replace chain-only shortcuts. Match
missing-record/overflow behavior, buffer interactions, and CLOSE.

Exit: differential cases at record, sector, side-sector, indexing, and capacity
boundaries for each model. Include interleaved channels/units and interrupted
growth or CLOSE with storage faults. Complete REL-dependent DOS command cases
deferred from M4. The operator verifies native REL
creation/growth and supported Waterloo/Super-OS/9 read/write workflows on
native images, then checks persisted data after remount.

### M6: Accept and migrate the native default

Complete the matrix below, measure actual worst-case RP2040 headroom/service
latency and SD behavior, and document exact revisions, exclusions, container
limits, and evidence gaps. Keep the replacement opt-in until the operator
accepts the complete hardware matrix.

After that approval, switch the native default and remove superseded legacy
DOS logic. Re-run affected host suites and provide a final operator smoke
runbook for the default configuration. Exit only after the operator confirms
the shipped-default path passes. Do not include hybrid implementation in this
cleanup.

## Acceptance matrix and operator handoff

Maintain case IDs and evidence links for the following dimensions. Cover
model-specific behavior for every model rather than extrapolating from D64.
Do not require an unbounded Cartesian product, but record why a dimension is
inapplicable instead of silently omitting it.

| Dimension | Required coverage |
|-----------|-------------------|
| Drive models | Approved 4040/D64, 8050/D80, and 8250/D82. |
| Mounts | Slots 0-only and 1-only, both same-type, both empty, six ordered mixed-type pair rejections, unsupported images, failed candidate open/validation. |
| Units/resources | Devices 8-11, concurrent different models on different units, both disks active, native channel/buffer exhaustion, independence from another unit's reset/media change. |
| Controllers | Corrected PET KERNAL 2.0, PET KERNAL 4.0/4.1, Waterloo revision 12, and native-image OS-9 workflows. |
| Lifecycle | Repeated OPEN/CLOSE, partial status reads, interrupted/resumed TALK/LISTEN, pending-write failures, explicit reset/IFC distinctions, remount and configuration reload. |
| Storage | Writable/read-only/full images, allocation limits, malformed images, partial/failed reads and writes, flush failure, persistence after close/remount. |
| Evidence layers | ROM-derived unit expectations, independent drive-ROM execution, production C through SPI/FPGA, real EconoPET board. Original-drive captures resolve reference/ROM discrepancies where required. |

For each milestone, hand the operator a self-contained runbook with:

- Exact firmware/FPGA build identifiers, engine selection, board setup,
  controller ROMs, device/slot configuration, and required SD files.
- Disposable fixture generation/copy instructions and initial image hashes.
  Identify destructive steps and preserve originals.
- Exact PET/Waterloo commands or test-program invocations in execution order.
  State expected bytes/status, persisted image effects, and time limits from
  the approved evidence. "Try loading a file" is not an acceptance test.
- How to collect logs, image exports, memory/service measurements, and any
  required bus capture. Do not require unavailable instrumentation silently.
- A result record per case: pass, fail, or blocked, with observations and
  artifacts. Missing hardware or unrun cases are blocked, not passed.
- The specific approval requested. On failure, isolate the discrepancy, add a
  regression, fix it, and supply the updated rerun procedure. Remain at the
  current milestone until it passes.

Store reproducible manifests, fixtures/generators, and evidence with their
owning tests or documentation. Keep a compact milestone status and
source-to-C/test map with this plan. Link large logs/captures rather than
duplicating them, and record external artifact identities/hashes. Do not add
ROM binaries or operator disk contents without distribution permission.

### Validation commands

Use root presets. Production is `RelWithDebInfo`, tests/frameworks and tested
production sources are `Debug`, and Verilator models are separately optimized
with assertions preserved. Configure before building:

```sh
cmake --preset default
cmake --build --preset fw
cmake --build --preset fw-test
ctest --preset fw --parallel
cmake --build --preset sys-test
ctest --preset sys --parallel
```

Use focused existing selectors while iterating, then complete affected suites
for the milestone. Add a reproducible command for the independent reference
harness in M1 (it does not exist at this baseline).

When gateware behavior changes, run the focused Icarus/Verilator IEEE cases and
the full test preset, because `gw` excludes boot coverage:

```sh
ctest --preset gw --parallel -R 'ieee_.*tb'
ctest --preset all --parallel
cmake --build --preset gw
```

Run the FPGA build when preparing changed gateware for operator deployment.
Host results never replace the milestone's hardware gate.

## Primary sources and supporting references

- [Compatibility requirements](../../.github/instructions/ieee.instructions.md)
- [Firmware facade](../../fw/src/ieee/ieee_drive.c)
- [Image boundary](../../fw/src/ieee/diskimage.h)
- [FPGA boundary](../../gw/EconoPET/src/ieee.sv)
- [Host integration contracts](../../test/system/reference.md)
- [Existing ROM-derived EOF regression](PET/ieee.md)
- [Pinned 4040 composition](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/master)
- [Pinned 8050/8250 composition](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/master)
- [4040 deferred commands in IDLE](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/idle)
- [DOS 2.7 IEEE readiness](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ieee)
- [DOS 2.7 controller jobs](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/jobs)
- [MAME 8050/8250 hardware model](https://github.com/mamedev/mame/blob/master/src/devices/bus/ieee488/c8050.cpp)

The primary source illustrates why mechanical translation is insufficient:
IEEE readiness and GET/PUT interact with interrupt-driven control, while disk
jobs wait on another controller's shared state. MAME independently models the
main CPU, controller CPU, RIOT/MIOT/VIA, and floppy controller. Keep these
hardware details out of the production DOS core where possible, while using
original-ROM execution to check observable behavior.
