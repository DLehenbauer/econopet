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

### Firmware layer boundary

[ieee_drive.c](../../fw/src/ieee/ieee_drive.c) owns primary addressing,
listener/talker routing, SPI/FIFO owners, bounded scheduling, backing-file
handles, and mount orchestration. [dos.c](../../fw/src/ieee/dos.c) owns per-unit
OPEN/command collection, secondary-channel interpretation, request dispatch,
logical channels, streams, and status. [dos_media.c](../../fw/src/ieee/dos_media.c)
owns media bindings, identities, sector access, and flush lifecycle.

The facade delivers decoded secondary/OPEN/CLOSE operations and routed input
bytes to DOS. EOI records completion but does not execute a request. Before
dispatch or CLOSE can replace a source, the facade pauses new presentations,
commits observed GET progress, waits for active presentations to finish, and
retires affected staged copies. DOS previews output with a captured source
token. The facade commits only actual GETs to that token, not to the currently
selected talker. Discarding an unconsumed FIFO suffix does not consume DOS data.
DOS exposes pending work and read-only status progress instead of requiring
the scheduler or diagnostics to inspect its internal fields.

This boundary refactor preserves the current read/status subset, including
explicit syntax errors for unsupported commands. It adds no write support or
new claim of complete ROM compatibility.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| OPEN/command collection | Approved 4040 revision 2 and 8050/8250 DOS 2.7 | Pinned [4040 tst2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2) and [8250 tst2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2), PUT/L42/L50/L45 (4040 receive-limit overlay below) | Retain one byte beyond 58, observe EOI even at saturation, defer execution | `dos_input_put` and `dos_input_dispatch` in [dos.c](../../fw/src/ieee/dos.c), deferred OPEN and capacity cases in [dos_test.c](../../fw/test/dos_test.c) | [Commodore DOS layer](https://www.pagetable.com/?p=1038) distinguishes channel meaning from transport |
| Secondary source resolution | Same approved models | Pinned tst2:FNDRCH in both source sets | SA 15 selects status, an unopened ordinary secondary produces no data | `dos_output_stage` and captured-source tests in [dos_test.c](../../fw/test/dos_test.c) | Same layer description |
| GET ownership and status clearing | Same approved models | Pinned [4040 ieee](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ieee):TLK20/TLK30/TLK35 and [tst4](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4):GETERC, corresponding [8250 ieee](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ieee) and [tst4](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst4) | Preview does not consume, partial status retains its suffix, actual CR GET publishes OK | `dos_output_get`, direct source/status tests in [dos_test.c](../../fw/test/dos_test.c), interrupted-transfer tests in [ieee_drive_test.c](../../fw/test/ieee_drive_test.c) | Existing ROM/hardware evidence below remains authoritative (the layering article alone does not establish GET timing) |

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

### Milestone status

M0 and M1 were explicitly accepted by the operator on 2026-10-24. M2 was
accepted on 2026-10-25 as the read/status and lifecycle foundation, with
measured timing parity, runtime headroom and board-only live-remount isolation
explicitly deferred to M6 final acceptance before the MVP. These checks are
not recorded as passed. The M3 ordinary-file implementation and its separate
hardware acceptance handoff are recorded below. M3 was explicitly
operator-accepted on 2026-10-26 with the documented reduced board coverage.
The pinned full-drive reference
cases and unresolved legacy baseline observations are described below. The
[M0 contract and baseline](drive-emulation-m0.md) and
[candidate ROM manifest](../../tools/drive-roms/drive-emulation-roms.json) record the initial evidence,
proposals, measurement gaps, and hardware procedure. These two local operator
artifacts are git-ignored, not supplied by a fresh checkout. The verifier needs
an explicitly supplied approved manifest. The operator approved the
drive candidates subject to source verification, raw-only image variants, and
exclusion of the DOS 2.7 `&` code loader. Operator-confirmed adaptation and
resulting C/firmware distribution terms are recorded in M0, with no ROM binary
redistribution. The [source verifier](../../tools/drive-roms/drive_rom_verify.py) confirms
DOS 2.7's emitted main-DOS bytes. The older 4040 source requires the explicit
revision-2 overlays documented below, not a change of target ROMs. Static maps
now cover both selected controllers and the embedded formatter/startup payloads.
The combined baseline D80 was reproduced exactly. The operator reported the
documented OS-9 board checks passed on 2026-10-24, including boot and existing-file
readback. The device-8 status read/consumption check was also reported passed.
The operator also confirmed file loading after a short MENU-button reset.
Waterloo `basic` loading and the `test.main` memory test are also
operator-reported passed. The operator accepted native-sized state and on-demand
SD access as the resource approach, targeting 4 KiB per unit plus bounded
implementation overhead. The operator also accepted original-hardware latency
and throughput parity or better for the MVP, with throughput optimization
deferred until post-MVP. Acceptance records the agreed contract and reported
baseline, not completed compatibility or measured replacement timing parity.
M2 began with the replacement's bounded media and resource foundations.
The operator subsequently instructed "You don't need to preserve the legacy
engine. Delete the legacy code." The replacement is now the only engine.
This explicitly supersedes the original opt-in-until-M6 rollout, not the
remaining milestone acceptance gates or the scope of unfinished features.
The temporary firmware baseline probes and serial `profile` command have been
removed. Historical measurements remain recorded in the M0 runbook.
The operator has deferred further memory profiling. Use ROM-sized bounded
structures and static RAM accounting for the initial budget, not a profiling
campaign as another M0 gate. The initial resource direction is settled below.

### Approved ROM correspondence

Source is pinned to `mist64/cbmsrc` commit
`01bd60f162ef92212ef0cb67546ae8f42be34168`. Keep the unmodified comparison
separate from semantic adaptations. [drive_rom_map.py](../../tools/drive-roms/drive_rom_map.py)
applies only explicit source-line overlays from the supplied manifest, retains
the original assembly, and emits a per-source-line old/new address map.
Independent zero/255 gap-fill assemblies check emitted coverage.

| Approved binary | Static correspondence established | Limits |
|-----------------|-----------------------------------|--------|
| 4040 DOS 2.0 revision 2, 901468-14/-15/-16 | 15 source-line overlay ranges across 11 units reconstruct all 11613 compared bytes at $D2A0-$FFFF. Three declared checksum placeholders are excluded. | Unmodified source still matches revision 1, not revision 2. $D000-$D29F is the separately mapped formatter, not padding. |
| 8050/8250 DOS 2.7, 901887-01/901888-01 | Unmodified main source matches all 14026 compared emitted bytes, with two declared checksum placeholders. | Reserved controller payloads and verified gaps remain separately accounted for. |
| 8050/8250 MPI controller, 901869-01 | 14 explicit overlays of `CONTROLLER_8250` reconstruct all 1004 compared bytes. Coverage is $FC00-$FFE7 and $FFFB-$FFFF, with one declared checksum placeholder. | The archived source identifies Tandon hardware. Do not use it unmodified or replace the approved MPI target. $FFE8-$FFFA is verified padding. |
| 4040 controller, 901466-04 | Classified disassembly reconstructs all 1024 bytes, including tables, padding, checksum, and vectors. | No matching original controller source was established. Disassembly/reassembly is byte accounting, not independent semantic verification. |
| Embedded payloads | Classified disassembly reconstructs all 672 bytes of the 4040 formatter, 880 bytes of the DOS 2.7 formatter, and 162 bytes of DOS 2.7 initialization. | These are distinct controller programs uploaded from main ROM, not main-CPU routines or unused gaps. |

No drive CPUs, peripherals, GCR hardware, or disk operations were executed by
these checks. Static correspondence is not the M1 oracle or hardware acceptance.
Recipes and exact approved input hashes remain in the local manifest. All
generated assembly, ROM slices, listings, and byte reports are private and must
not be redistributed.

#### Revision-2 differences that affect implementation

The table refers to original source line numbers, before overlay application.
Use generated `source_map` and symbol files for addresses outside these anchors.
Do not infer equivalence from matching routine names or relative offsets.

| Source units and lines | Revision-2 difference | Address anchors (old source -> approved revision 2) |
|------------------------|-----------------------|--------------------------------------------------|
| `dskint:319-325` | Queue power-on bump jobs and set ATN state through X rather than A. | `IDLE` $D4A7 -> $D4A6 |
| `tstfnd:12-16` | Use the shared BAM-pointer setup routine. | `NXTDS` $D6B7 -> $D6B6, `SETBMP` $D789 -> $D780 |
| `lstdir:115` | Preserve A across the directory helper instead of tail-jumping. | `GD1` $DB31 -> $DB28 |
| `autoit:26-28`, `lookup:48-49` | Use the shared drive-toggle helper. | `TOGDRV` $DD8C -> $DD86 |
| `copyall:88` | Also clear record state while initializing copy state. | `PUPS1` $E571 -> $E567 |
| `block:337` | Reload the logical-channel index before the direct-channel helper. | `BW20` $EA32 -> $EA2B |
| `tst2:87` | Command receive pointer comparison changes from 41 to 59. Do not use the older source or current legacy limit as the replacement's length contract. | `L42` $EC1E -> $EC19 |
| `tst3:123` | Wait for the reused buffer's job to finish before clearing its status. | New wait at $EF18 |
| `opchnl:185` | Map out the channel buffer before opening the overwrite path. | `OROW` remains $F797 |
| `record:124-126,135,149` (five overlay ranges) | Change active/inactive REL-buffer sequencing, tail-call the final buffer swap, and relocate the shared return. | `POSBUF` $FD7A -> $FD7D, `P20` $FDA9 -> $FDCE |

The mapped checksum placeholders are $D2A0 (`romtbl:10`), $E7A7 (`echksm:1`),
and $FFE9 (`vector:8`). The older source places the middle placeholder at
$E7AE. Do not mask the older address when checking the revision-2 reconstruction.
These findings constrain future implementation and tests. They do not change
legacy runtime behavior during M0.

#### Controller and upload boundaries

Controller addresses below use the assembled $FC00-$FFFF aliases. Both
controllers have 15 pending job entries starting at controller $0403 (main DOS
$1003), with bit 7 meaning pending and bit 0 selecting the drive. Completion
codes are controller results, not decimal command-channel status numbers.

| Entry or state | 4040 controller | Selected MPI controller |
|----------------|-----------------|-------------------------|
| Idle target through $FC00 | $FC54 | $FC17 |
| Completion target through $FC02 | $FF08 | $FEDB |
| Hardware reset vector target | $FC09 | $FC04 |
| Queue scan | $FC56 | $FC19 |
| Execute uploaded buffer | $FD20 | $FD00 |
| Bump | $FD2A | $FD08 |
| Read / write / verify | $FDC4 / $FDFD / $FE57 | $FD98 / $FDD2 / $FE3B |
| Header / sync | $FE82 / $FF3E | $FED5 / $FEF7 |
| IRQ vector target | $FF82 | $FF3F |

The 4040 $FC04 wait entry is not its hardware reset entry. Its reset initializes
RAM, I/O, and shared readiness before scanning. MPI reset waits for main-DOS
upload and jumps to controller $0500. That uploaded initialization tests RAM
and ROM, initializes I/O, and derives side count from MIOT PB6 before publishing
readiness. The MPI source overlays also change queue scanning, settle handling,
header/sync control flow, and the IRQ timer reload. Preserve the selected
reload sequence (including its consecutive timer stores), not the Tandon one.

Do not treat reserved ROM lengths as transfer lengths:

| Payload reservation in main ROM | Actual host upload | Controller interpretation |
|---------------------------------|--------------------|---------------------------|
| 4040 formatter $D000-$D29F | `duplct:FORMAT` copies $300 bytes from $D000-$D2FF to main $1100. | Controller $0500-$07FF. Formatter code/data occupies $0500-$079F, and the copy also includes following ROM bytes. |
| DOS 2.7 formatter $C000-$C36F | `duplct:FORMAT` copies $400 bytes from $C000-$C3FF to main $1D00. | Mirroring maps the destination to controller $0500-$08FF. Formatter code/padding occupies $0500-$086F. The following uploaded bytes are reused as working state, not a separate simultaneous initializer. |
| DOS 2.7 initialization $C370-$C411 | `dskint` copies $100 bytes from $C370-$C46F to main $1100. | Controller $0500-$05FF. Initialization occupies $0500-$05A1. The transfer also includes $C412-$C46F from the main-DOS source region. |

Both formatters return through the controller's completion pointer at $FC02.
The DOS error path translates results after its job/retry handling. For example,
controller results 2/3/4/5/16 become DOS errors 20/21/22/23/24, and 7/8/9/11
become 25/26/27/29. DOS 2.7 additionally translates formatter result 12 to 75.
This identifies the translation boundary, not full retry, failure, or status
lifecycle parity. Those still require original-drive execution.

With ACME and CC65 tools installed, supply the approved local manifest and
external source/ROM directories. Use new output directories for each run:

```sh
# Check the original and adapted source comparison tools without ROM inputs.
python3 -m unittest discover -s tools/drive-roms -p 'test_*.py'

# Reconstruct the approved revision from explicit source overlays.
python3 tools/drive-roms/drive_rom_map.py --mapping 4040-revision2 \
  --source-dir "$DOS_SOURCE/DOS_4040" --rom-dir "$DRIVE_ROMS" \
  --manifest tools/drive-roms/drive-emulation-roms.json \
  --output-dir build/drive-rom-verification/4040-revision2

# Reconstruct the MPI controller from the explicitly adapted controller source.
python3 tools/drive-roms/drive_rom_map.py --mapping 8050-8250-mpi-controller \
  --source-dir "$DOS_SOURCE/CONTROLLER_8250" --rom-dir "$DRIVE_ROMS" \
  --manifest tools/drive-roms/drive-emulation-roms.json \
  --output-dir build/drive-rom-verification/mpi-controller

# Reconstruct the selected 4040 controller with explicit table classification.
python3 tools/drive-roms/drive_rom_map.py --binary-map 4040-controller \
  --rom-dir "$DRIVE_ROMS" --manifest tools/drive-roms/drive-emulation-roms.json \
  --output-dir build/drive-rom-verification/4040-controller
```

The other `--binary-map` names are `4040-formatter`,
`8050-8250-formatter`, and `8050-8250-initialization`. Their recipes identify the
approved input chips, slice hashes, runtime origins, data ranges, and entry
labels. Each report records its recipe digest. Keep negative controls:
the unmodified 4040 source must still differ from revision 2, and an altered
ROM/source input or existing output directory must fail rather than silently
produce a new correspondence claim.

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

For menu/configuration reload, the operator explicitly approved the original
destructive workflow: halt the CPU, unmount all disks, then execute the selected
setup actions in order. Each mount validates its type against the other slot
and a failed configured mount is fatal. There is no configuration rollback,
complete-set preflight, or retry menu. Clearing the old disks first allows a
valid pair to change model in either mount order. This supersedes the earlier
requirement for transactional configuration publication. Direct mount failures
still retain the old unit. Cover these entry-point-specific rules, unmount, and
bulk teardown. Distinguish
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
| Configuration/menu | [config.c](../../fw/src/config/config.c), [menu.c](../../fw/src/menu/menu.c), [menu_config.c](../../fw/src/menu/menu_config.c) | Keep unmount-all followed by sequential setup actions. `action_mount` makes a failed mount fatal, including a type mismatch with the other disk. This operator-approved path does not promise machine rollback. |
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

### Resource design contract

Use the original drives' bounded state and algorithms as the sizing model.
The operator's initial target is 4 KiB of ROM-equivalent DOS working state per
unit (16 KiB across devices 8-11), shared by both media slots in each unit.
This is a design target, not a measured C-layout result or a reason to reduce
native buffer/channel limits.

- Use native buffer pools, tables, and REL indexing. Fetch the required
  side-sector/index and data sectors from SD instead of retaining entire
  file-chain caches.
- Keep images on SD and use bounded sector reads/writes. Image size and REL
  file length must not drive proportional growth in DOS RAM use. This does
  not expand the approved image-format scope.
- Keep read-only code and tables in flash. Flash capacity is not an initial
  resource concern for this replacement.
- Account separately for bounded transport FIFOs, storage/file handles, C
  representation/alignment, stacks, and other implementation workspace.
  Do not treat 16 KiB as a whole-firmware RAM cap or duplicate a complete
  native resource pool for each mounted disk.
- Check concrete state sizes and the linker map as each slice is implemented.
  Document deviations from the target, keep overhead bounded, and investigate
  measured pressure if it appears. Do not count legacy cache savings before
  those caches are actually removed.

This is the accepted initial resource approach for M0. An exact future C-layout
proof or another profiling campaign is not required before M1. Resource checks
for later implementations remain part of their milestone acceptance.

### Latency and throughput contract

For the MVP, preserve controller-ROM timing compatibility. Faster user-visible
performance is desirable, not a mismatch. Do not introduce delays merely to
reproduce drive mechanics. Performance relative to VICE is an advisory
optimization benchmark, not an acceptance gate.

- Compare equivalent operations for the selected model, ROMs, media, controller
  configuration, and relevant initial state. Cover command completion and
  individual protocol phases, not just average bytes per second or total
  file-load time.
- Preserve the selected controllers' finite polling deadlines. A faster
  whole-file transfer does not excuse a stalled handshake or new timeout.
  Faster responses must still preserve required protocol ordering and timing.
- Physical Commodore-drive comparison is unavailable for M6 and is not an
  operator prerequisite. Use pinned raw VICE observations as a benchmark
  baseline. Use emulated cycles and exported PET counters, never
  host wall time or warp FPS. VICE C-FDC simplifies mechanics, jobs and
  whole-media formatting. Retain and investigate slower results without
  silently changing the frozen observations or stopping compatibility tests.
- Compare equivalent PET-visible windows in the separate `M6BENCH` program,
  retaining individual maxima and reporting performance differences while
  continuing. Apply no percentage allowance, only intrinsic
  paired-counter endpoint uncertainty. A controller timeout threshold is a
  separate ceiling, not evidence of acceptable response time by itself.
- Measure uniformly spaced controller frame ticks (50 Hz PAL, 60 Hz NTSC).
  PAL BASIC jiffies include two closely spaced increments every fifth IRQ,
  as documented in the pinned
  [FIFTYH editor source](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/EDITOR_PET_EURO/edt4src#L1-L17).
  Remove that compensation using the selected editor's observed phase, not
  by padding a failed reference. Changing the timebase requires fresh
  source-bound observations.
- PET frame ticks do not directly measure every RP2040 service gap, SD flush or
  electrical edge. Keep still-required internal/electrical measurements
  separately open or blocked rather than claiming the VICE comparison covers
  them.

The M6 policy above supersedes the previous strict VICE speed gate after the
board's short REL8/0 result (metric20, actual11 versus reference6 ticks).
Controller deadlines and protocol correctness remain compatibility gates.
Existing benchmark measurements do not establish physical timing parity.

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
- The initial resource contract above: a 4 KiB-per-unit native-state target,
  bounded implementation overhead, and SD-backed sector access using native
  REL indexing. Check concrete sizes as the C implementation is added rather
  than requiring its final layout during M0. Do not count legacy caches as
  reclaimed until their removal is implemented. Heap-peak/both-stack profiling
  is optional unless accounting or failures expose a concrete resource risk.
  Use the accepted original-hardware latency/throughput parity policy. Retain
  selected-controller polling limits and observations as supporting evidence,
  then establish operation/phase reference timings in M1, not from host
  execution speed.
- A source-to-C/test map convention and third-party licensing/distribution
  assessment. Keep ROM binaries out of commits unless redistribution is allowed.

Exit: the operator approves exact ROM sets and contract decisions, runs and
accepts the documented baseline, and approves the scoped budgets. Known
baseline limitations are recorded, not relabeled as successful compatibility.
No replacement implementation begins before this gate.

#### Remaining compatibility baseline

The D64/D80 read checksums are already recorded. Do not repeat the slow BASIC
checksum loop merely to obtain more profiles. For a read after reset, use the
native [drive-test](../../prgs/drive-test/README.md) program.

| Case | Procedure | Required observation |
|------|-----------|----------------------|
| Status consumption | Cold-start a D64 or D80 case and run the two-read status program in the local M0 runbook, before another disk command | Initial legacy version status followed by model-specific OK. Record exact text, not just its number |
| PET reset | Short-press MENU for an in-place PET reset, then load a file again. Do not use a long press or debugger CPU-only reset | The existing mounts remain usable without selecting or reloading a configuration |
| Waterloo | SuperPET/6809 with the pinned combined Waterloo image in device 8, drive 1. Enter `basic`, then separately reset and enter `test.main` | Language prompt and actual memory-test results, not just the Waterloo boot menu |
| OS-9 | Same SuperPET configuration, with the pinned OS-9 D80 in device 8, drive 0. Enter `disk8/0.os9`, answer the date/time prompt, then run `dir /d0`, `dir /d0/cmds`, and `list /d0/startup` | Interactive OS-9 shell, existing directories, and startup-file contents read through the REL-backed filesystem |

For OS-9, the pinned image contains `os9` and a 129-byte-record
`OS9 DRIVE A` REL file. Its 1248 records each carry 128 filesystem bytes and
a CR, exposing 624 logical 256-byte OS-9 sectors. The root contains `startup`,
`cmds`, `sys`, `defs`, and `os9boot`. The 995-byte startup file begins with
`setpr 1 255`, `setime < /term`, and `-p t`, and contains
`WELCOME TO OS-9 LI V1.2`. These are static readback expectations, not a claim
that EconoPET has completed an OS-9 boot.

The boot command is documented by the
[MMU project](https://mikenaberezny.com/hardware/superpet/super-os9-mmu/)
and the [software notes, pages 2-3](https://retro-bobbel.de/zimmers/cbm/pet/SuperPET/os9/OS9_SW_Docs.pdf).
The configured SuperPET machine already includes the MMU. Leave latch setup to
the loader rather than adding manual register pokes or a new configuration flag.
Do not follow the software notes' format/backup instructions for this baseline.
Use disposable images and compare hashes afterward. OS-9 on `/d0` does not
establish `/d1`-`/d3`, REL creation/growth, or 8250 compatibility.

The operator reported OS-9 and device-8 status read/consumption passed on
2026-10-24, then confirmed file loading after a short MENU-button reset.
Long MENU opens configuration selection, which unmounts images and applies
the selected configuration, so that path cannot establish mount preservation.
The operator also reported Waterloo `basic` loading and the `test.main`
memory test passed. No detailed memory-test output was supplied, and these
passes do not establish coverage of `test.banks` or `test.os9`.
Existing host status/Waterloo/REL/reset-vector cases are supporting evidence,
not substitutes for board execution or the M1 original-drive reference.

#### Controller polling limits and proposed margins

These limits concern PET-side controller ROMs, not the original drive's job
deadlines or a whole-file transfer limit. Primary evidence is the installed
PET KERNAL 2/4 and Waterloo revision-12 binaries, checked against the pinned
reconstructed PET sources and disassembled Waterloo paths. The local M0
contract records their identities and anchors.

| Controller phase | Native polling limit |
|------------------|----------------------|
| PET KERNAL 2/4 `ISOUR` initial NRFD readiness | Unbounded |
| PET KERNAL 2/4 `ISOUR` NDAC acceptance and `ACPTR` DAV arrival | T1 high latch reloaded with `$FF`. Loaded count is `$FF00 + L`, where `L` is the existing low latch. At nominal 1 MHz VIA, the minimum count spans 65.280 ms, plus device load/underflow latency |
| PET KERNAL 2/4 `ACPTR` after sampling, and `LIST4`, waiting for DAV release | Unbounded |
| PET KERNAL 4 timeout with `BOB` high bit set | STOP-key retry path can reload the timer, so a finite overall timeout is not guaranteed |
| Waterloo revision 12 initial NRFD readiness and post-sample DAV release | Unbounded |
| Waterloo revision 12 NDAC acceptance or DAV arrival, wait word zero | Unbounded |
| Waterloo revision 12 timed inner polls, nonzero wait word `W` | 3200 polls per unit, 29 nominal MC6809 cycles per idle poll, plus setup/outer-loop overhead |

At nominal 1 MHz MC6809 E rate, the timed Waterloo receive path from `$BF2A`
to timeout entry `$BF7C` takes `92828 * W - 3` nominal cycles, including loop
setup and outer overhead (92.825 ms for `W=1`). The output path from `$BEC2`
to its timeout fallthrough `$BEE1` takes `92825 * W` nominal cycles. The
initial readiness wait and earlier call setup are excluded. Do not infer a
default `W`: the timeout setter stores the caller's word at parameter-block
offset 7. Tests explicitly choose zero or one.

Cycle arithmetic uses the
[Motorola programming manual, appendix D](https://www.maddes.net/m6809pm/appendix_d.htm).
VIA interpretation follows the
[6522 timer register description](https://www.westerndesigncenter.com/wdc/documentation/w65c22.pdf).
These hardware references and selected-ROM execution tests corroborate the
static analysis, not deployed timing. Independent
[MAME VIA](https://github.com/mamedev/mame/blob/9d8a6727a206fc822b8b69d67a51e6fd7dbf4d4b/src/devices/machine/6522via.cpp#L1020-L1037)
and
[VICE VIA](https://github.com/VICE-Team/svn-mirror/blob/a0e14df25846cd7f06043f24b48677bf45c02149/vice/src/core/viacore.c#L747-L767)
implementations corroborate reuse of the low latch and timer restart/flag
clear on the high-byte write. Their clock-phase bookkeeping is not a reason
to invent an exact deployed load-to-IFR constant. The production clock schedule
generates one CPU PHI2 pulse per 64 system-clock cycles, with PIA2/VIA on that clock.
Numerical conversions assume the nominal 64 MHz system clock, 1 MHz controller
execution, and the stated latch/interrupt settings. Soft-core instruction
timing, interrupts, and bus stalls must be measured rather than assumed to
match nominal MC6809 cycles.

The earlier 20 ms service-gap and 40 ms response proposals are superseded by
the accepted original-hardware parity contract above. They are not MVP
acceptance limits or evidence of native timing. The supplied maximum gap
(18.192 ms) and maximum inclusive service
duration (16.136 ms) sum to 34.328 ms as a scheduling diagnostic, not a proven
response bound. Do not add nested read latency again. Multiple service passes,
queueing, and blocking storage can invalidate that proxy.

Later replacement-slice timing acceptance requires nonzero service coverage
and an on-wire trace of
controller NRFD release to drive DAV assertion for input, and controller DAV
assertion to listener NDAC release for output. For the selected PET paths these
external intervals conservatively include the following timer reload. Record
the actual controller configuration and supported workload. Measure timing-only
builds as well as diagnostic builds. Unbounded native waits still need an explicit
operator stop limit, not an invented native timeout. Other clocks, wait words,
or controller modes require rederivation. The observed gap alone, host test
speed, and successful file reads do not prove original-hardware parity.
These implementation checks are not additional prerequisites for M0 policy
acceptance or for building the M1 reference.

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
| Timing reference | Emulated-time command/transfer and protocol-phase timings for equivalent model/media/controller/initial-state cases. Distinguish controller timeouts from observed native response times and document physical timing-model limits. |
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

#### M1 reference and differential tooling

[drive_reference.py](../../tools/drive-ref/drive_reference.py) and its
[Lua observer](../../tools/drive-ref/drive_reference.lua) execute a pinned MAME build
using the approved 4040 revision-2 and 8050/8250 MPI ROMs. Both the main 6502
and controller 6504 execute original firmware, with shared RAM, peripherals,
GCR hardware and writable floppy images. Active main/controller ROM bytes are
checked against the approved input files after execution.

The evaluated VICE source implements these drives' FDC jobs in C
([pinned `fdc.c`](https://github.com/VICE-Team/svn-mirror/blob/a0e14df25846cd7f06043f24b48677bf45c02149/vice/src/drive/ieee/fdc.c)),
so that candidate does not meet this plan's full-controller requirement.
At this M1 checkpoint, VICE's `c1541` and `petcat` only seed synthetic fixtures,
tokenize the PET controller program, and independently extract the persisted
result. Neither supplies reference DOS behavior. The later M4 VICE
main-DOS/C-FDC corroboration below does not replace full-controller evidence.

The initial runtime is Linux package `mame=0.285+dfsg1-1`, upstream release
commit `f379cd060ac3b2bd567ebbd0957aed059d6895ff`. The tool requires executable
SHA-256 `b8de871c99effd28e90c426b811694811bcb533741189f93ad63a962621ae40c`
rather than trusting a version string or accepting an unverified substitute.
That package remains a functional smoke-test option, not complete wire evidence.
The traced mode instead verifies a Debug source build at the same upstream
commit, its trace/interleave-precision patch, and the resulting executable hash.
The controller is MAME `cbm4032`, revised BASIC 4, using the approved PET
ROM-4 components plus non-CRTC editor `901447-29`
(SHA-256 `44c79cce9be64be26000821957ef17ca2d86ab5cd66c1e6c4342b91fd5f04b67`).
Both drive FDCs also require the separate 2048-byte `901467` hardware GCR
lookup ROM (SHA-1 `97df576397608455616331f8e837cb3404363fa2`), available as
[`901467-01.bin`](https://www.zimmers.net/anonftp/pub/cbm/firmware/drives/old/8050/901467-01.bin).
These supporting assets do not change the approved DOS/controller revisions.
Keep all ROM inputs and staged outputs private.

All three models execute the following synthetic cases:

- Cold-start status, status consumption, and native `UJ` reset/status.
- Exact 261-byte PRG read, with an interposed status TALK after two data bytes
  followed by resumption on the still-open file.
- One-byte SEQ EOF quirk and 254-/255-byte sector-boundary reads.
- SEQ creation/write/CLOSE, native OK status, readback, and independently
  extracted bytes from the exported D64/D80/D82.

The captured one-byte stream is `X,0,2,X`, with KERNAL EOF set only on the
fourth byte, for every model. DOS 2.7 status includes the trailing drive field
that 4040 status lacks. These results independently corroborate those native
quirks rather than deriving them from current EconoPET output.

Supply the existing approved local manifest and external asset directories.
The source build needs Git, Make, GCC, Python, pkg-config, SDL2/SDL2_ttf,
fontconfig and ALSA development headers. On Ubuntu the missing development
packages are `libsdl2-dev libsdl2-ttf-dev libfontconfig1-dev libasound2-dev`.
VICE supplies the `c1541` and `petcat` fixture tools. Build only the PET target,
retaining Debug assertions. Use new, git-ignored run directories:

```sh
# Check artifact validation without requiring ROMs or emulator execution.
python3 -m unittest discover -s tools -p 'test_drive_*.py'

# Fetch the exact source with only the target's required subtrees.
git clone --quiet --depth 1 --filter=blob:none --sparse --branch mame0285 \
  https://github.com/mamedev/mame.git build/drive-mame
git -C build/drive-mame sparse-checkout set 3rdparty scripts regtests \
  src/emu src/devices src/lib src/osd src/frontend \
  src/mame/commodore src/mame/shared plugins hash

# Apply the pinned trace/interleave patch and identify the resulting Debug binary.
python3 tools/drive-ref/build_drive_reference.py --source-dir build/drive-mame --jobs 4

# Run the original-ROM slice on disposable generated media.
python3 tools/drive-ref/drive_reference.py \
  --model 4040 \
  --manifest tools/drive-roms/drive-emulation-roms.json \
  --drive-rom-dir "$DRIVE_ROMS" \
  --controller-rom-dir "$ECONOPET_MEDIA_DIR/roms" \
  --gcr-rom "$GCR_ROM" \
  --mame build/drive-mame/petd \
  --trace-build-info build/drive-mame/petd.json \
  --output-dir build/drive-reference-4040

# Build Debug firmware/framework code and the separately optimized FPGA model.
cmake --preset default
cmake --build --preset sys-test

# Replay the accepted native transactions against the exact initial image.
python3 tools/drive-ref/compare_drive_reference.py \
  --reference-dir build/drive-reference-4040 \
  --output-dir build/drive-comparison-4040
```

Repeat with `--model 8050` and `--model 8250`, each with a fresh output path.
The output includes `result.json`, unchanged `initial.*`, writable/exported
`final.*`, per-byte captures and KERNAL ST, controller command projections,
timed controller-I/O samples, BASIC operation markers, active ROM identities,
tool/harness hashes, and sector differences. Traced runs also produce
`ieee-wire.csv` and `replay.txt`. No operator disk is mounted.
The harness reserves RAM above BASIC's heap before recording results, using
the pinned [PET V4R workspace](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/BASIC_PET_V4R_REC/bassrc#L88-L124).

The MAME patch observes composed-net changes before downstream callbacks can
recursively change another net. It does not modify DOS/controller ROMs, CPU
instruction implementations, bus algorithms or clock frequencies. It selects
the PET CPU's single-cycle perfect scheduling quantum, rather than the default
coarse CPU interleaving whose callback timestamps can run backward across CPUs.
This precision setting is part of the pinned reference configuration, not a
timestamp correction. Each trace has ordered
sequence numbers, integer-attosecond timestamps, an initial snapshot and a
terminal coverage record. The controlled replay window begins at the BASIC
start marker, not at an inferred first GPIO access. Power-on activity remains
in the raw trace. Missing source coverage is an error.

The decoder requires NDAC acceptance followed by DAV release before counting
a byte. It retains aborted transfers separately, checks data/EOI stability
before acceptance, and measures readiness-to-DAV, DAV-to-NDAC,
acceptance-to-DAV-release and NDAC rearm phases. An already-released NRFD at
the observation boundary does not establish its release time. Every native
accepted byte and EOI is independently checked against PET data and KERNAL ST.

The replay executable mounts the exact reference `initial.*` bytes in an
independent writable fixture. The soft 6502 performs controller GPIO accesses
through production decoding, and production drive C services the production
FPGA over physical SPI transactions. Observation taps expose the composed
virtual bus used by that FPGA, consistently for all handshake nets. Do not
mix its acceptor edges with the separately delayed fitted PIA outputs.

`comparison.json` retains exact byte/ATN/EOI differences, individual phase times,
the executable hash, source revision/dirty paths and complete final-media
hashes/sector changes. Unsupported ordinary SEQ creation/write and its readback
are legacy limitations, not reference failures or permission to normalize
results. A blocked suffix (including native `UJ` after write/readback) remains
explicitly unobserved. D82 is reference-only until the replacement supports it.
Unexpected read/status differences fail the comparison.

These are simulation timings with different controller software pacing and no
physical SD/RP2040 execution latency. They establish protocol-phase observations,
not physical MVP latency parity. BASIC elapsed time, emulator host speed and
GPIO-access sampling remain insufficient for that conclusion. The pinned MAME
FDC source marks write-protect handling TODO, so fault coverage must be
demonstrated individually, not assumed.

#### Recorded M1 reference and baseline results

The traced Debug executable is pinned by SHA-256
`44a6187f0bf0ceaebd0030cef04ae48c8768ef2725ef3544d4012394440c706f`,
with trace/interleave patch SHA-256
`2524ceaa88ad8c145b1835f42b4bcf922269b315de13471cd29f6f3b4396b680`.
The private evidence directories are
`build/drive-rom-verification/m1-wire-{4040,8050,8250}-r4`.
Their wire events/timestamps, phase markers, byte/ST captures, and complete
final images reproduce the corresponding `r2` and `r3` runs exactly.

| Reference | Controlled-window events | Accepted transfers | Drive bytes | Drive EOI bytes | Changed sectors |
|-----------|-------------------------|--------------------|-------------|----------------------|-----------------|
| 4040 / D64 | 36,110 | 3,598 | 866 | 10 | 344, 357, 358 |
| 8050 / D80 | 38,224 | 3,654 | 880 | 10 | 1073, 1082, 1103 |
| 8250 / D82 | 38,226 | 3,654 | 880 | 10 | 1073, 1085, 1103 |

Sector numbers are zero-based 256-byte image offsets. Every reference has zero
handshake violations and zero aborted transfers. All ten drive streams match
the independent PET byte/ST captures, including final EOI. Each native write
persists `M1 ROM WRITE` followed by CR (13 bytes) after CLOSE/export.
Drive readiness-to-DAV is 27-33 us in these cases. Individual acceptance,
DAV-release, and NDAC-rearm times remain in the artifacts, not an elapsed-BASIC
proxy or a physical latency-parity claim.

The production comparisons are retained in
`build/drive-rom-verification/m1-comparison-{4040,8050}-r2`.
Their common accepted prefixes contain 3,454 D64 and 3,494 D80 transfers,
respectively, with exact bytes, ATN, EOI and direction and no unexplained
differences or handshake violations. This covers startup/consumed/interposed
status, interrupted/resumed seed reads, native one-byte EOF, and both sector
boundaries. All four protocol-phase timings are retained without adjustment.

The legacy engine accepts the write transaction but does not create `RESULT`.
Its subsequent status still matches native OK, so status alone would miss the
failure. Readback times out and the entire candidate image remains unchanged,
whereas each native image changes the three sectors above. These are known
legacy write/create limitations, not passing writes. The remaining 144 D64
and 160 D80 frames (readback and the following `UJ` reset/status) are explicitly
not compared. D82 remains reference-only.

Native executions of the operator program pass all six checks without changing
the image. The host regression suite passes, but neither result substitutes
for execution on EconoPET hardware or operator acceptance.

#### M1 operator comparison (D64/D80 available baseline)

Every generated `initial.d64`/`initial.d80` contains the executable `M1CHECK`.
The source is [drive-check.bas](../../prgs/drive-check/drive-check.bas), also
built by `cmake --build --preset prgs`. To exercise that exact executable on
the original drive, run the reference command with `--board-check`, a fresh
output directory and model `4040` or `8050`. This mode requires all six checks
to pass and the complete image to remain byte-identical.

Copy the two generated **initial** images to disposable SD-card filenames.
Mount one at a time as device 8, drive 0, using the ROM-4 BASIC controller.
Do not substitute the exported `final.*` or an operator's working disk.

The prepared copies are
`build/drive-rom-verification/m1-board-checks/m1-4040.d64` and
`build/drive-rom-verification/m1-board-checks/m1-8050.d80`, with adjacent
`SHA256SUMS`. Their complete bytes match both the differential inputs and the
images whose operator program passed on the original-ROM reference.

```basic
load "m1check",8
run
```

Enter `64` for D64 or `80` for D80. Expected output is `6 /6 PASSED`:
consumed status, status interposed after two seed bytes, exact resumed seed
data, the native one-byte EOF quirk, and both sector boundaries. The first
status is informational because loading the executable is already drive
activity, not a cold-start observation. The remaining statuses must match the
selected model's complete native text, including the DOS 2.7 drive field.
The program attempts no writes or format. If it hangs, stop after two minutes
with a short MENU press (long MENU enters configuration and remounts disks).

Record both summaries and any failure/status text, the deployed firmware
identity and controller configuration. The board comparison and explicit M1
acceptance were operator gates for M1. Neither a native-program pass nor a host
differential result closed them. The later explicit approval recorded below
authorized M2.

The operator reported **5/6 on both D64 and D80** on 2026-10-24. Consumed
status failed, while interposed status, resumed seed, one-byte EOF, and both
sector boundaries passed. The D64 failure displayed `00, OK,00,0`.
The operator identified the setup as a local build from this repository,
6502 CPU, and 8032 60 Hz ROMs. The exact flashed-image hash/revision was not
recorded. This board controller configuration differs from the reference's
CBM 4032/non-CRTC editor configuration.

The distributed checker's failure line reports data-loop `i`/`t`, not
status-loop `j`/`z`, so its `bytes 0 st 0` does not diagnose that failure.
The source now reports status and data measurements separately, retaining the
last status byte independently of subsequent data reads. Its native checks
still pass, but this diagnostic correction is not a fix for the board failure.
The published images remain unchanged to preserve their input identities.

On the existing loaded checker, insert the following line and rerun one image
to report the saved status measurements immediately after the failed check:

```basic
115 print "status bytes";j;" st";z;" last";v;" len";len(s$)
```

The status observations do not yet attribute the discrepancy to firmware,
gateware, or the controller. A production SPI/FPGA probe with three consecutive
OK reads matched all 59 returned bytes/EOI without handshake violations, but
does not reproduce the board's post-LOAD failure.

The operator's D80 diagnostic is `status bytes 9 st 64 last 48 len 9`:
the PET reports EOI on ASCII `0`, before the expected 15-byte OK response's
terminal CR. This is not merely a status-string formatting mismatch.
The production test `NativeRom4RepeatedOkStatusRetainsCrAndEoi` also passes for
both media models after a completed file transfer/CLOSE. It executes actual
ROM-4 TALK/TKSA/ACPTR/UNTALK for each byte with independently periodic MCU
service (1,000 and 64,000 FPGA cycles), rather than servicing each scripted
transaction immediately. Neither simulation reproduces the observed premature
EOI. Retain this as an unresolved legacy-system baseline discrepancy, not a
proven firmware-only defect. M1 establishes the independent reference and
baseline comparison, not full compatibility of the engine being replaced.
Repairing the legacy engine is not an M1 exit requirement. Carry this result
forward for replacement-path validation, alongside the known write limitations.
The operator accepted M1 on 2026-10-24 and authorized M2 with "Approved.
Start M2". The board baseline was built locally from this repository with a
6502 CPU and 8032 60 Hz ROMs. An exact flashed-artifact identity was not supplied.
Acceptance does not turn the legacy engine's 5/6 board results into passes or
attribute the premature EOI to a particular component.

### M2: Build the lifecycle, resources, and sector foundation

Status: **accepted by the operator on 2026-10-25**, with the explicit M6
deferrals recorded below. The replacement is the sole production
engine. `dos.c/.h` implement the ROM-structured ordinary-read/status slice,
`dos_media.c/.h` own the bounded media boundary, and `ieee_drive.c` implements
the public API and production SPI/FPGA transport directly.
The legacy DOS, proportional REL chain caches,
directory/stream helpers, engine selectors, and legacy-only tests are deleted.
Unsupported operations cannot fall back to another implementation.

The media boundary validates complete pairs before storage side effects,
assigns unit/slot-bound media references, rejects stale sector work, and retains
bindings and unflushed state after failure. A successful unit-local change
invalidates both slots' references without invalidating another unit's work.
Bulk teardown flushes all affected media before detaching any. Successful
earlier flushes remain real when a later flush fails (there is no fictional
rollback). The transport owns file handles, IEEE quiescence, and resetting the
affected unit's DOS state. DOS write finalization remains future work because
current production bindings are read-only.

The ARM layout is 68 bytes per media unit and 276 bytes for the four-unit bank.
These figures cover only borrowed media descriptors, identities, and dirty
flags, not native resource pools, file handles, FIFOs, or stacks.
The sector API uses caller-supplied 256-byte buffers and allocates no image-sized
or chain-sized storage. Shared geometry recognizes exact-size raw D82 and its
second side. The initial M2 ARM native DOS state was 3,344 bytes per unit
(13,376 bytes for four), including twelve sector buffers and the cooperative
OPEN job. Command collection has since moved into that pool (see the updated
consolidated accounting below). Both layouts are below the 4 KiB/unit target,
not complete stack/heap/SD service accounting. The initial production link
contained 38,832 bytes of BSS and no legacy chain-cache, REL-channel, or
stream-state symbols.

The read/status slice is wired through public mounts, configuration, and SPI.
Configuration uses the original unmount-all followed by sequential setup
actions. `action_mount` treats an invalid image, unavailable file, or type
mismatch with the other slot as fatal. Automatic and manually selected
configurations use the same loader. No retry/rollback path, temporary mount
plan, or complete-set replacement API remains. Unit removal preserves another
unit's GET cursor/status and withdraws the removed unit's FPGA presence.
OPEN directory search yields between sectors. Raw model-specific BAM-sector
access exists, but BAM allocation/repair algorithms do not.

The remaining implementation scope includes directory streams, wildcard and
full filename parsing, ordinary writes, REL, and DOS commands. These currently
fail explicitly with staged syntax errors, not native-parity claims. The old
REL/wildcard/write tests and their implementation-specific fixtures are retired,
not counted as replacement passes. Ordinary read, one-byte EOF, terminal
readiness, repeated ROM-4 status, model/unit status, and configuration/isolation
checks exercise the sole replacement path. Historical M0/M1 board results above
still describe the removed engine. No M2 physical acceptance is implied.

Post-removal validation passes the production firmware build, firmware unit
suite, 60 Python tool tests, and all 68 entries in `ctest --preset all --parallel 4`.
The sole replacement matches the pinned original-ROM read/status prefixes for
4040, 8050, and 8250 with 3,351, 3,383, and 3,383 accepted frames respectively,
zero unexplained differences, and unchanged read-only images. Evidence is in
`build/drive-rom-verification/m2-sole-{4040,8050,8250}-r1`.
These comparisons exclude the write-and-later suffix and do not establish
physical SD/RP2040 latency or hardware acceptance.

After restoring sequential menu loading and removing the complete-set API,
the production firmware and firmware/system test builds pass. The firmware
unit suite and all 151 firmware-labeled production-board integration cases
pass, including the 24 new model/interruption/lifecycle cases and the adapted
sequential reload case. The unit suite includes six fatal configuration cases
and all-model resource ownership and stale pending-OPEN checks. This closes
the selected M2 software-evidence work, not complete memory/service accounting
or physical acceptance.

After transport consolidation and removal of the temporary diagnostic modules,
the production firmware, firmware/system test targets, and PRG targets build.
All 68 entries in `ctest --preset all --parallel` and all 60 Python drive-tool
tests pass. The firmware test build no longer references the deleted display
suite or its printf wrapper. The shared input mock retains the deterministic
reader needed by manual-configuration failure tests and clears it on reset.
Fresh serialized read-only comparisons are retained in
`build/drive-rom-verification/m2-consolidated-{4040,8050,8250}-r2`.
They complete all 5,007 / 5,055 / 5,055 selected actions, matching
3,351 / 3,383 / 3,383 accepted frames with zero unexplained differences and
unchanged images. The earlier 4040 `r1` attempt exceeded the harness's
300-second host timeout while the full regression suite was running.
Its partial artifacts are retained, not counted as completed evidence.
The successful replays still exclude the write-and-later suffix and do not
establish physical latency parity.

#### Consolidated M2 resource accounting

The sole transport implementation now lives in `ieee_drive.c`, without an
engine wrapper or selector. The temporary heap/stack/timing probes and serial
unmount command are removed. Configuration still uses the public unmount API.

The current RP2040 ELF reserves the following fixed state, measured from its
ARM symbols with `arm-none-eabi-nm --print-size`. These are compiled sizes, not
host `sizeof` results or observations of available heap:

| State | Bytes | Scope |
|-------|-------|-------|
| Native DOS state (`units`) | 13,648 | Four units at 3,412 bytes each, including twelve sector buffers and a 59-byte command receive buffer per unit |
| Media bindings (`media`) | 276 | Four dual-drive units and the shared generation counter |
| Listener routing (`listening`) | 4 | Four transport-owned listener flags (command collection now belongs to `units`) |
| Borrowed file pointers (`files`) | 32 | Eight pointers, not the pointed-to stdio/filesystem allocations |
| TX ownership/progress (`data_queue`, `status_queue`) | 16 | Two shared queue descriptors |
| Selection and scheduling cursors | 4 | `addressed`, `talker`, `secondary`, and `next_job` |
| Total named drive state | 13,980 | Excludes linker padding, stacks, and storage-library allocations |

The DOS pool remains below the 16 KiB four-unit target without reducing native
channel/buffer counts. The FPGA's bounded storage is separate from RP2040 RAM:
32 twelve-bit RX entries, 1,024 nine-bit data TX entries, and 32 nine-bit status
TX entries, plus control state. The transport's publication buffer is 64 bytes
on the caller's stack. Sector buffers are in the native pool, not allocated
in proportion to an image or a file chain.

After the DOS/transport boundary refactor, the production ELF has text/data/bss
of 294,736 / 0 / 38,836 bytes. The earlier accepted M2 snapshot was
291,376 / 0 / 38,832 bytes.
The linker reserves 2,048 and 512 bytes for the two core stacks. Its interval
from `__end__` (`$200101B4`) to `__StackLimit` (`$20040000`) is 196,172 bytes,
not a measurement of free heap, contiguous allocation capacity, or stack
headroom. Eight open host files, their filesystem/SD state, IRQ nesting and
actual service latency require board evidence at M6 final acceptance, not
another M2 gate. Do not reintroduce
profiling merely to repeat the accepted initial sizing exercise.

#### M2 operator handoff and accepted board results

Use EconoPET Rev. B with the 6502 and the selected ROM-4 8032 configuration
at its recorded video frequency. Retain the selected BASIC/KERNAL/editor
filenames and any patches, not just the configuration's display name. Reuse
known artifact identities rather than requiring additional per-ROM hashing. Use the
same controller configuration when comparing durations with an original
drive. A host replay does not include physical SD latency.

Build the current firmware and FPGA, then record the deployed identities:

```sh
# Configure and build production firmware before identifying its artifact.
cmake --preset default
cmake --build --preset fw

# Run all simulations before building the FPGA for deployment.
ctest --preset all --parallel
cmake --build --preset gw

# Identify the exact firmware, FPGA, and SD configuration copied to the board.
sha256sum build/fw/src/firmware.elf.uf2 \
  build/gw/outflow/EconoPET.hex.bin sdcard/config.yaml
```

The last configuration hash is only the repository copy. Hash the actual
SD-card configuration after adding the fixture mounts below. Copy firmware
and FPGA using the normal deployment procedure. Do not deploy an older FPGA
with the new presence-mask firmware and count that as an M2 test.

The current production artifact identities are:

| Artifact | SHA-256 |
|----------|---------|
| `build/fw/src/firmware.elf` | `298b2c5bff4a5204963531855f3326d365468cc45cbdf0ba6d24562b78a18aa9` |
| `build/fw/src/firmware.elf.uf2` | `8339754645e6a4fcadd3a95e48af17196e5ba1b47ae77acd24fd7e186c7f8368` |
| `build/gw/outflow/EconoPET.hex.bin` | `6ef349e4ecaca315b86810bcce08f6df04ed77a363d2d68a5fa5fda8ddb857d8` |

These identify this build, not a deployment or an accepted board result.
Recompute them after any rebuild that changes the artifact.

The FPGA compile passes with Efinity 2025.2, C3 timing model and seed 0.
After adding the missing PIA1 output delays, the fitted clock-relationship
summary has positive setup/hold slack, with worst system-clock paths at
0.076 / 0.295 ns. `pia1_clock_o` shares `cpu_clock_o`'s 2 ns max / 0 ns min
output budget and has setup/hold slack of 4.430 / 3.417 ns.
There are no outputs lacking either output delays or explicit exceptions.

The 6809 NMI input is tied inactive in `main.sv`, not driven by another
periodic clock. Its unused event latch now has pin-scoped input and
through-state false-path exceptions. The ordinary Q/E samplers and active
IRQ/FIRQ paths remain timed. Efinity's structural `check_timing` still lists
two unclocked pins and two internal endpoints for that inactive latch, even
with the explicit exceptions. Revisit these exceptions if NMI is connected.
Its eight CPU-data input warnings refer to ports covered by the existing
`set_max_delay`, not input-delay constraints. These findings are accounted
for, not hidden by a fabricated NMI clock or new blanket false paths.
Retained reports are `.cache/ai/constraints-final-{check,summary,setup,hold}.txt`
and `.cache/ai/pia1-final-{setup,hold}.txt`.
This fitted timing evidence does not replace the physical M2 checks below.

Use disposable copies of these unchanged reference inputs, not `final.*` or
an operator's working disk:

| Model | Input beneath `build/drive-rom-verification/` | SHA-256 |
|-------|---------------------------------------------|---------|
| 4040 | `m1-wire-4040-r4/initial.d64` | `1275c974ee710652c4e89d6d24e7383b2e3c5a0b3ab27457e02a729e5f27f7d2` |
| 8050 | `m1-wire-8050-r4/initial.d80` | `a2756ff8183d457023b6b3b7e3fb7c5912f92cf0fa56b5bce90d5ae1b7347aaf` |
| 8250 | `m1-wire-8250-r4/initial.d82` | `95db601c0d582f106c3f898d423813a060a089eb1e2efde5d76d9262a3c86ead` |

Each input contains `M1CHECK`, `SEED`, `ONE`, `BOUND254`, and `BOUND255`.
Mount one model at device 8, drive 0, initially leaving all other slots empty.
Before loading any file after a cold boot, type and run this bounded two-read
status check:

```basic
10 open15,8,15
20 for r=1 to 2:s$="":j=0
30 get#15,a$:z=st:s$=s$+a$:j=j+1
40 if a$<>chr$(13) and z=0 and j<64 then 30
50 print s$:print "st,bytes";z;j
60 next r:close15
```

For 4040, expect `73,CBM DOS V2,00,00<CR>` (20 bytes), then
`00, OK,00,00<CR>` (13 bytes). For 8050 and 8250, expect
`73,CBM DOS V2.7,00,00,0<CR>` (24 bytes), then
`00, OK,00,00,0<CR>` (15 bytes). Every final byte must have KERNAL ST 64.
These exact strings/counts come from the original-ROM `startup` and `consumed`
observations in each fixture's adjacent `result.json`.

Then, for each of D64, D80 and D82, run:

```basic
load "m1check",8
run
```

Enter `64` for D64. Enter `80` for D80 **and D82** (the existing checker selects
the DOS 2.7 status shape with this answer, not image geometry). All six checks
must pass: consumed status, interposed status, resumed seed data, the native
one-byte EOF behavior, and both sector boundaries. The original-ROM read/status
comparisons establish the expected bytes/EOI as mapped in the evidence table
below. The checker's first status is informational because loading it already
accessed the drive. Never count a missing final EOI as a completed read.

Short-press MENU, then load and run `M1CHECK` again without selecting a
configuration. All six checks must still pass with the existing mount.
Stop a hung case after two minutes with short MENU and retain its last output
and `log debug` transcript. That two-minute stop is an operator escape, not a
native timing acceptance threshold.

Use tape-loaded `M2CHECK` to combine the remaining eight-slot read/status and
channel-reuse checks into one invocation. Configure this fixed matrix with
disposable copies of the pinned fixtures containing `SEED`, `ONE`, `BOUND254`
and `BOUND255`:

| Device | Drive 0 | Drive 1 |
|--------|---------|---------|
| 8 | D64 | D64 |
| 9 | D80 | D80 |
| 10 | D82 | D82 |
| 11 | D64 | D64 |

The same read-only fixture copy may be reused in multiple slots, but record
every binding and its initial hash. Build with `cmake --build --preset prgs`,
then copy `build/prgs/m2check/bin/m2check.prg` to SD `/prgs/m2check.prg`.
No firmware or FPGA update is needed for this program.

Keep SD2PET and other physical IEEE devices disconnected and terminal
mirroring enabled. Cold boot into this configuration and load through the
tape path before accessing any disk file (do not add `,8`):

```basic
load "m2check"
run
```

Expect `64 / 64 passed`: each unit's cold status score is `2 / 2`, and each
of its two slots scores `7 / 7`. All four cold startup/consumed-status pairs
are checked before any disk file access. Each slot then checks consumed
status, interposed status, seed/resumed TALK, native one-byte EOF, both sector
boundaries, and five-channel reuse. DOS 2.7 status expectations follow the
drive selected by OPEN, including drive 1, rather than always requiring `,0`.
The reuse case verifies five independent headers, closes and reopens one
stream, then verifies the four peers retained their cursors.

Five ordinary streams leave room for the original DOS internal directory
lookup channel (`lookup:SRCHST -> OPNIRD`, followed by `open:OPREAD -> OPNRCH`).
Do not equate six ordinary logical slots with six simultaneously admissible
filename-based OPENs. This check is not a native exhaustion proof or
simultaneous four-unit channel occupancy. Host resource-ownership tests cover
the larger native-pool cases within the documented M2 subset.

Retain the complete result screen. Short-press MENU without selecting a new
configuration, tape-load `M2CHECK` again, then run it and require the same
matrix. A warm rerun without reset legitimately fails the cold startup
checks. If an individual check makes no visible progress for two minutes,
use short MENU and preserve its last output and `log debug` transcript.
This escape is not a time limit for the entire 64-check run or a native
timing acceptance threshold. Record any timeout, status error, reset or
configuration failure.

After shutdown, rehash every disposable image and require byte-identical
contents. This program issues no disk writes or DOS commands. It does not
establish board service/handshake parity, runtime storage/stack headroom,
unit-local live media mutation, or final M2 acceptance.

The consolidated PRG SHA-256 is
`53df41030ba55f72120d217c9d791fb5233a53d92bdc6c7a0ccb3480f5f5c9d6`.
It completed 64/64 under VICE 3.9 using the PET ROM-4 BASIC/KERNAL, an 8032
editor and the pinned original 4040/8050/8250 DOS images with eight distinct,
read-only fixture copies. Each cold status score was 2/2 and each slot 7/7.
A separate negative-control PRG requested a nonexistent seed file and
completed with the expected 48 passes and 16 failures, still accounting for
all 64 checks. All eight image hashes were unchanged after both runs.
This validates the BASIC program against native ROM execution in VICE,
not production firmware/gateware or eight-slot board acceptance.

The operator then confirmed the program was copied and all eight mounts
were prepared, and reported 64/64 passing for the guided cold-boot board
run with terminal mirroring enabled and SD2PET disconnected. The operator
then short-pressed MENU without reloading the configuration, tape-loaded
the checker again, and reported another 64/64 pass.

After shutdown, the operator supplied SHA-256 hashes for SD
`/disks/initial.d64`, `/disks/initial.d80` and `/disks/initial.d82`.
All three match the corresponding pristine `m1-wire-*-r4/initial.*`
hashes in the fixture table above exactly. The eight-slot cold/read/reset
and unchanged-image checks pass. The configuration is recorded below.
The remaining timing/headroom/unit-local lifecycle measurements are explicitly
deferred to M6 final acceptance, not additional M2 gates.

The operator supplied the actual SD configuration SHA-256
`f208337633889c1909379a28107338e55ee43d9b7939b0be515f9bed1e68410e`
and its selected `pet-80xx-60hz` entry, displayed as
`PET 80xx (80 Col 60 Hz D64/80/64/82)`. Its mounts match the eight-slot
matrix above, reusing `/disks/initial.*` across each model's slots.
It selects 80 columns, 2 KiB video RAM, `/ukm/us.bin` and tape hook
`2ef415f4c9cad1d4da`. The PET ROM loads are:

| Address | File beneath SD `/roms/` |
|---------|-------------------------|
| `$B000` | `basic-4-b000.901465-23.bin` |
| `$C000` | `basic-4-c000.901465-20.bin` |
| `$D000` | `basic-4-d000.901465-21.bin` |
| `$E000` | `edit-4-80-b-60Hz.901474-03.bin` |
| `$F000` | `kernal-4.901465-22.bin` |
| `$68000` | `characters-2.901447-10.bin` |

The entry conditionally applies the repository's graphics-keyboard patches
at `$E545` and `$E6D1`. Whether that condition was active was not recorded.
No further per-ROM hash collection is required for this walkthrough.
The supplied configuration and known firmware identity are retained without
claiming independent SD ROM or programmer-loaded FPGA binary verification.

#### Cold-boot status failure (2026-10-25)

The operator loaded `drive-status.prg` through the tape path after a cold
boot, then read channel 15 twice without reading a disk file. The first
response matches the selected model. The second response reports EOI
before its terminal CR:

| Image | First response reads / ST | Second visible text | Second reads / ST | Required second bytes / ST |
|-------|---------------------------|----------------------|-------------------|----------------------------|
| D64 | 20 / 64 | `00, ok,0` | 8 / 64 | 13 / 64 |
| D80 | 24 / 64 | `00,` | 4 / 64 | 15 / 64 |
| D82 | 24 / 64 | `00,` | 4 / 64 | 15 / 64 |

Expected second responses are `00, OK,00,00\r` for 4040 and
`00, OK,00,00,0\r` for the selected 8050/8250 models. This reproduction
does not require file lookup, sector traversal, or a preceding disk LOAD.
It establishes premature controller-visible EOI, not which component
introduced it. The counter measures GET calls: a zero byte produces an
empty BASIC string, and terminal padding obscures trailing spaces. The
capture therefore does not establish the exact fourth D80/D82 byte.
Exact deployed firmware and FPGA identities remain unrecorded.

`Rom4ColdStatusGetRetainsCrAndEoi` covers the missing transition through
native ROM-4 OPENI, CHKIN, GETIN and CLRCHN. All six cases pass: three models
at independently periodic MCU service intervals of 1,000 and 64,000 FPGA
cycles. Each case requires the complete cold banner, two complete OK
responses, and EOI only on each CR. This is not a faithful reproduction of
the board: it uses the soft 6502, the catalog's non-CRTC editor and masked
interrupts. It does not establish physical PIA input levels or fitted
data-bus timing, and does not close the board failure.

A full 8032 BASIC cold-boot experiment exposed another simulator limitation:
the native editor writes `$10` to VIA ACR at `$E84B`, which the physical
I/O adapter explicitly rejects because shift-register modes are not modeled.
No guard was disabled or native editor patched to hide that limitation.
A non-CRTC full-BASIC experiment reached READY but did not complete within
its initial program budget. A longer attempt was stopped to prioritize
board diagnostics. These exploratory cases were removed, not retained as
passing regressions or evidence that the hardware failure was reproduced.

One narrow hardware observation is PIA1 port A and control A after the
failed read has returned to BASIC:

```basic
print "pa,cra";peek(59408);peek(59409)
```

After the repeated D64 failure, the operator observed `pa,cra 249 60`.
These are `$F9` at `$E810` and `$3C` at `$E811`. Control-A bit 2 is set,
so this reads port data, not the direction register. Port-A bit 6 is high,
so physical EOI is released while idle. This rules out persistently low
idle EOI and wrong data/direction selection, not a transient false EOI
sample during a transfer. The current gateware substitutes port A only
while asserting virtual EOI. Replacing every port-A read with synthesized
data is not a justified correction and would also replace cassette sense
inputs with constants.

The next controlled board comparison is the same cold-boot check with
`cpu: "6502"` added to the active configuration's existing `action: "set"`
block. This explicitly selects the soft core. An omitted CPU setting
defaults to `"auto"`, selecting the physical CPU when detected.
Keep the ROMs, image, firmware and FPGA unchanged and record the original
CPU setting and both status/read-count results. The CLI has no CPU-select
command. A soft-core pass would narrow the investigation to differences
such as external bus sampling or CPU-dependent execution cadence, not
establish either as the root cause. A failure would leave the shared
digital path and full BASIC/editor execution to investigate. This comparison
was subsequently performed: the soft CPU returned the same D64 startup
response (20 reads/ST 64), then `00, ok,0` at 8 reads/ST 64. The failure is
therefore not specific to the socketed CPU's external read sampling.
Both CPUs still share the physical PIA and the virtual IEEE implementation.
No production correction is justified by these observations alone.

Before extending simulation or making further hardware hypotheses, verify
the deployed FPGA artifact. The locally available raw image
`build/gw/outflow/EconoPET.hex.bin` was generated at 00:14:21 UTC on
2026-10-25, after the local prototype pin swap. Its SHA-256 is
`1b71fad5fbe8a2d5af17bd3245de2a690f20324e69cc1760fdd2c013ffc39aaa`.
The corresponding pinout has jiffy on pin 74 and PIA1 PHI2 on pin 78.
The mapped netlist contains the current 12-bit IEEE RX entries and unit
mask, not the older 11-bit RX layout. No current top-level gateware source
file has a modification time newer than that raw image, although timestamps
alone do not prove exact source identity.

`fw/src/main.c` reads `/fpga/EconoPET.hex.bin` from the SD card during FPGA
configuration. Flashing a new firmware UF2 does not update this file.
The CMake-generated image is under `build/gw/outflow`, not the older
`gw/EconoPET/outflow` location. Also check the boot log for
`FPGA config skipped: Programmer attached`: when the programmer holds
CRESET high, firmware skips the SD image and retains the programmed FPGA.
Compare the SD image's hash with the generated raw image, then power-cycle
with programmer configuration accounted for. The actual deployed artifact
remains unverified.

Buffered status diagnostics now record the native unit index (`u0` means
device 8), initial staged length (`n`), whether the final queued byte carries
EOI (`e`), and that byte's hex value (`t`, normally `0d`). DEBUG records
add the staged suffix's native position (`p`) and text without its CR.
GET records report the previous position, actual consumed-byte delta,
remaining staged count (`q`) and modulo-256 FPGA GET counter (`c`).
INFO retains complete-response and CLOSE-position records independently
of the small DEBUG ring.

After flashing the diagnostic firmware, cold boot and run `drive-status`
once. Exit terminal remote mode with Ctrl+C and run `log debug` immediately.
For D64, expect initial lengths 20 then 13, EOI set on their final `0d`
bytes, and GET progress reaching each terminal CR. If the board again
stops after eight OK bytes, the queued suffix and CLOSE cursor help
distinguish a short native response from an early controller-visible EOI.
These logs do not directly measure physical EOI or prove what the FPGA
drove during the failing read. They use existing rings, add no SPI reads
and do not stream to the terminal during transfers, but formatting still
changes MCU service cadence. Record if enabling diagnostics changes the
failure. No additional heap/profile machinery is required.

The initial buffered diagnostic firmware was `build/fw/src/firmware.elf.uf2`, SHA-256
`450caf5298b3e17b89d4f37f2d5fc96d574ba6fa2710da84b84e0341503865ed`.
It does not require a gateware change and is instrumentation, not a
correction for the premature EOI.

The operator's soft-6502 capture with this firmware recorded a complete
20-byte startup status, followed by a 13-byte OK status with terminal
`0d` and EOI requested. Every OK GET advanced by exactly one byte:
FPGA counters 21 through 28 corresponded to native positions 0 through 7.
CLOSE then recorded `p8/13`. Thus native formatting was not short and
native progress did not skip five bytes. The retained count is firmware's
staging accounting, not an independent measurement of physical FIFO
occupancy. These logs do not establish whether all SPI writes arrived,
which FIFO EOI flag the FPGA presented, or what PIA port A returned.
The `e1` field describes the final byte of a staged suffix, not its head.

The firmware-only A/B experiment replaces status `WRITE_SAME` bursts
with individual absolute SPI writes, preserving status bytes and EOI
ordering. File data retains its burst path. Its current
`build/fw/src/firmware.elf.uf2` SHA-256 is
`2d03fdae6fe5d03d13806006154fbfbdc91e8590cffedc940779d1c57ec48b0e`.
Initial status INFO records end in `abs` to identify this variant.
Repeat the same cold-boot check and immediate `log debug` capture without
changing gateware, ROMs, CPU or disk image. This experiment is not a proven
correction. A changed result would implicate publication or changed
execution cadence, not establish a particular SPI or EOI defect.

The operator's subsequent capture identified this variant with the `abs`
marker and reproduced the same failure. Startup completed at 20 bytes.
The OK response advanced once per GET from counters 21 through 28, then
CLOSE again recorded `p8/13`. Individual absolute status writes therefore
did not eliminate the truncation. This weakens a burst-specific explanation,
but does not independently verify SPI delivery or rule out the shared SPI
and downstream paths. The absolute-write variant is not a correction.

The next proposed diagnostic was to distinguish virtual EOI from the actual
PIA1 port-A sample on the failing byte. In the current RTL, `T_DAV` latches
the selected FIFO entry's EOI flag into `dev_eoi_n`, and the talker holds
it through NDAC acknowledgement. The PIA1 read is synthesized only when
that virtual EOI is asserted. Otherwise it falls through to the physical
PIA, even with the soft CPU selected. A retained capture must correlate
the status GET counter, virtual EOI, selected read source and actual
CPU-visible PA6 sample. Reading the idle PIA after UNTALK or adding more
native suffix logs cannot supply this missing observation. No such capture
was obtained before the physical-drive isolation check below.

The operator then tested the same BASIC program against SD2PET
`V1.0.0TFW8B`. It returned its complete 28-byte startup response and a
complete 13-byte OK response, each with ST 64. Crucially, the earlier
internal-drive failures also stopped after 28 total GET calls: D64 at
20 + 8, and D80/D82 at 24 + 4. The operator confirmed that SD2PET was
connected and powered during those failing internal-drive tests.

With SD2PET disconnected from IEEE-488, the operator repeated the internal
D64 cold-boot check and obtained the complete `73,CBM DOS V2,00,00\r`
banner (20 reads/ST 64), followed by `00, OK,00,00\r` (13 reads/ST 64).
This confirms interference from the attached physical drive. The exact
28-byte boundary and conditional PIA1 interception identify physical EOI
leakage as the supported explanation: virtual EOI terminates the internal
banner, but subsequent non-EOI reads can see the physical drive finish its
own 28-byte banner. No FIFO redesign or DOS-formatting change is required
to resolve this reproducer with the physical drive disconnected.

The current `ieee.sv` interface explicitly requires no devices on the
physical connector while emulation is enabled. It does not provide complete
physical EOI isolation despite replacing other IEEE read paths. Keep the
physical drive disconnected for internal-drive acceptance checks. This
result does not establish simultaneous physical/virtual-drive support,
nor justify synthesizing cassette-sense inputs as constants.

During the guided M2 board checks, the operator subsequently ran D64
`M1CHECK` with SD2PET disconnected and reported all six checks passing:
consumed and interposed status, seed/resumed TALK, native one-byte EOF,
and the 254/255-byte boundaries. Its informational first status after LOAD
was `00, OK,00,00`.

The immediate short-MENU reset-retention check failed with only 2/6
passing. Consumed and interposed status still passed, but seed/resumed
TALK stopped at 89 bytes/ST 2, native one-byte EOF at 3 bytes/ST 2,
the 254-byte boundary at 3 bytes/ST 2, and the 255-byte boundary at
1 byte/ST 2. ST 2 is a controller input timeout, distinct from the
earlier external-drive premature EOI. No root cause is established.
Pause acceptance at this failure and retain logs before another reset
or configuration change. D80/D82 and eight-slot checks remain pending.

The captured failure log identified the soft 6502, the `abs` firmware
variant and `FPGA config skipped: Programmer attached`. The MCU therefore
did not load the SD FPGA image during this boot. Status GETs reached the
terminal CR, but the existing instrumentation did not record file-data
transfers.

A controlled repeat of short-MENU reset and D64 `M1CHECK` passed 6/6 when
the operator left terminal remote mode immediately after answering `64`,
waited 30 seconds in the CLI, then returned to remote mode to read results.
Disabling mirroring removed the timeouts. The production display path
synchronously renders and flushes a full screen between IEEE service calls,
so terminal output can starve drive service. The evidence does not establish
a reset-state defect independent of terminal rendering.

A firmware-only candidate now flushes terminal output and calls the existing
IEEE task every 16 rendered PET cells, plus once at frame completion.
Firmware configuration/fatal windows do not call the drive task, avoiding
recursive entry from error display. Terminal bytes, geometry and character
mapping are unchanged, as are the FPGA, FIFO protocol and reset sequence.
The candidate `build/fw/src/firmware.elf.uf2` SHA-256 is
`5bb89b91be2400b92405204716e7369049fed31574646eac76be4be8abdf1b3c`.
Four new display regressions pass: bounded flushed chunks with alternating
reverse and VT-100 graphics at 80 and 510 columns, an empty frame, and a
firmware window that must not service IEEE. The ordinary firmware suite
also passes. Eight existing terminal-output expectations fail both before
and after this change because they expect an extra clear-to-end escape
sequence. That separate display discrepancy was not changed or counted as
passing. Board confirmation with mirroring enabled is still required.

The operator flashed this candidate and reported normal boot. With terminal
mirroring enabled throughout, D64 `M1CHECK` passed 6/6 after boot and again
passed 6/6 after a short MENU reset without reloading the configuration.
This verifies the D64 reset-retention correction for the observed
mirroring-dependent timeout. D80/D82, eight-slot operation and the remaining
service/headroom/lifecycle evidence are not implied by these two passes.

Guided board results for this candidate (SD2PET disconnected, terminal
mirroring enabled throughout):

| Fixture | Cold startup / OK reads (both ST 64) | M1CHECK | M1CHECK after short MENU reset |
|---------|--------------------------------------|---------|-------------------------------|
| D64 | 20 / 13 | 6/6 | 6/6 |
| D80 | 24 / 15 | 6/6 | 6/6 |
| D82 | 24 / 15 | 6/6 | 6/6 |

#### M2 acceptance and M6 deferrals (2026-10-25)

The operator accepted M2 as the read/status and lifecycle foundation, with
explicit M6 deferrals. Evidence includes all three models' cold-status and
6/6 read/reset checks, eight-slot `M2CHECK` at 64/64 both cold and after short
MENU reset, unchanged disk images, and the existing ROM-derived unit,
original-ROM differential and production firmware/gateware integration cases.
The bounded static DOS state is sufficient resource evidence for this stage.
No additional profiling, per-ROM hashes or board tests are required to close M2.

The following checks remain unmeasured and must be completed at M6 final
acceptance before the MVP. They are deferred, not passed:

- Observe RP2040 service/handshake timings on the board with the actual SD
  card and compare equivalent phases with the pinned original-drive reference.
  Record unavailable instrumentation as blocked, not a zero latency.
- Establish runtime storage/stack headroom for the eight-mounted-file case.
  The static accounting above is not a measured runtime peak.
- Observe unit-local remount/rejection and peer-cursor preservation on hardware.
  The existing configuration menu unmounts all units by design, and there is
  no serial live-remount command. Production SPI/FPGA integration cases cover
  these APIs in simulation, not on the board. A board debugger or other
  explicitly agreed access to the existing API is needed for this case.

Retain the existing results and configuration evidence. Collect the additional
measurements with their M6 cases, not as more M2 bookkeeping.
Do not count directories, wildcard
opens, writes, REL or OS-9 as M2 acceptance cases: those features remain staged.
M2 is closed and M3 is the next milestone. This acceptance does not claim
M3 functionality is implemented or waive its separate acceptance cases.

The operator reconfirmed the existing adaptation approval on 2026-10-24 and
instructed implementation to proceed.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| D82 track zones and size | Approved 8250 MPI mode, DOS 2.7 | Pinned [`begin`, `CONFI1-CONFI5`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/begin#L101-L164) and [`romtbl`, `NTRK80/NSEC80`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/romtbl#L137-L142) | Tracks 1-154, repeating the 77-track side geometry, 4,166 sectors, 1,066,496 raw bytes. Track 78 starts at byte 533,248. Invalid coordinates leave geometry output unchanged. | `diskimage_track_sectors/sector_offset/open`, `test_d82_geometry_covers_two_exact_native_sides`, `test_sector_bounds_and_read_only_fail_before_write` | Pinned MAME [`d82_dsk.cpp`](https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/lib/formats/d82_dsk.cpp), [`d80_dsk.cpp`](https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/lib/formats/d80_dsk.cpp), and `d64_format::get_image_offset/load` |
| Complete-pair model selection and mixed-pair rejection | EconoPET host policy, not a native command | Approved mount/model policy above | Either single-slot position and all three homogeneous pairs select the corresponding model. Reject all six ordered mixed pairs before flushing or changing bindings. | `dos_media_validate_pair/set_unit/set_slot`, `test_single_slot_selects_model_and_keeps_partner_absent`, `test_complete_pair_validation_is_side_effect_free` | Operator-approved plan, no native-parity claim |
| Remount, removal, and media identity | EconoPET host policy, not native initialize/reset | Approved lifecycle contract above | Both old references in a changed unit become stale. Other units remain usable. An empty requested slot never aliases a populated slot. | `dos_media_reference/read/write`, `test_remount_invalidates_unit_without_cross_unit_reset`, `test_unit_removal_preserves_other_unit_and_does_not_redirect`, `test_open_job_rejects_remounted_media_before_io`, `RemountDiscardsOldSourcesAndReopensReplacement`, `NativeUnitRemovalPreservesPeerGetCursorAndStatus` | Media callback counts plus production SPI/FPGA CPU and complete passive wire windows, not physical hardware acceptance |
| Write and flush failure retention | EconoPET storage boundary, not native error mapping | Approved persistence/failure contract above | Failed writes remain dirty even if partially applied. Flush failure retains mounts and unflushed state. Teardown does not detach an earlier unit before a later unit's failure is known. | `dos_media_write/flush_unit/unmount_all`, `test_partial_write_and_flush_failure_retain_old_media`, `test_second_slot_flush_failure_keeps_complete_pair`, `test_bulk_teardown_retains_bindings_then_invalidates_jobs` | Independently injected backing-write and flush failures, no power-loss atomicity claim |
| Ordinary channel and buffer limits | Approved 4040 revision 2 and shared 8050/8250 DOS 2.7 | Pinned [`DOS_4040/equate`, `MXCHNS/BFCNT/CMDCHN/ERRCHN/LXINT`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/equate#L16-L31), [`dskint`, `DSKIN1/DSKIN2`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/dskint#L214-L250), [`DOS_8250/equate`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/equate#L13-L26), [`begin`, `DSKIN1/DSKIN2`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/begin#L43-L70) | Eight logical indices with command/error reservations at 6/7 leave six ordinary channels. `LINUSE=$3f` and `BUFUSE=$f000` initialize six logical slots and twelve data buffers shared across both disks. BAM buffers are separate from this data pool. | `DOS_CHANNEL_COUNT`, `DOS_FILE_CHANNEL_COUNT`, `DOS_BUFFER_COUNT`, `dos_reset`, `test_getrch_fndlnx_and_frechn_own_finite_resources` (all three models) | M0 pinned source-to-selected-ROM mapping, not an unselected startup listing |
| Ordinary allocation, release, and bounded directory work | Same selected models | Pinned [`DOS_4040/tst3`, `GETRCH/FNDLNX/GETBUF/FRECHN/RELBUF`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst3), [`DOS_8250/tst3`, `GETRCH/FNDLNX/JGETBF/FRECHN/RELBUF`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst3), [`DOS_4040/opchnl`, `OPNRCH`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/opchnl#L1-L10), [`DOS_8250/opchnl`, `OPNRCH`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/opchnl#L1-L10) | Claim the lowest free logical index and highest free data buffers, two per ordinary read. Exhaustion reports 70. CLOSE permits reuse without changing peers. The host adaptation's directory job yields with its temporary buffer retained. | `dos_open/work/close`, `test_getrch_fndlnx_and_frechn_own_finite_resources`, `test_open_job_yields_between_directory_sectors`, `PeerOpenRebuildsSharedFifoFromRetainedCursor` | Explicit buffer indices, alternating local disks, resource reuse and production shared-FIFO observations (not full parser/resource-pressure parity) |
| Raw 4040 BAM access | Approved 4040 revision 2 | Pinned [`DOS_4040/tst2`, `INITSU/INITDR`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2#L218-L247), [`tstfnd`, `SETBMP`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tstfnd#L127-L133) | The on-disk BAM is 18/0. `SETBMP` selects an in-RAM BAM pointer, not its disk coordinates. One valid raw part performs one 256-byte read against the bound slot. | `dos_media_read_bam`, `test_model_bam_access_is_bounded_and_media_bound` | Independent raw offset 357 * 256, callback counts, stale-reference and invalid-part rejection |
| Raw single/double-sided DOS 2.7 BAM access | Approved 8050 MPI single-sided and 8250 MPI double-sided | Pinned [`DOS_8250/romtbl`, `BMT80S/BMS80S/BMT80D/BMS80D`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/romtbl#L144-L152), [`begin`, `CONFIG/CONFI1/CONFI2`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/begin#L84-L148), [`map`, `NEWMPV/BAMOUT`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/map#L12-L101) | `NSIDES` selects and copies the tables into `BAMTRK/BAMSEC`. Initial BAM parts are 38/0 and 38/3, plus 38/6 and 38/9 for double-sided mode. The final table entry 39/1 is a directory link, not another BAM part. | `dos_media_read_bam`, `test_model_bam_access_is_bounded_and_media_bound` | Independent raw offsets from 37 * 29 * 256, one callback per part. Raw fixed-location access only, not BAM traversal/allocation/repair |
| Interrupted presentation and busy media rejection | Same selected native models plus EconoPET host mutation policy | Pinned [`DOS_4040/ieee`, `TLK10-TLK35`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ieee#L165-L186), [`DOS_8250/ieee`, `TLK10-TLK35`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ieee#L166-L187) | ATN before NRFD assertion aborts without GET. A GET reached after NRFD assertion remains consumed even if NDAC has not accepted the byte. Host mount/replacement/unmount must reject active DAV or ordered RX without withdrawing the presentation or deleting commands. | `quiescent`, `sync_queue`, `BusyMutationAndAtnRetainExactNativeGetProgress`, `PendingCloseRejectsMutationAndRemainsExecutable` | Twelve production SPI/FPGA cases cross three models, data/status, and before/after GET. Exact CPU bytes, EOI and passive DAV-edge records. Data cases cross both a sector boundary and the 8-bit GET-counter wrap |
| Status GET and one-byte EOF | Same selected models | Pinned [`DOS_4040/tst4`, `GETERC/SEQGET`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4), [`DOS_8250/tst4`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst4), respective `erproc` and `opchnl OR30` | Status clears on CR GET, not publication. DOS 2.7 adds the drive field. Ordinary one-byte files send X,0,2,X with EOI only on the fourth byte. | `dos_status_stage/get`, `dos_stage/get`, `test_geterc_retains_partial_status_and_consumes_cr`, `PreservesNativeEofBugOverProductionTop`, `NativeRom4RepeatedOkStatusRetainsCrAndEoi`, `Rom4ColdStatusGetRetainsCrAndEoi` | Pinned full-controller MAME wire references retained under M1, actual PET ROM-4 status calls through production SPI (cold transition uses OPENI/CHKIN/GETIN/CLRCHN, not the board's complete BASIC/editor execution) |
| Sequential configuration and fatal menu errors | EconoPET host policy, not a native DOS command | Operator-approved original flow, production `menu_enter -> menu_config_show -> load_config -> action_mount` and `fatal_no_alloc` | Halt the CPU, unmount all old disks, then execute setup actions in order. Per-mount type checks reject mixed pairs. Invalid default/manual selections reach the existing non-returning fatal screen, not PET reset or a retry loop. Valid homogeneous pairs replace an old pair in either mount order. | `load_config`, `action_mount`, `test_configuration_mount_failure_is_fatal_after_settings`, `test_configuration_unmounts_then_mounts_selected_disks`, `NativeRejectedMountRetainsCursorBeforeSequentialReload` | Six forked fatal cases cross direct/default/manual paths with mixed or missing media. Valid cases observe sequential mount callbacks and old-unit absence. No machine rollback claim |
| Drive reset ownership | EconoPET firmware reset policy, not a native DOS reset command | Approved lifecycle contract and production `pet_reset -> ieee_drive_reset` | Retain mounted file owners, retire pending RX and staged sources, reset DOS channels and startup status for every unit. | `ieee_drive_reset`, `DriveResetRetainsMediaAndCancelsOldSourcesAndRx` | Production SPI/FPGA drive-reset API on all three models, not a complete physical PET reset test |

The resource citations follow `master -> dskint` for the selected 4040 source,
not the separate `DOS_4040/init` listing (which is not in that composition).
Both selected `equate` files also specify `CMDLEN=58` and `MXFILS=5`.
The C receive buffer retains a 59th byte to detect overlength, matching the
approved revision-2 `tst2 L42` overlay and DOS 2.7 `tst2 L42` bound.
`DOS_FILENAME_COUNT=5` records the source limit but does not claim an implemented
five-filename parser/table in this M2 exact-name OPEN subset.
Raw BAM access uses caller-owned sector storage. Persistent BAM state and its
algorithms are not yet implemented or included in the current DOS-state size.

Implement isolated per-unit state, slot/media ownership, native finite
resources, D82 geometry, and model-specific sector/BAM access. Use the sole
replacement read/status vertical slice through the real facade and SPI/FPGA
path. Implement only the parser/channel subset needed for that slice, then
extend it in M3 rather than creating a throwaway second path.

Implement pair validation and the approved mount/remount/removal failure
contract across all entry points. Reject unsupported image formats without
changing existing media. Do not delete user media or silently convert images.

There is no development engine selector or legacy fallback. Fail unfinished
operations explicitly according to the staged contract. Do not claim native
parity for temporary unsupported-feature errors.

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

#### M3 implementation

The sole `dos.c` engine now implements ordinary SEQ/PRG/USR creation and
incremental writes, SAVE, append, replacement, persisted CLOSE, wildcard lookup,
formatted directories with filename/type filters and dual-drive selection,
and raw directory/header/BAM streams. General DOS commands and REL remain M4
and M5 work. There is no alternative engine or fallback.

Allocation follows the selected model's BAM ranges and links, data/directory
interleave, track-search order, count/bitmap checks, and final-sector rules.
The selected 4040 leaves an allocated extra block at an exact full-sector
boundary. DOS 2.7 frees that speculative block on CLOSE. Empty writes produce
CR, and a singleton initial sector retains the previously established
four-byte EOF quirk. These are native behaviors, not normalization targets.

The twelve native data buffers remain shared across the unit. Filename OPEN
holds the two internal lookup buffers until the target channel is allocated,
so five simultaneous filename streams leave no resources for a sixth OPEN.
A formatted directory reserves an internal logical index and a third buffer.
Two bounded BAM-sector caches are additional implementation overhead, not new
file buffers. A static assertion limits the complete unit to 4 KiB plus those
two sectors. No heap-usage profiler or new runtime measurement gate was added.

Directory search and append traversal yield between sectors. Storage callbacks
are synchronous, and replacement CLOSE still follows the old chain
synchronously. Physical worst-case service parity remains an M6 gate, not a
claim derived from host execution.

The facade prefers `r+b`, falling back to `rb`. Files are unbuffered so independent
stdio handles cannot retain conflicting sector caches. CLOSE finishes data,
replacement-chain deletion, directory read/modify/write, BAM persistence and
flush before releasing ownership. Failed host writes/flushes retain their phase,
buffers and file owner for retry. Mount/unmount reject pending OPEN work and
finalize writers before publishing new bindings. PET reset retains a failed
writer rather than erasing the only recoverable copy.

Host I/O error retention is an explicit backing-store policy, not emulation of
every controller error/retry sequence. Errors map to generic native 23/25, with
the failing coordinates where available. A native DISK FULL abort instead
releases the ordinary channel and leaves its unclosed entry. DOS 2.7's
less-than-three-free-block warning does not abort a successful allocation.
ENDCMD clears that warning after ordinary OPEN/CLOSE, while CLOSE 0/1 bypasses
ENDCMD and preserves status. A failed directory extension cannot finalize
through uninitialized coordinates into the disk header.

#### M3 ROM-to-C evidence

All source links below use `mist64/cbmsrc` commit
`01bd60f162ef92212ef0cb67546ae8f42be34168`, the approved 4040 revision-2
overlays, and shared DOS 2.7 in the selected single/double-sided mode.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Names, modes and SAVE | Selected 4040 and 8050/8250 | Pinned [4040 `open`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/open), [DOS 2.7 `open`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/open), `OP40/OP45/CKTM/OPFIN`, respective `setdrv:SETDRV` and `lookup:COMPAR` | Parameter initials select type/mode, SA 0/1 force LOAD/SAVE, names use 16-byte padding, `*`/`?` match natively, and SAVE remembers the program link | `parse_name`, `dos_open/work`, `test_save_override_last_program_and_status_retention`, boundary/wildcard readback tests | Original-ROM write/readback replays and native `m3check` SAVE/VERIFY |
| Write errors and unclosed reuse | Same selected models | Respective `open:OP75/OP77/OP81/OP82/OP115` and [DOS 2.7 `scrtch:DELDIR`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/scrtch) | Existing closed W gives 63, plain wildcard W gives 33, unmatched wildcard replacement gives 64. Unclosed exact W deletes only the old entry, not its abandoned chain | `dos_work`, `test_write_errors_and_unclosed_recreation` | [Commodore DOS overview](https://www.pagetable.com/?p=1038) and original-ROM operator/reference executions for ordinary writes |
| Native allocation and BAM | 4040/D64, 8050/D80, 8250/D82 | Respective `frets:INTTS/NXTTS/FNDNXT/FRETS/USEDTS`, [DOS 2.7 `map:JSCRBM/NUMFRE`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/map), `setmap:SETMAP/MBAM/AVCK`, `trnsfr:NXDRBK` | Follow real BAM links/ranges, native rotational wrap/interleave, paired free counts/bits and directory growth order | `bam_entry`, `allocate_sector/on_track`, `flush_bam`, native boundary and directory-growth tests | Byte-identical complete D64/D80/D82 outputs from original-ROM replay |
| Incremental PUT and CLOSE | Same selected models | Respective `tst2:WRT0`, `close:CLSWRT/CLSDIR/CLSCHN/CLOSE`, `opchnl:OPNWCH` | EOI does not close the file. Empty writes store CR. Final link is 0/(payload+1), with the model-specific exact-boundary extra-block behavior | `dos_listen_put`, `advance_write`, `finish_channel`, `test_native_create_close_and_boundaries`, `test_put_write_failure_retries_without_losing_data` | Complete original-ROM write/readback wire and image comparisons |
| Append and replacement | Same selected models | Respective `open:APPEND/OP82/OPWRIT`, `close:CLSDIR`, `scrtch:DELFIL` | Append preserves the prefix. Replacement reserves its alternate chain and frees the old chain on CLOSE without overwriting peer directory changes | `append_work`, `open_write`, `finish_channel`, `test_native_append_replace_and_peer_directory_entries`, `test_append_lookup_yields_and_failed_read_preserves_media` | Native operator checker plus production SPI/FPGA `OrdinaryWritesPersistBothDisksAndRetainInterruptedReadback` |
| Formatted and raw directories | Same selected models | Respective `open:LOADIR/OP04`, `parsex:TC35`, `setdrv:FS1SET/ALLDRS`, `lstdir:STDIR`, `trnsfr:GETNAM/MSGFRE` | Load address 01/04, dummy BASIC links, exact padded fields, native type/name filtering, dual-drive rows and final EOI. Nonzero SA exposes the linked raw payloads | `open_directory`, `prepare_directory_line`, `test_formatted_directory_retains_exact_rows_and_final_eoi`, `test_directory_type_filters_patterns_and_raw_chain`, `test_dual_drive_writers_and_directory_selection` | Native checker directory bytes and [Commodore DOS overview](https://www.pagetable.com/?p=1038) |
| Exhaustion, warnings and status | Same selected models | Respective `tst3:GETRCH/GETBUF`, `parsex:ENDCMD`, `close:CLS10`, `erproc:CMDER3/TLERR`, DOS 2.7 `frets:USEDT3` | Sixth filename OPEN gives 70. Actual disk-full abort releases its channel. DOS 2.7 warning permits the write. CLOSE 0/1 preserves status | `allocate_channel`, `dos_close`, `dos_listen_put`, `test_near_full_warning_and_full_channel_release`, resource and SAVE tests | Original-ROM operator checker exercises the five/six-stream boundary on each model |
| Paused cursors and exhausted reads | Same selected models, PET KERNAL `901465-22` | Respective `ieee:TALK1/TLK30`, `opchnl:OR30`, pinned [4040 `tst4:GET0/GETERC`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4), [DOS 2.7 `tst4:GET0/GETERC`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst4), and [PET `ob1src:GETIN/ACPTR/ERRP1/ER001`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/KERNAL_PET_4.0_REC/ob1src) | Status selection and reopening one stream preserve peer cursors. Consumed sequential EOF clears readiness, not replays the last byte. Fresh ROM 4 GET selections with timeout defeat disabled return ST 2 and the KERNAL's synthetic CR when the exhausted drive sends no data | `prgs/m3life/m3life.bas`, `DriveTerminalTest.SequentialRetalkAfterEofDoesNotInventData` | Read-only checker on original-ROM MAME with simultaneous 4040, single-sided 8050 and double-sided 8250 |
| Backing-store fault retention | EconoPET host policy | Native persistence ordering above plus the approved host media-lifetime contract | Partial data/link/directory/BAM writes or failed flush retain recoverable state. Retry does not duplicate payload or erase peer entries. Failed mount/unmount/reset does not discard the owner | `finish_channel`, `dos_finalize`, `ieee_drive_mount/unmount_all/reset`, CLOSE and growth fault tests, `test_failed_write_survives_lifecycle_and_remounts_persisted_bytes` | Independently mutable FILE fixtures and public-facade remount/readback, not claimed original-controller fault parity |

#### M3 validation record

Production firmware builds with the root production preset. The Debug firmware
runner passes 304 ordinary checks and 53 fatal-path checks. All 158 production
firmware/SPI/FPGA integration cases pass, including writable persistence across
both disks, append/replacement, wildcard readback and an interposed status TALK.
The reference-tool unit suite passes 66 tests. The separate display suite still
has seven pre-existing output-expectation failures and is not recorded as passing.

The original-ROM ordinary-file replay excludes the later M4 `UJ` transaction,
but retains the complete captured write/CLOSE/status/readback prefix and its
final UNTALK/ATN release. It does not synthesize a different transaction script.
Each comparison checks every accepted byte, ATN, EOI and direction, then compares
the complete exported image. Source reference inputs remain unchanged.

| Model | Accepted frames | Unexplained differences | Persisted image |
|-------|-----------------|------------------------|-----------------|
| 4040 | 3,504 | 0 | Complete D64 identical |
| 8050 | 3,544 | 0 | Complete D80 identical |
| 8250 | 3,544 | 0 | Complete D82 identical |

Local reports and full traces are under `.cache/ai/m3-4040-verified/`,
`.cache/ai/m3-8050-verified/` and `.cache/ai/m3-8250-verified/`. These are local ignored
artifacts, not portable test dependencies. Reproduce from the M1 reference
instructions above using `tools/drive-ref/compare_drive_reference.py --ordinary-files`.
This proves the captured scenario, not every M3 parser/fault edge case or
physical hardware timing.

The actual `m3check.prg` also passes 13/13 on each original-ROM MAME model
(39 native checks), and `c1541` independently extracts an identical saved PRG
from each exported image. Screens, completion mailboxes and images are under
`.cache/ai/m3-check-{4040,8050,8250}/`. Initial runs reached the existing
two-million-event controller-register log cap. Those logs are explicitly
incomplete, not wire-comparison evidence. `--ordinary-check` relies on the
completed checker/mailbox, verified active ROMs and extracted persisted SAVE,
and reports register-log completeness separately. Full transaction comparisons
remain the independently captured replay evidence above.

The read-only `m3life.prg` passes 12/12 with all three original-ROM MAME
peripherals attached together, including both 4040 slots. The completion
mailbox is `255,12,12`, and all four mounted images remain byte-identical.
The controller-register log is complete for this run, but no differential
wire comparison is claimed. Local artifacts are under `.cache/ai/m3life-native/`.

#### M3 operator results (accepted 2026-10-26)

These are reported board results, separate from the original-ROM and automated
evidence above. The operator agreed to reduced D80/D82 board coverage (drive 0
only), rather than repeating the writable checker on both slots of each model.

| Device/drive | Image | M3CHECK | VERIFY after short reset | VERIFY after configuration remount |
|--------------|-------|---------|--------------------------|------------------------------------|
| 8/0 | D64 | 13/13 | OK | OK |
| 8/1 | D64 | 13/13 | OK | OK |
| 9/0 | D80 | 13/13 | OK | OK |
| 10/0 | D82 | 13/13 | OK | OK |
| 11/0 | D64 | Not reported | Not reported | OK (additional operator check) |

Both returned device-8 images were independently inspected with `c1541`.
Each contains `M3SAVE` identical to the 3095-byte deployed checker,
`M3APPEND` equal to bytes 0..21, and `M3N509` equal to 509 bytes cycling
0..255. Both report 636 free blocks. D80/D82 exports have not been reported.

The separate `m3media.prg` passed 6/6 on device 11 with a FAT-read-only D64 in
drive 0 and a writable, zero-free-block D64 in drive 1. This covers rejected
creation and append on the protected image, unchanged protected data, error
72 on full-image creation, release of the rejected OPEN's channel, and
readable full-image data afterward. This tests EconoPET's host-read-only
policy, not native physical write-protect sensor coordinates.

The operator selected completing the remaining lifecycle board checks rather
than accepting the existing results alone, then reported `m3life.prg` passing
12/12 on the board. This covers simultaneous streams on both device-8 slots
and device 9/10 drive 0, status interposition, reopening one slot with peer
cursors paused, repeated exhausted retalks and recovery after reopening.
The agreed board checks have passed. The operator explicitly approved M3
acceptance on 2026-10-26 with the documented coverage limits: D80/D82 drive 0
only, no independently exported D80/D82 images, and no reported device-11
13/13 writable run. The original both-slot D80/D82 gate is not recorded as
fully exercised. Acceptance was recorded at revision
`d5ee14e3ebaee8496d56262b7f46c2ace8045e71` with a dirty worktree (96 modified
or untracked paths), not as acceptance of that commit alone. M6's deferred
measurement and final-acceptance gates remain unchanged.

#### M3 disposable-image operator handoff

M3 is implemented and **operator-accepted with the coverage limits above**.
The following procedure is retained for reproduction. Run one operator stage
at a time and wait for its result before advancing. Keep the accepted M2 gateware,
prototype pin overrides and controller setup. Deploy the new production
firmware and tape-load `build/prgs/m3check/bin/m3check.prg`.
No gateware change is needed for M3.

**Prepare disposable media.** Disconnect all physical IEEE drives, including
SD2PET. Use fresh, explicitly disposable raw images, never an original or the
only copy of operator data. `c1541 -format "M3 CHECK,01" d64 new-name.d64`
creates D64, with `d80`/`d82` and matching extensions for the other models.
Mount both slots with the same image type, retaining M2's 8/11 D64, 9 D80 and
10 D82 layout. Keep the source copies off the board. Record the firmware banner
and selected configuration, without reopening M2 or requiring new exhaustive
artifact pinning.

**Run the writable checker.** Build with `cmake --build --preset prgs`, copy
`m3check.prg` to the normal tape-program directory, then enter
`LOAD"M3CHECK"` and `RUN`. Supply the device (8-11), local drive (0/1), image
model (64/80/82), and `1` to permit destructive writes. The checker replaces
only its `M3*` files, performs seven length cases (0, 1, 253, 254, 255, 508, 509),
append, replacement, USR, native SAVE/VERIFY, exact filtered directory rows and
five/six-channel exhaustion. Payload reads interpose status after two bytes.
Expected summary: `13 /13 PASSED`, with no first-failure record.
Run separately for both slots and each model. A checker error or timeout is a
failure, not a skipped check.

**Verify persistence before rewriting.** Close channels, short-reset and
tape-load the checker again, but do not RUN it yet. For unit 8/drive 0, enter
`VERIFY"0:M3SAVE",8` and require `OK`. Substitute the actual device/drive for
each image. This reads the persisted SAVE against the tape-loaded program
without overwriting the evidence. Export the images after successful unmount
and inspect them with `c1541` before another checker run. `M3SAVE` must equal the checker PRG,
`M3APPEND` contains bytes 0..21 after replacement, and `M3N509` contains
509 bytes cycling 0..255. Preserve failing images before any rerun.

**Exercise protected/full media separately.** On a disposable read-only mount,
WRITE to a previously absent `0:M3PROTECT,S,W`, and APPEND to existing
`0:M3APPEND,S,A`, must report `26, WRITE PROTECT ON,00,00` (plus the
DOS 2.7 drive field), without changing an existing readable file. On a
disposable full image, creation must report 72 without gaining a live channel.
Capture status before CLOSE 2, since ENDCMD can replace the error with OK.
An in-progress file that runs out of blocks remains unclosed natively.
Restore a fresh disposable image rather than using unimplemented M4 validate
or scratch commands as recovery.

**Record remaining board cases.** Reconfirm simultaneous channels on both disks,
interrupted/repeated EOF and peer independence. For the current M3-only media,
tape-load `build/prgs/m3life/bin/m3life.prg` with `LOAD"M3LIFE"`, then `RUN`
and enter `1`. Use the ordinary PET configuration, not protected/full mode.
The read-only checker expects completed `M3CHECK` fixtures on 8/0 and 8/1
(D64), 9/0 (D80), and 10/0 (D82). It holds four streams open, interposes exact
status reads, rewinds 8/0 with peers paused, and checks every payload byte and
final EOI while resuming round-robin reads. It also requires two exhausted
retalks per stream to return the ROM 4 timeout result, then checks reopen
recovery. It temporarily clears ROM 4's `BOB` timeout-defeat flag at `$03FC`
and restores it on completion. Expected summary: `12 /12 PASSED`.
Allow several minutes. No disk or configuration edits are needed.
Unchanged `M2CHECK` instead requires its original `SEED`/boundary fixtures
and must not be run against these M3-only images.

Record exact status/data, persistence after remount and any reset/rejection
failures. Resource/service measurements explicitly deferred to M6 remain
there. Do not mark this stage accepted until the operator reports the required
M3 results and approves it.

The same `m3check.prg` can be checked on an independently emulated original drive
using the M1 reference command with `--ordinary-check`, selecting each model
and a fresh ignored output directory. The harness verifies 13 completed cases,
executed ROM identities, changed disposable media and an independently
extracted `M3SAVE` equal to the actual operator executable.

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

#### M4 command foundation (in progress)

M3 remains accepted. M4 is not complete or operator-accepted. Its first
implemented group is `I`/`INITIALIZE`, `U0`, and the default user-vector cold
DOS reset entry (`UJ`, `U:`, and selectors with the same folded table index).
These execute through the sole production engine, not a separate command
implementation.

Initialize abandons the selected disk's open channels and dirty BAM cache
without finalizing unfinished writers, then cooperatively reloads header/BAM
metadata. The peer disk's readers, writers, buffers, and cache survive.
Omitted drive selection starts from the last successful native completion,
initializes both disks, and restores that first drive. Missing raw-image slots
retain the documented status-74 policy, not a claimed mechanical-drive error.

`U0` preserves channels, cursors, last-program state, and the last successful
drive. Its native early return also preserves command bytes, so a subsequent
one-byte `U` can reuse the previous `0` selector. Cold DOS reset abandons
unfinished channels and cached allocations without CLOSE or host-remount
finalization. Mounted backing owners and already-dirty storage survive.
This does not emulate RAM diagnostics, head bumps, or physical reset timing.

Command dispatch now applies native CMDSET CR normalization before the
58-byte limit and returns exact error 31 for unknown command initials.
Still-unimplemented commands and excluded execution paths retain explicit
policy error 30 rather than pretending to implement native behavior.

The correspondence uses the same pinned source commit and approved ROM
revisions as M0/M1:

| Native source/routines (both models unless noted) | C implementation | Regression evidence |
|--------------------------------------------------|------------------|---------------------|
| `parsex:CMDSET/ENDCMD`, `erproc:ERRTAB` | `command_start`, `command_end`, status 31 | `test_command_termination_unknown_and_execution_rejection` |
| `parsex:SIMPRS/PARSE`, `setdrv:SETANY` | `initialize_drive`, `last_drive` | `test_initialize_native_drive_selection` |
| `tst2:INTDRV/INITDR`, `tst3:CLDCHN/FRECHN` | `initialize_work` | Selected-disk abandonment, peer preservation, missing-slot, partial-read and stale-media tests in `dos_test.c` |
| `block:USER/USRINT` | `command_start` U0 path | `test_user_zero_retains_native_resources_and_stale_selector` |
| `vector:UBLOCK`, `dskint:DSKINT`, DOS 2.7 `begin:INTTAB` | `command_start`, `dos_reset` | `test_user_reset_abandons_writers_but_retains_backing_ownership` |
| IEEE dispatch through the above routines | Unchanged production facade and SPI/FPGA path | `NativeCommandsRetireSelectedSourcesAndPreservePeers`, all three models |

The new initialize job adds 24 host bytes. At this foundation checkpoint,
the measured host `sizeof(dos_unit_t)` was 4428 bytes, within the existing
4608-byte assertion. There is no new heap allocation or per-slot data-buffer
pool.

#### M4 foundation evidence

At this foundation checkpoint, the Debug firmware regression executable
passed, including 174 direct DOS cases. The existing 158 firmware/SPI/FPGA
integration cases and three new model-specific command cases passed.
Production `RelWithDebInfo` built, and the 73 reference-tool tests passed.

`tools/drive-ref/drive_reference.py --command-check` adds a reproducible original-ROM
controller sequence with twelve independently checked observations per model:
startup, partial read, U0, short-U stale-selector reuse, resumed cursor,
initialize status, timeout on the abandoned channel, reopened prefix, cold
reset status, timeout after reset, reopened prefix after reset, and unknown
command status. The timeout CR is generated by the PET, not counted as a
drive wire byte. Complete composed-net captures are replayed through Debug
production C and the real SPI/FPGA path. All three exported native images
remain unchanged and match the candidate images byte-for-byte.

The full existing M1 ordinary-file sequence is also replayed without
`--ordinary-files`, now including its previously excluded UJ/reset suffix.
It covers writes/CLOSE/readback and exact resulting media, not just the new
read-only command scenario.

| Model | Full ordinary/reset actions, accepted frames | Command actions, accepted frames |
|-------|----------------------------------------------|----------------------------------|
| 4040 | 5362/5362, 3598 | 766/766, 520 |
| 8050 | 5446/5446, 3654 | 862/862, 584 |
| 8250 | 5446/5446, 3654 | 862/862, 584 |

Every comparison completes with zero unexplained byte/ATN/EOI/direction
differences and equal complete media. Retained private native inputs/captures
are `.cache/ai/m4-command-<model>-r1/`. Each comparison records the exact native
report, input, replay, executable hashes, source revision, and dirty worktree.
The comparison reports and their SHA-256 identities are:

| Report (`.cache/ai/` prefix) | SHA-256 |
|----------------------------|---------|
| `m4-reset-4040-r1/comparison.json` | `21c9dc65901571c06f157b4a9120ab20f3b6b1a5181700b821240e22e0a51e5b` |
| `m4-reset-8050-r1/comparison.json` | `b0c19f2991eedab6c8a0a508cceb3a5a1f262df0a706075d9242f2501c5aeaa0` |
| `m4-reset-8250-r1/comparison.json` | `de31df99fabb49b7501368d1c96da2583aba292629cc48810ade2ca8a34ea417` |
| `m4-command-4040-compare-r1/comparison.json` | `746521933d907df9bb67b88c4010494e647d4ca745b74840e3aa69f8d65e22e5` |
| `m4-command-8050-compare-r1/comparison.json` | `b52ab01c48e86e02fcae2c4a4c2cc2ee1bb7d7ac9235e8184a670aa35269fa1c` |
| `m4-command-8250-compare-r1/comparison.json` | `c6e6ef4e6d9d6bc5810e019af626d509a7f05be3fe73733bae0b03c101d6b294` |

Reproduce the new scenario with the M1 reference invocation above, adding
`--command-check` and using fresh ignored output directories for each model.
Run `compare_drive_reference.py` without prefix-selection flags. The new
firmware integration selector is:

```sh
ctest --test-dir build/system --parallel \
  -R 'NativeCommandsRetireSelectedSourcesAndPreservePeers' \
  --no-tests=error --output-on-failure
```

Native execution corroborates the twelve-observation scenario. Additional
initialize spellings, folded reset selectors, selected/peer writer teardown,
and injected storage failures currently have ROM-source-derived host
regressions, not separate original-ROM executions. Timing comparisons remain
simulation diagnostics, not physical RP2040/SD latency acceptance.

These reports identify the foundation executable, not the later rename
executable recorded below.

#### M4 rename implementation

Native `R`/`RENAME` now runs cooperatively through the same production engine
for the approved 4040 revision-2 ROM and shared DOS 2.7 ROM in 8050 and 8250
hardware modes. M3 acceptance and its recorded limitations are unchanged.
M4 remains incomplete and is not operator-accepted.

The parser preserves native error precedence, drive-prefix defaults,
16-byte name limits, and the empty-target bug. In particular, differing drive
prefixes do not move a file between disks. The source disk receives the new
name, but a destination collision on either disk can reject the command.
Missing source status 62 precedes target-exists status 63. A missing colon
returns 34 (`SYNTAX ERROR`), whereas missing equals, wildcards, and grouped
names return 30.

An empty target such as `R0:=ONE` is intentionally not sanitized. Native
`FNDLMT`/`TRCMBF`/`TRNAME` stores `=` followed by fifteen `$a0` filename bytes
and spills one additional `$a0` into raw entry offset 21. Independently
executed original-ROM captures corroborate this behavior on all three models.
Normal rename changes only the directory name, not type, links, block count,
or unrelated metadata.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Structure and bounded names | 4040 revision 2, 8050/8250 DOS 2.7 | [4040 parsex][m4-r-4040-parsex] and [8250 parsex][m4-r-8250-parsex], `TAGCMD`; [4040 trnsfr][m4-r-4040-trnsfr] and [8250 trnsfr][m4-r-8250-trnsfr], `FNDLMT/TRCMBF/TRNAME` | Required colon/equals, rejected patterns/groups, 16-byte bounds, ignored extra-equals tail, native empty-target spill | `command_name`, `rename_start`, `rename_work` in [dos.c](../../fw/src/ieee/dos.c), `test_rename_native_parser_and_error_precedence` in [dos_test.c](../../fw/test/dos_test.c) | Original-ROM 22-observation captures, complete media comparison |
| Source-before-target errors | Same approved models | [4040 rename][m4-r-4040-rename] and [8250 rename][m4-r-8250-rename], `RENAME/CHKIN/CHKIO` | Missing source gives 62 before collision 63; self-rename gives 63 | `rename_work`, `test_rename_native_parser_and_error_precedence` | Original-ROM status bytes and resulting media |
| Explicit/default drives | Same approved models | [4040 setdrv][m4-r-4040-setdrv] and [8250 setdrv][m4-r-8250-setdrv], `ALLDRS/SETDRV`; respective `rename:RENAME` | Omitted/invalid prefixes retain native defaults; differing prefixes rename on source disk, searching both disks for target collision, with the actual error drive | `rename_start`, `rename_work`, `test_rename_cross_drive_collision_and_source_selection`, `test_rename_default_drive_and_absent_source` | Original-ROM slot-0 default-prefix cases; dual-drive selection is currently source-derived host evidence only |
| In-place rename and unfinished writer | Same approved models | Respective `rename:RENAME`, [4040 lookup][m4-r-4040-lookup] and [8250 lookup][m4-r-8250-lookup], `LOOKUP/COMPAR`; respective `trnsfr:TRNAME` | Preserve file data and other metadata, allow unfinished-writer rename, retain the new name after CLOSE | `rename_work`, `update_directory`, `test_rename_changes_only_directory_name_and_preserves_reader`, `test_rename_unclosed_writer_survives_close_without_restoring_old_name` | Original-ROM rename/writer/CLOSE/readback observations and full exported images |
| Bounded shared ownership | Same approved models, with cooperative raw-image scheduling | [4040 tst3][m4-r-4040-tst3] and [8250 tst3][m4-r-8250-tst3], `GETRCH/FNDLNX`, `GETBUF/JGETBF`, `FRECHN/RELBUF` | Resource exhaustion gives 70, command cleanup releases only its own scratch ownership; peer channels remain usable | `rename_start`, `rename_finish`, `test_rename_shared_resource_exhaustion_and_recovery`, storage-failure and yielding cases | Source-derived resource tests, production SPI/FPGA reader-preservation and remount cases; not original-drive fault injection |

[m4-r-4040-parsex]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/parsex
[m4-r-8250-parsex]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/parsex
[m4-r-4040-trnsfr]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/trnsfr
[m4-r-8250-trnsfr]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/trnsfr
[m4-r-4040-rename]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rename
[m4-r-8250-rename]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rename
[m4-r-4040-setdrv]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/setdrv
[m4-r-8250-setdrv]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/setdrv
[m4-r-4040-lookup]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/lookup
[m4-r-8250-lookup]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/lookup
[m4-r-4040-tst3]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst3
[m4-r-8250-tst3]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst3

The deferred job uses two buffers from the existing shared pool and one
available internal logical index, with no heap allocation. It yields between
directory sectors, validates captured media identity before access, and
rereads the source sector before commit to preserve later unrelated edits.
Read/write/flush failure cleanup retains peer ownership and backing dirty
state. Raw-image missing-media status 74, traversal caps, and host storage
failure mappings remain explicit policies, not native mechanical-error or
atomic-rollback guarantees.

An already-published writer's CLOSE no longer rebuilds its directory name
and reserved fields. This prevents CLOSE from undoing a successful rename
or erasing the native empty-target spill. First publication and retry retain
their existing behavior. At this rename checkpoint, the job added 64 host
bytes, bringing `sizeof(dos_unit_t)` to 4492 bytes under the existing
4608-byte assertion. `sizeof(dos_channel_t)` remained 72 bytes.

#### M4 rename evidence

The complete Debug firmware regression passes, including 279 direct DOS
cases (105 additional rename cases). All 164 firmware/SPI/FPGA integration
cases pass, including `RenamePreservesBothDisksAndReopensPersistedName` for
each model. Production `RelWithDebInfo` builds, and all 79 reference-tool
tests pass.

`tools/drive-ref/drive_reference.py --rename-check` independently checks 22 observations
per model: startup, renamed data and missing old name, collision and error
precedence, malformed names, truncation, prefix defaults, extra equals,
full keyword, the empty-target spill, and unfinished-writer rename/CLOSE/
readback. It also independently extracts the renamed `RESTORED` PRG and
requires its bytes to equal the original `SEED`.

| Model | Completed/requested actions | Accepted frames | Equal reference/candidate media SHA-256 |
|-------|-----------------------------|-----------------|----------------------------------------|
| 4040 | 2655/2655 | 1877 | `50ef443aa66b0ae18b0f835020ef5c13ebbc55c9f2c7043a0c9bfa071b820c1a` |
| 8050 | 2907/2907 | 2045 | `5a0fad81e296e8bc63b341c7abae819f0cdeebb86a52907020ed0fb495970e8d` |
| 8250 | 2907/2907 | 2045 | `4c7b301dc0cb9e7004dfbd0afd7070ec392d29682f75fce76666e994de5667e4` |

Every comparison completes the entire transaction sequence with zero
byte/ATN/EOI/direction differences, wire issues, aborted frames, or omitted
cases. Complete resulting media match byte-for-byte. All three reports
identify the rename replay executable SHA-256
`2172b186524f40a73f78de8e080b0b0f8470d86988621a7a38b3d392f93d0353`.
Retained private native captures are `.cache/ai/m4-rename-<model>-r1/`.

| Report (`.cache/ai/` prefix) | SHA-256 |
|----------------------------|---------|
| `m4-rename-4040-compare-r1/comparison.json` | `24aad75ba092b52a9ad7257a4f2f77bce8863c03501134c3624884ac6b5da0d3` |
| `m4-rename-8050-compare-r1/comparison.json` | `0143eb8c6bfc7010627a9ebe1be2ccd139592120363335d54a0645a1e9d64e34` |
| `m4-rename-8250-compare-r1/comparison.json` | `be3fc63e62b00912a4fb45c51035dd3f1b80b40f072b8ac99688d11cda2a9c37` |

Reproduce with the M1 reference invocation, adding `--rename-check` and using
fresh ignored output directories for each model. Replay with
`compare_drive_reference.py` without prefix-selection flags.

The native rename scenario mounts slot 0 only. Cross-drive collisions,
slot-1-only defaults, empty slots, read-only media, storage failures, stale
references, and shared-resource exhaustion have source-derived host tests,
not a complete original-ROM dual-drive or injected-fault capture matrix.
Production SPI/FPGA tests additionally exercise both disks, retained readers,
and persisted-name readback after remount. These are simulator observations,
not physical board timing or SD persistence acceptance.

#### M4 scratch implementation

Native `S`/`SCRATCH` now executes through the same cooperative production
engine for the approved 4040 revision-2 and 8050/8250 DOS 2.7 targets.
Scratch reports status 01 (` FILES SCRATCHED`), with the deleted-file count
in the track field and zero in the sector field. No matching files is a
successful zero count, not status 62. DOS 2.7 retains its final drive field.

The native parser accepts up to five comma-separated patterns, `*` and `?`,
and an optional equals-type suffix such as `S0:NAME*=P`. A comma introduces
another filename, not a type suffix. Only the first suffix character selects
the type. Unknown suffixes disable filtering, whereas patterned or grouped
suffixes return 30. Missing colon or a comma before it returns 34. Drive
prefixes, inherited/default group drives, optimal search order, and
already-resolved exact names follow the source tables rather than a host
glob/delete API.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Pattern groups and type suffix | 4040 revision 2, 8050/8250 DOS 2.7 | [4040 parsex][m4-r-4040-parsex] and [8250 parsex][m4-r-8250-parsex], `TAGCMD/PARSE`; [4040 setdrv][m4-r-4040-setdrv] and [8250 setdrv][m4-r-8250-setdrv], `FS1SET/ALLDRS`; respective `romtbl:STRUCT` | Five-name bound, wildcards, equals-type selection, native 30/34 syntax errors | `scratch_start`, `command_name` in [dos.c](../../fw/src/ieee/dos.c), `test_scratch_native_parser_patterns_and_counts`, `test_scratch_pattern_limit_and_resource_recovery` in [dos_test.c](../../fw/test/dos_test.c) | Original-ROM group, wildcard, known/unknown type and malformed-input observations |
| Search order and matching | Same approved models | [4040 lookup][m4-r-4040-lookup] and [8250 lookup][m4-r-8250-lookup], `OPTSCH/SCHTBL/FFST/FFRE/COMPAR/CMPCHK` | Explicit drives restrict matches, omitted group drives can search peers, later patterns take precedence, exact names resolve once | `scratch_start`, `scratch_work`, `test_scratch_cross_drive_patterns_and_default_selection` | Original-ROM slot-0/default-group observations; installed dual-drive matching remains source-derived host coverage |
| Active and locked entries | Same approved models | [4040 scrtch][m4-s-4040-scrtch] and [8250 scrtch][m4-s-8250-scrtch], `SCRTCH/SC15/SC25`; [4040 tstflg][m4-s-4040-tstflg] and [8250 tstflg][m4-s-8250-tstflg], `TSTCHN` | Skip open readers/writers and lock bit `$40`, preserving data and cursor without counting deletion | `scratch_active`, `scratch_work`, `test_scratch_active_locked_unclosed_and_rel_entries` | Original-ROM active-reader/resumed-GET and active-writer observations; lock-bit coverage is source-derived host evidence |
| Persisted deletion and allocation release | Same approved models (non-REL scope) | Respective `scrtch:DELDIR/DELFIL/SC17/SC20`, `frets:FRETS/JFRETS`, `close:MAPOUT` (4040) and `map:MAPOUT` (DOS 2.7) | Zero only the type byte before freeing closed-file links, preserve other directory/data bytes, do not reclaim an abandoned unclosed ordinary chain | `scratch_work`, `mark_sector`, `flush_bam`, `test_scratch_releases_exact_chain_and_preserves_peer_reader`, `test_scratch_releases_chains_across_bam_parts`, unclosed-entry cases | Complete original-ROM resulting images; multi-part/second-side BAM traversal and abandoned-unclosed cases additionally have source-derived host tests |
| Count and command completion | Same approved models | Respective `scrtch:SC20/SC30`, `parsex:SCREND`, `erproc:ERRTAB/HEXDEC` | Status 01 with byte-sized count, zero sector, last-successful-drive update, retained status until actual GET | `scratch_work`, `dos_status`, count/status assertions and `ScratchProtectsReadersAndPersistsSelectedDeletion` in [drive_test.cpp](../../test/system/ieee/drive_test.cpp) | Exact original-ROM status/data/EOI captures, production SPI/FPGA status and remount observations |

[m4-s-4040-scrtch]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/scrtch
[m4-s-8250-scrtch]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/scrtch
[m4-s-4040-tstflg]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tstflg
[m4-s-8250-tstflg]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tstflg

At this scratch checkpoint, the job added 112 host bytes, bringing `sizeof(dos_unit_t)` to 4604
bytes within the unchanged 4608-byte assertion. Channels remain 72 bytes.
It uses two existing shared buffers and an available internal logical index,
not a heap allocation, per-file chain cache, or new per-slot buffer pool.
Directory and file-chain work yield between sectors. Directory re-entry
rereads persisted bytes rather than restoring a saved sector over later edits.
Media identities guard lookup, chain traversal, and dirty-BAM publication.

Host read/write/flush failures release only the job's scratch ownership and
retain unrelated channels, dirty BAM, and backing dirty state as applicable.
Deletion is not transactional: a published deleted entry is not restored
after a later chain or BAM failure. Raw-image traversal caps, missing-media
status 74, invalid-link mappings, and backing-store flushes remain explicit
host policies, not exact native mechanical-fault or atomic-rollback parity.
Matching REL entries still return policy error 30 before deletion. Complete
REL-dependent scratch behavior and acceptance remain M5 work.

#### M4 scratch evidence

The complete Debug firmware regression passes, including 410 direct DOS
cases (131 additional scratch cases). All 167 firmware/SPI/FPGA integration
cases pass, including the three model-specific
`ScratchProtectsReadersAndPersistsSelectedDeletion` cases. Production
`RelWithDebInfo` builds, and all 85 reference-tool tests pass.

`tools/drive-ref/drive_reference.py --scratch-check` checks 22 independent observations
per model: startup, missing/empty names, missing colon, active reader
protection and resumed cursor, deletion after CLOSE, one-byte-file EOF,
known/unknown type filters, grouped and wildcard deletion, malformed type
suffixes, active-writer protection, deletion after writer CLOSE, full keyword,
default-group search, and an untouched surviving PRG prefix. It independently
extracts `M1CHECK` from the final native image and requires equality with the
original operator PRG.

| Model | Completed/requested actions | Accepted frames | Equal reference/candidate media SHA-256 |
|-------|-----------------------------|-----------------|----------------------------------------|
| 4040 | 3269/3269 | 2267 | `737ec9bc576d11b5b1e623fe230a088692cad66018151d990fa4efc4fa871d5f` |
| 8050 | 3497/3497 | 2419 | `e2897c6bf0f4a68e4be6d9f30c2a123a9e7f1bcbfdc9c2a9a6734408a11b7b57` |
| 8250 | 3497/3497 | 2419 | `fe58e13b5dafdd5383911f74f73d733e01bbe0de7bdbb00f646335dbbfdf76ba` |

Every complete replay has zero byte/ATN/EOI/direction differences, wire
issues, aborted frames, or omitted cases. Complete resulting images match
byte-for-byte, including BAM updates and untouched data. All three reports
identify replay executable SHA-256
`60199d39741601bf3f73c6df60a4d224bec2ce437667dec2c79d238d1cfd5505`.
Retained private native captures are `.cache/ai/m4-scratch-<model>-r2/`.

| Report (`.cache/ai/` prefix) | SHA-256 |
|----------------------------|---------|
| `m4-scratch-4040-compare-r1/comparison.json` | `6664314b7d98f2d3f0eebeeef26f2dc4040840ff8e3e9107069e989c85e9b257` |
| `m4-scratch-8050-compare-r1/comparison.json` | `014b4f7221a498daead117506cca4c0c7e1af5071eb50fbec41b0888e556197a` |
| `m4-scratch-8250-compare-r1/comparison.json` | `78983b5936fbdc5d612c555ccd48b61abd9c543535fb52dec594b56a4a13385c` |

Reproduce with the M1 reference invocation, adding `--scratch-check` and
fresh ignored output directories. Run `compare_drive_reference.py` without
prefix-selection flags. Scratch's media writes produce 3.4-4.1 million PET
I/O polling events in this scenario, so the Lua observer's fixed bound was
increased from two to six million events. Incomplete register logs still
fail full-reference validation. The capped `r1` captures are not accepted
comparison evidence.

The native scenario mounts slot 0 only. Same-model dual-drive selection,
mixed inherited-drive third-search behavior, lock bits, abandoned unclosed
files, multi-part/second-side BAM release, resource pressure, corrupt
links, and injected storage failures have source-derived host coverage,
not a complete original-ROM dual-drive/fault matrix. SPI/FPGA cases
additionally preserve the peer disk's reader and verify selected deletion
and peer contents after remount. These remain simulator observations,
not physical SD persistence, RP2040 service-latency, or operator acceptance.

#### M4 named copy and concatenate implementation

Named `C`/`COPY` now copies ordinary SEQ, PRG and USR files, concatenates
up to four named sources into a new file, and supports native same-first-source
SEQ/USR append. This slice does not implement the separate wildcard/all-files
`CPYDTD` path or the colonless `C1=0` drive-copy form. Those remain M4 work,
returning explicit policy error 30. REL copy remains M5 policy error 30.
M3 acceptance is unchanged. M4 remains incomplete and not operator-accepted.

Source existence is checked before destination collision. The first source
determines a new file's type, and subsequent ordinary sources need not share
that type. Payloads are concatenated literally, including PRG load addresses.
Internal `GBYTE` reads begin at payload offset two, so a one-byte source
copies exactly one byte, not the externally visible singleton EOF sequence.

An existing destination can be the first source only with at least one
additional source and matching native drive descriptors, including their
default-prefix flag. Thus `C0:JOIN=0:JOIN,0:ONE` can append, while
`C0:JOIN=JOIN,ONE` gives 63. PRG self-append gives 64. COPY's append path
does not mark the existing entry unclosed before copying. Its retained native
read/write block counter can exceed the actual chain length, and this behavior
is intentionally preserved rather than normalized.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Named command classification and bounded parsing | 4040 revision 2, 8050/8250 DOS 2.7 | [4040 copysetup][m4-c-4040-copysetup] and [8250 copysetup][m4-c-8250-copysetup], `DSKCPY`; respective `parsex:TAGCMD`, `setdrv:ALLDRS` | Normal COPY permits grouped literal sources, rejects malformed structure with 30, retains sixteen-byte names and at most five total names | `copy_start`, `test_copy_parser_precedence_and_command_union_reuse` | Original-ROM syntax/full-keyword observations; excess-name cap is source-derived host coverage |
| All-source existence before target collision | Same approved models | [4040 rename][m4-r-4040-rename] and [8250 rename][m4-r-8250-rename], `COPY/CY/CHKIN/CHKIO` | Missing any input gives 62 before existing-output 63; first input selects output type | `copy_lookup_finish`, parser and mixed-type tests | Original-ROM missing/collision/self-copy statuses and extracted mixed-type output |
| Explicit and inherited drive search | Same approved models | Respective [4040 lookup][m4-r-4040-lookup] and [8250 lookup][m4-r-8250-lookup], `OPTSCH/SCHTBL/LOOKUP/COMPAR` | Grouped defaults may require three searches; explicit cross-drive inputs retain independent source identities and cursors | `copy_search`, `test_copy_cross_drive_defaults_use_native_third_search`, production `CopyConcatenatesAcrossDisksAndPersistsAfterAppend` | Source-derived dual-drive host/SPI coverage, not native dual-drive execution |
| Literal byte copy and boundaries | Same approved models | Respective `rename:OPIRFL/GIBYTE/CY15/CY20`, [4040 tst4][m4-c-4040-tst4] and [8250 tst4][m4-c-8250-tst4], `GBYTE/GET`; respective `tst2:STRDBL/RDBYT/WRTBYT` | No singleton IEEE EOF artifact, no stripped PRG headers, correct 254/255-byte and concatenation boundaries | `copy_read_start`, `copy_work`, shared `write_byte`, boundary/concatenation tests | Independently extracted native payloads and complete resulting images |
| Same-first-source append | Same approved models | Respective `rename:COPY/COP05`, [4040 open][m4-c-4040-open] and [8250 open][m4-c-8250-open], `APPEND`; respective `tst2:RDBUF/WRTBUF`, `close:CLSWRT/CLSDIR` | Descriptor-sensitive append, PRG rejection 64, preserved closed entry, native read/write block count and buffer residue | `copy_lookup_finish`, `append_work`, `update_directory`, `test_copy_concat_append_mixed_types_and_metadata` | Native append/default-append/PRG observations, extracted JOIN and full-image comparison |
| Finite ownership and command-error abandonment | Same approved models, with cooperative raw-image scheduling | Respective [4040 tst3][m4-r-4040-tst3] and [8250 tst3][m4-r-8250-tst3], `GETRCH/GETWCH/FRECHN/RELBUF`; respective `tst4:FREICH` | Internal secondaries 17/18 use the shared pool, status 70 on exhaustion, errors abandon internal writers without finalizing them or releasing peers | `copy_finish`, resource/fault tests, command-union transition tests | Source-derived fault/resource tests and production SPI/FPGA peer-cursor/remount coverage |

[m4-c-4040-copysetup]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/copysetup
[m4-c-8250-copysetup]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/copysetup
[m4-c-4040-tst4]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4
[m4-c-8250-tst4]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst4
[m4-c-4040-open]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/open
[m4-c-8250-open]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/open

Rename, scratch and copy now share a discriminated transient-state union.
Only its tag selects the active worker, never an aliased pending flag.
At this copy checkpoint, the job occupies 152 host bytes and
`sizeof(dos_unit_t)` is 4584 bytes (down from 4604), below the unchanged
4608-byte ceiling.
Channels remain 72 bytes. Internal channels and their two buffers come
from the existing six-channel/twelve-buffer pool, with no new data pool,
heap allocation or whole-file cache. Work yields between directory sectors,
append links and at most 64 copied bytes. Native directory/data prefetch
and CLSDIR's opposite-buffer reuse preserve bytes beyond logical EOF
that subsequent allocations expose in complete exported images.

Errors release only the job's internal ownership. A partially created file
is not host-finalized after command failure, and no atomic rollback is promised.
Raw-image identity checks, absent-slot status 74, traversal caps,
host read/write error mappings and backing flushes remain explicit policies,
not exact mechanical fault parity. Full fault/dual-drive native comparisons,
active-writer/locked/unclosed source combinations, and physical board
acceptance remain outstanding.

#### M4 named copy evidence

The complete Debug firmware regression passes, including 518 direct DOS
cases (108 additional named-copy cases). All 170 firmware/SPI/FPGA integration
cases pass, including three cross-drive copy/append, command-state reuse,
peer-cursor and remount cases. Production `RelWithDebInfo` builds, and all
90 reference-tool tests pass.

`tools/drive-ref/drive_reference.py --copy-check` checks 24 native observations on
each approved model. These include new PRG copy, one-byte and 254/255-byte
sources, literal and mixed-type concatenation, explicit/default self-append,
PRG append rejection, four inputs, missing-source/collision precedence,
malformed structure, empty source/target, full keyword and retained peer GET
progress. Ten resulting files are independently extracted and compared with
known payloads. The native captures are
`.cache/ai/m4-copy-<model>-r1/`, with complete 4.4-4.8 million-event
register logs below the unchanged six-million-event observer bound.

| Model | Completed/requested actions | Accepted frames | Equal reference/candidate media SHA-256 |
|-------|-----------------------------|-----------------|----------------------------------------|
| 4040 | 2893/2893 | 2041 | `9cf67fa559784608cb9fc332dd613fdc7dc2541e142e439b68701e959fee9519` |
| 8050 | 3169/3169 | 2225 | `01bc280397258a36623578b0459005cc23207723dd97f99b82144e6d2dbea5dd` |
| 8250 | 3169/3169 | 2225 | `0361d175302684b6509231711a9114e53ae2509364386f7eaf969cf69c49c639` |

All three complete replays have zero byte/ATN/EOI/direction differences,
wire issues, aborted frames, omitted cases or unequal image bytes.
Reports identify replay executable SHA-256
`d55eb890284f77f2f5193fdb69bbe3edf5215b930dac323f2725302aea85277f`.

| Report (`.cache/ai/` prefix) | SHA-256 |
|----------------------------|---------|
| `m4-copy-4040-compare-r3/comparison.json` | `a73092c690495152cadf3b6548e8df4163f6beb42c16d1f0c7c3638bf3414dd0` |
| `m4-copy-8050-compare-r3/comparison.json` | `6b754d6d0193dad3ae7355da12e1922c90fef1676741206a7dd9ce1126516f8c` |
| `m4-copy-8250-compare-r3/comparison.json` | `64c4d5817d822fc6547c124b92f13dc4f4840cc51bed7ed59bdde65b3e0852b7` |

Reproduce with the M1 reference invocation, adding `--copy-check` and fresh
ignored output directories, then run `compare_drive_reference.py` without
prefix-selection flags. Initial candidate comparisons were rejected despite
matching transactions: unused data-sector tails and the self-append directory
block count differed. Implementing native directory/data prefetch, opposite
writer-buffer reuse during CLSDIR, and COPY's retained read/write block count
removed those differences. The `compare-r1` and `compare-r2` results are
diagnostics, not accepted evidence.

The original-ROM scenario mounts slot 0 only. Dual-drive defaults and
cross-drive concatenation remain source-derived host/SPI evidence. Firmware
integration also exercises command-state transitions `C -> R -> S -> C`,
two retained public readers and persisted remount readback. These simulator
observations do not establish physical SD durability, RP2040 latency,
full native storage-fault behavior or operator acceptance.

#### M4 non-REL validate implementation

`V`/`VALIDATE` now rebuilds allocation maps for ordinary native files.
Like `I`, its `INITDR` entry abandons the selected disk's readers and unfinished
writers without closing or flushing them. Unlike an unqualified `I`, an
unqualified `V` selects only the last drive. Locked closed files survive.
Unclosed entries are deleted even when locked, without traversing their
abandoned chains. Directory block counts are preserved, not recomputed.
M3 acceptance is unchanged. M4 remains incomplete and not operator-accepted.

The 4040 builds its map in the existing per-drive BAM cache and tolerates
duplicate references through `USEDTS`. DOS 2.7 builds two/four complete maps
in `VBAMS`, then writes them in order through `WTMAPS`. Its `VUSED` rejects a
previously marked sector with `65,NO BLOCK,<track>,<sector>,<drive>`.
Both models mark closed file chains, nonzero side-chain links, and the linked
header/BAM/directory chain. Neither model rewrites surviving file payloads.
DOS 2.7's ordinary BAM cache is not silently refreshed by `WTMAPS`, and a
command-level error 65 does not take the controller-error `INITDR` path.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Simple parser and selected-disk abandonment | 4040 revision 2, 8050/8250 DOS 2.7 | [4040 parsex][m4-r-4040-parsex]/[8250 parsex][m4-r-8250-parsex], `SIMPRS`, [4040 setdrv][m4-r-4040-setdrv]/[8250 setdrv][m4-r-8250-setdrv], `SETANY`, [4040 tst2][m4-v-4040-tst2]/[8250 tst2][m4-v-8250-tst2], `INITDR`, and [4040 verdir][m4-v-4040-verdir]/[8250 verdir][m4-v-8250-verdir], `VALDAT` | `V`, full keyword and ignored suffixes select one drive, discard its channels without CLOSE, and leave the peer cursor intact | `command_start`, `initialize_work`, `test_validate_drive_selection_and_peer_cursor`, production `ValidateRebuildsMapsAndAbandonsOnlySelectedDisk` | Native startup/keyword/closed-reader/abandoned-writer observations. Dual-drive selection has source-derived host/SPI coverage |
| Complete native BAM construction and publication | Same approved models | [4040 verdir][m4-v-4040-verdir], `NEWMPV/NEWMAP/VD10`, [8250 map][m4-v-8250-map], `NEWMPV/BAMOUT`, and [8250 verdir][m4-v-8250-verdir], `VSETB/WTMAPS` | Rebuild counts and valid bitmap bits, preserve the 4040 header, clear DOS 2.7 map padding, and publish maps only after successful traversal | `validate_work`, `validate_entry`, exact-image repair and all-BAM-part/directory-extension tests | Independently reconstructed maps and complete original-ROM final-image comparisons on all models |
| Closed, locked and unclosed entries | Same approved models | Respective `verdir:VD15/VD20/VD25`, [4040 lookup][m4-r-4040-lookup]/[8250 lookup][m4-r-8250-lookup], `SRCHST/SRRE`, and [4040 scrtch][m4-s-4040-scrtch]/[8250 scrtch][m4-s-8250-scrtch], `DELDIR` | Preserve locked closed entries and their block counts. Delete an unclosed entry regardless of lock, leaving its data bytes untouched | `test_validate_repairs_maps_deletes_splats_and_preserves_counts`, repeated-operation and writer-abandonment tests | Native locked ONE, locked/unclosed BOUND255, SEED block count 77 and absent ABORT observations. Five independently extracted surviving/copied files |
| Duplicate ownership and side-chain traversal | 4040 versus DOS 2.7 | [4040 verdir][m4-v-4040-verdir], `MRKBAM` -> [4040 tst2][m4-v-4040-tst2], `USEDTS`, and [8250 verdir][m4-v-8250-verdir], `VMKBAM/VUSED/USDERR` | 4040 duplicate references are idempotent. DOS 2.7 returns 65 at the duplicate coordinates before publishing maps. Nonzero side-chain links are traversed before data | `validate_used`, crosslink and side-link tests | Primary-source-derived direct tests. These corrupt/side-chain cases are not separate native captures |
| Controller errors and partial publication | Same approved models, with explicit raw-image error policy | [4040 erproc][m4-v-4040-erproc]/[8250 erproc][m4-v-8250-erproc], `ERROR`/`JERROR`, `CMDER2/CMDER3`, and respective `tst2:INITDR` | Controller failures reload selected metadata and abandon internal work, but do not undo earlier deleted entries or already written maps | `validate_recover`, storage read/write/flush, read-only and retry tests | Source-derived failure tests and persisted retry checks, not mechanical fault parity |
| Allocation-visible initial prefetch residue | Same approved models | [4040 opchnl][m4-v-4040-opchnl]/[8250 opchnl][m4-v-8250-opchnl], `OPNRCH/INTPNT`, respective `tst2:STRDBL/RDBUF`, [4040 tst3][m4-r-4040-tst3]/[8250 tst3][m4-r-8250-tst3], `FRECHN/RELBUF`, and [4040 rename][m4-r-4040-rename]/[8250 rename][m4-r-8250-rename], `CY/OPIRFL` | Initial successor data remains in an abandoned reader's buffer and can appear beyond EOF in a later COPY allocation. Asynchronous failure is reported only when that buffer is selected | Ordinary OPEN prefetch, `load_buffer`, `test_validate_preserves_abandoned_reader_prefetch_residue`, deferred-prefetch-failure test | Full native wire/media comparisons reject the original 247-byte tail mismatch and pass after preserving the prefetch |

[m4-v-4040-verdir]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/verdir
[m4-v-8250-verdir]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/verdir
[m4-v-8250-map]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/map
[m4-v-4040-tst2]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
[m4-v-8250-tst2]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
[m4-v-4040-erproc]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/erproc
[m4-v-8250-erproc]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/erproc
[m4-v-4040-opchnl]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/opchnl
[m4-v-8250-opchnl]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/opchnl

The pinned MAME [8050 memory map][m4-v-mame-map] corroborates the
`$1D00` mirror of `$1100`: native `VBAMS` occupies fixed ordinary pages
0..3, rather than an additional allocation pool. EconoPET borrows those
existing shared buffers for the maps and one ordinary internal read channel
for traversal. It refuses resource pressure with policy 70 instead of
overwriting a live peer's fixed page. This safety difference remains an
explicit native resource-matrix gap, not a claim of exact high-pressure parity.
The 4040 uses its existing BAM sector instead of these shared map pages.

VICE's [virtual validate worker][m4-v-vice-validate] independently corroborates
rebuilding maps from closed chains and deleting unclosed entries. It is not
the revision authority: it can recompute directory block counts, closes files,
and uses different side-sector and error-recovery rules. The selected ROMs
and their native captures take precedence over those differences.

[m4-v-mame-map]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c8050.cpp
[m4-v-vice-validate]: https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/vdrive/vdrive-command.c

The validate job occupies 32 host bytes in the existing tagged command union.
Initial OPEN prefetch adds a retained failure flag to each existing buffer.
At this validate checkpoint, `sizeof(dos_unit_t)` is 4596 bytes, below the
unchanged 4608-byte ceiling.
Channels remain 72 bytes. There is no new sector pool, heap allocation,
whole-image bitmap, or whole-file chain cache. Work yields between map pages,
directory entries and linked sectors. Stale media is rejected before replacement
I/O. Traversal bounds, generic raw-image read/write mappings, absent-image 74
and backing flushes remain host policies.

Closed REL entries still return policy 30. This host-policy rejection reloads
persisted maps before returning, so the incomplete rebuilt map cannot escape
through a later allocation or CLOSE. The regression reproduces a dirty 4040
map and incorrect DOS 2.7 free counts before this recovery was added.
Earlier unclosed-entry deletions are not rolled back if a later REL, crosslink
or storage error terminates the command. Complete REL/super-side-sector
validation remains M5 work.

#### M4 validate evidence

The complete Debug firmware regression passes, including 593 direct DOS cases
(75 additional validate/prefetch cases). All 173 firmware/SPI/FPGA integration
cases pass, including three selected-writer abandonment, retained-peer,
copy-after-validate and remount cases. Production `RelWithDebInfo` builds,
and all 98 reference-tool tests pass.

`tools/drive-ref/drive_reference.py --validate-check` checks 12 observations on each
approved model. Its independent fixture has an allocated orphan, a locked
closed file, a locked/unclosed two-sector file and an intentionally inflated
directory block count. It exercises initial repair, reader invalidation,
writer abandonment, missing-file status, repeated/full-keyword validation and
copy after repair. It reconstructs every final BAM byte from surviving chains
and independently extracts SEED, ONE, BOUND254, M1CHECK and NEW.

| Model | Completed/requested actions | Accepted frames | Equal reference/candidate media SHA-256 |
|-------|-----------------------------|-----------------|----------------------------------------|
| 4040 | 1029/1029 | 713 | `e097c499e87c820d0343141d162c59f4b3a6cf7eff099c85df469e578dfcadb2` |
| 8050 | 1149/1149 | 793 | `41f507014a7b15c825590ae315cde856416f90d855156f5517ddf544ba0c41c1` |
| 8250 | 1149/1149 | 793 | `453207228409388715c77a19b85dea0144be4ef6b2fbeacb653fa21977a555ef` |

Every complete replay has zero byte/ATN/EOI/direction differences, wire issues,
aborted frames, omitted cases or unequal image bytes. Reports identify replay
executable SHA-256
`550851d3763445b32c1a64752dc5cdc2dcf242fba2d1ff188c56dd1d8def46e6`.
Native captures are `.cache/ai/m4-validate-<model>-r1/`. Their complete
register logs contain 2,962,540, 3,523,962 and 4,369,717 events respectively,
below the unchanged six-million-event bound.

| Report (`.cache/ai/` prefix) | SHA-256 |
|----------------------------|---------|
| `m4-validate-4040-compare-r3/comparison.json` | `3f49f1ae6ce30ea1d2288e85d3b59dda74c24c981583e5dd230b413ad7405037` |
| `m4-validate-8050-compare-r3/comparison.json` | `fdd73187362e3ed074f956702b8fb03e18b1e6d76b35ca4e20690b034159db62` |
| `m4-validate-8250-compare-r3/comparison.json` | `ae45215b49cc7be86a7c4c427a11be4d37f0e89476ca443bf9ef63285f60ad69` |

Reproduce with the M1 invocation, adding `--validate-check` and a fresh ignored
output directory, then run `compare_drive_reference.py` without prefix flags.
The initial `compare-r1` results had matching complete transactions but
different unused COPY tails. They are retained diagnostics, not acceptance
evidence. Preserving ordinary OPEN's native initial prefetch removed every
media difference without normalizing or excluding unused bytes.
The `compare-r2` checkpoint already matched every image. Final `compare-r3`
repeats these complete comparisons after adding the REL-policy map recovery.
The prefetch-corrected checkpoint also matches the earlier complete named-copy
captures on all models (`m4-validate-copy-<model>-regression/`). Those reports
precede the REL-policy fix and retain their checkpoint's executable identity.

Native captures mount slot 0 only. Crosslinks, side links, all-part data chains,
directory extension, stale bindings, resource pressure and injected faults have
source-derived direct coverage, not a complete native dual-drive/fault matrix.
SPI/FPGA cases additionally preserve a peer-disk cursor and verify persisted
remount results. These are simulator observations, not physical durability,
RP2040 latency or operator acceptance.

#### M4 direct-buffer channel foundation

Raw direct channels support `OPEN "#"` and numeric `OPEN "#n"` on
secondaries 0..14 (the later LOAD/SAVE slice below records SA0's distinct
GET behavior). They use one of the existing twelve data pages, rather
than an ordinary file's two buffers. OPEN returns the binary page number
(not an ASCII decimal string) and sets the shared GET/PUT pointer to 1.
No directory entry is created. This is the channel foundation only, not
completion of block commands or M4 acceptance.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Direct OPEN and finite ownership | 4040 revision 2, 8050/8250 DOS 2.7 | [4040 block][m4-d-4040-block]/[8250 block][m4-d-8250-block], `OPNBLK/OB10/OB30`, and respective `tst3:GETRCH/GETBUF/FNDLNX` | Bare `#` takes the highest available page. Explicit pages 0..11 must be free. The prepared first byte is the page number, without EOI. Reopening through GETRCH abandons the old channel without CLOSE | `open_direct`, variable-count `allocate_channel`, parser/ownership, capacity and writer-abandonment tests | Complete native captures cover implicit/explicit/leading-zero requests, occupied/reserved-page 70, same-secondary rejection and retained file readers |
| Numeric parameter conversion | Same approved models | Respective `block:BP05/ASCHEX/AH10/AH40` | Separators include spaces, commas and PET cursor-right. Conversion retains three low-nibble digits and the low result byte. A fifth parsed parameter returns syntax 30 | Shared `block_parameters`, `test_direct_parser_and_single_buffer_ownership` | Source-derived numeric boundary tests. Non-numeric scratch-dependent parsing is explicitly excluded below |
| Shared PUT pointer and prepared data | Same approved models | [4040 tst2][m4-v-4040-tst2]/[8250 tst2][m4-v-8250-tst2], `PUT/L46/PUTBYT`, and respective `tst4:RNGET2/RNGET4` | PUT writes the current raw offset, increments modulo 256, and refreshes CHNDAT. Reaching offset 255 sets EOI, which later PUTs do not clear. Incoming EOI does not close the page | `dos_listen_put`, `test_direct_put_wrap_sticky_eoi_and_partial_get`, production `DirectBufferWrapRetainsPeerAndSurvivesRepeatedEoi` | Native 256-byte PUT after reading the page number returns byte 0 with sticky EOI |
| Circular reads after EOI | Same approved models | [4040 tst4][m4-c-4040-tst4]/[8250 tst4][m4-c-8250-tst4], `GET/GET00/RNDGET/RNGET1` | GET clears a consumed direct EOI without retiring readiness. At LSTCHR 255, subsequent GET wraps through offset 1, not 0. Staged but unconsumed bytes must not advance the pointer | Direct `next_byte`, reversible-preview/partial-GET tests and production repeated-EOI/peer-cursor case | Native 253-byte tail followed by a 255-byte circular record, with exact binary CR and final EOI |
| Page drive affinity survives release | 4040 versus DOS 2.7 | Respective `block:OB10`, [4040 jobs][m4-d-4040-jobs]/[8250 jobs][m4-d-8250-jobs], `SETJOB/SJB1`, and respective `tst3:GETBU4/CLDCHN` | Allocation and sector jobs retain the page's LSTJOB drive. Explicit 4040 OPEN retains that old drive, while DOS 2.7 overwrites it with LSTDRV. INITIALIZE closes only matching-drive pages | `buffer_drives`, `remember_buffer_drive`, `test_direct_explicit_page_drive_and_initialize`, including fallback directory jobs | Primary-source-derived dual-drive tests. The complete native scenario mounts only slot 0 |
| CLOSE, failure retention and lifecycle | Same approved models, with host backing-flush policy | [4040 close][m4-d-4040-close]/[8250 close][m4-d-8250-close], `CLSCHN/CLSC30/MAPOUT`, respective `tst3:FRECHN/RELBUF`, and `tst2:INITDR` | Direct CLOSE skips file finalization but publishes dirty BAM state. INITIALIZE/validate abandon matching pages. A failed host map/flush retains its owner and unrelated writers for retry | Direct `finish_channel`, dirty-map/flush retry tests, stale/replaced-media RAM tests and production remount readback | Native CLOSE15/reallocation and unchanged complete images. Injected faults and replacement handling have host-policy coverage, not mechanical fault parity |

[m4-d-4040-block]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/block
[m4-d-8250-block]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/block
[m4-d-4040-jobs]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/jobs
[m4-d-8250-jobs]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/jobs
[m4-d-4040-close]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/close
[m4-d-8250-close]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/close

The pinned MAME [4040][m4-d-mame-4040] and
[8050/8250][m4-v-mame-map] RAM/controller maps corroborate ordinary page
sharing and execute the selected ROMs. VICE's [virtual channel implementation][m4-d-vice]
independently corroborates memory-buffer channels and their initial pointer
of 1. Its requested-buffer path is marked FIXME and its virtual allocator is
not an authority for original page selection or reuse. Follow the pinned
ROMs and their complete composed-wire captures instead.

[m4-d-mame-4040]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c2040.cpp
[m4-d-vice]: https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/vdrive/vdrive-iec.c

The retained CHNDAT, LSTCHR and EOI values alias existing channel storage.
A 16-bit per-page drive bitmap preserves LSTJOB drive affinity, including
scratch-page reuse after a default-drive directory search falls back to the
peer. It is updated for actual shared-page sector jobs, not just allocation.
Host unit state is 4600 bytes and channels remain 72 bytes, below the unchanged
4608-byte unit ceiling. No new sector pool, heap allocation or per-file cache
was introduced.

Explicit page reservation precedes the logical-channel search in the ROM.
If all six logical slots are occupied, a newly reserved explicit page can
remain reserved even though OPEN reports 70. CLOSE15 frees allocated channels,
not that unassigned page. Unit reset releases the complete pool. This bounded
source-derived behavior is tested directly, not claimed as a complete native
resource-pressure capture.

Direct RAM I/O itself does not require a writable or currently mounted image.
Host CLOSE flushes backing storage only when publishing dirty metadata, with
a retained retry phase. Replacement-identity checks prohibit a pending flush
from being redirected to a new image. Physical DOS 2.7 AUTOI/WPSW behavior,
controller retries and exact mechanical errors remain outside this raw-image
foundation.

The full malformed-input matrix is still deferred. In particular, ASCHEX
clears `TEMP`, `TEMP+1` and `TEMP+3`, but leaves `TEMP+2` untouched if its first
parameter has no numeric characters. Thus `#A` can depend on unrelated scratch
RAM. The rejected 4040 `r1` and `r2` diagnostics timed out on that request,
while the first DOS 2.7 captures happened to accept page 0. Inspection of the
selected 4040 parser opcodes corroborated the archived routine. EconoPET
returns explicit policy 30 for this nonnumeric form instead of inventing
deterministic scratch state. Accepted `r3` captures use numeric `#000`.
Direct LOAD/SAVE secondaries 0/1 also remain policy 30. These are known
compatibility gaps, not newly accepted native behavior.

#### M4 direct-buffer evidence

`tools/drive-ref/drive_reference.py --direct-check` records 14 observations per approved
model, with separate 255-byte data/status capture areas. Unlike the earlier
status-oriented generator, binary CR does not terminate a data observation.
The scenario checks binary page numbers, a complete 256-byte PUT, sticky EOI,
circular GET, exact status 70, same-secondary reservation rejection, a paused
ordinary-file peer and CLOSE15/reallocation. Both initial and final native
images remain byte-identical.

All three numeric scenarios completed with `trace_complete=true`, below the
unchanged six-million-event limit. Complete C-firmware/SPI/FPGA replays use
`.cache/ai/m4-direct-<model>-r3/` as reference and
`.cache/ai/m4-direct-<model>-compare-r1/` as output. No prefix selector, stopped
case, omitted action/frame/case, wire issue, aborted frame or unexplained
byte/ATN/EOI/direction difference remains. The replay executable SHA-256 is
`0c671dc57e74e98e53c3a129e9d08f2ccced362e158833620aaa0d3c19ab5805`.

| Model | Native I/O events | Native completion | Completed/requested actions | Accepted frames | `comparison.json` SHA-256 |
|-------|-------------------|-------------------|-----------------------------|-----------------|--------------------------|
| 4040 | 475240 | 41.48 simulated seconds | 4046/4046 | 2786 | `137ee24f85f361b79126d593ac7f4e2bf7a40813b8f93f34c6d0bb34fed3b490` |
| 8050 | 376181 | 41.30 simulated seconds | 4118/4118 | 2834 | `11afaff436efba225ffaf1dc51e1df85b0d051084b1c411af2c6bc65edde3b3b` |
| 8250 | 429042 | 41.70 simulated seconds | 4118/4118 | 2834 | `da6cccfd49acdfe96d69e8d9e5e8411fc226d3506710d7b2306a6f0982500ae3` |

The complete initial, final native and final candidate images have these
equal SHA-256 values. There are no changed sectors or excluded unused bytes.

| Model | Equal image SHA-256 |
|-------|--------------------|
| 4040 | `612bd40b23d94c5ccafe814b17e4e880807252100c7a45581996a17ed187738b` |
| 8050 | `fce28fb285e32e55c62ea4d4605992c22a21949037cd4415a2152883b6695b52` |
| 8250 | `9a98ffb11443219a2eb7c21899ffd9bf9e60a78f478bea9cb9cb5afeb4e14b00` |

Reproduce using the M1 reference invocation with `--direct-check` and a fresh
ignored output directory, then run `compare_drive_reference.py` without
prefix flags. The numeric `r3` captures supersede the earlier `r1`/`r2`
parser diagnostics. They do not establish malformed-input compatibility.

The direct DOS suite now passes 671 cases (78 added for this foundation).
The complete firmware regression, production build and 102 tool tests pass.
All 176 affected firmware/SPI/FPGA integration cases have passing results:
175 in the broad run, followed by the corrected unsupported-path test and
all three direct model cases in a four-case rerun. The former unsupported
`#` expectation now tests the explicitly excluded `#A` form. Dirty-BAM and
flush retries include a remount-generation change that must not flush the
replacement or discard a peer writer.

These results cover the selected numeric channel scenario and source-derived
host cases, not the complete dual-drive/resource/fault matrix. The native
captures mount slot 0 only. Simulation phase durations are diagnostic and
do not establish physical SD latency, RP2040 headroom or hardware timing
parity. M4 remains in progress and is not operator-accepted.

#### M4 direct-channel buffer positioning

Numeric `B-P` now positions an existing direct channel on any raw page offset
0..255. It refreshes the prepared output byte without reading, writing or
flushing media and does not allocate another channel or buffer. The operation
uses the same pointer as direct PUT and GET. It does not clear an already
pending EOI. This is the direct-channel subset of BLKPTR, not completion of
all block commands or their ordinary-file-channel side effects.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Command selection and parameter start | 4040 revision 2, 8050/8250 DOS 2.7 | Respective [4040 block][m4-d-4040-block]/[8250 block][m4-d-8250-block], `BLOCK/BLK40/BLK50/BLKPAR`, and `parsex:PARSE` | The first hyphen selects the following operation letter. A preceding comma returns 30. Parameters follow the first colon, or begin at byte index 3 without one. A longer prefix such as `BUFFER-P` is accepted with a colon | `block_delimiter`, `block_start`, `test_block_pointer_parser_and_rejection_atomicity` | All-model native `B-P2,000` and `BUFFER-P:258,1255` captures. VICE's command parser corroborates the operation family, not every native delimiter rule |
| Bounded numeric conversion | Same selected models | Respective `block:BP05/BP10/ASCHEX/AH40` and `parsex:CMDRST` | Spaces, commas and PET cursor-right separate values. Retain the last three low-nibble digits and each result's low byte. A missing second value defaults to zero. Four parsed values are accepted, a fifth returns 30 before changing the page | Shared `block_parameters`, parser/rejection-atomicity tests | Native low-byte channel wrap, pointer truncation, four-value success and fifth-value error without cursor movement |
| Channel eligibility | Same selected models | Respective `block:BUFTST/BT15/BT20` and [4040 tst2][m4-v-4040-tst2]/[8250 tst2][m4-v-8250-tst2], `FNDRCH/FNDC25` | Byte-sized `(secondary - 2) < 12` allows secondaries 2..13, not 14. An absent or write-only channel returns 70. There is no new page claim or change to another channel | `block_start`, channel-permission tests | Native missing-secondary 70. Boundary and write-only cases are source-derived direct tests |
| Prepared byte, raw offset zero and sticky EOI | Same selected models | Respective `block:BLKPTR`, `tst2:GETPRE`, and [4040 tst4][m4-c-4040-tst4]/[8250 tst4][m4-c-8250-tst4], `RNGET2/RNGET4/GET00/RNDGET` | Set BUFTAB to the low position byte and refresh CHNDAT without incrementing. Equality with LSTCHR 255 sets EOI. Positioning elsewhere does not clear EOI, and consuming it leaves a direct channel ready | `test_block_pointer_all_offsets_put_and_sticky_eoi`, production `BufferPointerReplacesStagedBytesAndRetainsPeer` | Original-ROM offset 0, offset 255, binary CR, sticky EOI after a second B-P and retained direct continuation |
| Transport and host lifecycle | Same selected models, plus host media policy | Respective `parsex:PARSXQ/ENDCMD`, `block:BLKPTR/BUFTST`, and existing IEEE command dispatch | Retire only stale staged copies, not unconsumed source bytes. Preserve a paused peer reader. B-P does not select a new drive, so DOS 2.7 status retains the current drive field | Existing `ieee_drive.c:dispatch`, three production model cases, `test_block_pointer_media_changes_do_not_issue_storage_jobs` | Native paused-reader and direct-cursor continuation. Production SPI/FPGA cases additionally exercise a peer on drive 1 and its retained status suffix |

VICE's [virtual block-command implementation][m4-p-vice] corroborates
`B-P` as an in-memory pointer update. It restricts the target to a virtual
memory buffer, unlike the native FNDRCH path, and must not override the ROM's
secondary bounds, prepared byte or sticky EOI rules.

[m4-p-vice]: https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/vdrive/vdrive-command.c

The generalized BP05/ASCHEX helper is also used by numeric direct OPEN,
preserving the previous channel allocation behavior and no-digit exclusion.
No persistent state was added: unit state remains 4600 host bytes and each
channel remains 72, below the unchanged 4608-byte unit ceiling.

Native BUFTST does not require DIRTYP. It can position an ordinary readable
channel's active page, with subsequent sequential/file-link behavior. That
path is not implemented here and returns explicit policy 30 without modifying
the reader. Write-only channels receive native 70 instead. Raw direct OPEN
still permits secondary 14, but native B-P's stricter range rejects it.
First parameters containing no numeric digits return policy 30 because
arbitrary ASCHEX scratch residue is not modeled. Later no-digit fields now
retain the hundreds nibble established by an earlier numeric field in the
same command, including `B-P:258,A` selecting byte 200.
Missing-first-parameter and other
scratch-dependent malformed forms are not accepted compatibility evidence.
The remaining block operations and utility aliases retain their existing
incomplete policy.

The 96 added direct cases cover parser and low-byte boundaries, all 256 raw
offsets, PUT after positioning, sticky EOI, rejection atomicity, secondary
13/14, excluded ordinary readers and removed/replaced media with no storage
jobs. Production cases check paused peer readers and queued bytes which must
be discarded before a new pointer takes effect. The peer's drive-1 selection
is intentionally retained in DOS 2.7's status, rather than being changed to
the direct page's drive.

#### M4 buffer-pointer evidence

`tools/drive-ref/drive_reference.py --pointer-check` captures 14 observations per
approved model. All selected original ROMs completed the scenario with
unchanged complete images. Numeric B-P is checked at offsets 0, 13, 15 and 255,
including a pending EOI followed by another position, binary CR, colonless
syntax, a longer command prefix, numeric truncation, extra parameters,
exact syntax/no-channel statuses and continued peer/direct data.
The reference directories are `.cache/ai/m4-pointer-<model>-r1/`.

Complete production-C/SPI/FPGA comparisons are retained in
`.cache/ai/m4-pointer-<model>-compare-r1/`. All actions, accepted frames and
cases were compared without prefix flags. There are no byte/ATN/EOI/direction
differences, stopped cases, omitted suffixes, wire issues or aborted frames.
All native captures report `trace_complete=true` below the unchanged
six-million-event cap. The replay executable SHA-256 is
`cad674d2df705277527c4a7029fb795684637d14ea9b0ddecb6345e718ba45a6`.

| Model | Native I/O events | Native completion | Completed/requested actions | Accepted frames | Equal initial/native/candidate image SHA-256 |
|-------|-------------------|-------------------|-----------------------------|-----------------|--------------------------------------------|
| 4040 | 286010 | 14.36 simulated seconds | 928/928 | 736 | `612bd40b23d94c5ccafe814b17e4e880807252100c7a45581996a17ed187738b` |
| 8050 | 314878 | 14.94 simulated seconds | 976/976 | 768 | `fce28fb285e32e55c62ea4d4605992c22a21949037cd4415a2152883b6695b52` |
| 8250 | 367742 | 15.34 simulated seconds | 976/976 | 768 | `9a98ffb11443219a2eb7c21899ffd9bf9e60a78f478bea9cb9cb5afeb4e14b00` |

| Retained report | SHA-256 |
|-----------------|---------|
| `m4-pointer-4040-compare-r1/comparison.json` | `28b7d16b4065b25e12af0eecfc72267f2d4dd9220f4ade0ab59bb66f23946180` |
| `m4-pointer-8050-compare-r1/comparison.json` | `414969ec578ba653d6160f28d704e632c4897b8e195fe000eb83c7a9f47953bc` |
| `m4-pointer-8250-compare-r1/comparison.json` | `ee3c3fff505a064f0e181bd86c568490fa5a1f5223b29c0eb0e30d25d0ce5d0c` |

Reproduce using the M1 reference invocation with `--pointer-check`, a fresh
ignored output directory and complete `compare_drive_reference.py` replay.
The generalized binary BASIC generator emits the same direct-channel program
as the earlier direct foundation, but this pointer checkpoint has its own
recorded harness identities. Earlier evidence retains its historical hashes.

The full direct DOS suite passes 767 cases, the complete firmware regression
and production build pass, and all 106 tooling cases pass. The complete
affected firmware/SPI/FPGA suite passes 179/179, including three new pointer
model cases. Full native images are unchanged, with no excluded unused bytes.
The captures mount native slot 0 only. Dual-drive source-derived and
production tests do not constitute the complete original-ROM dual-drive,
resource, malformed-input or fault matrix. The quoted simulation durations
are not physical timing or RP2040/SD latency measurements. M4 remains in
progress and is not operator-accepted. No SD update is requested for this
incremental slice.

#### M4 direct-channel block reads

Numeric `B-R` and the raw `U1`/`UA` aliases now read a sector into an existing
direct channel's owned page. Dispatch checks the channel and geometry without
media I/O, then cooperative work performs one sector read. No extra sector
pool or heap allocation is introduced. Unit state remains 4600 host bytes,
each channel remains 72, and the 4608-byte unit ceiling is unchanged.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Dispatch, numeric parameters and channel eligibility | 4040 revision 2, 8050/8250 DOS 2.7 | Respective [4040 block][m4-d-4040-block]/[8250 block][m4-d-8250-block], `BLOCK/BLKPAR/BKOTST/BUFTST/BLKTST`, and `tst2:FNDRCH` | Low-byte secondary, drive, track and sector use the shared bounded conversion. Mask the drive to bit 0. Check the existing readable channel before geometry. Native secondaries are 2..13, with absent/write-only channels returning 70 | `block_start`, parser and shared channel-permission tests | All-model native fifth-parameter 30 and absent-channel 70. VICE corroborates the command family but restricts it to virtual memory buffers |
| Geometry and exact errors | Same selected models | Respective [4040 jobs][m4-br-4040-jobs]/[8250 jobs][m4-br-8250-jobs], `TSCHK`, and [4040 erproc][m4-br-4040-erproc]/[8250 erproc][m4-br-8250-erproc], `HEXDEC/ERRMSG` | Reject track 0, out-of-range tracks and illegal sectors with 66 before any job. Report requested coordinates. The 4040 retains two decimal digits, while DOS 2.7 emits hundreds | `block_start`, `test_block_read_parser_geometry_and_user_aliases`, production three-model error cases | Selected revision-2 binary capture reports `55,55` for requested `255,255`. Both DOS 2.7 captures report `255,255` |
| Counted record and fresh EOI | Same selected models | Respective `block:BLKRD2/BLKRD3/BLKRD` and `tst4:DRTRD/DRT/RNGET1/RNGET2/RNDGET` | Put page byte 0 into LSTCHR, clear old EOI, start at byte 1 and set EOI on equality. After EOI, circular GET repeats bytes 1..LSTCHR. Count 0 spans bytes 1..255,0, while count 1 repeats byte 1 with EOI | `block_read_work`, `test_block_reads_counted_and_raw_boundaries` | Native four-byte counted record and its repeat include binary CR, zero and 255. Counts 0, 1, 2, 4, 254 and 255 are source-derived direct cases |
| Raw user aliases | Same selected models | Respective `block:USER/UBLKRD`, user-vector table, and `tst4:RNGET1/RNGET2/RNDGET` | Fold the selector by `((selector - 1) & 15)`. The read vector includes U1, UA and U!. Expose byte 0 immediately, set LSTCHR 255 and clear old EOI. First record spans 0..255, later records 1..255 | USER dispatch, `block_read_work`, alias/parser and boundary cases | All-model native U1 raw page, repeated raw prefix, UA restart and U! no-channel status. VICE corroborates raw pointer 0, not every native selector or EOI rule |
| Page ownership and job drive | Same selected models | Respective `tst4:DRTRD/DRT`, `jobs:DOIT2/SETJOB/SJB1`, and initialization MAPOUT paths | Read into the existing active page and record the actual job drive. Preserve peers and allow read-only media. INITIALIZE subsequently identifies the direct page by its latest job drive | `test_block_read_drive_affinity_and_readonly_last_track`, production peer cases | Source-derived reads cover both drives and each model's last legal track/sector. Native captures cover slot 0, not the complete dual-drive matrix |
| Cooperative transport and storage failures | Native command sequencing plus explicit host media policy | Respective `parsex:PARSXQ/ENDCMD`, `block:BLKRD2`, and existing production IEEE dispatch | Retire stale TX previews before replacing the prepared byte. Do not advance a paused peer. Retain media identity so deferred work cannot access replacement media | `BlockReadsReplaceStagedBytesAndRetainPeer`, `test_block_read_failures_and_stale_identity` | Complete native transaction replay covers binary bytes, EOI and retained cursors. Callback failure/remount handling is host policy, not established native controller-fault parity |

[m4-br-4040-jobs]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/jobs
[m4-br-8250-jobs]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/jobs
[m4-br-4040-erproc]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/erproc
[m4-br-8250-erproc]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/erproc

Native FNDRCH also permits ordinary readable channels. Their raw block-read
and subsequent sequential/file-link behavior remains explicitly deferred
(policy 30 without changing the reader). Write-only channels receive native
70. B-P shares the same eligibility check. Numeric conversion's existing
no-digit/scratch-residue exclusion remains. Mechanical retries, controller
fault detail and AUTOI behavior are not established by this slice.

A failed host read retains the prepared byte, pointer and readiness. The
page's job/media affinity follows the attempted job. A callback which
partially overwrites its output may leave partially changed RAM, so this is
not an atomic page replacement guarantee. Stale-generation and removed-media
cases issue no callback against a replacement. They are host lifecycle
policies, not native media-change acceptance.

#### M4 block-read evidence

`tools/drive-ref/drive_reference.py --block-read-check` captures 14 observations per
approved model using the same pinned ROMs and PET ROM 4 controller as the
preceding slices. A known binary page is placed in unallocated sector 1/0
before the initial snapshot. The generator first checks its free BAM bit and
does not change allocation metadata. This fixture intentionally has different
initial image hashes from the preceding direct/pointer scenarios.

The original-ROM scenario covers a counted record and its repeat, a rejected
fifth parameter, the complete raw page with EOI at byte 255, a repeated raw
prefix, UA restart, exact model-specific 66, folded U! no-channel 70, and
continued peer/direct data. Binary CR, zero and 255 are data, not status
terminators. Native captures are in `.cache/ai/m4-block-read-<model>-r1/`.

Complete production-C/SPI/FPGA comparisons are retained in
`.cache/ai/m4-block-read-<model>-compare-r1/`. No prefix flags were used.
Every requested action, accepted frame and case was compared, with no
byte/ATN/EOI/direction differences, omitted suffixes, stopped cases, wire
issues or aborted frames. All native traces report `trace_complete=true`
below the unchanged six-million-event cap. Current harness identities match
each capture's recorded hashes. The replay executable SHA-256 is
`6736970a0228788391ca938a4012fd604306ac7657b681272536da01c6da30db`.

| Model | Native I/O events | Native completion | Completed/requested actions | Accepted frames | Equal initial/native/candidate image SHA-256 |
|-------|-------------------|-------------------|-----------------------------|-----------------|--------------------------------------------|
| 4040 | 483967 | 27.54 simulated seconds | 2386/2386 | 1616 | `d067119724e84bdf45bfc56d09a6e8b94a2b89bd48bc5174695b6e7339b26264` |
| 8050 | 569177 | 28.74 simulated seconds | 2458/2458 | 1664 | `af27ed5b4394295d2a0e41a97d82e632c9e843be2c20550a3b0c4e8a985a26a8` |
| 8250 | 622037 | 29.14 simulated seconds | 2458/2458 | 1664 | `eddf09f8243310bf08d9270f1b84ec3e1223e9cb7535f933d459bc1150b88f03` |

| Retained report | SHA-256 |
|-----------------|---------|
| `m4-block-read-4040-compare-r1/comparison.json` | `255f2ea56d47fab716a8cefba0634f5933d4a63adb954afa4d300268a92b4616` |
| `m4-block-read-8050-compare-r1/comparison.json` | `3aa545cd0a6d4b2f3d0489f2d05a49c24fa28bc8b3a71979ed9237335ef42fce` |
| `m4-block-read-8250-compare-r1/comparison.json` | `1ee2dcef909c5a6a91e13ad511a40bd8a75790b63557ab28c5ea90aeb1794c0d` |

Reproduce using the M1 reference invocation with `--block-read-check`, a fresh
ignored output directory, and complete `compare_drive_reference.py` replay.
The complete initial, native-final and candidate-final files were independently
hashed. They are unchanged in their entirety, without unused-byte exclusions.

The direct DOS suite passes 890 cases (123 added for block reads and expanded
channel eligibility), the complete firmware regression and production build
pass, and all 112 tooling cases pass. The complete affected firmware/SPI/FPGA
suite passes 182/182, including three new block-read model cases. The status
test helper now services firmware between received bytes so long 66 messages
can span multiple TX chunks, like the existing payload helper.

The direct cases additionally cover stale EOI reset, count boundaries,
positioning and PUT after a counted read, secondary 13/14, excluded ordinary
readers, read-only media, drive-bit masking, last-track geometry and deferred
storage failures. These are source-derived host cases where no corresponding
native observation is listed above. Native captures mount slot 0 only.
Complete dual-drive, malformed-input, pressure and fault/media matrices remain
open. Simulation durations are not physical SD/RP2040 latency or timing
evidence. M4 remains in progress and is not operator-accepted. No SD update is
requested for this incremental slice.

#### M4 direct-channel block writes

Numeric `B-W` and raw `U2`/`UB` now write the existing direct channel's
complete 256-byte page. Dispatch shares block-read parameter, channel and
geometry checks without issuing storage I/O. Cooperative work performs the
write and a bounded host flush before publishing success. No buffers or
channels are allocated. The shared transient job union keeps unit/channel
state at 4600/72 host bytes, below the unchanged 4608-byte unit ceiling.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Parameters, aliases and precedence | 4040 revision 2, 8050/8250 DOS 2.7 | Respective [4040 block][m4-d-4040-block]/[8250 block][m4-d-8250-block], `BLOCK/BLKPAR/USER/BKOTST/BUFTST/BLKTST` | Use low-byte parameters and the low drive bit. Existing readable secondaries 2..13 precede geometry checks. The write vector includes U2, UB, UR and other matching low-nibble selectors | `block_start`, USER dispatch, parser and shared eligibility matrices | All-model native UB success, folded UR no-channel 70 and exact invalid-coordinate 66. Other numeric/alias cases are source-derived host tests |
| Count byte and whole-page write | Same selected models | Respective `block:BLKWT/BW10`, `tst4:GETPNT/SETPNT`, and `tst2:PUTBYT` | Store `max(1, pointer - 1)` in byte 0, reset pointer to 1, and write all 256 bytes. Pointers 0 and 1 produce count 1. Do not update LSTCHR or BAM allocation | `block_write_work`, `test_block_write_pointer_count_and_raw_state` | Native pointer 5 writes count 4, pointer 0 writes count 1 and pointer 255 persists count 254. Full destination-sector comparison includes unused tail bytes. VICE independently corroborates the count formula and pointer 1 |
| Prepared data and sticky EOI | Same selected models | Respective `block:BW20` and `tst4:RNGET2/RNGET4` | After successful B-W, prepare byte 1 using the existing LSTCHR. Set EOI on equality without clearing an existing EOI. A newly written count is not a new read limit | Count/LSTCHR/EOI matrix and production `BlockWritesPreserveStateAndPersistAfterRemount` | Native four-byte GET after B-W has no EOI until a separate B-R establishes the new counted record |
| Raw write leaves channel state intact | Same selected models | Respective `block:UBLKWT` and `tst4:DRTWRT/DRT` | Write the page without changing byte 0, cursor, prepared byte, LSTCHR or pending EOI | Raw-state matrix, native raw-state case and production model cases | Native U2 retains pending EOI on byte 0, and UB uses the same raw write vector. VICE corroborates direct unmodified sector output |
| Job drive and ownership | Same selected models | Respective `tst4:DRTWRT/DRT` and [4040 jobs][m4-br-4040-jobs]/[8250 jobs][m4-br-8250-jobs], `DOIT2/SETJOB/SJB1` | Use the active owned page and record the actual job drive. Preserve peers, without allocating a file or marking the destination used | `test_block_write_drive_affinity_and_last_track`, production paused drive-1 reader | Host cases cover both drives, drive-bit masking and each model's last legal sector. Native slot-0 images change only sector 1/1, leaving all allocation metadata unchanged |
| Failure retention and host durability | Native command ordering plus explicit host storage policy | Respective `block:BLKWT/UBLKWT`, `tst2:PUTBYT`, and `jobs:SETJOB/WATJOB` | B-W mutates the count and pointer before submitting the job. Do not refresh its prepared byte after a failed job. Preserve the owned page and peer, and reject stale identities | `test_block_write_failures_and_stale_identity`, shared `write_sector/storage_error`, remount integration | Injected write, flush, read-only, removed and replacement-media outcomes are host policy. Native captures establish successful persistence, not complete controller-fault/retry parity |

VICE's [virtual block-command implementation][m4-p-vice] corroborates the
counted/raw distinction, whole-sector writes and B-W pointer reset. Its
virtual-buffer eligibility and error handling do not override the selected
ROM's FNDRCH, sticky EOI or pre-job mutation order. The original-ROM reference
executes both CPUs using the pinned MAME hardware model rather than VICE's
virtual DOS shortcut.

Ordinary readable-channel writes remain policy 30, as with the deferred
B-P/B-R file-link interactions. Write-only channels receive native 70.
No-digit scratch-dependent forms remain excluded. Native SETJOB also checks
the DOS version before writes. Mismatched-version rejection, metadata-sector
and cached-BAM interactions, mechanical/controller fault recovery and the
complete media/resource matrix remain unaccepted work, not implied parity
from the successful normal-image cases.

The host flush is an explicit persistence policy, not a native timed command
phase. On write or flush failure, B-W retains its already-mutated count and
pointer. Resubmitting B-W recomputes the count from that new pointer. A raw
U2/UB retry preserves the intended page and is covered by the failure tests.
There is no atomic-write or rollback guarantee. Identity checks prevent a
stale job from writing or subsequently flushing replacement media.

#### M4 block-write evidence

`tools/drive-ref/drive_reference.py --block-write-check` captures 14 observations per
approved model. The fixture requires sectors 1/0 and 1/1 to be free, seeds a
known binary source in 1/0 and zeros the distinct destination in 1/1. Neither
fixture preparation nor commands change BAM ownership. The scenario covers
counted writes/readback, raw aliases, preserved EOI, minimum and maximum
counts, exact errors and resumed peer data.

Native captures are `.cache/ai/m4-block-write-<model>-r1/`. Complete
production-C/SPI/FPGA comparisons are retained in
`.cache/ai/m4-block-write-<model>-compare-r1/`, without prefix flags.
Every requested action, accepted frame and case was compared. There are no
byte/ATN/EOI/direction differences, omitted suffixes, stopped cases, wire
issues or aborted frames. All native traces are complete below the unchanged
six-million-event cap, and their recorded harness identities match the
current files. Earlier slice captures retain their historical harness hashes.
The replay executable SHA-256 is
`083549c6c3b4802f682c6c0d176eed33826b1f712ed2102ffabea58b6aa384a7`.

| Model | Native I/O events | Native completion | Completed/requested actions | Accepted frames | Equal native/candidate final image SHA-256 |
|-------|-------------------|-------------------|-----------------------------|-----------------|-----------------------------------------|
| 4040 | 671180 | 16.20 simulated seconds | 936/936 | 682 | `386771a3d6c3fc7b248a3bde2714995b5bc4ae0c703d4dc6739a6ae740c64220` |
| 8050 | 684765 | 16.86 simulated seconds | 1008/1008 | 730 | `f420ea09e95e014751bda06798a040cb94a528219bc4858e20ee023c867a74ed` |
| 8250 | 737626 | 17.26 simulated seconds | 1008/1008 | 730 | `56968e3925ef8b94677cfe5e076770ccb721f657bca9d351471d1cc4addf21a1` |

| Retained report | SHA-256 |
|-----------------|---------|
| `m4-block-write-4040-compare-r1/comparison.json` | `9b378a8932bb8ccc7f0472bc6472f38b90db343ea422104f0c95b33f88c33d94` |
| `m4-block-write-8050-compare-r1/comparison.json` | `f19b640fee80be63e634e56d83dccef31686d97ca010bfe247683907b4fd65c1` |
| `m4-block-write-8250-compare-r1/comparison.json` | `a631c9a78ed140e5b7bc97726917bad18b901ccb9fecd993d8fe7e279531f645` |

Initial hashes equal the preceding block-read fixture hashes. For every model,
the complete final native and candidate files match exactly. Only sector 1/1
changes, with final byte 0 equal to 254 and every remaining byte equal to the
source page. There are no excluded unused bytes or metadata differences.
Reproduce using the M1 invocation with `--block-write-check`, a fresh ignored
directory, and complete `compare_drive_reference.py` replay.

The direct DOS suite passes 1073 cases (183 added or expanded for this slice),
the complete firmware regression and production build pass, and all 118
tooling cases pass. The complete affected firmware/SPI/FPGA suite passes
185/185, including three new write/remount model cases. The production cases
also resume a paused drive-1 reader and check all 256 destination bytes after
unmount/remount. Native captures use slot 0 only. Simulation durations are not
physical SD/RP2040 latency evidence. M4 remains in progress and is not
operator-accepted. No new SD update or destructive hardware batch is requested.

#### M4 block allocation and free

`B-A` and `B-F` now operate on the selected drive's native BAM without
requiring or allocating a direct channel. Dispatch retains the media identity
and three low-byte parameters without storage I/O. Cooperative work processes
at most one searched track per call, yielding between full tracks. It shares
the existing per-unit BAM cache, not the twelve data pages. Unit/channel
state remains 4600/72 host bytes, below the unchanged 4608-byte unit ceiling.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Parameters, drive and geometry | 4040 revision 2, 8050/8250 DOS 2.7 | Respective [4040 block][m4-d-4040-block]/[8250 block][m4-d-8250-block], `BLOCK/BLKPAR/BLKTST/BT05` | Use drive, track and sector, masking drive to bit 0. No channel check. Retain the shared four-value parser limit and model-specific 66 coordinate formatting | `block_start`, `test_block_map_parser_geometry_and_no_channel` | All-model original-ROM invalid 255/255 free gives exact 66. Parser/drive-mask boundaries are source-derived host cases |
| Idempotent free | Same selected models | Respective `block:BLKFRE`, [4040 frets][m4-ba-4040-frets], `FRETS`, and [8250 frets][m4-ba-8250-frets], `JFRETS/FRETS2/DTYBAM` | Free the requested bit. Increment the count only when allocated becomes free. Repeated free succeeds without a second increment or immediate map write | `block_map_work`, `mark_sector`, `test_block_map_claim_free_and_close_persistence` | Native repeated B-F and complete final-image comparison on all models |
| Requested allocation | Same selected models | Respective `block:BLKALC/BA10/BA20/BA40`, [4040 tstfnd][m4-ba-4040-tstfnd], `AVAIL/AV2/AVCK`, and [8250 tstfnd][m4-ba-8250-tstfnd], `GETSEC/GS10/GS20/GS30/AVCK` | Validate bitmap/count, claim a free requested sector and return OK. Otherwise report 65 with a successor, without claiming that successor | `block_map_work`, `valid_bitmap`, claim/free and successor tests | Native claim, 65,01,01 suggestion and subsequent successful claim. Independent final-image checker requires exactly the two requested claims |
| Successor order and exhaustion | Same selected models | Respective `block:BA10/BA20/BA40` and the preceding `AVAIL/GETSEC` routines | Search higher sectors, then higher tracks from sector 0. Do not wrap. Directory-track suggestions are allowed. Exhaustion returns 65,00,00 | `test_block_map_successor_search_and_exhaustion`, production `BlockMapSuggestionsAndFreesPersistAfterRemount` | Native last-sector exhaustion on all models. Same-track, later-track, directory-track and linked-BAM boundary cases are source-derived host coverage |
| Dirty-map publication | Same selected models | Respective [4040 close][m4-d-4040-close]/[8250 close][m4-d-8250-close], `MAPOUT/MAPCHK`, and [8250 map][m4-v-8250-map], `SETMAP/RDBAM/RDNBAM/SCRBAM` | Mutate cached BAM first. Appropriate CLOSE publishes it. DOS 2.7 cached-part replacement can publish an earlier dirty part before CLOSE | `bam_entry`, `mark_sector`, `flush_bam`, claim/free CLOSE and remount tests | Complete native/candidate images match after direct CLOSE. Production remount checks read the persisted count and bitmap through U1/B-P |
| Read-only and failed persistence | Native cache ordering plus explicit host storage policy | Respective `block:BLKFRE/BLKALC`, `frets:FRETS/JFRETS`, and `close:MAPOUT` | RAM mutation does not immediately test write protection. A later dirty CLOSE may fail and must retain ownership for retry. Never publish onto replacement media | `test_block_map_failures_and_readonly_close`, existing media-identity and CLOSE machinery | Source-derived host read-only, read failure, removed/replaced binding, invalid count and flush-retry cases. Native controller-fault and complete read-only matrices remain unaccepted |

[m4-ba-4040-frets]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/frets
[m4-ba-8250-frets]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/frets
[m4-ba-4040-tstfnd]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tstfnd
[m4-ba-8250-tstfnd]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tstfnd

VICE's [virtual block commands][m4-p-vice] corroborate allocation/free and
error 65, but check read-only media immediately and use a generic successor
allocator. Those shortcuts do not override the selected ROM's deferred RAM
mutation or forward-only search. Suggested sectors remain available until
explicitly claimed. A map operation leaves existing direct-page contents,
prepared bytes, cursor/EOI state and paused peer streams alone.

Malformed-map error-coordinate parity is not accepted. The shared host
validator reports 71 at track/sector 0, while native routines can retain
different sector state. Native low-free warning/status behavior also needs
its own comparison matrix. The normal-image evidence below does not establish
those cases or every DOS 2.7 dirty-part transition.

#### M4 block allocation/free evidence

`tools/drive-ref/drive_reference.py --block-map-check` captures fourteen observations
per model, including startup status, a paused peer, the binary direct-page
number, repeated frees, claims, an unclaimed successor, exact invalid-geometry
status, last-sector exhaustion, resumed peer bytes and direct CLOSE/reopen.
The fixture independently reserves the last legal sector before capture.
The final-image checker independently resolves native BAM links and declared
ranges, then requires only track 1's count decrement by two and clearing
its sector-0/1 bits. Every other image byte must remain unchanged.

Native captures are `.cache/ai/m4-block-map-<model>-r1/`. Complete
production-C/SPI/FPGA comparisons are retained in
`.cache/ai/m4-block-map-<model>-compare-r1/`, without prefix flags.
Every requested action, accepted frame and case was compared. There are no
byte/ATN/EOI/direction differences, omitted suffixes, stopped cases, wire
issues or aborted frames. Native traces are complete below the unchanged
six-million-event cap. Current capture harness hashes, replay identity,
reference-report hashes and immutable input hashes were checked against the
retained files. Earlier slice captures retain their historical identities.
The replay executable SHA-256 is
`2891f251f81d2764933491c368fe6d3e0eca50142be27b5efd0160722be6778b`.

| Model | Native I/O events | Native completion | Completed/requested actions | Accepted frames | Equal native/candidate final image SHA-256 |
|-------|-------------------|-------------------|-----------------------------|-----------------|-----------------------------------------|
| 4040 | 332231 | 16.84 simulated seconds | 1233/1233 | 853 | `da207a4d1d5ee76e7ae85330abb8602fa47c3abc929f7e2a3b853a67e93ef7b9` |
| 8050 | 536554 | 19.52 simulated seconds | 1377/1377 | 949 | `e8e03998a6106db30ee97f00c02e47826fe890be16dcc684dc8c8011dcf10cee` |
| 8250 | 594881 | 19.96 simulated seconds | 1378/1378 | 950 | `dd6b11823ef242abbcc98f96dc386df84aa867b65780b14968d43ca7ff443074` |

| Retained report | SHA-256 |
|-----------------|---------|
| `m4-block-map-4040-compare-r1/comparison.json` | `0cebc5f5a15c6962d7331aa32c0e2e356c2c26b9475ad1e485ef83b7583b0cab` |
| `m4-block-map-8050-compare-r1/comparison.json` | `8430cb17306f9d0cb77767043da07cff8ced06eef99ed7944557970f5b3d3790` |
| `m4-block-map-8250-compare-r1/comparison.json` | `31285fb6690f30278e86bd5acb7ea09d738fd0a56ce93c5fc41de13ab061e498` |

| Model | Immutable initial image SHA-256 |
|-------|--------------------------------|
| 4040 | `7fe305b55e8b9a333636d1de6a8e6fd2fe6c0e1fa3d812e7909a2ea189f352ed` |
| 8050 | `50297b92a80a9086f7b49577dacd1c1fb4221b5679551e5b5633db1c6d6bc0a2` |
| 8250 | `3592cfd48f46303a98fd907974542374d0b50384bac5251ca6e7efd62e0aa2f3` |

Only absolute sector 357 (D64) or 1073 (D80/D82) changes. The independent
checker passed on both native and candidate complete final files, with no
unused-byte or metadata exclusions. Reproduce using the M1 invocation with
`--block-map-check`, a fresh ignored directory, and complete
`compare_drive_reference.py` replay.

The direct DOS suite passes 1154 cases (81 added for this slice), the complete
firmware regression and production build pass, and all 123 tooling cases
pass. The complete affected firmware/SPI/FPGA suite passes 188/188, including
three new allocation/remount model cases. Production cases preserve a paused
drive-1 peer, verify persisted BAM through raw reads after unmount/remount,
and exercise cooperative searches across full tracks. Direct cases cover both
drives, parser/geometry boundaries, no-channel operation, idempotency, exact
image persistence and host storage failures. Native captures use slot 0 only.
Complete dual-drive, malformed-input, low-free/resource-pressure and fault/media
matrices remain open. Simulation durations are not physical SD/RP2040 latency
or timing evidence. M4 remains in progress and is not operator-accepted.
No new SD update or destructive hardware batch is requested.

#### M4 cached-BAM write-version checks

The shared native write path now checks the selected drive's currently cached
BAM version before calling the backing writer. A zero byte is allowed, as is
the selected model's version (`A` for 4040 revision 2, `C` for 8050/8250 DOS
2.7). Other values return the native 73 version message with the rejected
job's track/sector, without writing or flushing media. This is the same text
as startup status, not a reset. Reads remain permitted.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Current cached version | 4040 revision 2, 8050/8250 DOS 2.7 | Respective [4040 jobs][m4-br-4040-jobs]/[8250 jobs][m4-br-8250-jobs], `SETJOB/SJB2/SJB3/SJB4`; [4040 equate][m4-wv-4040-equate]/[romtbl][m4-wv-4040-romtbl], `FM2040/SECTRK`; [8250 begin][m4-wv-8250-begin], `CONFIG` | Compare selected drive's `BAM0/1+2` with VERNUM, accepting zero. Do not fetch a fresh version for each job | `write_version`, `test_write_version_counted_raw_and_drive`, `test_write_version_read_retry_and_cached_part` | All nine selected-ROM model/version captures. Host cases cover both drives, both commands, matching/zero/255/other-model versions and unchanged cached versions until reload |
| DOS 2.7 map versus header | 8050/8250 DOS 2.7 | [8250 tst2][m4-wv-8250-tst2], `INITDR/NF05/NF10/NF20`, and [frets][m4-ba-8250-frets], `SETMAP/RDBAM/RDNBAM` | Free-count initialization replaces the directory-header page with successive allocation-map parts. Subsequent writes use whichever part is cached | Existing `initialize_work/bam_entry`, `test_write_version_follows_cached_map_part` | Native zero/C map versions allow writes despite deliberately foreign directory-header byte 2. A header-only foreign-version pilot also allowed writes, so header-only rejection was discarded |
| Exact rejection record | Same selected models | Respective `jobs:VNERR/HED2TS` and `erproc:CMDER2/ERRMSG/JERRMS/ERRTAB` | Reject foreign version with `73,CBM DOS V2,01,01` or `73,CBM DOS V2.7,01,01,0`, followed by CR with EOI. Keep job coordinates, not startup's 00,00 | Shared `dos_status`, direct version matrix and production `BlockWriteVersionsRejectForeignAndAcceptZero` | Complete native/candidate status bytes and ATN/EOI/direction match for both B-W and UB on each model |
| Pre-job counted state | Same selected models | Respective `block:BLKWT/BW10/BW20`, `tst2:PUTBYT`, `tst4:DRTWRT`, and `jobs:SETJOB` | B-W changes byte 0 and pointer before version rejection. Failed job does not prepare byte 1 or reset LSTCHR/EOI | `block_write_work`, counted/raw version matrix | Native pointer 5 becomes count 4 and pointer 1, but rejected GET starts with the old prepared byte 5, then 0,255,42. Successful versions start with 13,0,255,42 instead |
| Raw state and read isolation | Same selected models | Respective `block:UBLKWT/UBLKRD`, `tst4:DRTWRT/DRTRD/RNGET2`, and `jobs:SETJOB/SJB1` | Raw write does not mutate count/cursor/prepared state. A version rejection neither closes peers nor prohibits raw or ordinary reads | Direct matrix, native raw-state/readback/peer observations and production remount cases | Native UB retains prepared byte 5. Foreign-version destination remains wholly zero. Paused peer continues at its next byte on all models |
| Shared write gating and ownership | Same selected models plus host lifetime policy | Respective `jobs:SETJOB`, directory `tsutil:WRTOUT/WRTAB`, and `close:MAPOUT`; DOS 2.7 `frets:SCRBAM` | Apply the check to sector-writing paths, including dirty-map and directory writes. Retain owned state after failed host CLOSE and reject stale identities before I/O | `write_sector`, `test_write_version_metadata_jobs`, existing stale-media/write-retry matrices | Source-derived host rename and dirty direct-CLOSE rejection cases. These are not a complete native command-by-command wrong-version/fault matrix |

[m4-wv-4040-equate]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/equate
[m4-wv-4040-romtbl]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/romtbl
[m4-wv-8250-begin]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/begin
[m4-wv-8250-tst2]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2

No separate remembered version, page pool or allocation was added. Unit/channel
state remains 4600/72 host bytes. The existing host lazy-initialization policy
loads metadata on the first write if no BAM part is resident, selecting the
last part that native INITDR's free-count scan leaves cached. That bounded
cold check can read one/two/four map sectors before writing. Dispatch still
does no I/O, and an initialized write performs no version-check read.
Failed map reads do not publish partial cache contents or permit the write.
Warm caches are never replaced just to check the version.

Native evidence here explicitly initializes before testing writes. It does
not establish every cold-start/direct-only case or partial-initialization
failure sequence. The later M4 edge comparisons extend mixed map versions,
raw metadata writes and dirty-cache publication. Arbitrary malformed maps,
physical write protection and controller-error precedence remain separate
limitations.
The host tests demonstrate cached-part changes, initialization refresh,
failed-read retry, directory-write rejection and retained dirty CLOSE
ownership, without promoting them to complete native acceptance.

#### M4 write-version evidence

`tools/drive-ref/drive_reference.py --write-version-check foreign|zero|native` captures
twelve observations per run. Each model runs all three version cases.
The foreign byte is 255. DOS 2.7 fixtures set every map part to the selected
test version and deliberately leave directory-header byte 2 at 255.
The scenario covers initialization, ordinary/raw reads, exact B-W and UB
statuses, retained direct state and resumed peer bytes.

Native captures are `.cache/ai/m4-write-version-<model>-<version>-r2/`.
Complete production-C/SPI/FPGA comparisons are retained in matching
`m4-write-version-<model>-<version>-compare-r2/` directories.
All nine comparisons include every requested action, accepted frame and case,
without prefix flags, omitted suffixes, stopped cases, wire issues or aborted
frames. There are no byte/ATN/EOI/direction or full-image differences.
All native logs are complete below the unchanged six-million-event cap.
Capture harness hashes, reference/report/input identities and the replay
executable were checked against retained files. Earlier slice captures retain
their historical harness identities. The current replay SHA-256 is
`81f050bed6303dc1820bfa4a6b3502fb79cac28a20a8f42b3e0af91b63585130`.

| Model/version | Native I/O events | Native completion | Completed/requested actions | Accepted frames | Equal native/candidate final image SHA-256 |
|---------------|-------------------|-------------------|-----------------------------|-----------------|-----------------------------------------|
| 4040 foreign | 244007 | 13.24 simulated seconds | 807/807 | 557 | `60f1d9fc3da167b5728672ee0ef61710345345d370fc2e058496fcf8a340c7be` |
| 4040 zero | 429566 | 14.00 simulated seconds | 723/723 | 501 | `a85d1dc68c45dc0ee8c79f7d922e116ff7351fd0c0d5edc852517c20c3445887` |
| 4040 native | 429565 | 14.00 simulated seconds | 723/723 | 501 | `81729808ffad0fd97637fb6daa043733fdef18b30269d90bfd32c0198d00ed38` |
| 8050 foreign | 496989 | 16.02 simulated seconds | 915/915 | 629 | `23bf7577df961e4667e0b40585622ffafdbc1aa1833666d9627482f296b8ce4f` |
| 8050 zero | 425783 | 14.62 simulated seconds | 807/807 | 557 | `352febe00b12b9afe759278eac9676aad34593882aa6adfa5381017606fed4f5` |
| 8050 native | 425783 | 14.62 simulated seconds | 807/807 | 557 | `1b9071033d57e8f3b171b8e7d61d446cadedc3f8ac92c3504ce06d72a100a0c2` |
| 8250 foreign | 576283 | 16.62 simulated seconds | 915/915 | 629 | `1e080efa7532018a3a5dcad5cf0c5f50d6a02ec6a1ecd0b149e45553abbc200e` |
| 8250 zero | 505071 | 15.22 simulated seconds | 807/807 | 557 | `6c2e87e085ecd4921e74473144d6684f393499e7581fba04b7e8a8de0a6f0db5` |
| 8250 native | 505073 | 15.22 simulated seconds | 807/807 | 557 | `36e6e57783f3d464dd69c74cec838bca5ac4964453cbd2ee27fe1645395d1768` |

| Retained comparison report (model/version, `comparison.json` in the directory above) | SHA-256 |
|-----------------------------------------------------------------------------------|---------|
| 4040 foreign | `67f327a0febc3f2f47ff0a306939cf0b6aa4b0cdf9a4bed99f161e850d5f3a09` |
| 4040 zero | `fe193e1ce2648185a58a35a918990157134eebf26d55a0648c630273259862b8` |
| 4040 native | `f570bb3d4f5d4bc1964905b3a3eca6d53b7c167d393781658845ed7b51cd624e` |
| 8050 foreign | `de7c89dffd88a4084b498f2cae11468c37fb0d601f71dcdb692a42fc10c5b43d` |
| 8050 zero | `d69e3fe270b881235aa53bbaf61b68c227f8bb598d1c3614ba76229140b31609` |
| 8050 native | `b9f1ddbc902a61bd3c2c00931af3071adf0bbfddf15717444dbf290649ecd468` |
| 8250 foreign | `340ac86965a37db05e35c791f77b496c175e5300de23e4f26e9574a3e4d2beac` |
| 8250 zero | `bd68f9841596cb0d3a3b358d94ac2b6471ee7bfb2d3645ca4a36cdb329617c63` |
| 8250 native | `f61d8012664c7df927258e9a40d9e0c2efa53dd2fecfe9305d23ecb78f3bddc0` |

Every foreign-version final image equals its complete immutable input.
For zero and matching versions, only absolute sector 1 (track 1/sector 1)
changes, becoming the complete source page with count 4. The independent
image checker passed on native and candidate files for all nine runs,
including every unused tail byte and all metadata. Reproduce using the M1
invocation with each `--write-version-check` value and complete comparison
replay into fresh ignored output directories.

The direct DOS suite passes 1216 cases (62 added and cold-read expectations
updated), the complete firmware regression and production build pass, and
all 127 tooling cases pass. The complete affected firmware/SPI/FPGA suite
passes 191/191. Its three new model cases each exercise all three versions,
preserve a paused drive-1 reader and check all 256 destination bytes after
unmount/remount. Native captures use slot 0 only. Simulation durations are
not physical SD/RP2040 latency evidence. M4 remains in progress and is not
operator-accepted. No new SD update or destructive hardware batch is requested.

#### M4 warm-reset commands

`U9`/`UI` now enter the selected ROM's warm-reset path rather than returning
the previous unsupported-command status. Unlike cold `U:`/`UJ`, warm reset
releases channels and page ownership without clearing data RAM or finalizing
unfinished writers. Mounted-image bindings, buffer drive bits and last
drive/program state survive. The command buffer also survives, so a short
`U` can repeat the previous warm selector. Startup status is 73, with zero
track/sector and final CR/EOI.

The model difference is deferred metadata initialization, not immediate
DOS 2.7 RAM clearing. The 4040 retains dirty BAMs. DOS 2.7 startup sets each
drive's write-protect-change flag, and a later `AUTOI` caller reloads that
drive without publishing its old dirty map. Direct OPEN and ordinary file
lookup are such callers. `SETLDS` only changes activity LEDs and does not
call `AUTOI`. Consequently B-A/B-F before a subsequent lookup/direct OPEN
can still observe the retained claim. Consuming one drive's flag must not
discard the other drive's cache.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Selector folding and stale command bytes | 4040 revision 2 and 8050/8250 DOS 2.7 | Respective [4040 block][m4-wr-4040-block]/[8250 block][m4-wr-8250-block], `USER/USREXC` | Index `((selector - 1) & 15) == 8` selects warm reset. Preserve command bytes on this non-returning path, including a short `U` reading the previous selector | `DOS_USER_WARM_RESET_INDEX`, `command_start`, six-alias and overlong-request direct cases | Native UI, short-U-without-CR and U9 observations on all three selected models |
| Warm versus cold vector | Same selected models | Respective [4040 vector][m4-wr-4040-vector]/[8250 vector][m4-wr-8250-vector], `UBLOCK/NMI/VNMI`; [4040 dskint][m4-wr-4040-dskint], `DIAGOK`; [8250 begin][m4-wv-8250-begin], `BEGIN/INTTAB` | Enter default NMI -> VNMI -> warm startup, not DSKINT's cold RAM diagnostic/clearing loops. Release old channel ownership without normal CLOSE | `warm_reset`, `test_warm_reset_retains_ram_and_model_specific_maps` | Original-ROM old-reader timeout, repeated warm statuses and retained `WARM` bytes in explicitly reopened page 11 |
| Retained 4040 maps | 4040 revision 2 | [4040 dskint][m4-wr-4040-dskint], `DIAGOK`, and [close][m4-wr-4040-close], `MAPOUT` | Warm startup does not INITDR or flush the map. A later direct CLOSE can publish the retained dirty claim | `warm_reset`, existing `finish_channel/flush_bam`, production warm-reset model case | Native pre-OPEN and post-OPEN B-A both return `65,NO BLOCK,01,01` plus CR/EOI. Final image differs only in the expected BAM count/bit |
| Deferred per-drive DOS 2.7 reload | 8050/8250 DOS 2.7 | [begin][m4-wv-8250-begin], `CONFIG/CONF10`; [romtbl][m4-wr-8250-romtbl], `MSCCOM`; [lookup][m4-wr-8250-lookup], `JAUTOI`; [tst2][m4-wv-8250-tst2], `INITDR/NF05` | Set both WPSW flags at warm startup. AUTOI consumes the selected flag, closes its channels and reloads metadata without MAPOUT. Do not discard dirty maps at reset itself | `dos_bam_t.reinitialize`, `auto_initialize`, independent-drive and entry-point direct matrices | Native pre-OPEN B-A returns `65,NO BLOCK,01,01,0`, but post-OPEN B-A succeeds. After the second warm reset/reopen/CLOSE the complete image equals its immutable input |
| Actual automatic-initialization entry points | Same DOS 2.7 models | [block][m4-wr-8250-block], `OPNBLK`, and [lookup][m4-wr-8250-lookup], `LOOKUP/JAUTOI`; [frets][m4-ba-8250-frets], `JSETLD` | Direct OPEN calls AUTOI before parsing/allocation, and file searches call it on the searched drive. SETLDS and block allocation alone do not consume it | `open_direct`, ordinary OPEN worker, rename/scratch/copy search workers, `test_warm_reset_auto_initialize_entry_points` | Complete native direct-OPEN comparison plus source-derived host block-only, missing-file OPEN, rename, scratch and copy cases. This is not a complete native command-by-command post-warm matrix |
| Status, readiness and unit isolation | All three selected models, PET ROM4 KERNAL 901465-22 | Warm startup routines above, respective `erproc:ERRMSG/JERRMS` and PET `ob1src:ACPTR` | Warm reset returns 73 at 00,00. Released sources time out instead of supplying a drive CR/EOI. Reset one unit without advancing another unit's paused reader | Timeout-aware observation checker, `WarmResetRetainsPagesAndDefersModelSpecificMapReload` | Native closed-reader sample is PET CR with KERNAL status 2, not drive EOI status 64. Three production SPI/FPGA cases preserve an independent unit's next bytes |

[m4-wr-4040-block]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/block
[m4-wr-8250-block]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/block
[m4-wr-4040-vector]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/vector
[m4-wr-8250-vector]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/vector
[m4-wr-4040-dskint]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/dskint
[m4-wr-4040-close]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/close
[m4-wr-8250-romtbl]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/romtbl
[m4-wr-8250-lookup]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/lookup

No page pool or allocation was added. The deferred flag fits existing BAM
padding, leaving unit/channel state at 4600/72 host bytes. Command dispatch
performs no sector I/O. Automatic initialization invalidates metadata for
the existing lazy host reader rather than adding a synchronous metadata
scan inside direct OPEN. Warm reset does not run host lifecycle finalization.
Native head bumps, write-protect transitions and optional boot-clip utility
execution are not emulated by this logical reset.

#### M4 warm-reset evidence

`tools/drive-ref/drive_reference.py --warm-reset-check` captures fourteen observations
per model, including startup, paused reader, page number, dirty claim, UI,
short U, released-reader timeout, pre-AUTOI claim, retained page bytes,
post-AUTOI claim, U9, second retained-page read, direct CLOSE and reopened
file. Native captures are `.cache/ai/m4-warm-reset-<model>-r3/`, with complete
production-C/SPI/FPGA comparisons in matching
`m4-warm-reset-<model>-compare-r3/` directories.

All requested actions, accepted frames and fourteen cases are included,
without prefix flags, omitted suffixes, stopped cases, wire issues or aborted
frames. There are no byte/ATN/EOI/direction or full-image differences.
All native logs are complete below the unchanged six-million-event cap.
Capture harness, reference/report/input and replay executable identities
were checked against retained files. The replay SHA-256 is
`58e51f8d92aee015a6edd35cfc8044204ca687285a5e19a1085abd3445a9b371`.

| Model | Native I/O events | Native completion | Completed/requested actions | Accepted frames | Equal native/candidate final image SHA-256 |
|-------|-------------------|-------------------|-----------------------------|-----------------|-----------------------------------------|
| 4040 | 614490 | 17.617245 simulated seconds | 1139/1139 | 783 | `8811fa8e9160b7ba3a3500408f316c2b48db60051e645c3950a2f6648fb5bab8` |
| 8050 | 693263 | 19.062505 simulated seconds | 1253/1253 | 859 | `fce28fb285e32e55c62ea4d4605992c22a21949037cd4415a2152883b6695b52` |
| 8250 | 851835 | 20.294297 simulated seconds | 1253/1253 | 859 | `9a98ffb11443219a2eb7c21899ffd9bf9e60a78f478bea9cb9cb5afeb4e14b00` |

| Retained comparison report (`comparison.json` in the directory above) | SHA-256 |
|---------------------------------------------------------------------|---------|
| 4040 | `515a4076ea039c972871ecee43a7cb1cbfcef73447bf4c4620982117d5f0accd` |
| 8050 | `6e9a80cfd40844922942cf7e816660deefbb846098dfa198feb2763aa878f439` |
| 8250 | `cff26e8acd7aafe39648d6abe34e7409beaeece2e0d464abe98b23431e219bc5` |

The independent complete-image checker passes on native and candidate media.
For 4040 only track 18/sector 0 changes, decrementing track 1's free count
and clearing sector 0's free bit. Both DOS 2.7 final images equal their
immutable inputs, including every unused byte. Early pilot captures with
an immediate/retained-map assumption are not acceptance evidence. Reproduce
using the M1 invocation with `--warm-reset-check` and complete comparison
replay into fresh ignored output directories.

The direct DOS suite passes 1255 cases (39 added), the complete firmware
regression and production build pass, and all 132 tooling cases pass.
The affected firmware/SPI/FPGA suite passes 194/194, including three new
model cases for released staged sources, repeated warm aliases, stale
selector, retained explicit page, deferred map reload, independent-unit
read progress and actual unmount/remount persistence.

These captures establish the default NMI vector and normal attached-media
path, not modified VNMI/USRJMP/AUTOFG, boot clips, arbitrary buffer execution,
all post-warm file/metadata paths or a complete media/controller-error matrix.
Per-drive reload and unfinished-writer abandonment also have source-derived
host coverage, not a complete dual-drive native capture matrix. Native
physical bump completion, write-protect edges and RP2040/SD latency remain
unaccepted. M4 remains in progress and is not operator-accepted. No new SD
update or destructive board batch is requested for this slice.

#### M4 default user-interrupt commands

`U;`/`UK` and their folded selectors now implement the default user table's
IRQ entry instead of returning the previous unsupported-command status.
This is neither warm reset nor cold reset. The selected ROMs enter
`ieee:ATNIRQ`, discard their command-call stack and resume normal IEEE
dispatch without closing channels, reinitializing buffers or setting
DOS 2.7's warm-start WPSW flags. PARSXQ's initial `00, OK` status survives,
and bypassing ENDCMD also preserves command bytes and last-drive state.
A subsequent short `U` without CR can therefore repeat the selector.

The firmware implements the logical result, not a literal interrupt or
stack reset on the RP2040. FPGA/protocol handling already supplies IEEE
ATN dispatch. No host reset, metadata reload, sector I/O, flush or writer
finalization is added to this command path.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Factory IRQ vector | 4040 revision 2 and 8050/8250 DOS 2.7 | Respective [4040 block][m4-wr-4040-block]/[8250 block][m4-wr-8250-block], `USER/USREXC`, and [4040 vector][m4-wr-4040-vector]/[8250 vector][m4-wr-8250-vector], `UBLOCK` | Selector index `((selector - 1) & 15) == 10` addresses the factory ATNIRQ word at FFFE. U;/UK are not arbitrary buffer execution or NMI/DSKINT | `DOS_USER_INTERRUPT_INDEX`, `command_start`, six-alias direct matrix | Original-ROM UK, U; and folded U[ cases on all three selected models |
| Non-returning command completion | Same selected models | Respective [4040 parsex][m4-ui-4040-parsex]/[8250 parsex][m4-ui-8250-parsex], `PARSXQ/OKERR/ENDCMD/CLRCB`, and [4040 ieee][m4-ui-4040-ieee]/[8250 ieee][m4-ui-8250-ieee], `ATNIRQ/ATN70` | Reset the native stack instead of returning through ENDCMD. Preserve CMDBUF and LSTDRV. Return PARSXQ's 00 status, not 30 or 73 | Non-clearing `command_start` return, full channel/map/state snapshot and short-U direct cases | Native old error 31 becomes OK, then short U without CR also returns OK. Host matrix deliberately separates selected and last drives |
| Prepared data and readiness | Same selected models | Respective `ieee:ATNIRQ/ATN10/ATN50/ATN60/TALK/TLK30` | Release/re-enter bus handling without changing CHNRDY, CHNDAT, LSTCHR or buffer pointers. Do not call GET simply because the utility ran | Existing staged-source reconciliation, `test_user_interrupt_retains_channels_maps_and_command_bytes`, production `UserInterruptRetainsSourcesWriterAndDirtyMap` | Native direct page returns prepared `IRQ!` without another B-P, and paused SEED continues at its next byte with unchanged EOI |
| Dirty maps versus warm reset | Same selected models | Respective `ieee:ATNIRQ` contrasted with warm-start `dskint:DIAGOK`/`begin:CONFIG`; existing `close:MAPOUT` | No metadata initialization or warm-start WPSW assignment. Retain the dirty B-A claim until ordinary direct CLOSE publishes it | Unchanged BAM/free-count snapshots, native claim/CLOSE checker and production remount case | B-A still reports occupied sector 1/0 after all IRQ aliases on every model. Unlike DOS 2.7 warm reset, the claim survives later direct reopen |
| Owners, media and writer lifetime | Same selected models plus host lifetime policy | Respective `ieee:ATNIRQ/ATN50/ATN60` and normal `close:CLSWRT/MAPOUT` | Do not call CLOSE, INITDR, NMI or DSKINT. Preserve readers and unfinished writers until an actual close/reset path | Direct two-drive read/write/direct ownership snapshots and media/I/O counters, production peer-unit and writer-continuation case | Native reader/direct retention. Source-derived host coverage continues the same drive-1 writer after IRQ and verifies its prefix plus suffix after unmount/remount |
| Length and exclusion boundaries | Same selected models | Respective `parsex:CMDSET/CMDRST`, `block:USER/USREXC` and `vector:UBLOCK` | CMDSET's length check precedes execution. CR-trimmed boundary commands may run. Custom vectors, buffer code and out-of-table vector reads remain outside the supported factory path | Overlong/trimmed direct cases and existing explicit execution-policy rejections | No I/O or channel loss on either trimmed success or length error. This host policy is not a claim that arbitrary native user code returns syntax error |

[m4-ui-4040-parsex]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/parsex
[m4-ui-8250-parsex]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/parsex
[m4-ui-4040-ieee]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ieee
[m4-ui-8250-ieee]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ieee

Factory-table classification is now explicit: indices 0/1 are the raw
block-read/write entries, 2..7 execute buffer code, 8 enters NMI, 9 enters
DSKINT and 10 enters ATNIRQ. Indices 11..15 read outside the eleven-entry
factory table. U0 separately restores the factory table pointer. Buffer
execution, custom USRJMP/VNMI and out-of-table execution remain excluded,
with explicit host-policy rejection rather than a claimed native result.
Neither NMI nor IRQ aliases are blanket-excluded as arbitrary utilities.
Unit/channel state remains 4600/72 host bytes, with no added allocation.

#### M4 user-interrupt evidence

`tools/drive-ref/drive_reference.py --user-interrupt-check` captures fourteen
observations per model: startup, paused SEED reader, explicit page number,
prepared page/dirty BAM, old syntax error, UK, short U without CR, unchanged
prepared page bytes, resumed peer bytes, U;, folded U[, retained map claim,
direct CLOSE and reopened page bytes. Native captures are
`.cache/ai/m4-user-interrupt-<model>-r1/`, with complete production-C/SPI/FPGA
comparisons in matching `m4-user-interrupt-<model>-compare-r1/` directories.

All requested actions, accepted frames and fourteen cases are included,
without prefix flags, omitted suffixes, stopped cases, wire issues or
aborted frames. There are no byte/ATN/EOI/direction or full-image
differences. Every native log is complete below the unchanged
six-million-event cap. Harness/artifact/reference/input and replay
executable identities were checked against retained files. The replay
SHA-256 is
`bf58c765a6335cac1029f1b2099082dc390b50d524c44613f0044d8c7f389235`.

| Model | Native I/O events | Native completion | Completed/requested actions | Accepted frames | Equal native/candidate final image SHA-256 |
|-------|-------------------|-------------------|-----------------------------|-----------------|-----------------------------------------|
| 4040 | 328044 | 15.196439 simulated seconds | 1079/1079 | 739 | `8811fa8e9160b7ba3a3500408f316c2b48db60051e645c3950a2f6648fb5bab8` |
| 8050 | 532608 | 17.998419 simulated seconds | 1199/1199 | 819 | `62e99987bbec344771e290e311033b240d618701a2daa16223b3465991d9abea` |
| 8250 | 585468 | 18.398405 simulated seconds | 1199/1199 | 819 | `78718b767cd8c5c7eeb18ffcdf4d66fa8c1edf6ed9c6cc50594fe088be27fb0e` |

| Retained comparison report (`comparison.json` in the directory above) | SHA-256 |
|---------------------------------------------------------------------|---------|
| 4040 | `8ffd76aecc508649208b79881aa88561db7c9e200818d1b0e9f0479e8cfd155f` |
| 8050 | `fff3f990a2a34a599c222aa5ae15c00ef542ab1bb9aa97284eef9bdd066e9ea5` |
| 8250 | `c9137b04954c940161fd03e82be3b581ccfa38f283a1c5b2b84710da02634e94` |

The independent complete-image checker passes on native and candidate
media. Only track 1's free count and sector 0's allocation bit change,
inside track 18/sector 0 on 4040 or track 38/sector 0 on DOS 2.7.
Every other byte, including unused tails and directory/file contents,
equals the immutable input. Reproduce using the M1 invocation with
`--user-interrupt-check` and complete comparison replay into fresh ignored
output directories. Earlier slices retain their historical executable
and harness identities.

The direct DOS suite passes 1276 cases (21 added), the complete firmware
regression and production build pass, and all 137 tooling cases pass.
The affected firmware/SPI/FPGA suite passes 197/197. Its three new model
cases cover both OPEN-15 and ordinary secondary-address collection,
unchanged prepared page bytes, paused readers on the selected and
independent units, retained dirty maps and a drive-1 writer that continues
without reopening. Actual unmount/remount verifies the complete writer
prefix/suffix and the selected drive's persisted BAM count/bit.

Native evidence uses the default factory vectors, slot 0 and normal
attached media. Dual-drive writer continuation, distinct selected/last
drive state and six folded selectors also have source-derived host
coverage, not a complete native multi-drive/fault matrix. Custom vectors,
RAM execution and out-of-table vector execution remain excluded.
Simulation durations do not establish physical IRQ/ATN recovery latency
or RP2040/SD timing parity. M4 remains in progress and is not
operator-accepted. No SD update or destructive hardware batch is requested.

#### M4 ordinary-reader buffer pointers

`B-P` now accepts ordinary readable channels, including modify-opened files
and raw non-LOAD directory channels, rather than returning the previous
unsupported-operation status. This is not a seek within logical file data.
Native BLKPTR selects the active buffer, replaces its byte pointer and calls
RNGET2 to prepare CHNDAT without incrementing that pointer. Subsequent
ordinary GET uses SEQGET/RDBYT, not the circular direct-channel RNDGET.
The selected byte normally appears twice. Link bytes and unused sector
tails remain visible.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Readable channel selection | 4040 revision 2 and 8050/8250 DOS 2.7 | Respective [4040 block][m4-wr-4040-block]/[8250 block][m4-wr-8250-block], `BUFTST/BLKPTR`, `opchnl`/`tst2:FNDRCH` and `getact:GETACT` | Accept ordinary readable SA 2..13, select its active page and reject writers or absent channels with 70 | `block_start`, updated permission matrix, modify-opened/raw-directory direct cases | Source-derived host access and complete-image immutability checks. Ordinary B-R/B-W/raw jobs remain explicitly incomplete |
| Prepared byte versus cursor | Same selected models | Respective `block:BLKPTR`, [4040 tst4][m4-rp-4040-tst4]/[8250 tst4][m4-rp-8250-tst4], `RNGET2/RNGET4/SEQGET` | B-P prepares the selected byte without increment. The next ordinary GET rereads that offset, so it normally appears twice | `read_pointer_work`, pointer-mode branch of `next_byte`, raw-offset direct matrix | Linked/terminal original-ROM scenarios distinguish duplication from an ordinary logical seek |
| Active page at a boundary | Same selected models | Respective [4040 opchnl][m4-rp-4040-opchnl]/[8250 tst2][m4-rp-8250-tst2], `GETBYT/RDBYT/DBLBUF` | A full-page wrap changes the active buffer while the last old-page byte is still prepared. Later B-P selects the new page | `pointer_next_buffer`, lazy-to-native cursor conversion, 252/253/254-byte prefix matrix | Original-ROM B-P at 254 crosses a page, then B-P at 2 selects the successor's payload |
| Sticky EOI and revival | Same selected models | Respective `tst4:RNGET2/GET0/GET1` | RNGET2 changes CHNRDY only on equality with LSTCHR. Preserve pending EOI otherwise. An exhausted reader stays unavailable except when equality prepares one EOI byte | Stored prepared-byte/EOI state, singleton/sticky/revival direct cases | Terminal scenarios distinguish PET timeout CR/status 2 from a drive byte/status 64 and require the final revived byte |
| Header and tail visibility | Same selected models | Respective `tst4:RNGET2/SEQGET` and `opchnl`/`tst2:GETBYT/RDBYT` | Non-final LSTCHR is zero, so offset 0 sends the link-track byte with EOI. On a final page, offset 255 wraps and prepares EOI even beyond file data | Raw offset/tail cases and ordinary EOF retirement | Expected native link/tail bytes are derived independently from immutable raw fixture entries, never candidate output |
| Preview, I/O and ownership | Same selected models plus host lifetime policy | Native routines above, with cooperative host storage and staged FPGA copies | Do not advance the real cursor while previewing or issue normalization I/O during command dispatch. Preserve the old cursor if that normalization fails | Deferred `DOS_COMMAND_READ_POINTER`, snapshot/repeated-preview tests, failed-normalization case and production peer-unit test | Source-derived host failure-retention policy is not a claim of native controller retry/error parity |

[m4-rp-4040-tst4]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4
[m4-rp-8250-tst4]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst4
[m4-rp-4040-opchnl]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/opchnl
[m4-rp-8250-tst2]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2

The ordinary linear path is unchanged until the first B-P. A dedicated
pointer-mode flag occupies existing channel padding, and prepared byte/EOI
fields share storage already unused by ordinary readers. No pool or heap
allocation is added. Unit/channel state remains 4600/72 host bytes.
Formatted LOAD directory listings remain outside BUFTST's SA range.
This slice does not implement ordinary-reader block read/write jobs,
direct LOAD/SAVE or controller fault/timing parity.

#### M4 ordinary-reader B-P evidence

`tools/drive-ref/drive_reference.py --reader-pointer-check linked` and
`--reader-pointer-check terminal` each capture fourteen observations on
every selected model. Linked cases cover duplicate selected bytes,
successor-page selection, terminal EOI, exhausted-source timeout, equality
revival, raw link bytes and an unaffected peer. Terminal cases cover the
one-byte OPEN quirk, header bytes, pending-EOI preservation, revival at
offset 255, unused-tail wrap and a final revived payload byte.

Complete native captures are
`.cache/ai/m4-reader-pointer-<variant>-<model>-r2/`. Matching complete
production-C/SPI/FPGA comparisons are in
`m4-reader-pointer-<variant>-<model>-compare-r2/`. All six include every
requested action, accepted frame and fourteen cases, without prefix
flags, omitted suffixes, stopped cases, wire issues or aborted frames.
There are no byte/ATN/EOI/direction or full-image differences. Native logs
are complete below the unchanged six-million-event cap.

Retained harness/artifact/reference/input/active-ROM and executable
identities were audited against their files. The replay executable SHA-256
is `6050690ebdba1f56120d9037d9f1b3e769c3bb9852bc999b1c640d38840501e0`.
Earlier slices retain their historical executable/harness identities.

| Variant | Model | Native I/O events | Native completion | Completed/requested actions | Accepted frames |
|---------|-------|-------------------|-------------------|-----------------------------|-----------------|
| Linked | 4040 | 238393 | 10.540000 simulated seconds | 511/511 | 369 |
| Linked | 8050 | 388121 | 11.960000 simulated seconds | 547/547 | 393 |
| Linked | 8250 | 440982 | 12.360000 simulated seconds | 547/547 | 393 |
| Terminal | 4040 | 292113 | 11.240000 simulated seconds | 586/586 | 424 |
| Terminal | 8050 | 441748 | 12.760000 simulated seconds | 634/634 | 456 |
| Terminal | 8250 | 494624 | 13.160000 simulated seconds | 634/634 | 456 |

| Model | Equal immutable input/native/candidate image SHA-256 (both variants) |
|-------|---------------------------------------------------------------------|
| 4040 | `612bd40b23d94c5ccafe814b17e4e880807252100c7a45581996a17ed187738b` |
| 8050 | `fce28fb285e32e55c62ea4d4605992c22a21949037cd4415a2152883b6695b52` |
| 8250 | `9a98ffb11443219a2eb7c21899ffd9bf9e60a78f478bea9cb9cb5afeb4e14b00` |

| Retained comparison report (`comparison.json` in the directory above) | SHA-256 |
|---------------------------------------------------------------------|---------|
| Linked 4040 | `50e923a4c96c5f4513d1b8f9f1543af0e75894c1eca4678342d3a2a23b73aafb` |
| Linked 8050 | `ab9b4718e27119f3d8846fa5584fbed6e1be57ddc616be91ef8127706b38552d` |
| Linked 8250 | `447d00ba232790f99bd6c9db394cb2f2336e40cf093e671db16ab948d0c8c756` |
| Terminal 4040 | `5397f468881290bc410235fc232e002eadf4fe9ddaab74d088700419faa2b887` |
| Terminal 8050 | `e508a2e196f0f4f602cc20e44b71d48493dd4851c1376839138a3ea1823d8a3a` |
| Terminal 8250 | `1c6a97d99f5bd2c7928884596f366a98a1afaed48e7918e8582d53045958de4e` |

Every native and candidate image equals the immutable input byte for byte,
including metadata, payloads and unused tails. Expected link-track and
tail bytes come from the input's raw directory/file pages independently
of candidate output. Reproduce using the M1 invocation with either
reader-pointer variant, followed by complete comparison replay into fresh
ignored output directories.

The direct DOS suite passes 1354 cases (78 added). The complete firmware
regression, production build and all 142 tooling cases pass. The affected
firmware/SPI/FPGA suite passes 200/200. Its three new model cases cover both
variants, OPEN-15 and ordinary secondary-address collection, staged source
replacement, local/independent-unit peers, exhausted readers and passive
wire/EOI observations. Direct host cases additionally cover raw directory
and modify-opened files, cursor snapshots/repeated preview, failed
normalization and read-only CLOSE.

Native evidence uses slot 0, closed PRG/SEQ fixtures and normal attached
media. Modify-opened/raw-directory permissions, extra offset/boundary
combinations and failure retention have source-derived host coverage,
not a complete native multi-drive/resource/controller-fault matrix.
Simulation durations do not establish physical B-P latency, RP2040/SD
timing or controller-prefetch/error parity. M4 remains in progress and
is not operator-accepted. No SD update or destructive hardware batch is
requested for this slice.

#### M4 colonless whole-drive file copy

Colonless `C0=1` copies files from drive 1 to drive 0. It is not a
colonless spelling of named COPY and is not a block-for-block disk duplicate.
The pinned `copysetup:DSKCPY` enters `copyall:MOVLP2`, walks source directory
entries in order and invokes the ordinary COPY worker for each name.
Colon-bearing wildcard `CPYDTD` was deferred from this slice.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Entry selection and drive syntax | 4040 revision 2 and 8050/8250 DOS 2.7 | Respective [4040 copysetup][m4-ca-4040-setup]/[8250 copysetup][m4-ca-8250-setup], `DSKCPY/PRSEQ`, `setdrv:TST0V1` | Without a colon, only the distinct ASCII 0/1 bytes immediately beside the first `=` select destination/source. Other prefix/suffix text is not a filename | `copy_start`, syntax/alias matrix and reverse-drive case | Native cases distinguish `C0=1` from same-drive, invalid-drive and filename-like syntax |
| Bounded native parsing | Same selected models | Respective `copysetup:PRSEQ`, `parsex:PRSCLN/PARSE` and factory `vector:UBLOCK` | Restart the equals search while retaining the colon search's comma count. A source selector beyond effective CMDSIZ can be retained after an uncleared U0 | Shared `command_delimiter`, comma-limit cases and U0-retained selector case | Source-derived host coverage, not a complete native malformed/stale-input matrix |
| Source directory continuation | Same selected models | Respective [4040 copyall][m4-ca-4040-all]/[8250 copyall][m4-ca-8250-all], `MOVLP2/PUPS1/FFST/EXLP0/FFRE`, `lookup:SEARCH` | Save source directory position across each destination lookup/copy and continue in native order, ignoring zero-type entries | Cooperative `copy_all_next`, saved track/sector/entry and bounded traversal | Direct cases cross source and growing target directory pages. Native fixtures contain five source files |
| Per-file lookup and creation | Same selected models | Respective `copyall:FIXIT/TRFNME/EXLP2`, [4040 rename][m4-ca-4040-rename]/[8250 rename][m4-ca-8250-rename], `CY/CHKIN/CHKIO/COPY` | Transfer all 16 filename bytes, fake lookup for the target only, preserve payload/type, recalculate allocation/count and do not inherit source lock flags | Existing named-copy worker with a saved source descriptor, locked/count/type direct fixtures | Native copied PRG/SEQ payloads include singleton and 254/255-byte boundaries. Additional USR/metadata cases have host coverage |
| Completion and failure retention | Same selected models | Respective `copyall:EXLP0/ENDIT`, `rename:CY`, `close:CLSWRT` and `erproc:ENDCMD/CMDERR` | Close each completed destination, stop on the first error with its persisted prefix retained and call ENDCMD only after source iteration ends | Return to `DOS_COPY_ALL_NEXT` after each close, collision/recovery and empty-source cases | Native repeat/alias cases require exact 63 status. Completed-prefix collision retention is source-derived host coverage |
| Ownership and media | Same selected models plus host lifetime policy | Native routines above, cooperative storage and existing media identities | Do not perform storage I/O during command dispatch, retain paused source/other-unit channels and leave the source image unchanged | Existing shared reader/writer pools, read-only-source cases and production peer/remount cases | Complete dual-image replay verifies all source bytes. Generic injected read error 23 and absent-media policy are not native controller retry/error acceptance |

[m4-ca-4040-setup]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/copysetup
[m4-ca-8250-setup]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/copysetup
[m4-ca-4040-all]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/copyall
[m4-ca-8250-all]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/copyall
[m4-ca-4040-rename]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rename
[m4-ca-8250-rename]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rename

The iterator adds four host bytes to the existing COPY work union.
Unit/channel sizes are 4604/72 host bytes, within the existing 4608-byte
unit ceiling. No heap, new buffer pool or whole-image/file-list cache is
added. The named-copy path and its default-drive fallback are retained.
Successful all-files completion leaves the native current drive on the
source. Syntax errors retain the preceding selected drive, while a
destination collision selects the destination for its status drive field.

#### M4 colonless whole-drive copy evidence

`tools/drive-ref/drive_reference.py --copy-all-check` captures fourteen binary/status
observations on each selected model. Cases cover startup, a paused source
reader, three invalid command forms, successful all-files copy, copied
PRG/SEQ data including singleton and 254/255-byte boundaries, the retained
source cursor, repeated-copy/ignored-suffix 63 statuses and later named-copy
recovery. The source contains five files. Independent `c1541` extraction
checks all five destination payloads and the sixth recovery-created file.

Complete native captures are
`.cache/ai/m4-colonless-copy-<model>-r3/`. Matching complete
production-C/SPI/FPGA comparisons are in
`m4-colonless-copy-<model>-compare-r3/`. All three compare every requested
action, accepted frame and fourteen cases, without prefix flags, omitted
suffixes, stopped cases, wire issues or aborted frames. Both persisted
images match byte for byte, including metadata and unused tails. The
entire source image also equals its immutable input. Native logs are
complete below the unchanged six-million-event cap.

Retained harness/artifact/reference/input/active-ROM and executable
identities were audited against their files. The replay executable SHA-256
is `f02d8f1251a5633e297d5619fade19d0a4d042c390a7e165ccf5ecd7d56f185d`.
Audit output is `.cache/ai/m4-colonless-copy-audit.log`. Earlier slices
retain their historical executable/harness identities. Only the complete
r3 captures count here, not earlier incorrect-checker or interrupted runs.

| Model | Native I/O events | Native completion | Completed/requested actions | Accepted frames |
|-------|-------------------|-------------------|-----------------------------|-----------------|
| 4040 | 3317408 | 64.040000 simulated seconds | 4311/4311 | 2907 |
| 8050 | 3687784 | 67.680000 simulated seconds | 4419/4419 | 2979 |
| 8250 | 3767114 | 68.280000 simulated seconds | 4419/4419 | 2979 |

| Model | Immutable destination input SHA-256 | Equal native/candidate destination SHA-256 |
|-------|------------------------------------|-------------------------------------------|
| 4040 | `be3ca8bded4162917479a2bbc8702c4178e35450f196d0555ddd49ca5c019413` | `03abe431a3666ea1ba2cce0d044048ff98e4bcfcef08d4630610040af358ae36` |
| 8050 | `07c5ed19de0a0f351ddcd0ea2eb9f073a71fe87e2130e43c6da7e6b2b288e70f` | `8ae32c01103cde7bcdfddd379a2ee139fabcfc224baad4005fd3e76f91f48ac2` |
| 8250 | `a9d14fcc8409a1e2574eae419bd57cad368c132ce6bd5041989b4c935a591a17` | `37a47ea71b9b459f0c1ce30e860a34c949fd5238664f8afa576dc32d67d3457c` |

| Model | Equal immutable input/native/candidate source image SHA-256 |
|-------|------------------------------------------------------------|
| 4040 | `612bd40b23d94c5ccafe814b17e4e880807252100c7a45581996a17ed187738b` |
| 8050 | `fce28fb285e32e55c62ea4d4605992c22a21949037cd4415a2152883b6695b52` |
| 8250 | `9a98ffb11443219a2eb7c21899ffd9bf9e60a78f478bea9cb9cb5afeb4e14b00` |

| Retained comparison report (`comparison.json` in the directory above) | SHA-256 |
|---------------------------------------------------------------------|---------|
| 4040 | `98e4463c4b9260e18e89ac68db1b9ca2d6deba94306c4d0142e9a1bb9b70da76` |
| 8050 | `ea84ac62097361395fd00c543d147bf62bfecd87f13f32242be1ea3af790b72d` |
| 8250 | `185cac8f6a7cb2c7ae23b6bf584182169619391daaa3b02169ee15a2ccc672f4` |

Reproduce using the M1 invocation with `--copy-all-check`, followed by
complete comparison replay into fresh ignored output directories. Optional
slot-1 input/final identities are retained in `media.secondary` and
automatically verified/mounted by the comparison tool. The replay CLI
accepts an optional source image and exports both candidate images.
Historical single-image captures keep their original invocation and report
shape. A complete linked-reader B-P 4040 replay using the new executable
also passes with all 511 actions, 369 frames and unchanged image
(`.cache/ai/m4-colonless-copy-single-image-regression/`).

The direct DOS suite passes 1414 cases (60 added). The complete firmware
regression, production build and all 151 tooling cases pass. The affected
firmware/SPI/FPGA suite passes 203/203. Its three new model cases use
independently encoded SEQ/PRG/USR fixtures and exercise OPEN-15/ordinary
secondary command collection, source/other-unit peers, repeated-copy
status and actual unmount/remount payload/EOI persistence.

Native evidence uses drive 1 as source, closed PRG/SEQ fixtures, one source
directory page and normally attached writable media. Reverse direction,
U0-retained selectors, extra syntax/metadata/USR cases, directory growth,
read-only sources, empty sources, injected failures and completed-prefix
collision retention have source-derived host coverage, not a complete
native resource/controller-fault matrix. REL remains M5 work.
Simulation durations do not establish physical COPY latency, RP2040/SD
timing or controller retry/error parity. M4 remains in progress and is not
operator-accepted. No SD update or destructive hardware batch is requested.

#### M4 colon-bearing wildcard file copy

The `copysetup:DSKCPY`/`copyall:CPYDTD` path now accepts commands such as
`C0:*=1:BOUND25?`. A destination beginning with `*` selects per-file copy
using one source pattern, rather than concatenation into a wildcard name.
The source can be exact, use `?` for one non-padding byte, or use `*` to
accept the remaining suffix. Destination text following its initial `*`
is ignored. Each matched source supplies its own complete destination
filename. No match is successful, not 62 FILE NOT FOUND.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| CPYDTD dispatch | 4040 revision 2 and 8050/8250 DOS 2.7 | Respective [4040 copysetup][m4-ca-4040-setup]/[8250 copysetup][m4-ca-8250-setup], `DSKCPY/DX0005`, `parsex:TC30/TC75`, `romtbl:STRUCT` | Require one source and an initial target `*`. Reject patterned non-star targets and source groups with 30. Ignore the target's remaining text | `copy_start`, star-tail, non-star and group cases | Native captures include exact-source copying through `*IGNORED` and both invalid forms |
| Pattern matching and no matches | Same selected models | Respective [4040 copyall][m4-ca-4040-all]/[8250 copyall][m4-ca-8250-all], `CPYDTD/MOVLP2/PUPS1`, `lookup:FFST/FFRE/COMPAR/CP30/CP40` | Retain the source pattern across every per-file target lookup. `?` cannot match filename padding, and `*` ignores its remaining text. Return 00 when enumeration finds no match | Saved pattern in the unused third COPY descriptor, existing `name_matches` and `copy_all_next` | Native exact, question-mark, star-tail and no-match cases. Direct cases distinguish absent target files from accidental all-files copy |
| Explicit same-drive rejection | 8050/8250 DOS 2.7 | `copysetup:DX0005/DX0010`, `setdrv:ALLDRS/SETDRV` | Compare complete source/target drive bytes before CPYDTD and return 30 when equal | Model-gated comparison in `copy_start`, direct flag matrix and production model cases | Native `C1:*=1:SEED` returns 30, unlike the selected 4040 |
| 4040 and default-drive distinctions | 4040 revision 2, plus DOS 2.7 default flags | 4040 `copysetup:DX0005`, respective `copyall:FIXIT/EXLP2`, `rename:CY/CHKIO`, `setdrv:SD50` | 4040 has no explicit equality test and reaches 63 on an existing same-drive match. DOS 2.7 compares the default flag too, so omitted source prefixes can bypass that equality test and reach the nonterminating lookup below | Full drive-byte comparison before stripping the source default flag | Native 4040 same-drive case and direct per-model omitted/invalid-prefix cases. Default-flag variations remain source-derived host coverage |
| Existing patterned target | 4040 revision 2 versus 8050/8250 DOS 2.7 | Respective `lookup:CMPCHK/CC15`, `LOOKUP/LK25/LK26`, `COMPAR/CP40`, `copyall:FIXIT/TRFNME` | 4040 tests FILTRK and returns 63. DOS 2.7 tests PATTYP, whose pattern flag survives target substitution, so a matching target repeats LK25/COMPAR indefinitely rather than completing with 63 | `DOS_COPY_MATCH_WAIT` retains pending work and completed files without further I/O or spinning the MCU. Colonless and named COPY do not inherit this target-pattern flag | Failed native 8050/8250 collision captures corroborate the loop. Direct tests retain busy ownership and completed prefixes, then cancel through host reset. Complete stalled-wire/IRQ/ATN recovery parity is not accepted |
| Continuation, errors and ownership | Same selected models plus host lifetime policy | Respective `copyall:FIXIT/EXLP0/FFRE/ENDIT`, `rename:CY` and `close:CLSWRT` | Preserve source iteration and completed files, retain paused peers for completing commands and leave source media unchanged. Collision completion is model-specific as above | Existing cooperative all-files worker, patterned multi-page/prefix-collision cases and production remount test | Complete dual-image replay covers completing commands, exact statuses/EOI and paused-source observations. Broader failure/resource cases remain unaccepted |
| Unsafe relocation boundary | Host safeguard, not native error parity | Respective `copyall:CPYDTD/MOVLP1/TRFNME` and `trnsfr:FNDLMT` | CPYDTD moves source text to end at command byte 40, while TRFNME writes bytes 3..18. Empty relocation can wrap, and source text longer than 21 bytes overlaps the rewritten target name | Explicit 30 rejection for these unaccepted cases, with accepted-21/rejected-22-byte direct boundaries | Source-derived host corruption/overlap policy. Native malformed relocation and its RAM effects are not accepted or silently normalized |

The pattern shares existing descriptor storage. No unit/channel fields,
heap allocation or buffer pools are added. Unit/channel sizes remain
4604/72 host bytes. Named COPY and colonless `C0=1` retain their existing
paths, with `*` supplying the colonless enumeration pattern.

The first native collision scenarios for 8050 and 8250 reached the
six-million-event recording limit and did not finish in 180 simulated
seconds. Their final main-CPU PCs (`$CB6D` and `$CB85`) are inside COMPAR.
These incomplete captures are diagnostic evidence, not accepted replay
prefixes. The revised complete scenario selects a new `SEED` destination
with `S*IGNORED` instead of repeating an existing destination. The 4040
collision scenario completed with 63. No event limit was increased to
conceal the DOS 2.7 nontermination.

The host busy state intentionally does not emulate the ROM's instruction
loop. It leaves the command pending with no additional media activity.
Direct reset cancellation does not establish native stalled-wire recovery,
IRQ behavior, ATN recovery, or independent bus-address availability.
In particular, the firmware facade's global RX ordering fence remains
active while a DOS job is pending.

Corroboration uses original-ROM execution on the pinned MAME hardware
models and independent `c1541` payload extraction. The
[pinned MAME 8050/8250 model](https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c8050.cpp)
independently identifies the selected shared DOS 2.7 main/controller pair
and distinct hardware models. The
[Commodore DOS protocol description](https://www.pagetable.com/174)
corroborates channel/command layering and warns that device feature sets
differ. Neither overrides the ROM-specific same-drive behavior above.

#### M4 wildcard file-copy evidence

`tools/drive-ref/drive_reference.py --copy-all-check pattern` captures fourteen
binary/status observations per selected model. Cases cover startup, a
paused source reader, invalid patterned-target/source-group commands,
model-specific explicit same-drive rejection, successful no-match
completion, exact/question-mark/star-tail source selection, singleton and
254/255-byte streams with exact EOI, the retained source cursor and later
named-copy recovery. Independent `c1541` extraction checks all five
destination files. The source's unselected `M1CHECK` is not copied.

Complete native captures are
`.cache/ai/m4-pattern-copy-<model>-r2/`. Matching complete
production-C/SPI/FPGA comparisons are in
`m4-pattern-copy-<model>-compare-r3/`. All three compare every requested
action, accepted frame and fourteen cases, without prefix flags,
unexplained differences, omitted suffixes, stopped cases, wire issues or
aborted frames. Both persisted images match byte for byte, including
metadata and unused tails. The entire source also equals its immutable
input. These completing scenarios are distinct from the diagnostic
DOS 2.7 collision captures above.

Retained harness/artifact/reference/input/active-ROM and executable
identities were audited against their files. The replay executable SHA-256
is `ac432a8bd63973fccc973ac7fe0b650b61edf2641ba4d7cef13a02520bf2155e`.
Audit output is `.cache/ai/m4-pattern-copy-audit.log`. Earlier slices
retain their historical executable/harness identities.

| Model | Native I/O events | Native completion | Completed/requested actions | Accepted frames |
|-------|-------------------|-------------------|-----------------------------|-----------------|
| 4040 | 2471833 | 57.020000 simulated seconds | 4281/4281 | 2905 |
| 8050 | 3043281 | 62.280000 simulated seconds | 4401/4401 | 2985 |
| 8250 | 3148965 | 63.080000 simulated seconds | 4401/4401 | 2985 |

| Model | Immutable destination input SHA-256 | Equal native/candidate destination SHA-256 |
|-------|------------------------------------|-------------------------------------------|
| 4040 | `be3ca8bded4162917479a2bbc8702c4178e35450f196d0555ddd49ca5c019413` | `cbed926b932d21a8b360c213efc3585448a25de01bcb019ebdb01485999a905e` |
| 8050 | `07c5ed19de0a0f351ddcd0ea2eb9f073a71fe87e2130e43c6da7e6b2b288e70f` | `384f949dc17293dccdf44c95dc71536ee65e1d28fdae1fd15a827de743bcd20c` |
| 8250 | `a9d14fcc8409a1e2574eae419bd57cad368c132ce6bd5041989b4c935a591a17` | `5d3c666b13a621b127bb78893349af503bc26971b448214898f31c354622c008` |

| Model | Equal immutable input/native/candidate source image SHA-256 |
|-------|------------------------------------------------------------|
| 4040 | `612bd40b23d94c5ccafe814b17e4e880807252100c7a45581996a17ed187738b` |
| 8050 | `fce28fb285e32e55c62ea4d4605992c22a21949037cd4415a2152883b6695b52` |
| 8250 | `9a98ffb11443219a2eb7c21899ffd9bf9e60a78f478bea9cb9cb5afeb4e14b00` |

| Retained comparison report (`comparison.json` in the directory above) | SHA-256 |
|---------------------------------------------------------------------|---------|
| 4040 | `8f7d2d3a8799b86ae3b0b533a72e1deeb824496c3aa834b02285c2852e54a377` |
| 8050 | `57fedf8ab5e1148865715fae121c80e539e8acaf620dcf91becba4f6191f3d82` |
| 8250 | `8839be6e72a2b7f431430009338ae6d0e1622de63285899cab6247d383d50254` |

Reproduce using the M1 invocation with `--copy-all-check pattern`,
followed by complete comparison replay into fresh ignored output
directories. The historical argument without `pattern` still selects
colonless whole-drive copy. Slot-1 input/final identities remain in
`media.secondary`, and both images are automatically verified and mounted.

The direct DOS suite passes 1498 cases (84 added). The complete firmware
regression, production build and all 156 tooling cases pass (five tooling
cases added). The affected firmware/SPI/FPGA suite passes 206/206. Its
three new model cases use independently encoded USR fixtures, select only
the requested file despite an existing unselected target, alternate
OPEN-15/ordinary-secondary collection, retain source/other-unit peers and
verify exact payload/EOI after actual unmount/remount.

Native complete evidence uses drive 1 as source, closed PRG/SEQ fixtures
and normally attached writable media. USR/metadata cases, default-drive
flags, multi-page enumeration, completed-prefix busy retention, direct
host reset cancellation and unsafe-relocation safeguards have
source-derived host coverage, not complete native malformed-input,
controller-fault, IRQ or reset-recovery acceptance. Failed native collision
captures are not complete wire comparisons. Simulation duration does not
establish physical latency or RP2040/SD timing. M4 remains in progress and
is not operator-accepted. No SD update or destructive hardware batch is
requested.

#### M4 ordinary-reader block reads

`B-R`, `U1` and its factory `UA` alias now accept ordinary readable
channels, including modify-opened files and raw non-LOAD directories.
They replace the selected channel's native active page without changing
its file type. Subsequent GET therefore remains sequential, not circular
direct-buffer access. The prepared byte normally appears twice, and
sending an EOI byte makes the channel not ready. Another block read
re-arms it. B-P can revive an exhausted reader only through its existing
LSTCHR-equality rule.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Ordinary readable channel selection | 4040 revision 2 and 8050/8250 DOS 2.7 | Respective [4040 block][m4-wr-4040-block]/[8250 block][m4-wr-8250-block], `BUFTST/BKOTST/BLKTST`, `tst2:FNDRCH`, `getact:GETACT` | Use the readable channel's active page, without requiring the direct file type. Keep writer/SA-range and geometry error precedence | `block_start`, updated shared permission cases, ordinary/modify/raw-directory cases | Original-ROM PRG reader scenarios. Modify and raw-directory permissions have source-derived host coverage |
| Native active-page selection | Same selected models | Respective `tst2:GETBYT/RDBYT/DBLBUF`, `block:BLKRD2`, `getact:GETACT` | Preparing old-page byte 255 has already switched the active buffer. A later block command overwrites that successor, not the lazy old-page cursor | Shared `materialize_reader`, direct 252..255-byte prefix matrix with and without preceding B-P | Every native scenario consumes 253 SEED bytes before B-R, crossing the native active-page boundary |
| B-R count and prepared byte | Same selected models | Respective `block:BLKRD3/BLKRD`, `tst4:RNGET1/RNGET2/SEQGET`, `tst2:GETBYT/RDBYT` | Set LSTCHR from page byte 0, clear old EOI and prepare byte 1 without a sequential GET increment. A count of 4 emits bytes 1,1,2,3,4, with EOI only on the last | Existing block worker plus saved ordinary `read_last`, direct count matrix and production byte/EOI cases | Complete original-ROM count-4 captures distinguish this from four-byte direct-channel B-R |
| U1/UA raw read and EOF | Same selected models | Respective `block:UBLKRD/BLKRD3`, `tst4:SEQGET/GET0/GET1`, `vector:UBLOCK` | Prepare byte 0, set LSTCHR to 255 and keep sequential GET. Emit 257 bytes (byte 0 twice, then bytes 1..255), with EOI only on byte 255. Retire readiness, unlike circular direct GET | Unchanged file access/type, shared prepared-byte/EOI fields and raw boundary cases | Native captures require the complete raw page, exhausted-source PET timeout and alias prefix |
| LSTCHR versus sector links | Same selected models | Respective `block:BLKRD3`, `tst2:GETBYT/RDBYT/RD1/DBLBUF/SETDRN` | Keep the command's LSTCHR independently of raw page link bytes. A zero count can later switch to the old prefetched alternate rather than follow the newly read page's link | Reused channel union and next-link fields, per-buffer job-drive retention, zero-count cross-drive direct case | Source-derived host coverage for zero-count wrap and cross-drive prefetched continuation, not a complete native multi-drive matrix |
| Deferred I/O and ownership | Same selected models plus host lifetime policy | Native routines above, cooperative media identities and staged source ownership | Do not perform sector I/O during command dispatch, advance real cursors during preview, or turn the borrowed reader into a writer. Preserve independent peers and unchanged persisted images | Existing scheduled block job, repeated-preview assertions, failed-normalization rollback and production remount test | Complete production-C/SPI/FPGA replay and complete-image comparison. Host fault retention is not controller retry/error acceptance |

No page pool, heap allocation, per-channel field or whole-file cache is
added. LSTCHR shares the existing singleton/direct-last union after
pointer-mode conversion, and the original prefetched link uses fields
otherwise needed by writers. Unit/channel sizes remain 4604/72 host bytes.
Direct-channel B-R/U1 retain their existing count/circular behavior.
Ordinary-reader B-W/U2, formatted LOAD listing interactions and direct
LOAD/SAVE remain separate work.

#### M4 ordinary-reader block-read evidence

`tools/drive-ref/drive_reference.py --reader-pointer-check block` captures fourteen
binary/status observations per selected model. Cases cover startup, a
paused independent reader, 253 consumed SEED bytes before active-page
replacement, counted B-R output, exact status, exhausted-source timeout,
B-P equality revival, the complete 257-byte sequential U1 stream,
another timeout, UA's raw prefix, retained peer progress and successful
ordinary CLOSE/reopen. The fixture independently seeds one free raw sector
without changing BAM ownership or any existing file.

Complete native captures are
`.cache/ai/m4-reader-block-<model>-r2/`. Matching complete
production-C/SPI/FPGA comparisons are in
`m4-reader-block-<model>-compare-r1/`. All three compare every requested
action, accepted frame and fourteen cases, without prefix flags, omitted
suffixes, stopped cases, wire issues or aborted frames. There are no
byte/ATN/EOI/direction or full-image differences. PET timeout CR/status 2
is distinguished from drive data/EOI. Native logs are complete below the
unchanged six-million-event cap.

Retained harness/artifact/reference/input/active-ROM and executable
identities were audited against their files. The replay executable SHA-256
is `07b04acd66b09d3eeaf882c82be35c2ae61beb8e107ef80953dae1779680b120`.
Audit output is `.cache/ai/m4-reader-block-audit.log`. Earlier slices
retain their historical executable/harness identities.

| Model | Native I/O events | Native completion | Completed/requested actions | Accepted frames |
|-------|-------------------|-------------------|-----------------------------|-----------------|
| 4040 | 577853 | 38.100000 simulated seconds | 3580/3580 | 2408 |
| 8050 | 836047 | 40.440000 simulated seconds | 3628/3628 | 2440 |
| 8250 | 888905 | 40.840000 simulated seconds | 3628/3628 | 2440 |

| Model | Equal immutable input/native/candidate image SHA-256 |
|-------|------------------------------------------------------|
| 4040 | `d067119724e84bdf45bfc56d09a6e8b94a2b89bd48bc5174695b6e7339b26264` |
| 8050 | `af27ed5b4394295d2a0e41a97d82e632c9e843be2c20550a3b0c4e8a985a26a8` |
| 8250 | `eddf09f8243310bf08d9270f1b84ec3e1223e9cb7535f933d459bc1150b88f03` |

| Retained comparison report (`comparison.json` in the directory above) | SHA-256 |
|---------------------------------------------------------------------|---------|
| 4040 | `489df6de9f5656aa95abbb0d51358d2d60097aa24d01b2b18a2128e6ebdc36fb` |
| 8050 | `eca51527d1f7b25db12fbe2e5cef92aa4cda415878086bba4ab8b96156e04b52` |
| 8250 | `1cff3d7cb40d55ed6cb19d966be61ef32e5d03cf05c487dc57200e0aed757c7b` |

Reproduce using the M1 invocation with `--reader-pointer-check block`,
followed by complete comparison replay into fresh ignored output
directories. Every native and candidate image equals the immutable input
byte for byte, including metadata, payloads and unused tails. The
historical linked/terminal B-P scenarios remain separate variants.

The direct DOS suite passes 1582 cases (84 added). The complete firmware
regression, production build and all 158 tooling cases pass (two new
tooling cases plus expanded variant coverage). The affected
firmware/SPI/FPGA suite passes 209/209. Its three new model cases use
independently encoded raw bytes on drive 1, select an ordinary drive-0
reader at the active-page boundary, alternate OPEN-15/ordinary-secondary
collection, retain local/independent-unit peers and verify counted/raw
bytes, exact EOI, no-data exhaustion and actual remount/reopen behavior.

Complete native evidence uses slot 0, closed PRG/SEQ files, a count of 4,
U1/UA and normally attached media. Additional count boundaries (including
0/1/254/255), modify/raw-directory access, prefetched zero-count
cross-drive continuation and failed-normalization rollback have
source-derived host coverage, not a complete native multi-drive,
malformed-input, controller-fault or prefetch-timing matrix. Uninitialized
alternate-page RAM and formatted LOAD listings are not newly accepted.
Simulation durations do not establish physical B-R/U1 latency or
RP2040/SD timing parity. M4 remains in progress and is not
operator-accepted. No SD update or destructive hardware batch is requested.

#### M4 NEW, DUPLICATE, ordinary-reader writes and special channels

The remaining non-REL raw-image command paths are implemented. This does
not close M4 acceptance: healthy full-FORMAT/DUPLICATE native comparisons
and destructive physical acceptance remain open. No SD files have been
copied and no physical result is inferred from host coverage.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Quick NEW | 4040 revision 2, 8050/8250 DOS 2.7 | [4040 new][m4-n-4040-new], [8250 new][m4-n-8250-new], `NEW/ONEDRV/INITDR/NEWMAP`, and respective `trnsfr:TRNAME/FNDLMT` | No ID rebuilds the native BAM/header/directory while preserving disk ID and unrelated raw data. A foreign cached BAM version returns 73 before destructive metadata writes. Empty directory has final EOI and 664/2052/4133 free blocks | `format_start/format_work/format_map`, complete-image tests and `QuickAndFullFormatPersistNativeHeadersAndFreeCounts` | Complete original-ROM quick captures on all three models, independent complete-image construction and production SPI/FPGA replay |
| Full NEW | Same selected models | Same `new` routines, `FORMAT`, and pinned formatter payload identities in [the ROM manifest](../../tools/drive-roms/drive-emulation-roms.json) | An ID formats all sector data, then rebuilds native metadata with the new ID. Trailing comma without an ID remains quick mode. One-byte ID reads the retained next command byte | Complete-image, inherited-drive/ID-boundary, storage-failure and stale-generation tests | Selected formatter disassemblies establish zero-filled raw sector data. Healthy full native execution remains blocked below |
| NEW channel scope | 4040 versus DOS 2.7 | Respective `new:NEW/FORMAT`, `tst2:INITDR`, `tst3:CLDCHN/CLRCHN` | Quick INITDR abandons selected-drive channels without CLOSE. Full 4040 NEW retains channels. DOS 2.7 full NEW frees SA14..1 across both disks but retains SA0 | `test_format_channel_lifecycle`, fixed-scratch ownership checks | ROM-derived lifecycle cases, not a complete native resource-pressure/fault comparison |
| Sector DUPLICATE | Same selected models, equal-format pair | [4040 duplct][m4-n-4040-duplct], [8250 duplct][m4-n-8250-duplct], `DUPLCT/READS/WRITES/SETRH`, and `copysetup:PRSEQ` | Adjacent distinct 0/1 selectors surround `=`. Initialize/version-check source, abandon target channels, FORMAT target, then copy every source sector (including free data). Tracks ascend, eleven-page groups descend, sectors within each group ascend. Final INITDR reloads target metadata | `format_copy_group/format_work`, both-direction whole-image/source-isolation tests and `DuplicateCopiesRawSectorsWithoutResettingOtherUnits` | Source-derived complete-image and production remount/independent-unit coverage. Healthy full native comparison is not yet accepted |
| Ordinary-reader B-W/U2/UB | Same selected models | [4040 block][m4-d-4040-block]/[8250 block][m4-d-8250-block]: `BUFTST/BT15/BT20/BLKWT/BW10/BW20/UBLKWT`, respective `opchnl:INTPNT/OR30`, `tst2:GETPRE/GETBYT/RDBYT/DBLBUF`, and `tst4:GETPNT/SETDIR/RNGET2/DRTWRT` | A readable ordinary channel may supply the raw write page without gaining LISTEN permission. B-W writes count 1 for pointer 0/1, otherwise pointer minus 1, resets pointer to 1 and refreshes the prepared byte. U2/UB writes the full page without rewriting the count, pointer or prepared byte. Alternate-page/source-drive ownership and EOF transitions survive | Reader-write DOS cases, `ReaderBlockWritesPersistNativePagesWithoutGrantingListenAccess`, `--reader-pointer-check write` | Three complete original-ROM scenarios independently check every persisted byte and exact returned data/EOI, then replay through production firmware/SPI/FPGA |
| Direct LOAD/SAVE and block rejection | Same selected models | Respective `block:OPNBLK/BUFTST`, `tst3:GETRCH`, `tst4:GET1/GET00/SEQGET/RNDGET`, and `tst2:PUT` | `#` accepts SA0/1. Before EOI, SA0 takes sequential GET and repeats its prepared byte. GET immediately after EOI takes RNDGET. SA1 remains circular direct GET. B-P/B-R/B-W/U1/U2 reject SA0/1 with 70 before allocation/type inspection | Expanded direct tests, 30 allocated-listing/direct BUFTST cases and `DirectLoadAndSaveRetainTheirNativePreparedBytes` | Separate complete native `--direct-check load` and `--direct-check save` scenarios retain binary data, exact EOI and unchanged complete media |

[m4-n-4040-new]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/new
[m4-n-8250-new]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/new
[m4-n-4040-duplct]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/duplct
[m4-n-8250-duplct]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/duplct

NEW and DUPLICATE share the existing command-work union and sector pool.
Each worker turn performs at most one synchronous media operation. Unit
state remains 4604 host bytes and channels 72 bytes (4608-byte unit ceiling).
Both captured identities are checked throughout DUPLICATE. Replacement
generations receive no old reads, writes or flushes, including after a
destructive prefix. Failure releases only command-owned scratch. Successful
commands are not equivalent to a host backing-file flush: facade
finalization/remount still owns that persistence boundary.

Fixed formatter scratch cannot overwrite live channels. EconoPET reports
70 if retained channels occupy the low three formatter pages on 4040,
low four on DOS 2.7, or the eleven DUPLICATE pages. This is an explicit
host safeguard, not a claim to reproduce native scratch-RAM corruption.
Magnetic headers/gaps, retries, parallel controller-job failure ordering
and formatter-code echoes into data buffers are not modeled by raw images.

After a direct page number has been read and 256 bytes 0..255 have been
PUT, the next GET returns 0 with EOI. SA0's following complete record is
`1,1,2,...,253` (254 bytes, final EOI). Its next record starts
`255,255,0,1,...` (a 255-byte prefix has no EOI). SA1/ordinary direct
channels retain their previous 253-byte tail and 255-byte circular record.
Do not normalize SA0 to the ordinary direct stream.

Reader B-W uses the verified 4040 revision-2 block overlay (`LDY LINDX`
before `RNGET2`), not the archived older revision's retained Y value.
Its native captures cover valid drive-0 commands, a two-page boundary and
sticky EOF. Failed prefetch, broader refill patterns, read-only/version
errors and partial writes have host coverage, not new native fault/timing
evidence. The pinned MAME controller maps and VICE's direct-channel
implementation corroborate the source-defined resource model without
overriding the selected ROM routines.

Native inputs/captures use the same selected drive/controller/PET ROMs and
pinned MAME identities as the preceding slices. Reproduce quick NEW with
`--format-check quick`, reader writes with `--reader-pointer-check write`,
and special channels with `--direct-check load` or `--direct-check save`,
then run complete `compare_drive_reference.py` replay without prefix flags.
Captures live in `.cache/ai/m4-format-quick-<model>-r2/`,
`.cache/ai/m4-reader-write-native-<model>-r2/` and
`.cache/ai/m4-direct-{load,save}-<model>-r2/`. Comparisons use
`.cache/ai/m4-final-<scenario>-<model>-compare-r1/`.
Every accepted replay must complete all actions/frames and compare entire
images, not only changed sectors, status strings or a successful prefix.

The frozen replay executable is
`.cache/ai/m4-final-replay`, SHA-256
`0513b06633b269e6d1b83d85f50e0eb6bfab9c691c5bcf9016981d0964af1ad8`.
The current reference harness SHA-256 is
`7cd449bce6ca26d815f7547e78b481b82f45f95d5eb5c5f263cc59779894e0b6`.
Historical captures/replays retain their own checkpoint identities.

| Scenario | Model | Native seconds | Native I/O events | Required replay actions | Accepted native frames |
|----------|-------|----------------|-------------------|-------------------------|------------------------|
| Quick NEW | 4040 | 17.52 | 323750 | 1247 | 839 |
| Quick NEW | 8050 | 21.70 | 814349 | 1307 | 879 |
| Quick NEW | 8250 | 24.50 | 1184461 | 1307 | 879 |
| Reader write | 4040 | 28.04 | 874036 | 2109 | 1439 |
| Reader write | 8050 | 31.86 | 1329972 | 2157 | 1471 |
| Reader write | 8250 | 32.26 | 1382834 | 2157 | 1471 |
| Direct LOAD | 4040 | 41.22 | 434875 | 4052 | 2790 |
| Direct LOAD | 8050 | 41.36 | 376650 | 4124 | 2838 |
| Direct LOAD | 8250 | 41.76 | 429517 | 4124 | 2838 |
| Direct SAVE | 4040 | 41.20 | 438407 | 4046 | 2786 |
| Direct SAVE | 8050 | 41.30 | 375522 | 4118 | 2834 |
| Direct SAVE | 8250 | 41.70 | 428380 | 4118 | 2834 |

All twelve production comparisons complete every required action and frame:
34866 actions and 23898 accepted frames in total, zero byte/ATN/EOI/direction
differences, no excluded suffixes or unexamined cases, and exact complete
native/candidate image equality. Phase timings remain recorded without
claiming physical timing parity.

| Comparison (`comparison.json` in the corresponding directory) | Model | SHA-256 |
|-------------------------------------------------------------|-------|---------|
| `format-quick` | 4040 | `141ab4c04b5860fe2f5092ce5a21821c02fc25b9b37a63ff57a1e0ffe62410e9` |
| `format-quick` | 8050 | `85587cc15588f1550077d7eabfd69d956689dc841c86272031a1fd559b262a74` |
| `format-quick` | 8250 | `c7931e5965963b4ea586e8fd27a19ddecd1b44aba1e43305ec53f0ba0920cdde` |
| `reader-write-native` | 4040 | `7837f3a1fa284ba1536e2a150c263b97ba097daa0789c3fc641e66d1e7660bc7` |
| `reader-write-native` | 8050 | `1c30668b20ac37aaacaf1c2ee3bfc5405cfbd27967c49d48e447b764956eec93` |
| `reader-write-native` | 8250 | `015d77c7e2b99ea911f93c018d3c5332d41414bc8fc34b96ce9c24cd0d8acbb6` |
| `direct-load` | 4040 | `fd1ddd8384fbcf80604a0b0a9cac076ed3c4e0cb8e85947b0d6146cc1ca6668e` |
| `direct-load` | 8050 | `cc72f2cccc12e204a41e6cfe5712db76a41107cff9ecc06ef8ce3b9364262596` |
| `direct-load` | 8250 | `9a501b49f816c863cc262907ec42df8fcbe873088ae0a82f0a57ca94fa7bbfd0` |
| `direct-save` | 4040 | `aa8ca9021bd4aba0bdd5bb85bf4d322125e378a69c8abff164834fe40e302505` |
| `direct-save` | 8050 | `bd42086917e7b2d5e1c8aab0b18bc143818022a492dd42d0f732babb070b9720` |
| `direct-save` | 8250 | `a11b0b41858b32b4965cc6e9ba12dfea24abdcb6da0e1b336ae2cd8746f0fade` |

| Model | Initial raw image SHA-256 |
|-------|--------------------------|
| 4040 | `612bd40b23d94c5ccafe814b17e4e880807252100c7a45581996a17ed187738b` |
| 8050 | `fce28fb285e32e55c62ea4d4605992c22a21949037cd4415a2152883b6695b52` |
| 8250 | `9a98ffb11443219a2eb7c21899ffd9bf9e60a78f478bea9cb9cb5afeb4e14b00` |

| Model | Complete quick-NEW image SHA-256 | Complete reader-write image SHA-256 |
|-------|---------------------------------|------------------------------------|
| 4040 | `db1387eaf29c290995955f248f38ab952424960b913b406569ffc8e94a0b4dcc` | `bb5e3d72f6c035c8ccd76764c202fd3b9dc113059b9a5cb0c711c5b35d3d4959` |
| 8050 | `42ea5a579c8ef2aa73cae3d92e2d8e288b20cb31d906eb6818cab81caf188049` | `25184084ad75d662fb187d5ea1784795299b2904805aae26448c38b588b414bb` |
| 8250 | `683253a70cb1cd1945310a5204f31c2477345f3688455a767a850ca413702564` | `8d09148a2de748cad65938309bccb3f5f89c28c195339f40d1e71e18053a82eb` |

Special-channel scenarios must leave the entire input image unchanged.
Quick NEW and reader-write final images are independently constructed
before comparison with native output, rather than blessed from candidate
output. The DOS suite passes 1930 cases and tooling passes 168 cases.
Production cases cover every model, actual remounts, exact directory/EOI
bytes and an independently addressed reader during DUPLICATE. The complete
affected firmware/SPI/FPGA suite passes 221/221, and the complete firmware
regression and RelWithDebInfo deployment build pass. The deployment ELF
`build/fw/src/firmware.elf` has SHA-256
`78bda62a715ebc38441d2ad0174543277a86f637e6d21ac50fbf770c93a90139`.
This ELF identity is not a claim that the SD-card UF2 package was refreshed
or flashed. Host fault
cases do not establish original mechanical error ordering or timing.

**Full original-controller FORMAT/DUPLICATE evidence remains blocked.** MAME's selected
8050/8250 controller returns `75,FORMAT SPEED ERROR,01,00,0` on full NEW.
The pinned [controller source][m4-n-mame-fdc] explicitly lists that same
error in its TODO. Those diagnostic runs complete their controllers but
fail the healthy-output check, so they are not accepted references.
The 4040 full run times out at the unchanged 900-second wall limit without
a complete result. DUPLICATE invokes the same formatter, so quick-NEW
success cannot substitute for healthy full-FORMAT/DUPLICATE evidence.
The early VICE main-DOS/FDC-model attempt did not reach its bounded
monitor checkpoint. It is superseded by the successful corroborative
captures below, not by an original-controller comparison. No ROM/controller
check has been patched, no error has been normalized, and no completion
or trace cap has been raised to manufacture success.

[m4-n-mame-fdc]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c8050fdc.cpp#L9-L15

Remaining M4 evidence includes a healthy full FORMAT/DUPLICATE reference
(original hardware or a sufficiently accurate controller model),
additional malformed/fault/cache comparisons beyond the bounded healthy
native scenarios, the six ordered mixed-type rejection cases on the board,
and destructive disposable-image operator acceptance. Scratch-dependent
corruption behavior is not implicitly accepted. Memory access/arbitrary
code execution remains excluded and REL-dependent cases remain M5.

Real-drive captures are unavailable for this project. Proceed with the selected
ROM/source rules, retained original-controller emulator captures where they
complete, and the explicitly limited VICE main-DOS/C-FDC corroboration below.
Do not schedule an original-drive capture as an actionable prerequisite for
the automated suite or EconoPET board run. The healthy full original-controller
FORMAT/DUPLICATE gap remains a documented evidence limitation, not a passed
capture or a claim that VICE executes the controller ROM. EconoPET board
acceptance is still required and is distinct from access to an original drive.

##### Destructive M4 operator handoff (not yet run)

Use disposable copies only. Keep the approved M3 images and real disks
unchanged. This handoff is a procedure, not authorization to flash a board,
copy the SD card or mark M4 accepted. These command-by-command milestone
checks are not the final acceptance UX. Final acceptance must use the
self-contained, automated eight-slot tape suite specified below.

| Case | Procedure | Required observation and retained artifact |
|------|-----------|-------------------------------------------|
| M4-H0 | Record the firmware ELF/UF2 and FPGA SHA-256 values, PET KERNAL/editor revision, device number and both slot names. Use corrected KERNAL 2.0 or approved KERNAL 4.0/4.1. Repeat on 4040/D64, 8050/D80 and 8250/D82, including device 11 | Unavailable model/slot/controller combinations remain blocked, not extrapolated from device 8/D64 |
| M4-H1 | Copy the model's immutable native `initial.<ext>` into separate target/source disposable files. Verify both against the initial-image table above, then mount them as local drives 0/1 of one device | Both files have the exact expected initial hash. A different model may occupy another unit, never the partner slot |
| M4-H2 | On that device, open command channel 15 and send `N0:M4 FORMATTED` without an ID. Read the entire status, then `LOAD "$0",device` and `LIST` | Status 00. Label `M4 FORMATTED`, ID `01`, DOS `2A` or `2C`, no files and 664/2052/4133 free blocks. Eject/export target and compare its entire hash to the quick-NEW table. Source still has its initial hash |
| M4-H3 | Remount the disposable pair and send `N0:M4 FORMATTED,AB`. Read status and load/list the directory again. Eject/export target | Status 00, ID `AB`, the same empty listing/free count. Compare all raw bytes with `drive_reference.format_expected_image(model, "full", initial_bytes)`, not only the listing. Keep the exported image and comparison result |
| M4-H4 | Remount target/source, send `D0=1`, read status, then read the first four bytes of `0:SEED,P,R` and `1:SEED,P,R` on ordinary channel 2. Eject/export both images | Status 00. Prefix is decimal `0,32,0,1` without EOI. Both complete exported images equal the immutable original source, including unused sectors and IDs |
| M4-H5 | Repeat the established configuration-rejection procedure for all six ordered pairs of unlike D64/D80/D82 types | Reject the pair with the documented configuration-failure diagnostic. Never silently mount a mixed-model unit. Configuration reload is not an atomic per-slot replacement test |
| M4-H6 | Preserve each transcript, elapsed time, initial/exported hashes and pass/fail/blocked result. Retain an independently addressed reader during an additional destructive run where accessible | Another unit must retain its cursor. Report any hang or mismatch as failed/blocked. Native full-format latency and physical handshake parity are not established by simulator durations |

For manual status collection, substitute the mounted device number for `D`:
`OPEN15,D,15`, `PRINT#15,"N0:M4 FORMATTED"` (or the command for the
current case), then `INPUT#15,E,M$,T,S` and `PRINT E,M$,T,S`.
DOS 2.7 has an additional drive field: use `INPUT#15,E,M$,T,S,R`.
Exact wire status is `00, OK,00,00` plus CR/EOI for 4040 and
`00, OK,00,00,0` plus CR/EOI for these DOS 2.7 target-drive-0 cases.
Use `ASC(A$+CHR$(0))` when inspecting binary GET results so a zero byte is
not mistaken for absent data. For M4-H4, execute
`OPEN2,D,2,"0:SEED,P,R":FORI=1TO4:GET#2,A$:PRINT ASC(A$+CHR$(0)),ST:NEXT:CLOSE2`,
then repeat with `"1:SEED,P,R"`. Expect `0,32,0,1` with ST zero for
each byte.

For the independent-reader case, mount a native SEED fixture on another
device `P` and execute
`OPEN3,P,3,"0:SEED,P,R":GET#3,A$:PRINT ASC(A$+CHR$(0)),ST`.
Expect byte 0, ST zero. Run the destructive command on `D` without a MENU
reset, configuration reload or global unmount, then execute
`GET#3,A$:PRINT ASC(A$+CHR$(0)),ST:CLOSE3`. Expect byte 32, ST zero.
This tests cursor retention across the command, not peer service latency
while the global command fence is active. Close data files before exporting images.
Do not write a new target or rerun a failed destructive step until its
originals, transcript and partial exported image are preserved.

#### M4 malformed-command, precedence and cache comparisons

The bounded `--edge-check` scenarios supplement the earlier command-specific
captures. They use the same selected 4040 revision-2 and 8050/8250 DOS 2.7
ROMs, approved PET KERNAL 4 and pinned traced MAME. Each observation has an
isolated 64-byte data/status region and an explicit phase marker. Binary CR
does not end a data record. A closed-channel timeout is PET-synthesized CR
with ST 2, not a drive byte or EOI.

| Edge variant | Observations per model | Source-derived coverage |
|--------------|------------------------|-------------------------|
| `structure` | 26 | NEW, DUPLICATE, RENAME, COPY, SCRATCH and block delimiters, required groups, patterns and unknown commands |
| `precedence` | 27 | Numeric parsing before channel lookup, channel before geometry, unknown opcode before numeric fields, missing source before target collision, retained peer |
| `numbers` | 16 | Initialized raw/counted reads, truncated pseudo-decimal fields, omitted sector, separator 29, folded and literal lowercase utility selectors, B-P retained hundreds nibble |
| `numeric-errors` | 16 | Fifth-field rejection, no-digit fields after a numeric field, omitted/zero geometry and large track/sector fields |
| `direct` | 28 | Explicit pages, low-byte/three-digit truncation, reserved pages, skipped separators, multiple fields and pseudo-decimal punctuation, with observed CLOSE fences |
| `direct-values` | 17 | Actual returned page bytes, occupied page, release/reuse and terminal CR in OPEN |
| `limits` | 11 | 57-60 character commands, two transmitted CRs at saturation, NUL, empty command and subsequent recovery/read |
| `cache-drives` | 13 | Version-rejected drive-1 write preserves a drive-0 direct channel across I1, while the drive-1 peer closes. Successful drive-0 write and dirty CLOSE affect only drive 0 |
| `cache-parts` | 13 | Foreign first map versus native later maps, cache replacement without dirtying, subsequent write-version decisions |
| `cache-raw` | 13 | Raw modification of on-disk map version leaves cached permission unchanged until I0 |
| `cache-errors` | 16 | Structural/channel/geometry/name errors before foreign-version rejection, no-match SCRATCH and permitted reads |
| `cache-metadata` | 15 | Foreign-version RENAME, SCRATCH, COPY, VALIDATE, quick NEW and DUPLICATE-source failures, with I0 between failures |
| `cache-dirty` | 12 | Cached B-A claim is absent from raw reads, survives a competing raw bitmap write, and replaces that write at CLOSE |
| `validate-cross` | 5 | 4040 duplicate ownership is tolerated and maps rebuilt. DOS 2.7 reports 65 at the duplicate coordinates without publication |
| `validate-side` | 5 | VALIDATE follows the ordinary entry's side-chain pointer and rebuilds exact maps. This is not REL creation or record access |

The primary rules and implementation corrections are:

| Behavior | Selected ROM source | Required result | Code/test mapping and corroboration |
|----------|---------------------|-----------------|-------------------------------------|
| Deterministic no-digit numeric fields | [4040 block][m4-d-4040-block] and [8250 block][m4-d-8250-block], `BLKPAR/BP05/ASCHEX/AH10-AH40` | ASCHEX clears three temporaries but retains `TEMP+2`. A preceding numeric field establishes its hundreds nibble, so `B-P:2,A` selects 0 and `B-P:258,A` selects 200 | `block_parameters`, DOS parser tables, production `BufferPointerReplacesStagedBytesAndRetainsPeer`, `numbers`/`numeric-errors` native cases |
| COPY comma before an existing colon | [4040 copysetup][m4-c-4040-copysetup] and [8250 copysetup][m4-c-8250-copysetup], `DX0000`, respective `parsex:TC30/TC25` | `C,0:NEW=SEED` returns exact syntax status 34, not 30. The colonless COPY path remains distinct | `copy_start`, `test_copy_parser_precedence_and_command_union_reuse`, all three original `structure` comparisons exposed the old 30 |
| Rejected direct-write affinity | [4040 tst4][m4-c-4040-tst4]/[8250 tst4][m4-c-8250-tst4], `DRTWRT/DRT`, and [4040 jobs][m4-d-4040-jobs]/[8250 jobs][m4-d-8250-jobs], `SETJOB/SJB2/SJB3/SJB4/VNERR/SJB1` | Version rejection occurs before LSTJOB changes. Preserve channel binding and last-job drive, while retaining counted B-W's earlier count/pointer mutation | `block_write_work`, six `test_rejected_block_write_preserves_drive_affinity` cases, two-drive `cache-drives` capture |
| Receive saturation before command trimming | [4040 tst2][m4-v-4040-tst2], approved revision-2 `L42` overlay, [8250 tst2][m4-v-8250-tst2], `L42`, and respective [4040 parsex][m4-r-4040-parsex]/[8250 parsex][m4-r-8250-parsex], `CMDSET/CS08` | Retain at most 59 bytes. With 58 X bytes followed by two CRs, the second CR is discarded and CMDSET sees a 58-byte unknown command (31). With 59 X bytes, return 32 | Existing `dos_input_put` and command trimming, complete `limits` outgoing-wire and status comparisons |

No persistent DOS state was added. Direct-write prechecking intentionally
does not move the ordinary WRTAB/SJ10 affinity update in the shared sector
writer: that path updates LSTJOB earlier than DRTWRT. The successful direct
path reuses the shared writer after the gate.

Rejected diagnostic captures remain separate from acceptance. The original
`numbers` fixture omitted I0 and consequently observed native disk-ID error
29 instead of an initialized block-read result. The revised fixtures
initialize before raw access and split numeric cases to preserve capture
bounds. Lowercase utility selection is emitted with `CHR$(97)`, not a quoted
letter that PETCAT folds. The earlier combined CLOSE/OPEN diagnostic did not
isolate completion and produced unexpected page-selection observations.
Revised direct fixtures capture the entire CLOSE status before the next OPEN.
No native mismatch was normalized into an expected byte.

The limits diagnostic transmitted both CRs: its wire stimulus is 58 X bytes
plus CR plus CR, not an assumed BASIC suppression of the second CR. Its
original 32 oracle was wrong because it omitted receive saturation. The
corrected 31 expectation follows the selected source overlay and complete
captured bytes. `N0:NAME,AB,CD` was also removed from syntax-only diagnostics:
the ROM accepts those fields and enters full formatting, which remains
controller-model blocked rather than a syntax failure.

#### M4 edge-comparison evidence

All 45 complete comparisons pass (15 variants on each selected model),
covering 699 observations, 81,700 requested/completed replay actions and
56,106 accepted frames. There are no stopped cases, excluded suffixes,
uncompared frames/cases, wire issues, aborted frames, unexplained differences,
or byte/ATN/EOI/direction differences. Every complete candidate image equals
its native final image. All three `cache-drives` runs additionally compare
the entire unchanged foreign drive-1 image.

| Model | Observations | Completed/requested actions | Accepted native/candidate frames | Largest native I/O log |
|-------|--------------|-----------------------------|----------------------------------|------------------------|
| 4040 | 233 | 25429/25429 | 17499/17499 | 1399743 events |
| 8050 | 233 | 28135/28135 | 19303/19303 | 1767541 events |
| 8250 | 233 | 28136/28136 | 19304/19304 | 2507577 events |

Native logs are complete below the unchanged six-million-event cap.
Captures are `.cache/ai/m4-edge-<variant>-<model>-<revision>/`.
`structure` uses `r2` for all three models. Reused `r3` captures are
4040 `precedence`, `cache-dirty` and `cache-errors`,
8050 `cache-parts`, `cache-metadata` and `validate-cross`, and
8250 `cache-raw` and `validate-side`. The other 34 captures use `r4`.
All 45 final comparisons are
`.cache/ai/m4-edge-<variant>-<model>-compare-r3/`.

The frozen final replay is `.cache/ai/m4-edge-replay-r2`, SHA-256
`2ab137e45ef91d4d5531e80acca5bdee1511c4a1b175ca040e8ae2e8928496a5`.
The retained current harness snapshot is `.cache/ai/m4-edge-harness-r4.py`,
SHA-256
`f45e1366c7057e498626ff41e5b732f973bdd6796d11135d5e6521e0fb4209d7`.
Earlier captures retain their historical Python hashes rather than claiming
the current source identity. Their actual controller source is byte-equal
to the current generator for those unchanged variants, and their stored
observations and complete images pass the current independent oracles.

The compact evidence index is `.cache/ai/m4-edge-evidence.json`, SHA-256
`4744038d01ea568a7959902aeeb5398f28e878ed3a24d13a9ff3a6c3977cf310`.
It records every capture/comparison path, observation/action/frame count,
native duration and I/O count, result/report/controller/replay hashes, full
initial/final image hashes, harness identities and secondary-image hashes.
Audit output is `.cache/ai/m4-edge-audit.log`. The audit rechecks retained
artifact hashes, live selected main/controller ROM bytes, manifest, emulator
and tools, every decoded observation, full action coverage, all paired wire
bytes/flags and both complete persisted images. Simulation phase durations
remain diagnostic, not measured physical handshake or SD latency.

Before-fix evidence remains rejected, not part of the 45 successes:

| Rejected comparison under `.cache/ai/` | Observed defect | Report SHA-256 |
|--------------------------------------|-----------------|----------------|
| `m4-edge-structure-4040-compare-r1/` | COPY returned 30 instead of 34 | `c822d419c6fde843c75242e8509dd6667c3e6f601e98aeb739096b1d570a5063` |
| `m4-edge-structure-8050-compare-r1/` | Same COPY discrepancy | `c6ce63e85aa60238dd6cb21d332c7a9325789b27361f8727e09d79bfa557769f` |
| `m4-edge-structure-8250-compare-r1/` | Same COPY discrepancy | `efddddb95f74abdbe8720aabba36fecd3807652e458b858740134e7c520dd243` |
| `m4-edge-numbers-4040-before-retained-digits/` | Frame 957 returned byte 4 instead of 200. All 965 frames completed | `507bc490742b503e48d697ef2a9f044b0598b734b60b0a41c633e17212255715` |
| `m4-edge-cache-drives-4040-before-affinity/` | Incorrectly closed the retained direct channel. Replay stopped at `direct_zero_retained`, with 191 frames uncompleted | `b40f92f54efc559b7c94e4c29d4242ca23e2d90beafc6875dddff75013278161` |

The COPY and numeric failures use the earlier `.cache/ai/m4-final-replay`
(`0513b06633b269e6d1b83d85f50e0eb6bfab9c691c5bcf9016981d0964af1ad8`).
The affinity failure uses the numeric-only `.cache/ai/m4-edge-replay-r1`
(`de4a3d50ab21f9359bfcb2a991bf768df1136a9d934bcb891a59ea1f362df5e8`).
All final comparisons use the corrected frozen executable above.

The tooling suite passes 174 tests, including independently encoded
observations, every-record missing/byte/EOI negatives, capture bounds,
stimulus fences and full-image mutations. The DOS suite passes 1942 checks,
the complete ordinary firmware aggregate passes, and all 221 affected
firmware/SPI/FPGA cases pass. The production `RelWithDebInfo` build and
scoped formatting/whitespace checks pass.
The broader firmware preset still exposes the unrelated existing
`display_tests` failure (7 of 12 terminal-rendering checks). Neither display
source nor its tests was changed for this work.

To repeat one case with approved local ROM inputs and the frozen replay:

```sh
# Capture all native phases on a fresh disposable image.
python3 tools/drive-ref/drive_reference.py --model 4040 \
  --manifest tools/drive-roms/drive-emulation-roms.json \
  --drive-rom-dir .cache/ai/m3-native-inputs \
  --controller-rom-dir .cache/ai/m3-native-inputs \
  --gcr-rom .cache/ai/m3-native-inputs/901467.bin \
  --mame build/drive-rom-verification/mame-trace-source/petd \
  --trace-build-info build/drive-rom-verification/mame-trace-source/petd.json \
  --edge-check numbers --output-dir .cache/ai/m4-edge-numbers-4040-recheck

# Compare the entire wire stimulus and persisted images, without prefix flags.
python3 tools/drive-ref/compare_drive_reference.py \
  --reference-dir .cache/ai/m4-edge-numbers-4040-recheck \
  --output-dir .cache/ai/m4-edge-numbers-4040-recheck-compare \
  --replay-tool .cache/ai/m4-edge-replay-r2
```

Arbitrary first-field scratch contents (`#A`, `B-P:A,0`), arbitrary memory
or code execution, physical write-protect/controller/media faults, healthy
full FORMAT/DUPLICATE and physical timing/board acceptance remain outside
these bounded comparisons. These are explicit limits, not passing cases.
REL record semantics remain M5.

#### M4 VICE full-NEW and DUPLICATE corroboration

[drive_vice.py](../../tools/drive-ref/drive_vice.py) now completes full NEW and sector
DUPLICATE on 4040/D64, 8050/D80 and 8250/D82 under VICE 3.9. It executes the
same approved main-DOS and PET BASIC/KERNAL/editor bytes as the MAME scenarios.
Exports of every active main-DOS, BASIC, KERNAL and editor ROM are byte-checked.
The main-ROM `NEW/FORMAT` and `DUPLCT` paths remain the primary rules cited
above. VICE's C FDC job handlers replace original controller execution, so
these six results are corroboration, not full-controller acceptance.

The former VICE timeout was a harness defect, not evidence that VICE cannot
format. BASIC must be composed in CPU address order B000/C000/D000, not the
old C000/B000/D000 order. The new file-based monitor loads the bounded BASIC
controller at READY, initializes its end pointer and keyboard RUN command,
and observes the actual completion-mailbox store. An early BASIC error,
missing record or timeout fails capture. No remote-monitor prompt, elapsed
delay, patched ROM or forced command result establishes completion.

DUPLICATE's literal ASCII-range PETSCII header oracle also requires lowercase
host input to c1541's formatter. Its uppercase host input encodes high-bit
PETSCII instead. Rejected `r6` captures correctly copied those entire
high-bit source images but failed the header oracle. The corrected fixture
uses `m1 fixture,01` only for DUPLICATE, before either emulator executes.
Historical non-DUPLICATE fixtures are unchanged. No captured output is
rewritten or normalized. c1541 extraction explicitly selects PRG or SEQ
because its unqualified read defaults to PRG.

Each complete VICE byte/status record is compared with production C through
the real SPI/FPGA model using reconstructed KERNAL OPEN/PRINT/GET/CLOSE
stimulus. This is **not a captured VICE IEEE wire trace**. The reconstruction's
complete quick-NEW scripts are byte-equal to all three previously captured
MAME scripts (1247/1307/1307 actions), protected by independent trace hashes
in [test_drive_vice.py](../../tools/drive-ref/test_drive_vice.py). Production's actual
accepted bus frames must match every reconstructed byte, direction, ATN and
EOI flag. Every requested action, read and observation must complete.

All six comparisons pass: 39 complete observations, 1048 received bytes,
6741 requested/completed actions and 4537 accepted production frames.
There are no omitted suffixes, stopped cases, wire issues, aborted frames
or unexplained byte/flag differences. Every full-NEW image equals the
independently constructed complete raw image. Every DUPLICATE target equals
its complete immutable source, including unused sectors, and both VICE and
production leave that source unchanged.

| Model | Full NEW actions/frames | DUPLICATE actions/frames | Equal VICE/production full-NEW SHA-256 |
|-------|-------------------------|-------------------------|--------------------------------------|
| 4040 | 1250/842 | 917/617 | `8352a17c597e18263336f102c67fcff8ddf54ab528070f815ef362b628e9be65` |
| 8050 | 1310/882 | 977/657 | `9b0ca9d132623256d078adb75bc2718e39bf69805c6fce99dcb9c1740bc3eea4` |
| 8250 | 1310/882 | 977/657 | `364a913a4031f858f3c3035a49a8e2946abdd44431e23d27786df268c7f32b44` |

c1541 independently reads directory labels/IDs/DOS types and every BAM bit
and count across all 35/77/154 tracks. Its free counts are 664/2052/4133
after NEW and 651/2039/4120 after DUPLICATE. Across reference targets,
production targets and duplicate sources, this checks 1330 track decodings
and 45 file extractions. All five copied files match their original fixture
bytes, including the 254/255-byte boundaries and `M1CHECK`. Inspection is
read-only and each entire image is rechecked afterward. This software-DOS
image decoding does not establish native parser, cache or handshake behavior.

Final captures, replay outputs and c1541 logs are
`.cache/ai/m4-vice-<full|duplicate>-<4040|8050|8250>-r8/`.
The evidence index `.cache/ai/m4-vice-evidence.json` has SHA-256
`1365e075bc15c3c2eee738380cd1d50dc579a718f2b50b3dea086ffdd63eac9a`.
It records every result/comparison identity, complete input/final/source
hash, observation/action/frame count and diagnostic main-CPU completion cycle.
Audit output is `.cache/ai/m4-vice-audit.log`, with frozen harness, tests and
audit source in `.cache/ai/m4-vice-harness-r8/`.
Each result/report additionally records approved staged inputs, active ROM,
capture/mailbox/controller, supporting Python, manifest, emulator/tool,
reconstructed stimulus, received data, coverage and actual production-wire
identities. Earlier pilot and rejected captures retain their own identities.

| Tool or source | SHA-256 |
|----------------|---------|
| VICE xpet 3.9 | `6d0e8f8439685c93bd05b331c248ccb6d7d3d6a380d64214ce5d8179729c674c` |
| VICE c1541 3.9 | `411b3a0a17d8b2444d2dd020518f1a999bc5a5e435167d77db099bb05b3bdc7b` |
| VICE capture/comparison harness | `5ce6c0f6358fb1fef5f20e04e945e78f100335cfff30379bf973846970148e7f` |
| Shared reference/fixture helper | `d00da6973a75653a7fcc154e6b6129b6d323db64c8dacc4def335f6cfa926c44` |
| Production replay executable | `2ab137e45ef91d4d5531e80acca5bdee1511c4a1b175ca040e8ae2e8928496a5` |

Reproduce each model/variant in a fresh ignored directory with approved local
ROM inputs (the defaults below use the retained M3 input directory):

```sh
# Execute the approved main ROM and compare every observation and complete image.
python3 tools/drive-ref/drive_vice.py --model 8050 --variant full \
  --output .cache/ai/m4-vice-full-8050-recheck \
  --replay-tool .cache/ai/m4-edge-replay-r2

# Protect capture bounds, protocol reconstruction, PETSCII and independent image checks.
python3 -m unittest discover -s tools -p 'test_drive_*.py'
```

The tooling suite passes 182 cases (eight added). No production firmware or
gateware change was needed for these comparisons. Healthy full-NEW and
DUPLICATE main-DOS/raw-image behavior now has executable corroboration, but
original controller/formatter execution, magnetic timing, retry/error
ordering, physical faults and the destructive operator handoff remain open.
M4 is not operator-accepted. No SD files were copied and no board was flashed.

#### M4 historical replay consolidation

The latest complete retained native capture of each older M4 scenario has
been replayed against the current production-C/SPI/FPGA executable. This
consolidation contains 24 scenarios on each of 4040/8050/8250, not older
superseded diagnostic revisions or the separately audited 45 edge cases.
It covers command/reset/utility paths, rename, scratch, named/concatenating
and dual-drive copy, validate, direct/pointer/block operations, ordinary-reader
pointer/read/write variants, quick NEW, direct LOAD/SAVE and all three
write-version variants.

All 72 comparisons pass. Every requested action, captured observation and
accepted frame is compared, with no prefix flags, stopped cases, excluded
suffixes, missing/extra frames, wire issues, aborted frames or unexplained
byte/ATN/EOI/direction differences. Entire target and optional source images
match their retained native counterparts.

| Model | Complete scenarios | Observations | Completed/requested actions | Accepted frames |
|-------|--------------------|--------------|-----------------------------|-----------------|
| 4040 | 24 | 344 | 49335/49335 | 34039 |
| 8050 | 24 | 344 | 51837/51837 | 35707 |
| 8250 | 24 | 344 | 51838/51838 | 35708 |
| Total | 72 | 1032 | 153010/153010 | 105454 |

Root `default` configuration and `sys-test` build confirm that
`build/system/board/econopet_drive_reference` is current and byte-equal to
the frozen `.cache/ai/m4-edge-replay-r2`, SHA-256
`2ab137e45ef91d4d5531e80acca5bdee1511c4a1b175ca040e8ae2e8928496a5`.
Configuration/build logs are `.cache/ai/m4-latest-{configure,replay-build}.log`.

The complete index is `.cache/ai/m4-historical-latest-r2/evidence.json`,
SHA-256
`fa4987b545900f9224563b3327666c4942fc018ac3607c69364ec296741b9318`.
It identifies every native/comparison path, result/report hash, model,
observation/action/frame count and entire input/final image identity.
Six already completed comparisons remain at their indexed `r1` paths.
Interrupted attempts without complete comparison reports are not successes.
The consolidation audit also verifies 504 original controller/capture/wire/
replay artifact hashes and all initial/final/secondary images.
Its log and frozen replay/audit scripts are beside the index. This checks
current production against retained native execution, not a new native capture,
physical timing comparison or EconoPET board result.

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

#### M5 current implementation and handoff (operator accepted 2026-10-29)

M4 is accepted (the reported 560 groups and separate 24/24 benchmark).
The following M5 implementation is now public: binary REL OPEN/create,
inferred reopening, LISTEN/write and TALK/read, record P, sparse growth,
side/super-side indexing, delayed overflow, CLOSE/CLOSE-all, REL COPY,
SCRATCH and VALIDATE. These use the existing shared twelve-page pool and
three pages per REL owner, not a second cache or whole-file index.
The chronological development sections below describe earlier states and
their evidence boundaries, not the current availability of public OPEN.

Final measured unit/channel/record/REL-reader sizes are 4588/68/36/40 bytes
on the Debug host and 4548/64/32/36 bytes on ARM. The unit stays within the
4608-byte bounded-overhead limit, including its 3072-byte shared page pool.
Host storage failures retain public creation/CLOSE ownership for explicit
retry. Internal COPY failure instead follows FREICH and releases its owners
without silently completing a failed output. Other units and public peers
remain independent. Callback failure tests are not physical controller-error
or SD power-loss emulation.

All primary links below pin `mist64/cbmsrc` commit
`01bd60f162ef92212ef0cb67546ae8f42be34168`. Targets are the approved 4040
DOS 2.0 revision-2 mapping and shared DOS 2.7 in the selected single-sided
8050 or double-sided 8250 mode.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| Public binary OPEN and record length | All three selected modes | [4040 open](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/open), [DOS 2.7 open](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/open), `CMDSET/FFST/CKTM/OP60/OP40/OPREAD` | Preserve raw comma, CR and high-bit size bytes, native command-tail trimming and parameter order. A nonzero requested length mismatch returns 50. Existing REL APPEND/replace rejects 64. SA defaults still apply after missing-file promotion | `dos_open_parse`, public OPEN lookup/dispatch, binary parser and existing/create tests | 1734 complete public OPEN vectors, including lengths 1..255 on both slots. VICE true-drive execution of the actual acceptance PRG checks the normal public path |
| Creation and directory publication | Same | [4040 opchnl][m5-current-4040-opchnl], [DOS 2.7 opchnl][m5-current-8250-opchnl], respective `INTTS/GETWCH/GETBUF/OPNWCH`, [4040 addfil][m5-current-4040-addfil]/[DOS 2.7 addfil][m5-current-8250-addfil] `ADDFIL/NXDRBK`, [4040 nulbuf][m5-current-4040-nulbuf]/[DOS 2.7 nulbuf][m5-current-8250-nulbuf] `NULBUF/ADDNR` | Retain native allocation/publication order, three-page resource pressure, closed type $84 and initial count zero. Write the old directory tail link before the new directory page. Zero-size new OPEN remains pending in NULBUF rather than inventing syntax | `public_rel_open_work`, public create/remount, publication fault/peer, directory-growth and cooperative-zero tests | Full-image public OPEN proof includes full directories/BAM boundaries and six bounded native zero-loop observations. VICE virtual-drive zero-size rejection differs and is not the oracle |
| Record I/O and growth | Same | [4040 rel2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel2), [DOS 2.7 rel2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel2), `RDREL/RD40/WRTREL/WR30`, respective `RECORD/ADDREL/SSEND` | Only committed GET advances ownership. Preserve embedded NUL, trim trailing NULs, set EOI at the record boundary, report missing 50 and delayed overflow 51, and grow through side/group boundaries with native image effects | Retained REL jobs and record/growth workers, `dos_rel_test.c`, public SPI creation/overflow tests | Native leaf/job/full-image proofs described below. Actual PET PRG crosses 120 data pages on D64/D80 and 720 on D82 under all eight selected native slots |
| COPY record/type selection | Same | [4040 rename (COPY body)][m5-current-4040-copy], [DOS 2.7 rename (COPY body)][m5-current-8250-copy], `COPY/CHKIN/OPIRFL/GIBYTE`, respective `lookup:COMPAR`, `PIBYTE/CLSREL/FREICH` | REC is the first nonzero raw matched directory byte 23 in lookup order. Prepare each source's own length. Pass record EOI to WRTREL without copying terminal synthetic missing CR. Preserve copy-all iteration, native mixed-type behavior and error precedence | COPY retained phases, `copy_read_start`, `copy_put`, public creation/CLSREL integration, COPY resource/fault tests | 157 actual main-ROM comparisons of both complete images, including read-only sources, both slots, concatenation, copy-all, side/group and directory growth |
| DOS 2.7 COPY preallocation | 8050/8250 DOS 2.7, deliberately not 4040 | Same DOS 2.7 `rename:CY10/CYEXT`, [addrel](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/addrel) `ADRELS/ADDR1/ADDBIG`, respective `ssend:SSEND` | Plan from source end coordinates with offset zero, not an ordinary record-number extension. Preserve 8050 zero-growth over-allocation, 8250 zero-plan return and inherited LRF before first WRTREL. Fresh ordinary-output/REL-source COPY waits on 8050, returns 66 at 0/0 on 8250, and streams on 4040 | `dos_rel_growth_plan_at`, COPY end/extend/wait phases, short-record/native-preallocation tests | Complete COPY proof executes successful/error/pending boundaries. Pending 8050 work yields cooperatively. VICE virtual-drive COPY is not authority for these quirks, and MAME independently corroborates the shared ROM/hardware-mode distinction |
| REL SCRATCH and VALIDATE | All three selected modes | [4040 scrtch][m4-s-4040-scrtch], [DOS 2.7 scrtch][m4-s-8250-scrtch], `SCRTCH/DELFIL/DEL1/DELDIR`, [4040 verdir][m4-v-4040-verdir]/[DOS 2.7 verdir][m4-v-8250-verdir] | Walk side/super and data allocations with native closed/splat differences. `=R` selects REL. Preserve 4040 versus DOS 2.7 duplicate-mark behavior and cross-group side links | Existing metadata engine without REL exclusions, independent chain/splat/duplicate fixtures and public SPI SCRATCH/VALIDATE | 36 complete native image comparisons (21,295,104 persistent bytes). True-drive PRG COPY/VALIDATE/cleanup corroborates valid-file operation |
| Native CLOSE versus host durability | All selected native modes plus explicit host lifecycle policy | [4040 close](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/close), [DOS 2.7 close](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/close), `CLSREL/CLSALL`, respective `ADDFIL/NXDRBK/MAPOUT` | Public CLSREL retains native writes/counts and CLOSE-all order. Host finalization additionally persists only the tracked late directory-BAM allocation made after creation MAPOUT, clearing its retry marker only after backing flush succeeds. Do not globally flush failed ordinary COPY effects | `dos_finalize`, `rel_directory_bam`, retryable directory-growth durability tests, public CLOSE-all/FIFO/remount tests | Native CLOSE/public-CLOSE proofs establish native effects. Finalization's additional flush is a labeled host durability adaptation, not a claimed native CLSREL job |
| Malformed image boundaries | Explicit bounded host policy, not native parity | [DOS 2.7 system](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/system), `RDLG/RDSS1C`, and both `open:OPREAD` | Reject an existing zero-length REL entry with host status 71. An empty 8250 super table stops before native underflow can inspect adjacent physical RAM. It does not modulo-wrap through unrelated host pool pages or invent a missing record | `test_public_rel_zero_size_directory_policy`, `test_end_empty_super_table_handoff`, bounded NO_GROUP/NO_INDEX paths | These are deliberate corruption-policy differences. Native malformed adjacent-RAM behavior is not implemented or certified by the clean-file differential results |

[m5-current-4040-opchnl]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/opchnl
[m5-current-8250-opchnl]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/opchnl
[m5-current-4040-addfil]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/addfil
[m5-current-8250-addfil]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/addfil
[m5-current-4040-nulbuf]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/nulbuf
[m5-current-8250-nulbuf]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/nulbuf
[m5-current-4040-copy]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rename
[m5-current-8250-copy]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rename

The independent secondary references are
[VICE vdrive-rel](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/vdrive/vdrive-rel.c)
and [MAME c8050](https://github.com/mamedev/mame/blob/master/src/devices/bus/ieee488/c8050.cpp).
They corroborate general REL structure and hardware selection, not every
selected-ROM quirk. VICE's virtual drive rejects new unspecified/zero lengths
and uses heap-backed side structures, unlike native NULBUF and this bounded
implementation. The actual-ROM execution, not virtual-drive agreement,
establishes the native rules above.

Current results and reproducible entry points:

- Production `cmake --build --preset fw` and Debug firmware/system builds
  pass. The complete affected `firmware_tests` target passes 3733 ordinary
  checks and 53 fatal-suite checks. Shared reporter tests pass at both
  screen widths.
- Thirteen new/updated public SPI cases and eighteen neighboring P,
  reset/remount/replacement/close-retry integration cases pass. Run these with
  `build/system/board/econopet_firmware_tests --gtest_filter='*NativeRelPublic*:*NativeRelOverflow*:*UnimplementedDriveFeaturesCannotFallBack*:*NativeRecordRejects*:*RemountDiscards*:*DriveResetRetains*:*ReplacementReadsAndConsumes*:*PendingCloseRejects*'`.
  The root `sys` CTest entry is a wrapper, so a root test-name regex does not
  select its individual GoogleTests.
- Complete public OPEN, COPY and metadata records are
  `.cache/ai/m5-public-open-rom/result.json`,
  `.cache/ai/m5-copy-rel-rom/result.json` and
  `.cache/ai/m5-metadata-rom/result.json`. Each records exact source/probe/ROM
  identities and its evidence exclusions. Earlier native component proofs
  remain described in the development history below. The twelve existing
  OPEN/job/EOI/reentry/position/index/CLOSE/create/growth/LISTEN/RECORD/public-CLOSE
  proofs were refreshed against the final production source hashes.
- The current automated [M5 suite](../../prgs/m5check/README.md) uses D64
  for both slots of unit 8, D80 for 9, D82 for 10 and D64 again for 11.
  The earlier M4 profile and frozen package remain unchanged. `M5CHECK`
  creates/exercises all eight slots (64 groups). After an actual remount or
  power cycle, `M5COLD` independently checks persistence (24 groups).
  `M5CLEAN` is optional (8 groups). Each needs only tape LOAD and RUN, with
  no POKEs, phase injection, manual per-drive loops or fixture preparation.
  All use the shared timed, pinned-title `m5check` reporting framework.
- `python3 tools/m5check_vice.py --output <new-directory>` runs those three
  actual executables under the selected native ROMs. The approved record
  `.cache/ai/m5-accept-automated-v2/result.json` passes 64/24/8 groups.
  It validates both 4040 units' runtime ROM mapping and retains unchanged
  remount-phase image hashes. A cold run on blank images and another with
  swapped D64 slots fail without changing media. Slot-tagged expected bytes
  are generated independently, not accepted from stored PASS flags.
- `python3 tools/package_m5_rel.py --evidence .cache/ai/m5-accept-automated-v2/result.json --output <new-directory>`
  requires matching successful evidence for every executable and this
  exact profile, rejecting stale cold binaries and the previous D82 unit 11.
  `build/m5-acceptance.zip` supplies fresh dedicated images, all three PRGs,
  firmware, a complete four-variant ROM-4 boot configuration, an optional
  mount fragment, runbook and checksums. The SD configuration must be backed
  up before copying the dedicated one. There is no source-config or SD-card
  mutation by the packaging tool. Frozen M4 archive/PRG hashes were
  rechecked unchanged.

The operator reported the following physical-board acceptance results on
2026-10-29. Times are UTC. The assistant did not flash the board or modify
the operator's SD card. These are operator reports, separate from the
automated native-ROM evidence above.

| Time | Board workflow | Reported result |
| --- | --- | --- |
| 13:13 | `M5CHECK`, both slots of units 8/9/10/11 with D64/D80/D82/D64 | `PASS 64/64 -- All OK` |
| 13:14 | `M5COLD` after the instructed remount/power cycle, retaining the modified images | `PASS 24/24 -- All OK` |
| 13:16 | Optional `M5CLEAN` | `PASS 8/8 -- All OK`, reserved test files removed |
| 13:24 | Waterloo microBASIC on the writable language D80 copy, device 8 drive 1 | `STORE 'disk/1.m5water'`, LOAD, LIST and RUN restored line 10 and printed `M5 WATERLOO PERSISTENCE` |
| 13:26 | Waterloo after the instructed power cycle, without recreating the program | LOAD, LIST and RUN restored the same line and output |
| 13:28 | Super-OS/9 from the writable system D80 copy, device 8 drive 0 | Boot, `dir /d0`, `dir /d0/cmds` and `list /d0/startup` passed, with BUILD available |
| 13:33 | OS-9 `build /d0/m5proof`, then `list /d0/m5proof` | Operator reported that LIST printed the entered text. The prescribed payload was four distinct numbered lines totaling more than 128 bytes |
| 13:35 | OS-9 after the instructed power cycle, without rebuilding the file | Operator reported PASS and the same text from `list /d0/m5proof` |

These reports close M5's automated-board and supported application
read/write/persistence gates. The application files were left intact.
Cleanup removed the automated suite's fixtures, so another `M5COLD` run
requires recreating them with `M5CHECK`. The application gate used native
D80 images, not a flat `.hdd` substitute.

The deployed firmware/FPGA identities and modified-image hashes were not
supplied by the operator. Packaged identities are not independent proof of
deployment. These results establish the reported completed-write workflows,
not interrupted-write power-loss tolerance, exhaustive application coverage,
or an original-drive timing comparison. The frozen acceptance package was
not regenerated to incorporate these later observations.
The malformed-image policy differences above remain explicit limitations,
not claims of exhaustive compatibility with arbitrary native RAM corruption.

#### Historical M5 foundation (superseded implementation snapshot)

`fw/src/ieee/dos_rel.c` now supplies record-address arithmetic and a bounded,
SD-backed side/super-side-sector resolver. It does not yet allocate or open REL
channels. The resolver borrows one existing 256-byte DOS page per step, performs
at most one media read per call (two reads for small REL, three for 8250), and
holds at most 32 bytes of cursor state. It has no heap allocation, private page,
file-length-proportional index, or new per-unit buffer pool. Completed lookup
coordinates remain bound to the captured media token, not replacement media.
Missing pointers, storage failures and cancellation have separate outcomes.

The command dispatcher now implements binary `P`'s initial channel/type errors.
It preserves embedded zero/CR bytes, raw secondary aliases and the prior status
drive. Missing/write-only channels return 70. Readable non-REL channels and the
error channel return 64. Short requests reach these checks without a synthetic
five-byte syntax requirement. REL OPEN and unimplemented REL positioning remain
explicitly rejected rather than exposing partial record I/O.

Primary evidence uses `mist64/cbmsrc` commit
`01bd60f162ef92212ef0cb67546ae8f42be34168`, the approved 4040 revision-2
mapping and shared 8050/8250 DOS 2.7 in the selected hardware mode:

| Contract | Pinned primary routines | Implemented/tested boundary |
| --- | --- | --- |
| Record/position zero aliases one. Out-of-record byte positions reset to offset zero while retaining overflow | [4040 RECORD](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/record), [DOS 2.7 RECORD](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/record), `R30/R40` | Pure address calculation, not record readiness, status 51 publication or EOI |
| 254 payload bytes, 120 data pointers per side sector, six sides per group. Small-file SSNUM is byte-wide | [4040 FNDREL](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/fndrel), [DOS 2.7 FNDREL](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/fndrel), `MULPLY/DIV254/DIV120/DIV006` | Record, payload, side, group and 16-bit record-limit arithmetic |
| 4040 DIV120 is not unrestricted mathematical division. Its final subtraction can leave remainder >=120 and wrap SSIND into bytes 0..15 | Same 4040 `DIV150..DIV700` | Record 65534, length 127, first byte produces SSNUM 16, SSIND 12, RELPTR 129. DOS 2.7 produces side 273 (small-file SSNUM 17), SSIND 28 |
| Side number equality precedes six-side range checking. Side-table track zero means absent, data-sector zero is legal | [4040 SSTEST](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/sstest), [DOS 2.7 SSTEST](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/sstest), `ST50/ST20` | Bounded lookup and logical absence, not SSEND's last-record fallback |
| 8250 uses super-side pairs at `3 + 2*group`, not the leading link. RDSS1 does not validate the identifier byte | [DOS 2.7 system](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/system), `BIGREL/RDSS1/RDSSS` | Borrowed-page super/group/side traversal, including nonstandard identifier and missing groups |
| FNDRCH runs before TYPFIL. Raw SA >18 is masked to four bits and SA15 maps to internal SA16 | Both `record` sources above and [DOS 2.7 tst2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2), `FNDRCH/TYPFIL` | Binary command collection, unavailable/writer/type rejection and retained peer cursors |

Native leaf execution compared production C against the already ROM-verified
assembled mappings: 1,552 arithmetic vectors and all 256 raw secondary bytes per
model (4,656 arithmetic comparisons and 768 decoder/type-error comparisons).
This exposed the 4040 divider quirk rather than normalizing it away.
Evidence is `.cache/ai/m5-rel-rom/result.json`, with private mapped-source/ROM
identities and an arithmetic-only probe. The RECORD leaf starts after CMDSET
and stops at CMDERR, so it does not prove complete command parsing or status
formatting. The model-dependent indexing layout is corroborated by
[VICE vdrive-rel](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/vdrive/vdrive-rel.c)
and the shared-ROM hardware distinction by
[MAME c8050](https://github.com/mamedev/mame/blob/master/src/devices/bus/ieee488/c8050.cpp).
These secondary sources do not replace the approved ROM arithmetic.

`dos_rel_test.c` covers fixed address boundaries, native byte wrapping, both
slots/all units, absent group/side/data pointers, invalid geometry, partial
storage failures, replacement before/between reads, terminal-call idempotence,
mid-lookup cancellation, restart and peer isolation. Three model cases in
`NativeRecordRejectsBinaryRequestsAndPreservesCursors` exercise 66 binary
requests through production firmware, physical SPI and the FPGA model,
checking exact status CR/EOI and paused local/peer reads. This is simulator
evidence, not a board REL result.

Validation: the production `fw` build, Debug `fw-test`/`sys-test` builds,
complete `firmware_tests` runner, 60 focused REL indexing cases and the three
new SPI/FPGA cases pass. The full `ctest --preset fw` attempt also ran
`display_tests`, which has seven failures in untouched terminal rendering.
Those are outside this REL change. Logs are
`.cache/ai/m5-{fw-build,fw-core,index-targeted,system-build,system-record}.log`
and `.cache/ai/m5-fw-tests.log` for the earlier full attempt.

Still required: native three-buffer REL channel allocation and readiness,
SSEND/POSITN channel integration and malformed-index behavior, record-trimmed EOI, writes and
overflow, creation/growth, side/super-side updates, CLOSE and storage-fault
retention, REL-dependent SCRATCH/COPY/VALIDATE, complete differential media
effects and the operator workflows above. No M5 acceptance is claimed.
The deployed M4 run remains on its frozen package/firmware. This work does not
flash the board or replace `build/m4-gaps.zip` and its packaged PRGs.

#### M5 positioned-record preparation (internal, not REL OPEN)

The next internal slice adds `dos_rel_record_*` in the same REL helper module.
It combines the existing index resolver with cooperative current/prefetch reads,
native low-byte NR/LSTCHR calculation, trailing-zero scanning and non-consuming
byte/EOI preview. Only an actual GET advances the cursor. A complete record
returns END. The read-only continuation is described below, while channel-level
counters and flags remain with its owner.

The cursor is bounded to 48 bytes including the index work. It borrows the
owner's existing side/data/data pages and reads at most one sector per step.
The two data pages are separate pointers into noncontiguous native pool entries,
not a private contiguous staging area. It has no new buffer pool or
file-length-proportional allocation. Partial reads
never become resident data. The captured media token is checked before cached
preview and GET, not only sector reads. Cancellation/restart does not flush,
release pages or mutate another cursor.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| Position to a prepared record | 4040 rev2 and 8050/8250 DOS 2.7 | Pinned [4040 record](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/record) and [DOS 2.7 record](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/record), `POSITN/P2/P30`, plus [rel1](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel1), `NXOUT/NXTR50` | Compute NR using byte-wide carry and two link bytes. A record ending exactly at the terminal page boundary remains readable, with NR subsequently zero | `dos_rel_record_start`, `record_prepare`, `test_record_read_preparation_and_actual_get` | Approved native POSITN/NXOUT leaf execution |
| Trailing-zero and blank-record EOI | Same selected ROMs | Pinned [4040 rel4](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel4) and [DOS 2.7 rel4](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel4), `SETLST/FNDLST` | Scan backward over the native one/two-page window. Trim trailing zero bytes, retain interior zeros, and fall back to the current pointer if the scan finds none | `record_find_last`, `record_prepare`, fixed blank/interior-zero/cross-page cases | VICE `vdrive_rel_position_internal` corroborates trimming, but its position-overflow heuristics are not the revision authority |
| Native pointer-alias quirks | Same selected ROMs | Same `SETLST`, plus [4040 rel3](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel3) and [DOS 2.7 rel3](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel3), `RD15/RD25/RD30` | Compare native low-byte pointers, not widened logical offsets. For length 255 at offset 2, a nonzero following-page byte 2 can mark the current first byte EOI. P beyond trimmed data can read through the next page until the low offset matches | `dos_rel_record_peek/get`, fixed long-record and positioned-past-trim cases | Approved SETLST/RDREL execution supplies the exact behavior rather than generic REL expectations |
| Truncated record after an existing index | Same selected ROMs | `rel1:NXOUT/NXTR45/NXTR40`, `record:R50`, `rel3:RD05` in the pinned sources above | If the record crosses a terminal data page without landing exactly at its end, prepare repeatable CR/EOI and indicate missing record. An absent index is different and still requires SSEND | `DOS_REL_RECORD_MISSING` versus `NO_INDEX`, repeated CR/EOI and explicit-incomplete-boundary cases | Native POSITN followed by the R50/RD05 error leaf, including native error 50 |
| Failed prefetch is deferred | Same selected ROMs | Pinned [4040 tst2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2) and [DOS 2.7 tst2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2), `STRDBL/STR1/DBLBUF/DBL2`, `rel1:NRBUF` | STRDBL submits following-page I/O without waiting. Observe a prefetch failure only when SETLST/GET needs that page. Unused failed prefetch does not discard the valid current record | `following_failed`, `test_record_defers_unused_prefetch_failure`, partial-read and end-scan failures | Source-level job/wait ordering. Host callbacks are synchronous and cooperative, not emulated FDC timing |
| Cached-byte lifetime and actual GET | Host scheduling/lifetime adaptation around those native routines | Native byte preparation above, with generation-guarded `dos_media_reference/read` | Preview is repeatable. A replaced medium cannot supply cached bytes or accept a stale GET. Peer-unit cursor and data remain unchanged | `record_current`, cached-generation, interleaved-unit and cancellation tests | Existing serialized media ownership contract, not a new native DOS error mapping |

The native record probe executes the approved mapped ROMs with two explicit
resident pages. It enters POSITN after POSBUF, executes NXOUT/SETLST/FNDLST
and RDREL, models only NRBUF's resident-page switch and stops before NXTREC.
For LRF it follows the R50/RD05 error path. Thus it verifies prepared-byte
semantics, not controller jobs, dirty-buffer replacement, complete channel
flags, SSEND, next-record state or physical timing.

All 2,160 positioned-record vectors pass (720 per model), comparing 104,573
byte/EOI pairs and their actual-GET handoffs. The corpus includes record lengths
1/2/10/127/128/200/254/255, zero/first/interior/end/overflow positions,
zero/dense/sparse pages and terminal/linked data. Local evidence is
`.cache/ai/m5-record-rom/result.json`, with exact approved ROM, symbols, probe
and production-source hashes. The earlier arithmetic/decoder probe also passes
against the expanded module.

The REL suite now passes 138 cases, including all 60 indexing cases and 78
record-preparation cases. The complete firmware core and production firmware/
Debug system builds pass. The full firmware preset still reports the seven
existing terminal-rendering failures in `display_tests` (untouched by M5).
Logs are `.cache/ai/m5-record-{targeted,fw-core,fw-tests,fw-build,system-build}.log`.

This remains internal groundwork. No public REL OPEN/read/write path is newly
enabled, so this slice does not claim production SPI/FPGA or board acceptance
of REL files. At NO_INDEX the channel must integrate the SSEND resolver below
before publishing a native reply. At END it can use the read-only continuation
below. NEEDS_LINK still requires unbuffered native NRBUF handling. Native
three-buffer allocation, revision-specific
resident/dirty-buffer reuse, complete channel flags and status publication,
writes, growth, CLOSE, REL-dependent commands and acceptance remain required.
The running M4 package and deployed board are unchanged.

#### M5 last-index fallback (internal, not REL OPEN)

`dos_rel_end_*` now resolves SSEND's last side-sector and byte-wide endpoint
from a clean directory side/super-side root. It borrows one existing page,
retains at most 32 bytes of cursor state, and performs at most one sector read
per call (two reads for small REL, three for 8250). FOUND identifies a side
page and its native group/side/index, not a prepared record or a data-sector
coordinate. Subsequent I/O must use the captured media token.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| Last side selection | 4040 rev2 and 8050/8250 DOS 2.7 | Pinned [4040 ssend](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ssend) and [DOS 2.7 ssend](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ssend), `SSEND/SE10/SE20` | Search all six side-table track bytes backward. Equality with the resident header number skips the read even when the table coordinates differ. Do not follow the side's leading link to find the end | `dos_rel_end_step`, `test_end_selects_native_last_side_and_group` | Native SSEND execution. [VICE vdrive-rel](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/vdrive/vdrive-rel.c) corroborates side/super-side layout and use of terminal link length, not these revision-specific search rules |
| Last group selection | 8250 shared DOS 2.7 in double-sided hardware mode | Pinned [system](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/system), `BIGREL/RDLG/RDSSS/RDSS1C` | Scan groups 90 down to zero at `3 + 2*group`, including holes. Group 91 is not searched even though FNDREL arithmetic can produce it. Neither the leading link nor identifier byte replaces the group-table lookup | `DOS_REL_END_GROUP_START`, group 0/1/45/89/90 cases with distracting group 91 | Native RDLG execution. [MAME c8050](https://github.com/mamedev/mame/blob/master/src/devices/bus/ieee488/c8050.cpp) independently identifies the shared `901887-01/901888-01` main ROM and hardware-mode distinction |
| Endpoint and structural errors | Same selected ROMs | Same `ssend`, `SE30/BREAK` | Nonzero leading link in the selected last side or an empty six-side table reaches native error $67. Otherwise SSIND is byte 1 minus one, including byte wrap. Do not validate the loaded side's header number or impose endpoint parity here | `DOS_REL_END_FOUND/CORRUPT`, endpoint 0/1/2/16/17/254/255 native vectors and corrupt-index tests | Approved ROM comparison checks SSNUM, SSIND, BUFTAB and native $67 at CMDER2 |
| Empty 8250 super table | 8250 DOS 2.7 | Same `system`, `RDLG1/RDSS1C` | Native GRPNUM underflows to $FF and continues byte-indexed traversal outside the supported group range. Stop before that traversal with NO_GROUP. Do not invent status 50 or 67 | `DOS_REL_END_NO_GROUP`, empty table and group-91-only tests | Native execution observes the underflow boundary. Its continuation remains unimplemented |
| Cooperative lifetime and failure handling | Host scheduling adaptation around those routines | Native index selection above, generation-guarded `dos_media_read` | Reject failed/partial reads and stale jobs before exposing an endpoint. Check media at each read. Cancellation changes no borrowed page or peer cursor. Terminal calls perform no I/O | Storage failures, removal/replacement at every read phase, geometry, cancellation/restart and peer-generation tests | Existing serialized media ownership contract, not an emulated FDC error mapping |

The private native probe executes SSEND and the 8250 RDLG/RDSSS/RDSS1 path
against the approved mappings, substituting only successful controller page
reads at IBRD/DOJOB. All 1,775 vectors pass: 882 selected endpoints, 891 native
BREAK/$67 results and two explicit group-underflow handoffs. This includes
sparse tables, all six side slots, native byte-wrap endpoints, mismatching
loaded side headers and distracting super metadata. It does not prove FDC
jobs/timing, dirty or resident native buffer reuse, public status formatting,
or the malformed underflow continuation. VICE's forward contiguous-group
search and larger in-memory index are not used as the authority for RDLG.
Evidence is `.cache/ai/m5-end-rom/result.json`, with exact approved ROM,
symbol, production-source and probe hashes.

The focused REL suite passes 203 cases (65 new last-index cases), and the
complete `firmware_tests` runner passes. Production firmware and Debug system
builds pass. The full firmware preset still has the same seven existing
`display_tests` terminal-rendering failures. Logs are
`.cache/ai/m5-end-{targeted,fw-tests,fw-build,system-build}.log`.

The RECORD owner still needs to combine this resolver with NO_INDEX handling,
native LRF/status publication and buffer ownership before REL can be enabled.
Native next-record channel integration, writes/growth, CLOSE/fault retention,
REL-dependent commands and acceptance remain incomplete. No public REL path,
M4 package, deployed firmware or SD-card image was changed by this slice.

#### M5 read-only next-record sequencing (internal, not REL OPEN)

`dos_rel_record_next` now continues a consumed record at END, using native NR
and the current low-byte pointer rather than recomputing a logical file offset.
It performs no I/O. PENDING schedules reads through `dos_rel_record_step`,
reusing only consumed data pages. The original two independently supplied
data pages can alternate across multiple linked sectors without rereading
the side index. The cursor remains bounded to 48 bytes and adds no pool,
heap allocation or file-length-proportional state.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| EOI-to-next-record handoff | 4040 rev2 and 8050/8250 DOS 2.7 | Pinned [4040 rel1](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel1) and [DOS 2.7 rel1](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel1), `NXTREC/NXTR15/NXTR20/NXTR45`, plus both `rel3:RD40` | Compare the current low-byte pointer to NR before choosing the current/following page. NR zero prepares sticky CR/EOI missing-record state. Otherwise start at NR and calculate the next end using full RS, not the previous P byte offset | `dos_rel_record_next`, dense/trimmed/blank multi-record streams | Approved native execution. [VICE vdrive-rel](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/vdrive/vdrive-rel.c) corroborates record-boundary advance and zero trimming, not the selected ROM's low-byte aliases |
| Clean read-buffer switching | Same selected ROMs | Both pinned `rel1:NRBUF/NRBU50/NRBU70`, plus [4040 tst2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2) and [DOS 2.7 tst2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2), `DBLBUF/DBL2`, `rel4:SETLST` | Alternate the existing current/following pages, refill only a consumed page, and select the correct physical page for SETLST/GET after either direction of a switch. An unused failed read-ahead remains deferred, including across multiple record ends | Generalized `dos_rel_record_step/get`, nonadjacent `dos_buffer_t` stream and deferred-refill failure cases | Native NRBUF and DBLBUF execute, with only successful controller READ completion substituted. [MAME c8050](https://github.com/mamedev/mame/blob/master/src/devices/bus/ieee488/c8050.cpp) corroborates the shared ROM/hardware-mode selection |
| Counter and flag ownership | Same selected ROMs | Both pinned `rel1:NXTREC`, `rel3:RDREL/RD05` | Native NXTREC increments RECL/RECH before checking NR zero, but not when LRF was already set. 4040 clears GETFLG plus OVRFLO, whereas DOS 2.7 clears OVRFLO. These channel fields and exact status publication remain with the future owner, not this byte cursor | Explicit END-only API, sticky MISSING, documented integration boundary | Selected ROMs establish the distinction. The byte-sequence proof does not claim C counter/flag or public status parity |
| Deferred failure and lifetime | Host cooperative-read adaptation around those routines | Generation-guarded media reads and cached-token checks, with the native buffer-selection boundary above | Keep usable current records after a failed initial/recycled-page prefetch. Fail when a later NRBUF selection or next-record end scan needs that failed page, without retrying it or exposing partial bytes. Replaced/removed media cannot advance cached END or fulfill pending refills. Cancel/restart preserves shared pages and peer cursors | `test_record_next_retains_failed_prefetch`, generation and cancellation/peer cases | Host failure callbacks are not an FDC error/timing model |

The new private probe runs actual RDREL/NXTREC/NRBUF, ADDNR/NXOUT/SETLST,
DBLBUF and job-wait code against the approved mapped ROMs. It supplies four
clean data pages and substitutes only sector transfer plus immediate successful
READ job completion. All 1,944 sequences pass (648 per model), comparing
354,431 data/EOI pairs, 16,390 EOI-to-next handoffs and native low-byte/page
state. The corpus issues 716 native READ jobs and covers one/two/four-page
chains, record sizes 1/2/10/127/128/200/254/255, partial P positions, dense,
zero and sparse content, missing records and up to 16 record advances per case.
It stops at 51 unbuffered terminal-NRBUF handoffs rather than treating stale
inactive data as a supported result.

This proves read-only byte sequencing in that clean-buffer envelope, not
controller timing, storage-error status mapping, dirty-buffer replacement,
channel counters/flags, public FIFO integration or unbuffered terminal NRBUF.
Host page refills are cooperative and may occur after the explicit END
handoff rather than at the native controller's speculative submission instant.
The source/probe/approved-ROM identities and counts are retained in
`.cache/ai/m5-next-rom/result.json`. All previous arithmetic/decoder,
positioned-record and SSEND probes also pass against the expanded module.

The REL suite passes 236 cases, including 33 new sequence/fault/lifetime cases.
The complete `firmware_tests` runner, production firmware and Debug system
builds pass. The full firmware preset still reports the seven existing
terminal-rendering failures in untouched `display_tests`. Logs are
`.cache/ai/m5-next-{targeted,fw-core,fw-tests,fw-build,system-build}.log`.

Public REL OPEN/read/write remains disabled. Native allocation, POSBUF
resident/dirty-buffer ownership, complete channel counters/flags and status,
unbuffered NRBUF and malformed-index continuation, writes/growth, CLOSE and
fault retention, REL-dependent commands and end-to-end acceptance remain
required. The frozen M4 package, deployed board and SD-card images are unchanged.

#### M5 speculative read staging and actual-GET bookkeeping (internal)

`dos_rel_reader_*` adds a bounded adapter around the existing record cursor,
not public REL channel support. Its state is at most 52 bytes, including the
48-byte cursor, the raw RECORD counter and two GET bookkeeping flags. It
borrows the same independent data pages and owns no FIFO copies, queue, page
pool or proportional index. It has not been added to every production channel
or charged against the final per-unit allocation budget.

Staging copies only the cursor onto the stack. It performs no sector I/O,
does not modify borrowed pages and stops at the existing DOS staging capacity,
EOI or an unprepared page. Repeated staging reconstructs the same unconsumed
suffix. Only `dos_rel_reader_get` commits one actual GET. At EOI it advances
the raw counter and begins next-record preparation, with PENDING completed
through `dos_rel_record_step`. The future channel owner must retire staged
copies before repositioning, changing ownership or reusing borrowed pages.
The existing transport remains responsible for captured FIFO ownership and
the count of bytes actually consumed.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| Prepared bytes versus actual GET | 4040 rev2 and 8050/8250 DOS 2.7 | Pinned [4040 ieee](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ieee) and [DOS 2.7 ieee](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ieee), `TLK10/TLK20/TLK30`, plus both `tst4:GET` | Present CHNDAT/CHNRDY before GET. A speculative or discarded FIFO suffix must not increment the record, run NXTREC, alter GETFLG or publish a missing-record error | `dos_rel_reader_stage`, repeated/partial prefix tests, cross-page reads through noncontiguous native pool entries | Approved native execution compares each consumed prefix. The FIFO-copy purity rule is a host adaptation, not a second native queue |
| Raw counter and model-specific GET flag | Same selected ROMs | Pinned [4040 record](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/record) and [DOS 2.7 record](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/record), `R30/R40`, plus both `rel1:NXTREC` and `rel3:RD10/RD40` | Retain P's raw RECL/RECH, including zero. A consumed EOI increments modulo 65536 before next-record preparation. 4040 sets GETFLG on an ordinary GET and clears it in NXTREC. DOS 2.7 has no read GETFLG operation | `dos_rel_reader_start/get`, raw-zero/wrap and cooperative-refill tests | Native RECL/RECH and 4040 FILTYP.GETFLG compared after every GET. [MAME c8050](https://github.com/mamedev/mame/blob/master/src/devices/bus/ieee488/c8050.cpp) independently identifies the shared DOS 2.7 ROMs and hardware modes |
| Deferred missing-record error | Same selected ROMs | Pinned [4040 rel3](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel3) and [DOS 2.7 rel3](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel3), `RDREL/RD05/RD40`, plus both `rel1:NXTREC` | A last-good-byte GET may prepare missing CR/EOI without CMDERR 50. Only the next actual GET of that already-missing CR invokes CMDERR 50. Repeated missing GETs report it again without incrementing RECL/RECH | `no_record` is a last-GET publication event for the future status owner, not a preview effect. Missing/repeated-GET/reposition tests | Native proof stops at CMDERR and checks A=$50. [VICE vdrive-rel](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/vdrive/vdrive-rel.c), `vdrive_rel_read`, corroborates CR/error 50 on a read past the last record, not exact 4040 flags or byte-wide aliases |
| Pending work, deferred faults and media lifetime | Host cooperative-read adaptation around those routines | Both pinned `rel1:NRBUF/NXTREC`, `rel3:RDREL` and existing generation-guarded media contract | Preview cannot expose a partial failed page or poison the real cursor. GET reaches the deferred error only after consuming the usable prefix. EOI counter/flag changes precede pending work and later I/O failure. Replaced/removed media cannot commit cached counters or signal a missing-record error | Failed-page preview, pending-refill/error counter and stale preview/GET cases | Host callbacks test scheduling and lifetime only, not native FDC error mapping or physical timing |

The private native probe passes 2,673 sequences (891 per model), comparing
485,820 actual GET/data/EOI results, raw counters and GET flags. It checks
129,756 previews, 10,874 discarded tails, 23,285 EOI-to-next-record handoffs
and 4,275 actual missing-record error events. The envelope includes P0,
P1/P2/P3, raw-counter wrap at 65535 with record size one, partial positions,
dense/sparse/zero content and one/two/four clean data pages. Actual native
NRBUF/DBLBUF/job-wait code issues 960 READ jobs, with only successful sector
transfer and immediate completion substituted. It stops at 66 unbuffered
terminal-NRBUF handoffs. Identities and model-specific counts are retained in
`.cache/ai/m5-reader-rom/result.json`. All four earlier native probes pass
against the same expanded source.

The REL suite passes 269 cases, including 33 new staging/GET/fault cases.
The complete `firmware_tests` runner, production firmware and Debug system
builds pass. The full firmware preset still has the seven known failures in
untouched `display_tests`. Logs are
`.cache/ai/m5-reader-{targeted,fw-tests,fw-core,fw-core-final,fw-build,system-build}.log`.

This is not complete FILTYP/OVRFLO, RECORD command/status, POSBUF or public
FIFO integration. Three-buffer allocation and the final per-unit state
budget, dirty ownership, malformed/unbuffered continuation, writes/growth,
CLOSE and fault retention, REL-dependent commands and end-to-end acceptance
remain incomplete. Public REL OPEN/read/write remains gated. The frozen M4
archive/PRGs, deployed firmware and SD-card images are unchanged.

#### M5 bounded channel ownership (internal, public REL still gated)

The Debug host unit was already 4,604 bytes against its 4,608-byte bound.
Adding a separate 52-byte reader to every channel would exceed that limit.
The internal lookup/record state tags now use byte storage while retaining
their named enum values and public function result types. This reduces Debug
host lookup, record and reader state to 24, 36 and 40 bytes respectively. Channel metadata
now overlays the REL reader with mutually exclusive ordinary-file metadata,
selected by `file_type`. Media identity, pool indices, routing, readiness,
access, directory/close state and failure bookkeeping remain outside the
union. Ordinary enum fields retain their existing types.

Debug host retains 72-byte channels and the 4,604-byte unit. The production ARM
ELF has 64-byte channels and a 4,532-byte unit, with 20/32/36-byte
lookup/record/reader state under its target layout. Both include the existing
twelve data pages and two BAM caches.
There is no new per-unit pool, heap allocation or whole-file cache. The existing
per-unit compile-time bound and tighter cursor bounds remain enforced.

`dos_rel_channel_claim` reserves one logical channel and three existing shared
pages, initializing a non-ready, read-only REL owner. It does not open a file,
read media, prepare records or publish command status. The caller must retire
any previous owner of the secondary first. Allocation retains the existing
host policy of publishing a channel only after every requested page is claimed.
Exhaustion returns `DOS_UNUSED`, releasing partial page claims without
disturbing peers or the directory's internal logical reservation. Existing
cleanup returns all three pages. This is clean resource teardown, not complete
native REL CLOSE or dirty-buffer retention.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| REL data/data/side ownership | 4040 rev2 and 8050/8250 DOS 2.7 | Pinned [4040 opchnl](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/opchnl) and [DOS 2.7 opchnl](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/opchnl), `OPNRCH/OR10/OR20`, plus both `tst3:GETRCH/GETBUF` | Two GETRCH pages plus a third GETBUF page held in SS. Four clean REL owners exhaust twelve shared pages even with logical indices still available | Generalized `allocate_channel`, `dos_rel_channel_claim`, full-pool and mixed ordinary/REL tests | Actual approved-ROM GETRCH/GETBUF and OR20's SS store execute. [MAME c8050](https://github.com/mamedev/mame/blob/master/src/devices/bus/ieee488/c8050.cpp) independently identifies the shared DOS 2.7 ROM/hardware modes |
| Selection, drive residue and release | Same selected ROMs | Pinned [4040 tst3](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst3) and [DOS 2.7 tst3](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst3), `FNDLNX/GETBUF/GBERR/RELBUF/FRECHN` | Lowest available ordinary logical index, highest free pages, native last-job drive bits, and release of BUF0/BUF1/SS. Exhaustion releases partial page ownership | Resource-failure, reserved-index, peer-preservation, three-page cleanup and reuse tests | Native execution compares chosen logical/page indices, page bitmaps, LSTJOB drive bits and successful FRECHN release. It stops before CMDERR formatting/global cleanup |
| Reader state over actual owned pages | Same selected ROM read-only envelope | Both pinned `rel1:NXTREC/NRBUF`, `rel3:RDREL/SETLST` and the allocation routines above | Existing positioned-reader byte/EOI/counter behavior remains unchanged when its state resides in the actual channel and its three pages come from the real unit pool | `test_reader_claimed_channel_pool_and_teardown`, all earlier REL tests/probes | All five earlier native proofs pass after tag compaction. No dirty POSBUF or public FIFO integration claim |
| Bounded host representation | EconoPET adaptation, not a native ABI | Twelve-buffer/six-file-channel resource limits established by the selected ROMs | Overlay mutually exclusive state without adding pages or exceeding the existing unit budget. Ordinary metadata, commands, streams and peer cursors retain their behavior | Existing unit/cursor static bounds, complete firmware core and three production SPI/FPGA RECORD regression cases | Compiled host/ARM sizes and production-path tests, not a new controller or physical-timing model |

The new private allocation probe passes 12,288 vectors (every twelve-page
availability bitmap in each selected model), with varied logical ownership,
directory reservations and both drives. It compares 11,772 successful
three-page claims/releases and 516 native resource-error exits. Only the
allocation/SS-store/release routines execute. Native STRDBL, side reads,
POSBUF, complete OPEN, native CMDERR cleanup/status formatting and controller
timing are outside this proof. In particular, native GETRCH publishes transient
logical ownership before its resource-error handler, whereas the C host
adapter retains its existing atomic-publication policy. Evidence and source
identities are in `.cache/ai/m5-channel-rom/result.json`.

The focused DOS suite passes 1,969 cases and REL passes 272, including 24 new
resource/pool-integration cases. The complete `firmware_tests` runner passes,
as do production firmware and Debug system builds. All three model variants
of `NativeRecordRejectsBinaryRequestsAndPreservesCursors` pass through the real
firmware/SPI/FPGA path. The full firmware preset still reports the seven known
untouched `display_tests` failures. Logs are
`.cache/ai/m5-channel-{dos-targeted,rel-targeted,fw-tests,fw-core,fw-full,fw-build,system-build,system-tests}.log`.
Exact production sizes are recorded in `.cache/ai/m5-channel-arm-sizes.log`.

Public REL OPEN/read/write remains gated. Actual OPEN/POSBUF preparation,
complete RECORD/status/FIFO wiring, persistent REL metadata, malformed and
unbuffered paths, dirty ownership, writes/growth, CLOSE/fault retention,
REL-dependent commands and end-to-end acceptance remain required. No deployed
firmware or SD-card content was changed, and the frozen M4 archive/PRG hashes
remain unchanged.

#### M5 cooperative clean-read pipeline (internal, public REL still gated)

An internally claimed REL channel can now enter positioned preparation through
`dos_rel_channel_prepare`, supplying validated directory side/super-root
coordinates, raw record number, nonzero record size and position. This is not
directory lookup, public OPEN or a complete RECORD command. The caller must
retire that channel's staged copies before restarting it. Successful preparation
preserves retained status instead of inventing an OPEN/P completion response.

The actual DOS work/output path now services these readers. Pending work stays
in each channel's existing record cursor, not the unit's tagged command-work
union. `dos_work_pending` includes those cursors, and `dos_work` advances one
after earlier ordered command/initialize/OPEN work. Each REL step performs at
most one media read. Preview and actual GET dispatch by `file_type`, using the
three pages already owned by that channel. A consumed EOI may schedule a
refill without corrupting another command's state. Closing/resetting the
channel cancels that work by retiring its owner, so no old job can read into
pages subsequently assigned to a new channel.

The captured model uses an existing channel-padding byte, allowing read/lifetime
errors to retain the selected model's status layout even after removing its
last mounted image. Compiled sizes remain 4,604/72 bytes per unit/channel in
Debug host tests and 4,532/64 bytes in production ARM. There is no new queue,
private page, per-unit cache or independent pending flag.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| Cooperative prepared-read ownership | 4040 rev2 and 8050/8250 DOS 2.7 clean-buffer envelope | Pinned [4040 record](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/record) and [DOS 2.7 record](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/record), `POSITN`, plus both `rel1:NXTREC/NRBUF`, `rel3:SETLST` | Prepare the next byte before making the channel ready. EOI counter/flag progress precedes refill completion. Host yielding must preserve independent channel pages and ordered command ownership | `dos_rel_channel_prepare`, `dos_work_pending`, `rel_read_work`, pending-refill/command-union and close/reuse tests | Actual native reader corpus through the real DOS scheduler. Cooperative yielding is an explicit host adaptation, not FDC timing |
| Captured-source preview and actual GET | Same selected ROMs | Both pinned `ieee:TLK10/TLK30`, `tst4:GET`, `rel3:RDREL/RD40` | Staging never advances records, flags, pages or retained status. Consume only the source captured for that staged prefix, even after another source is selected. Stop staging at EOI or an unprepared page | REL dispatch in `dos_stage/dos_get`, actual `dos_output_stage/get`, interrupted-prefix/status-source and peer/reposition cases | 2,673 approved-ROM sequences compare actual DOS outputs and native channel state. Existing SPI/FPGA RECORD regression cases remain passing |
| Exact missing-record publication | Same selected ROMs | Both pinned `rel3:RD05`, [4040 erproc](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/erproc) `MOVERR`, [DOS 2.7 erproc](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/erproc) `ERMOVE`, and both `parsex:CMDERR` (DOS 2.7 `JCMDER`) | GET of an already-missing CR publishes `50, RECORD NOT PRESENT,00,00` plus DOS 2.7's drive and terminal CR. Preserve the leading space. CMDERR zeroes track/sector. Last-good EOI, pending work and missing-byte preview do not publish 50. Repeated missing GET restarts status without advancing the raw counter | `DOS_STATUS_NO_RECORD`, actual GET publication, retained status-position and both-drive cases | Native MOVERR/ERMOVE execute to establish message bytes. Native RDREL supplies the actual error-50 event. [VICE vdrive-rel](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/vdrive/vdrive-rel.c) corroborates CR/error 50 past the last record |
| Deferred read failures and unsupported boundaries | Host adaptation around the selected native routines | Native buffer-selection boundary above, existing generation-guarded media contract | Keep current bytes after unused failed prefetch. Retire when actual GET/next-record preparation needs the failed page, without retry or error 50. Replaced/removed media cannot commit cached counters. Explicit NO_INDEX/NEEDS_LINK boundaries remain policy syntax 30, not fabricated native missing-record results | Initial/deferred I/O, pending/cached/GET removal, explicit boundary and peer isolation cases | Generic host read/lifetime failures retain code 23 with zero coordinates. This is not native FDC error mapping or complete CMDERR cleanup |

The private pipeline probe runs the same 2,673 native sequences through actual
DOS preparation, cooperative work, staging and captured-source GET. All
485,820 GET/data/EOI comparisons pass, including raw counters, 4040 GETFLG,
129,756 pure previews, 10,874 discarded tails, 23,285 EOI handoffs and 4,275
missing-record error events. It additionally compares error-50 text against
native token expansion, including its leading space. The native four-clean-page
envelope, 960 successful READ jobs and 66 explicit unbuffered handoffs remain
unchanged. Evidence is `.cache/ai/m5-pipeline-rom/result.json`. The 12,288-vector
allocation proof was also rerun against the updated DOS module.

The REL suite passes 305 cases, including 33 new pipeline cases. The complete
`firmware_tests` runner, production firmware, Debug system build and all three
production SPI/FPGA RECORD regressions pass. The full firmware preset still
reports the seven known failures in untouched `display_tests`. Logs and size
evidence use `.cache/ai/m5-pipeline-*`.

Public REL OPEN and complete binary P handling remain gated. Persistent
directory metadata, dirty POSBUF reuse, SSEND/unbuffered and malformed
continuations, writes/growth, native failure cleanup, complete CLOSE,
REL-dependent commands and end-to-end/physical acceptance remain required.
The new internal entrypoint is not permission to feed arbitrary live REL
channels through those unfinished mutating paths. No deployed firmware or SD
contents changed. Frozen M4 archive/PRG hashes remain unchanged.

#### M5 cached existing-record write leaves (internal, no writeback yet)

`dos_rel_record_put` and `dos_rel_record_pad` implement only the existing-record,
in-page `RELPUT` and `CLREC` leaves. They borrow the same independent data pages
and OR physical-page bits into a caller-owned dirty mask. No channel field,
private page, media write, allocation or file-proportional index is added.
Host unit/channel/record/reader sizes remain 4,604/72/36/40 bytes. Production
ARM sizes remain 4,532/64/32/36 bytes.

The owner must retire read readiness and all staged FIFO copies before handing
over a prepared record. An accepted write enters `WRITING`, disabling the
read-preview/GET/next-record helpers so stale read endpoints cannot expose
modified data as a prepared native read. `FULL` represents native OVRFLO, not
an error-51 publication event. A dirty page wrap returns `NEEDS_WRITEBACK`
with the just-written byte, dirty bit, original active page and pointer zero
retained. This stops **before** `NRBUF`, even if the following page is resident.
The owner must retain dirty pages/masks across failure, cancellation and
repositioning. Existing clean-reader CLOSE/reset paths are not dirty-writer
cleanup and must not be used to discard them.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| Cached byte and physical dirty page | 4040 rev2, 8050/8250 DOS 2.7 | Pinned [4040 rel2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel2) and [DOS 2.7 rel2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel2), `RELPUT/SDIRTY` and their active-buffer selection | Mark only the active data descriptor dirty, store at its current byte pointer and preserve the peer page. PUT does not rewind a preceding GET. DOS 2.7's additional FILTYP.DYFILE update is a separate owner obligation, not implemented by this leaf | `dos_rel_record_put`, dirty-mask mapping, both borrowed-page orders, preceding-GET and independent-owner tests | Actual native PUT through TYPFIL/WRTREL plus SDIRTY. Both initial GET/DYFILE flag states are traced. [VICE vdrive-rel](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/vdrive/vdrive-rel.c) `vdrive_rel_write` independently marks cached sectors dirty |
| Native byte-wide record limit | Same selected ROMs | Both pinned `rel2:RELPUT/RELP05/RELP07/RELP10`, `WRTREL/WR50/WR51` | Compare the next low-byte candidate to NR. On pointer wrap, NR=0 or NR=2 fills without advancing from byte 255. Preserve the final byte on later non-EOI input rather than overwriting it | `FULL`, native-wrap and repeated-input cases, including unused failed prefetch | Native execution compares pointer/NR/OVRFLO and both complete data pages. VICE corroborates refusing writes into the next record, but its virtual-drive immediate error 51 is not the selected native EOI-delayed status rule |
| Zero-pad only the unfinished suffix | Same selected ROMs | Both pinned `rel2:CLREC/CLR10`, `WRTREL/WR40` | Store zeros from the current pointer through the remaining in-page record bytes. An already-full record is unchanged. Stop before dirty NRBUF if the suffix crosses a page | `dos_rel_record_pad`, suffix/partial-position, full-record and writeback cases | Actual native CLREC, including direct padding and padding after a short input prefix. This does not claim WRTREL's subsequent RD40/status completion |
| Retain partial effects before unsupported work | Same selected ROMs, with host generation guards | Both pinned `rel2:RELP10/WR60`, respective `rel1:NRBUF/WRTOUT` | Store/dirty byte 255 and wrap its pointer before NRBUF. Do not select or overwrite the next page, silently grow a missing record, or claim persistence. Reject new stores to removed/replaced media without losing prior dirty bytes/masks | `NEEDS_WRITEBACK`, unchanged `MISSING`, stale first/subsequent store/pad and peer-generation tests | 240 native NRBUF and 336 ADDREL boundary stops. Cached lifetime rejection is explicit host policy, not native FDC failure parity |

The private `.cache/ai/m5-write-rom/result.json` proof executes actual approved
`tst2:PUT` through `TYPFIL/WRTREL` with non-EOI input, `RELPUT`, `CLREC` and `SDIRTY`.
Across 4,608 vectors (1,536 per model), 24,120 operations compare both complete
physical pages, dirty descriptors and valid pointer/NR state. They include
265,152 native stored bytes, 8,304 padding calls, both active buffers,
zero/dense content, raw record zero, partial positions, 4,032 full records and
4,032 ignored overflow inputs. Missing-record NR is not compared because
native POSITN leaves it unprepared before ADDREL. The top-level PUT probe sets
the native REL type bits explicitly, unlike the older direct-RDREL fixture.
No native ROM source or binary is included in the repository.

These leaves deliberately omit the rest of the write protocol. Native IEEE
`LSN30` samples active-low EOI, so `EOIFLG=0` enters WRTREL's CLREC/RD40
completion, not merely a byte-store call. Overflow error 51, model-specific
OKERR completion, DOS 2.7 DYFILE publication, native prepared-read refresh,
dirty POSBUF reuse, writeback/fault retention, growth, side/super updates and
complete CLOSE remain owner/integration work. Neither helper is wired into
public LISTEN, OPEN or binary P, and no new status-success claim is made.

The REL suite passes 356 cases, including 51 new write-leaf cases. The complete
`firmware_tests` runner, production ARM firmware and Debug system build pass.
The existing pipeline proof still passes all 2,673 native read sequences and
485,820 actual GET/data/EOI comparisons against the updated sources. The
previous seven unrelated display-test failures are not addressed or rerun by
this slice. Formatting, source identities and frozen M4 hashes are checked
separately. Logs and measured layouts use `.cache/ai/m5-write-*`.

Public REL remains gated. The frozen M4 acceptance archive/PRGs, deployed
firmware and SD images are unchanged. This is bounded cache mutation, not
persistent REL writing or completed M5 acceptance.

#### M5 internal dirty-page WRITE jobs (no NRBUF continuation yet)

`dos_rel_channel_writeback` now submits the active dirty data page through the
existing native DOS write gate. It is an internal WRTOUT job leaf, not public
LISTEN, NRBUF completion, CLOSE or a durability acknowledgement. The caller
must retire read readiness/FIFO copies, retain the dirty mask and keep the
three pages owned. The record must already be in `WRITING`, `FULL` or
`NEEDS_WRITEBACK`.

The cooperative read pipeline now records track/sector, residency, failure
and drive in the actual pooled data pages. Each completed read publishes its
own header, including reads into a reused physical page. Failed/partial reads
retain attempted coordinates but cannot publish residency. Clean repositioning
invalidates both data-page residency flags. This adds no channel field, page,
queue or cache. Debug host unit/channel/record sizes remain 4,604/72/36 bytes,
and production ARM sizes remain 4,532/64/32 bytes.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| Physical data-page job identity | 4040 rev2, 8050/8250 DOS 2.7 | Pinned [4040 tst2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2) and [DOS 2.7 tst2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2), `STRDBL/SETHDR/DBLBUF`, respective `tsutil:RDAB/SJ10` | Keep each job's saved header attached to its physical data page across GET swaps and refill reuse. Do not infer the current page's location from a side-index cursor or its next-sector link. Failed data must not be usable as a completed page | `rel_read_work`, clean prepare invalidation, header-reuse and failed current/prefetch tests | Actual native reads followed by 864 active-header comparisons at WRITE submission. Host header publication is synchronous/cooperative, not native prefetch timing |
| Saved-header dirty WRITE | Same selected ROMs | Pinned [4040 tsutil](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tsutil) and [DOS 2.7 tsutil](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tsutil), `WRTOUT/SJ20/SJ30`, both `jobs:SETLJB/SETJOB` | Submit the active page's complete 256 bytes to its saved track/sector and drive. Do not switch pages, change pointer/NR/counter, consume the peer dirty bit or publish a successful command completion | `dos_rel_channel_writeback`, both active pages and WRITING/FULL/NEEDS_WRITEBACK cases, retained partial status and clean-repeat cases | 576 approved-ROM WRITE jobs compare target drive/header and all 147,456 payload bytes. [VICE vdrive-rel](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/vdrive/vdrive-rel.c) `vdrive_rel_commit` independently writes the saved current-page coordinates |
| Existing cached-BAM version gate | Same selected ROMs | Pinned [4040 jobs](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/jobs) and [DOS 2.7 jobs](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/jobs), `SJB2/SJB3/SJB4/VNERR/HED2TS` | Use byte 2 of the selected drive's resident BAM. Permit zero or the native version, reject foreign versions with 73 and the saved job coordinates before issuing a data write | Reused `write_sector/write_version`, both-drive version rejection and correction/retry tests | 288 native VNERR stops. The probe initializes NUMSEC/TRKNUM from the approved ROM tables so the real WRITE geometry/version gate executes without a stub |
| Failure retention and separate persistence | Native job boundary plus explicit host storage policy | Both `tsutil:SJ20` calling `rel2:CDIRTY`, and `jobs:SETJOB`, existing generation-bound media contract | Native clears descriptor dirty before job submission. Host instead clears the caller's active dirty bit only after successful callback consumption. Keep bytes, mask and record on gate/partial-write failure for explicit retry. Reject stale media before BAM/callback access. Callback success is not a flush or CLOSE | Partial failure/retry, read-only, cached-version, removed/replacement, flush-failure and peer-status cases | Host policy deliberately differs from native CDIRTY timing. Media-bank dirty survives failed callbacks/flushes. A peer's successful write preserves the failed writer and retained error |

The existing write gate may perform its usual lazy BAM I/O before the one
data-page write. This entrypoint is not a new cooperative one-sector scheduler.
An already-clean active page performs no I/O. Successful jobs preserve the
prior status and selected drive. Failures retain owned cache bytes/masks and
mark only this channel failed, with no automatic retry or record progress.
Removal retains the captured model's error layout even when the unit has no
mounted image. A retry that succeeds does not itself clear retained status.

The private `.cache/ai/m5-flush-rom/result.json` proof passes 864 vectors
(288 per model), including 78,588 actual GET/data/EOI comparisons before
writing, both drives, both active pages, page reuse, zero/dense records, raw
record zero/65535, partial positions and zero/native/foreign cached BAM
versions. Zero-filled record prefixes are deliberately shorter because native
SETLST trims them to early EOI. Actual native WRTOUT/SETLJB/SETJOB execute to
successful WRITE submission or VNERR, with ROM-derived geometry tables and
immediate successful synthetic controller completion.

The existing bounded host reader defers some inactive-page prefetches until
record continuation. Accordingly, this proof compares the **active WRITE
target**, not the native inactive buffer's earlier speculative READ header.
It does not establish inactive-prefetch timing, mutable-peer observation
parity, native controller faults, dirty POSBUF reuse or complete NRBUF.

The REL suite passes 404 checks, including 48 new header/write-job cases.
The complete `firmware_tests` runner, production ARM firmware, Debug system
build and all three production SPI/FPGA RECORD regressions pass. The existing
2,673-sequence native pipeline proof still passes all 485,820 actual
GET/data/EOI comparisons against the updated DOS sources. The previously
recorded seven unrelated display-test failures are outside this slice and
were not rerun. Logs and measured layouts use `.cache/ai/m5-flush-*`.

Public REL OPEN and complete binary P remain gated. Buffer switching after a
dirty write, file-flag/status completion, growth, side/super updates, complete
CLOSE/fault cleanup and end-to-end/board acceptance remain required. Dirty
owners still must not enter the existing clean prepare/reset/teardown paths
as though their cached changes had been finalized. No deployed firmware or SD
images changed, and frozen M4 archive/PRG hashes remain unchanged.

#### M5 linked-page dirty NRBUF continuation (internal)

`dos_rel_channel_advance_write` now continues a pending dirty-page boundary
after `dos_rel_channel_writeback` has consumed the old active page's dirty bit.
It follows that page's link, selects the other physical page at pointer 2 and
returns to `WRITING`. The next PUT and bounded CLREC padding can continue in
that page. NR, raw record number, read flags and retained status do not advance.
No new channel field, page, queue or command-work state is added. Debug host
and production ARM unit sizes remain 4,604 and 4,532 bytes respectively.

The entry is internal and requires retired read readiness/FIFO copies. Public
OPEN, LISTEN, binary P and CLOSE do not dispatch to it. `dos_work` does not
automatically retry this cursor. The caller still owns both data pages, the
side page and the dirty mask. This is not a complete persistent REL writer.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| Successful dirty NRBUF handoff | 4040 rev2, 8050/8250 DOS 2.7 | Pinned [4040 rel1](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel1) and [DOS 2.7 rel1](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel1), `NRBUF:SETDRN/RDLNK/GAFLGS/WRTOUT/DBLBUF/SETPNT/TSTWRT/RDAB/WATJOB` | After the old page's WRITE, select the linked page with pointer 2, preserve NR/counter, then accept the next byte. Do not reuse lookup coordinates as the next-page address | `dos_rel_channel_advance_write`, cached/refilled handoffs on both active pages, next PUT/padding/writeback tests | 504 approved-ROM vectors execute actual NRBUF, including 252 recycled-WRITE refills, followed by PUT/CLREC/WRTOUT. All physical bytes, pointer, NR, counter, dirty descriptors and saved WRITE targets agree within this successful-job envelope |
| Resident page versus refill | Same selected ROMs | Both `rel1:NRBUF/TSTWRT`, respective `tst2:DBLBUF/SETHDR`, `tsutil:RDAB/SJ10` | Native TSTWRT distinguishes previous READ/WRITE jobs. The host adapts this to its saved track/sector/drive and residency: reuse a matching valid page, otherwise read one clean target. Preserve a matching dirty target instead of rereading it | Saved-header/drive mismatch, dirty cached bytes and explicit read tests | The ROM probe exercises both native branches with immediate successful controller completion. [VICE vdrive-rel](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/vdrive/vdrive-rel.c) `vdrive_rel_write` independently reuses matching next-sector coordinates or reads the required page |
| Failed handoff ownership | Native job boundary plus explicit host storage policy | Both `rel1:NRBUF/DBLBUF/RDAB/WATJOB`, existing generation-bound media contract | Host retains the old active page, wrapped pointer and pending mode until the read succeeds. Partial/failed target reads cannot become resident. Explicit retry may finish without rewriting the already-consumed old page or clearing retained error text | Read-failure/retry, invalid link coordinates, cached/uncached removal/replacement and peer-isolation tests | Deliberate host failure-retention policy, not native controller-error cleanup parity. Captured-model status 23 uses the attempted linked coordinates. Replacement is rejected before callbacks, including on the cached path |
| Blocked ownership and unsupported boundaries | Internal owner boundary, not a new native error | Both `rel1:NRBUF` and `rel2:WRTREL/ADDREL`, plus host retained-dirty ownership contract | Do not advance while the old dirty bit remains, overwrite another dirty page to refill, fabricate a target for a zero link or operate on a different record mode. Return false without I/O/status changes at these boundaries | Failed-WRITE continuation, mismatched/failed dirty target, missing-link, full-record and repeated-call cases | These remain explicit owner/growth boundaries. No invented native missing-record/overflow status or successful command completion |

Native DBLBUF selects the other buffer before waiting for a refill. The host
instead publishes that selection only after a successful read, so an explicit
retry retains an unambiguous old-page owner. This is a conservative host
adaptation. The native LSTJOB queue, fault unwinding and partial job side
effects are not implemented by this helper.

The host clean reader also defers some inactive-page prefetches, as recorded
above. Saved-header validation can therefore require a later READ where native
NRBUF would reuse its earlier speculative READ. The proof uses immutable
record contents and successful jobs. It does not establish speculative timing,
mutable-peer observation parity or behavior after arbitrary native buffer/job
memory edits.

The private `.cache/ai/m5-handoff-rom/result.json` records 504 vectors (168 per
model), 109,512 preceding actual GET/data/EOI comparisons, 1,032,192 physical
page-byte comparisons, 1,260 WRITE jobs and 252 native refill READs. It includes
both drives, both active pages, multiple preceding records, partial positions,
byte values 0/$A5/$FF and a one-byte remainder after handoff. Recycled pages
reach their WRITE state through actual WRTOUT, not a substituted TSTWRT result.
ROM/source/probe identities are recorded with the result.

The REL suite passes 458 checks, including 54 new handoff cases. The complete
`firmware_tests` runner, production ARM firmware, Debug system build and all
three production SPI/FPGA RECORD regressions pass. Formatting and frozen M4
archive/PRG hashes are checked separately. Logs use `.cache/ai/m5-handoff-*`.
The previously recorded unrelated display-test failures are not rerun.

Public REL remains gated. A real bounded writer owner, DYFILE/EOI/status
completion, dirty POSBUF reuse, growth, side/super updates, CLOSE/fault cleanup
and end-to-end/board acceptance remain outstanding. No firmware was flashed,
no SD-card writes were performed and the frozen M4 package remains unchanged.

#### M5 cooperative internal writer ownership

The internal writer now owns its dirty mask inside `dos_channel_t` rather
than requiring a separate caller variable. `file_type` selects an alias of
the ordinary channel's unused active-index byte for REL write flags.
`DOS_ACCESS_MODIFY` marks writer ownership. The flags retain both physical
dirty bits, the DOS 2.7 DYFILE representation and a queued CLREC request.
Native read flags and the raw record counter remain in the existing reader
state, without overlaying or clearing them.

`dos_rel_channel_begin_write` retires read readiness after the caller has
retired staged FIFO copies. `dos_rel_channel_put` accepts only an actual
cached store, without I/O. `dos_rel_channel_pad` queues bounded CLREC,
including when a previously accepted byte is waiting at NRBUF. `dos_work`
services one channel-owned padding chunk, WRITE or handoff per call, outside
the ordered command-work union. WRITE and following READ occur on separate
calls. The existing write/version gate can still perform lazy BAM I/O before
its data WRITE, as documented above.

Work stops at FULL with the final page still dirty and owned. A quiescent
work queue is not an EOI, flush, directory update or durable completion.
The low-level writeback entry can consume that final page separately, but
does not complete the record's status/read transition.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| Per-channel cached writer | 4040 rev2, 8050/8250 DOS 2.7 | Pinned [4040 rel1](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel1), [DOS 2.7 rel1](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel1), `RELPUT/NRBUF`, respective `rel2:WRTREL/CLREC/SDIRTY` | Retain dirty descriptors on their physical pages. A wrapped PUT completes its WRITE and next-page handoff before accepting another byte. Keep NR, record counter and read flags unchanged during these leaves | Channel-owned write flags, begin/PUT APIs, cooperative boundary and peer/pool cases | 504 approved-ROM vectors now run through owned PUT and real dos_work WRITE/handoff instead of external dirty-mask variables. Physical bytes, pointer, NR, counter, dirty descriptors and WRITE jobs still match |
| Model-specific dirty-file flag | Same selected ROMs | Pinned [4040 rel2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel2) and [DOS 2.7 rel2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel2), `SDIRTY/CDIRTY` | DOS 2.7 sets DYFILE on an accepted store, including zero padding. 4040 SDIRTY only sets descriptor dirty. Successful WRITE clears only its descriptor bit, retaining the file-modified representation | `rel_write_result`, owned flags, PUT/padding and failure cases | Every native owned-pipeline vector checks model-specific DYFILE in addition to the prior descriptor/payload comparisons. This is flag tracking, not its later EOI/metadata publication or clearing |
| CLREC across a dirty boundary | Same selected ROMs | Both `rel2:CLREC/CLR10`, `rel1:RELPUT/NRBUF` | Zero remaining record bytes, service the intervening dirty WRITE and linked-page selection, then stop at OVRFLO/FULL without a new record or successful command status | Queued padding, direct padding and padding queued before NRBUF completes, partial/refill cases | Native CLREC and successful NRBUF routines execute in the private proof. Focused owned-pipeline cases check each work-call boundary and final dirty-page retention. The prior VICE sector reuse/commit corroboration remains applicable, not authoritative |
| Failure retention and blocked finalization | Native leaves plus explicit host ownership policy | Both `rel1:NRBUF`, `rel2:WRTREL/RD40` and respective `close` paths, existing captured-media contract | Failed pending jobs stop polling but keep pages, dirty/model/padding flags and cursor. An explicit retry resumes only the pending job and does not clear retained error status. Do not let clean preparation or ordinary CLOSE reinterpret/discard the unfinished writer | Pending-query exclusion, retry API, cached PUT lifetime error, partial WRITE/READ, version, replacement and peer tests. Clean prepare asserts its read-only precondition. CLOSE returns false with 30 before ordinary metadata access | Host failure-retention policy and explicit unsupported CLOSE boundary, not complete native error unwinding or close parity. A cached stale PUT publishes captured-model 25 without accepting the new byte |

The private `.cache/ai/m5-writer-rom/result.json` passes the established 504
successful-job vectors (168 per model), with 109,512 preceding actual
GET/data/EOI comparisons, 1,032,192 physical page-byte comparisons, 1,260 WRITE
jobs and 252 native refill READs. The original ROM oracle is unchanged. Only
the host bridge now dispatches through channel ownership and `dos_work`.
Both dense data shapes, both drives, cached/recycled physical pages, partial
positions and one-byte remainders remain covered. Native controller completion
is immediate and successful. Controller faults, speculative timing,
mutable-peer observations, EOI, growth and CLOSE remain excluded.

The REL suite passes 488 checks, including 30 new owned-writer cases.
The complete `firmware_tests` runner, production ARM firmware, Debug system
build and all three production SPI/FPGA RECORD regressions pass. Host
unit/channel/reader sizes remain 4,604/72/40 bytes. ARM sizes remain
4,532/64/36 bytes. No page, queue, heap allocation or unit-bound increase is
introduced. Formatting, final proof identities and frozen M4 hashes are
checked separately. Logs use `.cache/ai/m5-writer-*`. The previously recorded
unrelated display-test failures are not rerun.

Public REL OPEN, LISTEN and complete binary P remain gated. Complete
WRTREL EOI/status/read restoration, dirty POSBUF reuse, growth, side/super
updates and CLOSE/fault/lifecycle acceptance remain outstanding. Reset and
remount retain their existing semantics and are not REL finalizers. This
slice does not establish unfinished-writer reset/remount parity. No deployed
firmware or SD-card writes were performed, and frozen M4 artifacts are unchanged.

#### M5 cached post-CLREC preparation and completion status

The internal writer can now complete the cached same-active-page portion of
`WR40/RD40` after owned CLREC reaches FULL. `dos_rel_record_finish_write`
drafts NXTREC/SETLST in a cursor copy and commits only a cached READY or
MISSING result. Required NRBUF selection or an unknown-page end scan leaves
the original FULL cursor unchanged. No READ, WRITE, page selection, allocation
or dirty clearing occurs during this completion.

`dos_rel_channel_finish_cached` advances the raw record counter modulo 65536,
clears the 4040 GET flag and retains physical dirty bits and DOS 2.7 DYFILE.
Its FINISHED flag blocks repeat completion, further PUT and padding. A
prepared byte is not transport readiness: the channel remains not ready,
preventing the generic clean-read path from overwriting dirty pages. Cached
completion is neither a durability acknowledgement nor a complete return to
reading.

The caller's overflow event means that an EOI-marked byte was rejected
because the record was already FULL. Merely filling the record exactly does
not produce 51. The next byte is prepared before publishing overflow.
Normal 4040 completion preserves retained status and its unread position.
Normal DOS 2.7 completion publishes OK through the OKERR-equivalent status
path, not ENDCMD, without updating LSTDRV. Error 51 expands to
`51,OVERFLOW IN RECORD,00,00\r` on 4040, with the selected drive field appended
on DOS 2.7. Unlike error 50, its text has no leading space.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| Prepare before completion status | 4040 rev2, 8050/8250 DOS 2.7 in their selected hardware modes | Pinned [4040 rel2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel2) and [DOS 2.7 rel2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel2), `WRTREL/WR30/WR40/RD40`, respective `rel1:NXTREC` | CLREC and next-byte preparation precede ERWORD testing. Only an EOI byte rejected after FULL selects overflow 51 | `dos_rel_record_finish_write`, `dos_rel_channel_finish_cached`, cached completion status/cursor cases | Native WR30 and WR40 execute CLREC/RD40. 216 normal and 216 overflow vectors compare prepared state and branch endpoints |
| Model-specific normal completion | Same selected ROMs | Both pinned `rel2:WR40`, DOS 2.7 `erproc:OKERR`, distinct from command ENDCMD | Preserve 4040 status. Publish DOS 2.7 OK without changing LSTDRV. Both models select the captured drive for any new status | Normal completion cases retain or replace partially consumed status as appropriate and preserve `last_drive` | Native normal branches return on 4040 and reach OKERR on DOS 2.7 |
| Exact overflow text and coordinates | Same selected ROMs | Respective pinned `erproc:CMDERR/MOVERR/ERMOVE` and error-message tokens | Publish 51 with zero track/sector and `OVERFLOW IN RECORD` without a leading space. DOS 2.7 includes the selected drive | Status/cursor and peer-isolation cases compare exact status bytes | Native token expansion independently produces the exact text. The proof stops at CMDERR/OKERR, not full error unwinding |
| Missing next record is preparation, not GET | Same selected ROMs | Both `rel1:NXTREC/NXTR40`, `rel2:RD40` and `rel3:RDREL` | Prepare CR/EOI and LAST at the current pointer without publishing 50. Preserve raw-zero addressing and 16-bit counter wrap semantics | Cached missing, raw-zero and raw-65535 completion cases | 72 native missing-record preparations compare pointer, LAST, NR, counter, data and EOI |
| Retain dirty ownership at unsupported read transitions | Internal host ownership boundary | Both native `rel1:NRBUF`, `rel2:RD40` plus the captured-media contract | Refuse uncached transitions without consuming FULL. Retain pages and dirty bits, block repeated input and withhold read readiness. Reject stale media without I/O | Boundary, stale-media, repeated-completion and peer-isolation cases | Deliberately bounded host policy. Native cached success is compared, but dirty-safe read scheduling and failure unwind remain unimplemented |

The private `.cache/ai/m5-eoi-rom/result.json` passes 432 vectors (144 per
model), including 72 preceding dirty handoffs and 442,368 physical page-byte
comparisons. Both drives, zero/dense records, input bytes 0/$A5/$FF, cached
end scans and final-page missing continuations are covered. Completion
itself performs no I/O. The proof uses the same pinned approved ROM images
and records the production-source, bridge and script hashes.

The REL suite passes 545 checks, including 57 new cases. The complete
`firmware_tests` runner, production ARM firmware, Debug system build and all
three production SPI/FPGA RECORD regressions pass. Host unit/channel/record
sizes remain 4,604/72/36 bytes, ARM 4,532/64/32 bytes. Formatting, scoped
whitespace, proof identities and frozen M4 hashes are checked separately.
Logs use `.cache/ai/m5-eoi-*`. The unrelated display failures are not rerun.

Public REL remains gated. Dirty-safe read readiness and actual GET
continuation, uncached NRBUF, complete EOI stream handling, full CMDERR
unwinding, growth, directory/side/super publication and CLOSE/reset acceptance
remain outstanding. This is cached preparation and status completion, not
complete WRTREL or M5 acceptance. No deployed firmware or SD-card writes
were performed, and frozen M4 artifacts are unchanged.

#### M5 bounded dirty-safe return to cached reading

`dos_rel_channel_resume_read` now exposes the byte prepared by cached
completion, without discarding writer ownership. It validates captured media
and retains MODIFY access, FINISHED, physical dirty bits, DYFILE, the raw
counter and retained status. It performs no I/O. Ordinary clean reprepare,
new input and CLOSE still cannot abandon these pages.

Staging stops at an outgoing dirty page's byte-255 boundary, or the earlier
EOI. Speculative tails and cancelled prefixes perform no WRITE or GET
accounting. When actual GET changes data pages, READBACK records the
outgoing dirty NRBUF job and retires readiness. The current cursor already
names the next page, so `rel_write_page` writes the other physical page's
saved header, not the current active page or its next-sector link.
`dos_work` consumes one WRITE without another GET, counter increment or
READ. Only successful consumption clears that descriptor bit and READBACK.
If the next byte is already prepared, readiness is restored. Job success
does not flush the backing store or finalize file metadata.

Failed WRITEs retain both pages, dirty/model flags and the already-consumed
cursor. Failed owners stop polling while peers can progress.
`dos_rel_channel_retry_write` resumes that exact job without replaying GET
or clearing status. Cached lifetime rejection instead retires the read
cursor with the existing captured-model read-error policy. These host
failure-retention rules are not full native controller-error unwinding.

Any further PENDING refill/end-scan remains an explicit non-polling boundary
for FINISHED owners. Neither `dos_work` nor a repeated resume call may
reinterpret it as a clean reader and overwrite retained pages. Such a
boundary does not invent an error or claim the next byte is ready.
Complete native job-role/history handling and dirty-safe refill scheduling
remain separate work, even if a particular boundary currently has no dirty
bits left.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| Cached post-write staging and actual GET | 4040 rev2, 8050/8250 DOS 2.7 in their selected hardware modes | Pinned [4040 rel2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel2) and [DOS 2.7 rel2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel2), `WR40/RD40/RDREL/RD10/RD25`, respective `ieee:TLK10/TLK30` | Expose prepared bytes and EOI without clearing dirty pages. Advance flags/counter only on actual GET. Do not stage a suffix past a required dirty NRBUF WRITE | `dos_rel_channel_resume_read`, bounded `dos_stage`, captured-source `dos_get`, abandoned-tail and dirty-boundary cases | 80,568 native GET/data/EOI comparisons, including both physical orders and interrupted prefixes. [VICE vdrive-rel](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/vdrive/vdrive-rel.c) `vdrive_rel_read` commits before switching buffers, corroborating the dirty-sector boundary, not native staging |
| Write the outgoing physical page | Same selected ROMs | Pinned [4040 rel1](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel1), [DOS 2.7 rel1](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel1), `NRBUF:WRTOUT/DBLBUF`, respective `tsutil:SJ20/SJ30` | Consume the old active page's saved drive/track/sector/payload before publishing its cached successor. The post-GET cursor must not redirect that WRITE | Physical-page write helper, READBACK worker, byte-wrap and EOI-crossing cases in both physical orders | 576 actual native controller WRITE submissions compare drive, saved header, offset and all 147,456 payload bytes. VICE `vdrive_rel_commit` independently uses the retained sector coordinates |
| Missing record remains an actual-GET error | Same selected ROMs | Both `rel2:RD05`, respective `erproc:MOVERR/ERMOVE` | Restoring/staging CR/EOI does not publish 50. Actual missing GET publishes exact 50 without advancing the raw counter or clearing dirty pages | Missing-GET and repeated-GET cases | 144 native missing GETs reach CMDERR with 50. Exact token text and drive-field formatting are compared separately, without claiming complete CMDERR unwind |
| Failure retention, peers and unsupported refill | Native successful leaf plus explicit host ownership policy | Both `rel1:NRBUF`, respective `tsutil:WRTOUT` and existing captured-media contract | Failed jobs stop polling, preserve the consumed cursor and allow explicit retry. Peers remain usable. Unsupported refill never overwrites pages or publishes readiness | Partial-WRITE/version/stale retry, media replacement before resume/GET, peer isolation and frozen-refill cases | Native successful cursor/job outcomes are compared. Host failure retention deliberately waits for callback success before clearing dirty. Native controller faults, timing and further refill observations remain excluded |

The private `.cache/ai/m5-read-return-rom/result.json` passes 864 vectors
(288 per model), with 21,012,480 physical page-byte comparisons and 720
explicit refill-boundary stops. Both drives, zero/dense records, physical
page orders, normal/overflow completion, bytes 0/$A5/$FF, raw record zero and
16-bit wrap are covered. Approved ROM/image identities and production,
bridge and script hashes are recorded with the proof. READBACK performs no
READ, allocation or backing-store flush.

The REL suite passes 614 checks, including 69 new cases. The complete
`firmware_tests` runner, production ARM firmware, Debug system build and
three production SPI/FPGA RECORD regressions pass. Formatting, scoped
whitespace, final proof identities and frozen M4 archive/PRG hashes pass.
Host unit/channel/record sizes remain 4,604/72/36 bytes, ARM 4,532/64/32.
No structure or resource bound grows. Logs use `.cache/ai/m5-read-return-*`.
The unrelated display-test failures are not rerun.

Public REL OPEN/LISTEN and complete binary P remain gated. Complete EOI
stream handling, native refill/job-history continuations, growth,
directory/side/super publication and CLOSE/fault/reset/remount acceptance
remain outstanding. This slice restores bounded cached reads, not complete
WRTREL or M5 acceptance. No firmware flashing or SD-card writes were
performed. This slice does not modify frozen M4 acceptance artifacts or
prior results.

#### M5 cached NXTREC and retained-WRITE end scans

Cached next-record preparation now continues after an in-page EOI or an
acknowledged outgoing dirty NRBUF WRITE. SETLST uses the completed native
two-page window. It can scan the inactive page retained from a previous
WRITE, even when that page does not match the forward link. Scanning this
page does not make it a forward-link GET cache. Unknown clean NRBUF and
RDAB refills remain stopped before submitting the unknown job in this
verified implementation snapshot.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| Cached next-record preparation | 4040 rev2, 8050/8250 DOS 2.7 in their selected hardware modes | Pinned [4040 rel1](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel1), [DOS 2.7 rel1](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel1), `NXTREC/NRBUF`, respective `rel2:SETLST/FNDLST` | Advance native counters and flags once. Prepare cached SETLST without submitting a speculative READ or discarding dirty ownership | `dos_rel_record_prepare_cached`, `rel_next_cached`, actual-GET and acknowledged-WRITE joins in `dos.c` | 83,160 in-page and 504 post-dirty-NRBUF next preparations compare against native execution. [VICE vdrive-rel](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/vdrive/vdrive-rel.c) corroborates sequential record preparation, not the retained-WRITE window |
| Retained previous-WRITE end scan | Same selected ROMs | Both `rel2:SETLST/FNDLST`, `rel1:NRBUF`, `tsutil:WRTOUT/SJ20/SJ30` | Scan the completed inactive native page when the byte-wide endpoint precedes the current pointer. Do not refresh it merely because its saved header differs from the forward link | Retained-WRITE window and captured-media/window guards in `dos_rel_test.c` | 264 native scans compare physical pages, LAST, NR, pointer and subsequent bytes. VICE refreshes a mismatching forward-link page before trimming, a deliberate difference from the selected ROM behavior followed here |

The private `.cache/ai/m5-cached-next-rom/result.json` passes 1,008 vectors
(336 per model), with 214,872 byte/EOI comparisons, 792 outgoing dirty
WRITEs, 202,752 compared WRITE payload bytes and 45,096,960 physical
page-byte comparisons. It also checks 144 actual missing GETs and stops
before 792 unknown NRBUF jobs and 72 unknown RDAB reads after a known
WRITE. Both drives, physical page orders, zero/dense input, short/full
prefixes, normal/overflow completion and raw record-counter wrap are
covered. These are successful native controller-job comparisons, not
controller-fault or timing acceptance.

The REL suite passes 641 checks. The complete `firmware_tests` runner,
production ARM firmware, Debug system build and all three production
SPI/FPGA RECORD regressions pass. Formatting, scoped whitespace,
proof/source identities and all four frozen M4 hashes pass. Layouts remain
unchanged from the preceding snapshot. Public REL is still gated in this
snapshot.

The operator has now accepted M4. Full M5 work continues beyond these
bounded foundations, including native refill/job history, public REL
OPEN/P/LISTEN, creation/growth, deferred REL-dependent commands and
CLOSE/fault/remount workflows. M4 artifacts and recorded results remain
unchanged.

#### M5 complete existing-record job continuations

The owned read pipeline now services the NRBUF/RDAB boundaries previously
left stopped. Each physical pooled page retains its last READ/WRITE role
separately from its saved drive and header. TSTWRT requires an awaited READ
after a previous WRITE, even when the saved header already matches the
requested link. The dirty branch writes the outgoing page, selects its
successor and awaits that refill when required, without prefetch. The
clean branch awaits a reused WRITE page first, then submits the native
forward-link prefetch into the old page.

Staging stops at every owned page boundary, including clean boundaries.
Only actual captured-source GET commits the seam and queues work. Each
continuation performs at most one data job, preserving the already-consumed
pointer/counter. The existing BAM-version gate can separately load its
cached BAM before a WRITE. Prefetch failures remain deferred until SETLST
or NRBUF actually needs the failed page. Failed awaited reads stop polling
and retain their phase for an explicit retry. Replaced-media reads retire
the cursor and cannot be revived by that retry.

`dos_rel_channel_finish_write` now queues complete existing-record WRTREL
completion, including CLREC padding across a linked-page seam and RD40
through dirty NRBUF/refill. It advances the raw record counter once and
withholds readiness and delayed status until preparation completes.
Overflow means an EOI-marked byte rejected after FULL, not exact record
fill. Normal 4040 completion retains status, normal DOS 2.7 completion
publishes OK without ENDCMD/LSTDRV changes, and overflow publishes exact
51 after RD40. Failed jobs retain the pending EOI/overflow and dirty
ownership, allowing a peer's completion to proceed before explicit retry.
Callback success still does not imply a backing-store flush.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| Physical job roles and native NRBUF sequence | 4040 rev2, 8050/8250 DOS 2.7 in their selected hardware modes | Pinned [4040 rel1](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel1), [DOS 2.7 rel1](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel1), `NRBUF:NRBU50/NRBU70`, respective `tstflg:TSTWRT`, `tsutil:RDAB/WRTOUT` | Await a selected previous-WRITE page regardless of matching coordinates. Dirty NRBUF performs no following prefetch. Clean NRBUF prefetches only after the selected page is usable | `buffer_writes`, `rel_nrbuf_work`, `rel_queue_nrbuf`, matching-written-header, dirty-refill and saved-header cases | Both native jobs and retained page roles compare through missing GET. [VICE vdrive-rel](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/vdrive/vdrive-rel.c) corroborates committing dirty data before switching, but does not model native LSTJOB/dirty-versus-clean scheduling |
| Full existing-record EOI completion | Same selected ROMs | Pinned [4040 rel2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel2), [DOS 2.7 rel2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel2), `WRTREL:WR30/WR40`, `CLREC/RD40`, respective `rel1:NXTREC` | Complete padding and next-byte preparation before status 51/OK. Retain the single raw-counter advance across failed queued jobs. Do not update LSTDRV or flush media as an EOI side effect | `dos_rel_channel_finish_write`, `rel_finish_work`, queued padding, NR=2, NR=0, low-byte FULL alias, delayed status, retry and peer cases | 576 additional native completion vectors include physical page reversal and both drives. VICE `vdrive_rel_listen` corroborates record padding, not the selected native flag/status ordering |
| Missing record preserves native NR | Same selected ROMs | Both `rel1:NXOUT/NXTR45/NXTR40`, `rel2:RD05` | A rejected end candidate sets LRF and LSTCHR to the current pointer without replacing NR. Actual missing GET alone reports 50 and does not advance the raw counter | `record_prepare`, `dos_rel_record_next`, retained-WRITE continuation and full EOI missing-record cases | Full-stream native comparisons now reach each vector's actual missing GET, rather than stopping at an earlier unknown job |
| Failed refill/prefetch and retained EOI | Successful native sequence plus explicit host storage/lifetime policy | Both `rel1:NRBUF`, `rel2:SETLST/RD40`, and captured-media ownership contract | Never stage partial failed data or overwrite dirty ownership. Defer speculative failure until needed. Awaited failures stop polling, preserve consumed progress and allow explicit same-job retry. A failed owner does not block a peer | Awaited read failure/removal/replacement, deferred prefetch failure, full EOI write/version failure and peer-isolation cases | ROM execution compares successful jobs, bytes and state. Partial callback failure and retry are host-policy tests, not claims about full native CMDERR/controller retry behavior |

Independently, [MAME c8050](https://github.com/mamedev/mame/blob/master/src/devices/bus/ieee488/c8050.cpp)
identifies the selected 901887-01/901888-01 DOS 2.7 pair and maps the same
four physical RAM banks to both the DOS and controller CPUs in
`c8050_main_mem/c8050_fdc_mem`. This corroborates the oracle's physical
shared-page model, not EOI status ordering or controller-fault timing.

The private `.cache/ai/m5-native-jobs-rom/result.json` passes 1,008 vectors
with 472,008 actual byte/EOI comparisons, 1,728 READs, 864 WRITEs and 1,008
actual missing GETs. `.cache/ai/m5-native-jobs-eoi-rom/result.json` adds
576 vectors, 159,528 byte/EOI comparisons, 576 READs, 336 WRITEs and 576
actual missing GETs. Together they compare 897,024 complete job-payload
bytes and 114,683,904 physical page bytes. The second oracle also exercises
EOI completions that require NRBUF, rather than just cached same-page
completion. Both retain approved ROM identities and exact production/probe
hashes. These four-data-page fixtures do not establish growth, indexing
capacity or complete public-channel compatibility.

The REL suite passes 740 checks (99 new cases). The complete
`firmware_tests` runner, production ARM firmware and Debug system build
pass. All three production SPI/FPGA
`NativeRecordRejectsBinaryRequestsAndPreservesCursors` cases pass (182
seconds), retaining the public gate while the internal paths advance.
Formatting passes. Host unit/channel/record sizes remain
4,604/72/36 bytes. ARM sizes are 4,536/64/32 (the job-role bitmap adds four
aligned bytes per ARM unit), within the existing 4,608-byte bound.
The accepted M4 archive and all three frozen gap-checker PRG hashes remain
unchanged. No firmware flashing or SD-card writes were performed.

This completes the existing-record job-continuation prerequisite, not M5.
Public REL OPEN/P/LISTEN remains gated. Persistent channel metadata,
dirty-safe positioning, creation/growth and side/super publication, full
CLOSE/fault/remount behavior, deferred REL-dependent commands and real
M5 acceptance programs/workflows remain required.

#### M5 persistent channel metadata

Directory coordinates and block accounting now live outside the
ordinary-file/REL-reader union. A REL preparation previously overwrote that
metadata with lookup/cursor bytes, making later native directory finalization
unsafe. The common first-link storage also has explicit REL side/super-root
aliases, retained independently of the lookup's changing coordinates.
Byte-sized access/directory/close tags recover host enum-padding overhead,
without changing enum values or expanding the native page pool.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| Directory ownership survives record operations | 4040 rev2 and selected 8050/8250 DOS 2.7 | Pinned [4040 close](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/close), [DOS 2.7 close](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/close), `CLSREL/CLSDIR`, and [DOS 2.7 tstflg](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tstflg), `TSTCHN:TSTC40` | Retain this file's directory coordinates independently of RS/NR/pointer/index state. Current record reads and queued write retry must not redirect future metadata access to a peer entry | Common `directory_track/sector/entry/blocks`, `test_pipeline_retains_directory_metadata` across all three models, native GET page changes and failed EOI WRITE/retry | VICE `vdrive_rel_close` retains its independent slot descriptor while committing data. Its close-time padding is not adopted as selected-ROM behavior |
| Retain side/super root independently of current lookup | Same selected ROMs | Pinned [4040 opchnl](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/opchnl), [DOS 2.7 opchnl](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/opchnl), `OPNRCH:OR20/OR21` | Distinguish the file's side/super root from the data or index job currently selected. Do not recover root coordinates by interpreting ordinary-file union members | `rel_root_track/sector`, `dos_rel_channel_prepare`, the same retained-metadata cases | VICE's separate side/super coordinates corroborate the lifetime distinction, not a new host cache or private page |

All six new retained-metadata cases failed against the previous layout, then
passed after the ownership change. The REL suite passes 746 checks, the
ordinary DOS suite passes 1,969 checks, and the complete `firmware_tests`
runner passes. Production ARM and Debug system builds and formatting pass.
The 1,584 full native-job/EOI vectors above were rerun successfully against
the new layout and retain exact current source hashes. Host
unit/channel/record sizes are now 4,572/68/36 bytes. ARM sizes remain
4,536/64/32. This is metadata groundwork for native OPEN/P/CLOSE, not an
implementation or acceptance claim for those still-gated operations.

#### M5 directory-linked OPEN preparation and clean read jobs

The internal `dos_rel_channel_prepare_open` now follows OPNRCH rather than
performing a cold FNDREL lookup. STRDBL reads the directory's first data link
and its live successor before the small-model side-root READ. The 8250
captures its super root without reading it at OPEN. Initial RD40 prepares
raw record 1 with the native byte-wide NR/LSTCHR rules. An unrelated side
pointer cannot redirect this first data READ. Preparing a missing first
record does not itself publish 50.

Actual clean-reader GETs now use the owned NRBUF scheduler, without
changing READ access into MODIFY. Staging stops at the physical seam.
After actual consumption selects the cached successor, clean NRBU70 reads
its forward link into the consumed page immediately, not at a later
NXTREC. An unread NONE-job page retains its window bytes, rather than
inventing a controller READ solely because it lacks a resident READ
header. Speculative failures remain deferred until the failed page is
needed. Replacement media retires pending work before any replacement
callback. Private READ-channel close remains cancellation, not CLSREL.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| Directory-linked initial REL window | 4040 rev2 and selected 8050/8250 DOS 2.7 hardware modes | Pinned [4040 opchnl](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/opchnl), [DOS 2.7 opchnl](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/opchnl), `OPNRCH/OR20/OR21/OROW`, respective `tst2:STRDBL` and `rel2:RD40` | Read directory data and its live successor first. Read the small side root, but only capture the 8250 super root. Prepare raw record 1 without an OPEN-time missing-record status | `dos_rel_channel_prepare_open`, `rel_open_work`, directory/index disagreement, low-byte aliases, missing first record, failed job and peer cases | VICE's separate REL side/super coordinates corroborate the model distinction, not native OPEN job order. Complete selected-ROM OPNRCH execution supplies that order |
| Clean actual-GET NRBUF and NONE-job windows | Same selected ROMs | Pinned [4040 rel1](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel1), [DOS 2.7 rel1](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel1), `NRBU50/NRBU70`, respective `tstflg:TSTWRT`, `rel2:RDREL/RD15` | Queue clean prefetch at the actual seam, preserving READ ownership and the committed pointer. TSTWRT requires an awaited refill for WRITE, not NONE. Do not speculatively consume bytes or publish deferred read errors | `dos_stage/dos_get`, `rel_nrbuf_work`, `rel_next_cached`, clean-seam/deferred-failure/replacement and terminal-window cases | MAME's shared physical RAM corroborates retaining actual window bytes. Native executions compare every page, ordered READ and byte/EOI, including separately seeded terminal beyond-LAST cursors |
| OPEN and read-job storage failures | Explicit host storage/lifetime policy around the selected successful native path | Same routines and captured-media ownership contract | Never stage failed data or touch replacement media. Keep a speculative error deferred while current data remains usable. Clean cancellation releases the three borrowed pages | `test_pipeline_open_failed_jobs_and_lifetime`, `test_pipeline_open_peer_isolation`, `test_pipeline_open_clean_nrbuf` | Controller callbacks model success/failure, not full native controller retries, fault timing or CMDERR unwinding |

The private `.cache/ai/m5-existing-open-rom/result.json` passes 486
vectors, with 154,362 actual byte/EOI comparisons, 1,338 ordered READs,
342,528 complete job-payload bytes, 118,927,872 physical page bytes and
486 actual missing GETs. Of these, 480 execute fresh OPNRCH with normal
startup allocation state, both drives, five record sizes, different
directory data links, terminal/linked pages, zero/dense records and
normal/unrelated roots. Six additionally seed a terminal cursor beyond
LSTCHR and a retained NONE-job window before executing real RD15/RDREL.
Those six do not prove public binary P. ROM tables remain unmodified.
Exact production/probe hashes and approved ROM identities are retained.

All 788 REL checks, 1,969 ordinary DOS checks and the complete
`firmware_tests` runner pass. Production ARM and Debug system builds,
formatting and the prior 1,584 native-job/EOI vectors pass again. Unit,
channel and record sizes remain host 4,572/68/36 and ARM 4,536/64/32.
No new pages, heap or per-owner queue were introduced.

This closes internal successful-job OPEN preparation, not public OPEN
integration or M5. The existing private claim reserves all three pages
before I/O, whereas native OPNRCH obtains its third page after STRDBL.
Public resource-exhaustion ordering, dirty-safe positioning, growth,
full CLOSE, dependent commands and operator acceptance remain required.
Public REL gates remain in place.

#### M5 retained writer reentry

A ready retained-writer read return can now reenter cached input without
losing its dirty pages or DOS 2.7 DYFILE. `dos_rel_channel_begin_write`
retires readiness and removes only completed host-mode flags. It keeps
the current pointer, raw record counter and captured GET flags. It does
not rewind a previously consumed byte, allocate, write or flush.
Unexposed cached completion, pending writeback and incomplete EOI still
cannot reenter.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| Resume input after retained read return | 4040 rev2 and selected 8050/8250 DOS 2.7 | Pinned [4040 rel2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel2), [DOS 2.7 rel2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel2), `WRTREL:WR10/WR40`, `RELPUT/SDIRTY` | Store at the current cursor and retain dirty/DYFILE ownership across TALK/LISTEN changes. A successful next EOI still prepares RD40 before normal status | `dos_rel_channel_begin_write`, nine `test_cached_read_return_reenters_write` cases with no GET, one GET and a completed dirty NRBUF | Actual selected-ROM second WRTREL, including CLREC, jobs and later GETs, is compared independently. VICE `vdrive_rel_commit` corroborates retaining dirty ownership until commit, not native flag/cursor ordering |

The private `.cache/ai/m5-reentry-rom/result.json` passes 432 vectors with
224,880 actual byte/EOI comparisons, 864 READs, 864 WRITEs, 442,368 complete
job-payload bytes and 432 actual missing GETs. Cases cover both drives,
both physical data-page orders, initial normal/overflow completion, short
and longer first writes, three input bytes and 0/1/54 intervening GETs.
The 54-GET prefix completes the outgoing dirty-page seam before the
second write. Controller fault timing and full CMDERR unwinding remain
outside this successful-job proof.

The REL suite passes 797 checks. The ordinary 1,969-case DOS suite, full
`firmware_tests` runner, production ARM build, Debug system build and
formatting pass again. The 486 OPEN vectors and prior 1,584 native-job/EOI
vectors were rerun against the same final source identities. Public REL
OPEN/P/LISTEN, growth, CLSREL and acceptance remain incomplete.

#### M5 dirty-safe RECORD preparation

The internal positioning owner now retains the native side/data/data
window. `dos_rel_channel_position_buffer` implements the selected-data
POSITN/POSBUF continuation. `dos_rel_channel_position` additionally performs
resident SSTEST/SSPOS and SSEND fallback. Neither entry implements command
dispatch or publishes RECORD's final 50/51/ENDCMD. Callers must retire FIFO
copies before starting.

The approved 4040 revision-2 overlay, unlike the unmodified revision-1
`record` source, swaps at P80 before STRDBL, as DOS 2.7 does. Active BHERE
hits retain dirty bytes without SCRUB or padding. Other targets SCRUB the
outgoing dirty page before native buffer rotation. An inactive previous-WRITE
header can satisfy BHERE without NRBUF's forced refill. Successful data
selection applies the actual byte-wide POSITN/NXOUT/SETLST rules.

SSPOS first uses the resident side number and table. Its second SSTEST
branches on N only, without repeating a surviving V-requested READ.
The 8250 retains SSSGRP in the otherwise unused REL directory-phase byte.
Native group 90 reuse avoids rereading the super table during RDLG.
Absent records use SSEND's selected endpoint, preserving current data and
dirty ownership instead of inventing a data-sector lookup. Preparing a
missing record is still distinct from an actual missing GET.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| Dirty/header-aware positioning | 4040 rev2 and selected 8050/8250 DOS 2.7 | Pinned [4040 record](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/record), approved revision-2 P80 overlay documented above, [DOS 2.7 record](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/record), `POSITN/POSBUF/BHERE/P80`, respective `tst2:STRDBL` | Active matches skip SCRUB, inactive matches reuse the saved header, cold targets use the selected revision's rotation, and SCRUB never CLREC-pads | `rel_position_start`, `rel_position_work`, 39 header/dirty/failure cases | [VICE REL](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/vdrive/vdrive-rel.c) corroborates current/following buffer reuse, but its fill-record-before-position behavior is not copied. [MAME 8050/8250](https://github.com/mamedev/mame/blob/master/src/devices/bus/ieee488/c8050.cpp) corroborates shared physical RAM and the selected DOS 2.7 images. Selected-ROM execution establishes exact rotation and jobs |
| Resident side/group lookup and missing fallback | Same selected ROMs | Pinned [4040 sstest](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/sstest), [4040 ssend](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ssend), DOS 2.7 counterparts and [system](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/system), `SSPOS/SSTEST/SSEND/RDSSS/RDSS1/RDLG` | Reuse the current side/group, await at most the native selected side READ, and use SSEND rather than overwriting data on an absent pointer. Preserve raw record and delayed position overflow. BREAK publishes 67 with the selected model's exact status format | `rel_position_index_work`, side-header mismatch, missing/range/corruption and group-90 cases | VICE's separate side/super coordinates and MAME's hardware-mode/ROM mapping corroborate the model distinction, not native cache/job ordering |
| Positioning storage failures | Explicit host storage/lifetime policy around the selected successful path | Same selected routines and captured-media contract | Retain failed awaited jobs for explicit retry, defer speculative prefetch failure, and never invoke replacement-media callbacks | `dos_rel_channel_retry_position`, read/write failure and replacement cases | Successful native job comparisons do not prove controller fault timing, automatic retries or CMDERR unwinding |

The private `.cache/ai/m5-position-rom/result.json` passes 1,488 vectors,
37,860 actual byte/EOI GET comparisons, 2,124 READs and 420 WRITEs.
The additional `.cache/ai/m5-position-index-rom/result.json` passes 896
vectors, including 704 prepared-byte/EOI comparisons, retained groups
90/91/super/none, missing SSEND endpoints, corrupt index exits and delayed
overflow. Ordered jobs, full job payloads and physical pages are compared
against the approved mapped binaries, not reconstructed expectations.
The empty-super RDLG underflow into adjacent native RAM is still unsupported
and remains an explicit boundary, not a fabricated missing-record result.

#### M5 existing-record native CLOSE jobs

`dos_rel_channel_close` now queues internal CLSREL, retaining ownership
until its final host durability boundary. SCRUB writes only the dirty
active page, without CLREC padding. DBLBUF then awaits the other page's
completed job before SSEND. SSCALC/SSSCAL produce the native low-word count
from the final group, side and byte-wide endpoint. The 4040 always CLSDIRs,
whereas DOS 2.7 does so only with DYFILE. CLSDIR uses the selected physical
data page and rereads the current directory, preserving unrelated entries,
the original first/side links and native replacement-bit behavior.
No ordinary MAPOUT or ordinary writer-union fields are used.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| Native close sequencing and counts | 4040 rev2 and selected 8050/8250 DOS 2.7 | Pinned [4040 close](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/close), [DOS 2.7 close](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/close), `CLSREL/CLSDIR`, respective `ssend:SSEND` and `ssutil:SSCALC/SSSCAL` | SCRUB without padding, select/await the other page, derive counts from SSEND and apply the model-specific directory-publication condition. Preserve native malformed-endpoint subtraction and low-word rollover | `dos_rel_channel_close`, `rel_close_work`, `dos_rel_blocks`, clean/dirty/previous-WRITE/missing/replacement cases | VICE `vdrive_rel_close` fills the record before committing, which disagrees with the selected ROM and is deliberately not copied. MAME's shared physical RAM and selected-ROM execution corroborate actual page residue and model choice |
| Directory preservation and physical jobs | Same selected ROMs | Same `CLSDIR:CLSD5/CLSD6`, `DRTRD/DOIT` | Update only the native type/count fields after a fresh directory READ, preserving peer entries and the REL replacement branch | Close payload/directory comparisons and `test_pipeline_rel_close_peer_directory` | Actual selected-ROM directory jobs and complete persistent sectors are compared independently, including 4040 packed DIRENT versus DOS 2.7 DSEC/DIND |
| Failure retention and final durability | Explicit host storage/lifetime policy | Native successful close sequence plus captured-media and host-flush contracts | Keep all three pages on failed data/index/directory/flush work, do not poll, reread the current directory on retry after a failed WRITE, and permanently retire replacement-media work | `dos_rel_channel_retry_close`, 27 I/O/replacement cases and three interleaved peer-close cases | The additional flush is host policy, not native MAPOUT. Successful native controller callbacks do not establish native fault timing or CMDERR cleanup |

The private `.cache/ai/m5-close-rom/result.json` passes 4,608 vectors.
These compare CLSREL through its FRECHN boundary, all ordered successful
READ/WRITE jobs, full payloads, all three physical pages, the persistent
directory and 4,608 native block counts. Coverage includes both drives and
physical data-page orders, zero/dense input, no write, short/long unpadded
writes, an already-completed WRITE, locked/replaced/malformed type bytes,
side 0/1/5, group 0/90 and endpoint bytes 17/255/0/1.
Only the C owner executes the additional host flush and releases its pooled
pages. The evidence does not claim physical-board acceptance.

All 921 REL cases, 1,969 ordinary DOS cases and the complete firmware core
pass. Production ARM and Debug system builds pass. The four prior native
OPEN/job/EOI/reentry proofs and both positioning proofs pass again against
the same production source identities as CLOSE (9,494 vectors total).
Host unit/channel/record sizes remain 4,572/68/36 bytes and ARM sizes remain
4,536/64/32. No pages, heap or per-owner queue were added. Frozen M4 package
and deployed acceptance PRG hashes remain unchanged.

#### M5 native creation and ADDREL/NEWSS growth

The private creation owner now performs OW20 through OROW after INTTS has
allocated the first data sector. It initializes the native side page and
8250 super page, preserves NULBUF/ADDNR's byte-wide marker and terminal-link
quirks, writes the pages in native order and completes MAPOUT.
The growth owner performs SSEND, native capacity preflight, ADDREL/NEWSS,
physical descriptor transfers, side-table propagation, 8250 group publication,
MAPOUT and final positioning. It adds no pooled pages or allocation queue.
Public INTTS/resource ordering and ADDFIL directory publication are still
separate, unfinished integration work.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| Initial REL pages and record markers | 4040 rev2 and selected 8050/8250 DOS 2.7 | Pinned [4040 opchnl](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/opchnl), [DOS 2.7 opchnl](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/opchnl), `OW10/OW20/OROW`, respective `record:NULBUF/ADDNR` | Write side, optional super, data and BAM in native order. Preserve marker carry/zero behavior for every record length, including the initially missing size-255 record | `dos_rel_fill`, `dos_rel_channel_create`, native page/GET and creation failure tests | [VICE REL](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/vdrive/vdrive-rel.c) corroborates 0xff/zero record initialization, but uses corrected fill arithmetic. [MAME c8050](https://github.com/mamedev/mame/blob/master/src/devices/bus/ieee488/c8050.cpp) independently identifies the selected DOS 2.7 images. Actual selected-ROM execution establishes byte-wide edge cases |
| Growth preflight and allocation | Same selected ROMs | Pinned [4040 addrel](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/addrel), [DOS 2.7 addrel](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/addrel), `ADDREL/AR35`, respective `fndrel:FNDREL` | Preserve native pointer borrow, signed loop counts, low-word wrap, small-file side limit and boundary undercount. Publish 52 before allocation when native preflight rejects the request | `dos_rel_growth_plan`, `rel_growth_work`, capacity and boundary tests | VICE's corrected growth algorithm is not substituted for native arithmetic. MAME confirms the hardware modes. Direct selected-ROM preflight execution covers the native differences |
| Side/group growth and retained pages | Same selected ROMs | Same `ADDREL`, respective `newss:NEWSS`, `ssend:SSEND` and `record:POSITN` | Retain deferred dirty links, transfer actual buffer descriptors, propagate side tables, publish new 8250 groups, and preserve native READs of allocated but unwritten sectors | Growth page/job comparisons, repeated dirty growth, side 1/2/5 and group-1 tests | VICE corroborates side/data growth but explicitly repairs endpoint behavior. MAME's physical RAM mapping corroborates distinct page identities. Complete native job and raw-image comparisons establish actual ordering and residue |
| Explicit retry and media lifetime | Host storage/lifetime policy around the selected successful path | Same native sequence and captured-media contract | Stop polling after partial READ/WRITE failure, retain allocations/pages/phase, repair partial persistent writes on explicit retry and never follow replacement media | `dos_rel_channel_retry_create`, `dos_rel_channel_retry_grow`, partial-write, awaited-page, capacity and replacement tests | Native successful-job proofs do not establish controller-fault timing or native CMDERR unwinding. Host flush remains separate from MAPOUT |

The private `.cache/ai/m5-create-rom/result.json` passes 4,590 creation
vectors, 13,548 independent NULBUF vectors, 16,398 actual byte/EOI GET
comparisons and 15,300 WRITEs. It deliberately excludes the initial INTTS
allocation/resource prefix and public parser/ADDFIL.
The private `.cache/ai/m5-growth-rom/result.json` passes 1,688 complete
growth vectors and 2,880 direct native preflight vectors. It compares 2,138
READs, 26,014 WRITEs, 1,682 prepared-byte/EOI results, six native 52 exits,
3,884,544 physical-page bytes and 24,679,936 persisted bytes.
Every successful growth vector also compares the subsequent native RELPUT.
All 255 lengths, both slots, repeated growth after retained writes and side/
group seams are covered. The full raw backing image is intentional: native
final positioning can prefetch an allocated sector before any WRITE initializes
it. Treating every allocated page as NULBUF-filled would conceal that behavior.

#### M5 retained LISTEN and binary RECORD completion

LISTEN now retains one already-popped byte and its EOI bit in each REL owner
until WRTREL's growth, NRBUF and EOI work finishes. Packed reader flags leave
the channel and unit size bounds unchanged. No page, heap allocation or command
buffer is borrowed. A failed owner stops polling and retains the byte or its
already-consumed EOI phase. A peer can continue independently.
The public byte scheduler still cannot reach these channels through OPEN,
which remains gated until the complete feature is ready.

Binary P now enters the retained positioning owner through actual command
collection and dispatch. Its completion distinguishes immediate missing 50,
delayed position 51 and successful ENDCMD. Missing 50 takes precedence over
position 51. A storage failure completes the command with its error but retains
the failed channel job for explicit retry. Queued P also retains its owner
against public CLOSE, private CLSREL, clean reprepare and lifecycle finalization.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| Received-byte retention and growth | 4040 rev2 and selected 8050/8250 DOS 2.7 | Pinned [4040 rel2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel2), [DOS 2.7 rel2](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel2), `WRTREL:WR60/WR10/WR20` | Preserve DATA across ADDREL, store it once and await its page continuation before processing EOI or later input | `dos_listen_put`, `rel_input_work`, growth and post-store WRITE retry cases | VICE independently separates current/following REL buffers and record writes. MAME corroborates selected ROM/hardware mapping. Complete native WRTREL execution establishes input/job ordering |
| Overflow drop and EOI completion | Same selected ROMs | Same `WRTREL:WR50/WR30/WR40`, `CLREC/RD40` | Drop excess non-EOI bytes without 51. Publish 51 only for a rejected EOI byte after CLREC/RD40. Successful 4040 completion retains status, whereas DOS 2.7 publishes OK | Repeated short/exact/overlong LISTEN streams, native status-entry and prepared TALK comparisons | VICE corroborates record boundaries, not revision-specific status publication. Selected-ROM execution plus MAME's model mapping establishes the model distinction |
| Binary P dispatch and completion | Same selected ROMs | Pinned [4040 record](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/record), [DOS 2.7 record](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/record), `RECORD:R20/R30/R50/R60`, respective `rel2:RD05` and `idle:ENDCMD` | Decode binary record/position bytes even in short requests, preserve raw record zero, retain native SA remapping, prepare CR/EOI and status 50 for missing records, and defer position 51 until positioning completes | `command_start`, `record_work`, binary/short/missing/overflow and failed-job retry tests | VICE corroborates record/position semantics. MAME corroborates selected mode/ROM identities. Actual native RECORD execution establishes prepared bytes, error priority and model flag behavior |
| Interrupted input and peer ownership | Explicit host storage/lifetime policy | Same successful native paths and captured-media contract | Do not replay an already-stored byte on NRBUF retry, do not overwrite retained DATA with command collection, reject destructive CLOSE/position/finalize while input is owned, and let a peer finish independently | `test_listen_stored_byte_retry`, `test_listen_growth_retry_retains_byte`, `test_listen_failed_owner_peer` | These host fault-injection tests do not claim native controller-fault or IEEE cleanup timing |

The private `.cache/ai/m5-listen-rom/result.json` passes 16,520 received-byte
vectors, 148 READs, 4,070 WRITEs, 1,760 prepared TALK byte/EOI comparisons and
60 native error-entry comparisons. Coverage includes every record length,
both slots, repeated short/exact/overlong records and side/group growth on an
EOI byte. The private `.cache/ai/m5-record-command-rom/result.json` passes
2,124 complete collection/dispatch vectors with 2,124 prepared TALK byte/EOI
comparisons, 1,782 missing-50 exits, 84 delayed-51 exits and 844 READs.
These compare the real selected binaries through native status-publication
entries, not full CMDERR/IEEE unwinding.

At the LISTEN/P checkpoint, all 1,132 REL checks and the complete firmware core
pass. Production ARM and
Debug system builds pass. The eleven native proof suites pass against the
same production source identities. Host unit/channel/record/reader sizes remain
4,572/68/36/40 bytes and ARM sizes remain 4,536/64/32/36 bytes.
Eleven targeted production SPI, status-consumption and binary-P rejection
system tests pass. The broad 705-test system run was stopped in favor of this
focused selection and is not reported as a complete system-suite pass.
The next integration checkpoint follows below.

#### M5 routed CLOSE and lifecycle finalization

The public CLOSE extension now enters retained CLSREL rather than treating a
REL reader as an ordinary free or interpreting a REL writer through the
ordinary union. Its command tag survives descriptor release so CLOSE-all can
continue and publish its final status. CLSALL visits data secondaries 14..0,
not logical-index order. Explicit CLOSE 0 remains FRECHN without SCRUB or
directory publication. CLOSE 1 retains status and command storage. Successful
CLOSE 2..15 follows ENDCMD, including the native drive-selection effects and
58-byte CLRCB boundary.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
| --- | --- | --- | --- | --- | --- |
| Routed CLOSE and completion | 4040 rev2 and selected 8050/8250 DOS 2.7 | Pinned [4040 close](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/close), [DOS 2.7 close](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/close), `CLOSE/CLS10/CLSCHN/CLSREL`, respective `parsex:ENDCMD/CLRCB` and `tst3:FRECHN` | Explicit SA 0 frees without CLSREL. SA 1 retains status. Other successful closes clear status through ENDCMD and clear the native command region. IBOP and CLSDIR select the file's drive, but a cached DOS 2.7 no-write close need not change DRVNUM | `dos_input_address`, `dos_close`, `close_work`, 24 routed model/SA/write cases and complete native CLOSE comparisons | [VICE REL](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/vdrive/vdrive-rel.c) corroborates separate REL close handling, not its selected-ROM-incompatible record padding. [MAME c8050](https://github.com/mamedev/mame/blob/master/src/devices/bus/ieee488/c8050.cpp) corroborates selected DOS 2.7 hardware/ROM identities. Actual ROM execution establishes status, jobs and physical residue |
| CLOSE-all ownership and order | Same selected ROMs | Same `CLSALL:CLS20`, `CLSCHN`, `FRECHN` | Visit SA 14 down to 0, including native CLSREL for SA 0 inside CLSALL. Retain the requested close operation after an earlier descriptor is released | `close_channel`, `close_work`, nine ordered two-owner/failure cases with a direct-channel peer | Both ROMs execute the same descending-secondary loop. The native one-owner CLSALL vectors exercise that loop, while host tests cover multiple retained C owners and failures |
| Lifecycle durability and explicit retry | Host storage/lifetime policy around selected CLSREL | Same native close sequence plus captured-media and host-flush contracts | Drain quiescent REL owners without ordinary-union access. Retain failed and not-yet-completed owners, release successfully completed REL owners, preserve error status, and flush only the owning unit. Failed CLOSE stops polling and can be retried by another CLOSE or finalization. A new OPEN cannot discard its retained metadata/input owner | `dos_finalize`, public CLOSE retries across six fault points per model, later-owner/other-unit preservation and OPEN-after-failed-CLOSE tests | This synchronous lifecycle drain and host flush are not native controller timing or atomic rollback. Ordinary finalization behavior is unchanged |

The private `.cache/ai/m5-public-close-rom/result.json` passes 3,072 vectors
through native CLOSE, channel release and ENDCMD to the FREICH entry.
It compares 4,032 READs, 3,264 WRITEs, every job payload, 2,359,296 physical
page bytes, 786,432 persistent-directory bytes, selected/last drive, exact
published success text and CLRCB residue. Both slots, physical data-page
orders, empty/dense data, short/long unpadded writes, native replacement
behavior, side 0/5 and group 0/90 are covered. FREICH's internal-channel
release, multiple simultaneous native owners, controller fault timing and
empty-super underflow remain outside this private proof.
The 4,608-vector private CLSREL proof also passes again against the same
production source identities.

All 1,207 REL checks and the complete firmware core pass. Production ARM and
Debug system builds pass. Eleven targeted production SPI, pending-CLOSE,
remount and reset system regressions pass. These system tests preserve the
existing public path and do not claim REL OPEN/transport acceptance while
OPEN is gated. Host unit/channel/record/reader sizes remain 4,572/68/36/40
bytes and ARM sizes remain 4,536/64/32/36 bytes. No pages, heap or per-owner
queue were added. Frozen M4 package and acceptance PRG hashes are unchanged.

Public OPEN/create, INTTS/resource ordering, ADDFIL and directory growth,
empty-super underflow, REL-dependent SCRATCH/COPY/VALIDATE and real M5
acceptance remain required. Full REL reset/remount acceptance also depends
on enabling and exercising OPEN through the production transport.
The accepted M4 artifacts are not rebuilt or deployed by this work.
M5 remains incomplete.

### M6: Accept the complete replacement

Complete the matrix below, measure actual worst-case RP2040 headroom/service
latency and SD behavior, and document exact revisions, exclusions, container
limits, and evidence gaps. The operator has already authorized legacy removal
during M2. Being the sole engine does not waive complete hardware acceptance.

Complete the functional M2 deferrals: runtime storage/stack headroom with eight
mounted files, and board-only unit-local
remount/rejection with peer-cursor preservation. M2 acceptance did not measure
or pass these cases. They remain pre-MVP gates. Automated PET-visible response
timing against pinned VICE is now separate advisory benchmarking with
`M6BENCH`, not a pre-MVP performance gate. Internal/electrical controller
timing requirements remain explicit.

Re-run affected host suites and provide a final operator smoke runbook for the
default configuration. Exit only after the operator confirms the shipped path
passes. Do not reintroduce a hybrid implementation or a legacy fallback.
Use the automated eight-slot tape-suite contract below for the final run,
not repeated per-drive invocations of the earlier milestone checkers.

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

### Final acceptance: self-contained eight-slot tape suite

This is the required final test design. The implemented M4 subset and its
copy-ready artifacts are described below. The combined M6 implementation,
current coverage ledger and staged operator sequence are at
[`test/sdcard`](../../test/sdcard/README.md). Its resident candidate uses the
8096 64 KiB expansion, a shared 41-case smoke prefix and a 152-case main
schedule. These are aggregate reporting cases, not a claim that the complete
matrix is covered. Required gaps remain explicit in
[`coverage.json`](../../test/sdcard/coverage.json). There is not yet a qualified
copy-ready M6 package, and the complete final matrix is not accepted.
Successful writable roles persist checksummed per-metric maxima/counts on
device 8/0, then reread every report byte with exact EOI. The PC audit retains
decoded main/warm reports alongside independent file/index/BAM verification.
Cold remains strictly read-only, with raw timing evidence retained on screen.
Performance differences from VICE never abort compatibility acceptance.
The operator mounts eight distinct disposable, writable image files, loads
one main `.prg` from tape and runs it. The program performs the entire
applicable batch automatically across all eight slots. No per-drive selection,
manual DOS commands, preinstalled test files, disk-based checker loading or
host-generated test-data copies are required. The selected acceptance setup
authorizes destructive writes to all eight images and must clearly identify
them as disposable before the program runs.

The checker carries its case tables, model/ROM-specific expectations and
small literal payloads. Larger payloads are generated deterministically in
bounded RAM. The image files need only be valid, mountable containers of the
declared format. The checker formats them and installs every file, raw-sector
pattern, directory arrangement and later REL fixture that its cases need.
Do not require the old `M1 FIXTURE`, `M3N509` or checker PRG to exist on disk.
The executable remains in PET RAM while any mounted image is destroyed.

The default batch uses this fixed profile, with both slots of each unit
having the same model:

| Global slots | Device/local drives | Images | Purpose |
|--------------|---------------------|--------|---------|
| 0, 1 | 8/0, 8/1 | Two D64 | 4040 pair, both selector directions |
| 2, 3 | 9/0, 9/1 | Two D80 | 8050 pair, both selector directions |
| 4, 5 | 10/0, 10/1 | Two D82 | 8250 pair, both selector directions |
| 6, 7 | 11/0, 11/1 | Two D64 | Device-11 coverage and repeated-model unit isolation without redundant largest-media scans |

Use the same explicit slot table in all phases: device is `8 + slot / 2`
(integer division) and local drive is `slot modulo 2`. The model comes from
the declared profile, not a DOS-version string alone (8050 and 8250 share
DOS 2.7). Preflight checks all four units and all eight media before the
destructive batch. An absent, wrong-model, inaccessible or unexpectedly
read-only slot is a failure, never an omitted drive or an automatic smaller
test run. Alternate controller/model profiles must retain eight-slot coverage
and carry their own approved expectations.

#### Tape phases and reset boundaries

The table names the combined M6 target, not the historical M4 artifacts below.
Each PRG contains everything required to execute its own phase, including
generators and expected results. A separate `M6SMOKE` entry stops after the
identical smoke prefix. The main entry continues into bulk work automatically.
Calibration PRGs are not acceptance executables.

| Tape PRG | Automated work | Operator action at completion |
|----------|----------------|-------------------------------|
| `M6ACCEPT` | Preflight all eight slots, invalidate old completion evidence, format and generate fixtures, run the applicable batch, then install and verify suite/generation-bound warm checkpoints on every image | Record the run identity, short-press MENU without selecting a configuration, and tape-load `M6WARM` |
| `M6WARM` | Before any fixture writes, check all eight warm checkpoints and expected corpus/REL contents. Then install, close and verify cold checkpoints on every image | Power-cycle with the same configuration and images, then tape-load `M6COLD` |
| `M6COLD` | Read-only verification of all eight cold checkpoints and expected corpus/REL contents. Report remaining external gates separately | Retain the screen/results and perform the single PC-side card audit |
| `M6BENCH` | Run the full D64-first workload on separate disposable benchmark media, report slower windows without stopping, and retain all contextual maxima/sample counts | Optional stage 5, outside acceptance continuations. Export JSON/CSV comparisons with the bundled read-only benchmark helper. Benchmark completion is not acceptance |

#### Implemented M4 artifacts

The copy-ready package is `build/m4-acceptance/`, also archived as
`build/m4-acceptance.zip`. It contains three independently tape-loadable
programs in `prgs/` and eight pristine empty containers in
`disks/m4-accept/` (two D64, two D80 and four D82). The main program formats
the containers and generates its own fixtures. `manifest.json` records
program/image hashes and the exact corroboration evidence.

Sources, case coverage and reproduction instructions are in
`prgs/accept/README.md`. Build with `cmake --preset default` followed by
`cmake --build --preset prgs`. The corresponding build artifacts are
`build/prgs/accept/bin/accept.prg`, `accept-warm.prg` and `accept-cold.prg`.
The package's `suite-details.md` preserves the implementation runbook.

Copy the packaged `prgs/` contents to SD `/prgs/` and `disks/m4-accept/` to
SD `/disks/`. Append `mounts.yaml` to a dedicated ROM-4 configuration's
setup, not over the entire `config.yaml`, and select it as the boot default.
Run `LOAD "ACCEPT"` then `RUN` without an IEEE device number. Require
`PHASE 0 PASS`, short-MENU reset, then `LOAD "ACCEPT-WARM"` and `RUN`.
Require `PHASE 1 PASS`, power-cycle with the same images/configuration,
then `LOAD "ACCEPT-COLD"` and `RUN`. Require `PHASE 2 PASS`. Never restore
the initial empty containers between phases.

The matching PRGs completed 360 main, 32 warm and 8 cold case groups in
VICE 3.9, with all eight slots covered in every phase. Four read-only
rejection checks covered missing checkpoints, swapped slot identities,
wrong phases and truncated corpus data. Independent c1541 extraction
checked all sixteen final files. Evidence is
`.cache/ai/acceptance-timeout-fix-v1/result.json` (the original run is retained
at `.cache/ai/acceptance-v1-r4/result.json`). These are main-DOS-ROM/C-FDC
corroboration results, not production firmware/SPI/FPGA or board acceptance.
The physical tape, MENU and power-cycle paths were substituted by monitor
loading, PET soft reset and a fresh emulator process.

This is a broad M4 batch, not the full final matrix. Outstanding cases include
complete raw NEW/DUPLICATE effects rather than samples, deliberately
corrupted-BAM repair, full-disk fixtures, exhaustive parser precedence,
cross-unit destructive-command/reset isolation and a pure-prefix truncation
negative. M5 REL and the external fault/timing gates are not implemented by
these PRGs. The first board attempt failed as described below. The operator subsequently
reported the corrected subset passing 360 main, 32 warm and 8 cold case groups
on all eight slots (2026-10-28). This accepts that subset, not the full M4 matrix.

##### First board run: case 10 receive timeout

The first physical run passed all eight case-1 preflights, then failed on
slot 0, case 10. Its first status read returned CR (13) with `ST=2`
(receive timeout), rather than a complete DOS status. The printed
`offset 256, expected 246` values were leftover raw-sector generator state,
not evidence of a bad byte at a disk offset. Case 10 was not accepted.

The original checker forced `BOB` (`$03FC`, decimal 1020) to zero. This
selects one VIA timer period as the IEEE receive deadline, even while a
healthy full NEW is still working. The corrected checker writes `$80`,
selecting the KERNAL's STOP-key-aware timer retry, and restores the original
policy on normal completion or an explicit checker failure. Failures also
print the last DOS command. This is a controller/checker correction, not a
firmware speedup or a change to native DOS status/EOI expectations.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| IEEE wait during a long DOS command | PET ROM 4 `901465-22`, 4040 full NEW | [ob1src: ACPTR/ACP00/ERRS4/ERRP1](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/KERNAL_PET_4.0_REC/ob1src), [dos.write: FORMAT/FERRS/EREAD](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/KERNAL_PET_4.0_REC/dos.write) | With BOB nonnegative, timer expiry returns CR and merges ST bit 1. With BOB negative and STOP not pressed, retry the timer and wait for DAV. A successful format returns `00, OK,00,00` followed by CR with EOI. | `prgs/accept/accept.bas`, `DriveFormatWaitTest.Rom4FullFormatStatusWaitsBeyondOneTimerPeriod` | Production firmware through SPI/FPGA with deliberately spaced cooperative format work reproduces CR/ST=2 under the old policy and complete exact status/EOI under the corrected policy. VICE runs the tape programs separately. |

The regression runs both policies with the same healthy media and native
KERNAL. It does not accelerate or complete the format before starting the
status read. The short-policy timeout does not cancel the underlying NEW,
so test cleanup finishes that work before retiring media. Logs are
`.cache/ai/m4-case10-regression.log`. Updated all-eight-slot tape evidence
is `.cache/ai/acceptance-timeout-fix-v1/result.json`.

The normal native wait can wait indefinitely on a stuck bus. The STOP key
and an independent watchdog remain necessary. A hang is not a PASS and
must retain the printed phase/slot/case. The corrected PRGs subsequently passed
the reported 360/32/8 board rerun. Keep the first failure and its diagnosis as
historical evidence, not as the current status of that accepted subset.

##### Gap-only C continuation

The accepted BASIC programs and their results are retained unchanged. The
separate [M4 gap suite](../../prgs/m4-gaps/README.md) adds only missing checks,
using the measured baseline-6502 receiver and timed, scrolling reporter.
Its media, edge and isolation programs contain 40, 48 and 72 case groups,
respectively, and use fresh `m4-gaps/` images rather than the accepted BASIC
images. Complete generated raw-image oracles, corrupted-map repair,
generated disk-full recovery, pure-prefix rejection, additional parser
precedence and cross-unit destructive/drive-reset isolation are separate from
the completed BASIC warm/cold continuations.

The reported eight-slot receiver benchmark passed 24/24 checks. Assembly took
80 jiffies per 4096 bytes on every slot at 60 jiffies/second. This establishes
neither filesystem correctness nor whole-suite throughput. The new suite's
32 full-image reads alone transfer 22,728,704 bytes. Allow several hours for
reads and generated fixtures on hardware. The operator reported the media
stage passing all 40 groups across eight slots on 2026-10-28
(`pass: 40/40 - failed: 0`). On 2026-10-29 the operator reported the edge
stage passing all 48 groups across eight slots (`pass: 48/48 - failed: 0`),
covering corrupted-BAM repair, exact-prefix rejection, parser/length
precedence, retained-drive direct OPEN and generated disk-full recovery.
On 2026-10-29 the operator also reported isolation passing all 72 groups
across eight slots (`pass: 72/72 - failed: 0`), including full/quick NEW,
DUPLICATE, INITIALIZE and U9/UI/UK while three peer units retain data and
partial-status cursors. All three gap-suite board runs are complete
(160/160 groups). The retained BASIC runs bring the reported acceptance total
to 560 groups, plus the separate 24/24 benchmark. No further tape run or
reset is required for this batch. Deployed firmware/FPGA identities and
external fault/mount/timing evidence remain pending. No new real-drive
capture is an actionable prerequisite when that hardware is unavailable.

The final gap executables passed approved main-ROM/C-FDC corroboration:
40/40 media, 48/48 edge and 72/72 isolation groups. Each missing-drive negative
stopped read-only with the correct failed/unrun summary. All eight final
media images match the independent complete-image oracle, and c1541 verified
all final edge/isolation file bytes. Exact result paths and hashes are in the
[gap-suite runbook](../../prgs/m4-gaps/README.md#validated-local-results).
The frozen package is `build/m4-gaps.zip`. Its pending-board metadata predates
all three board reports above and is retained unchanged. The local native
records do not substitute for deployed identities or external gates.

#### Continuing after each phase

The only required interventions after initial setup are the explicitly
requested resets and tape loads/runs. No follow-up asks the operator to loop
over drives, create data, enter commands or select an image.

For this runbook, warm reset means the supported short-MENU `pet_reset()`
path. It retains mounts but resets PET/IEEE state. Configuration reload is
not equivalent: `load_config()` unmounts images first. Cold reset means board
power-off/on, not BASIC RUN/CLR, a drive `UJ` command or another PET reset.
The cold phase must use the same files and profile, without restoring pristine
images. These are orderly persistence checks, not uncontrolled power-loss
fault injection.

Every phase uses disk checkpoints rather than assuming PET or RP2040 RAM
survives. Each checkpoint identifies suite/profile version, slot, expected
next phase and the generated corpus. Every follow-up rejects missing,
inconsistent, wrong-slot or wrong-phase checkpoints. The expected bytes,
lengths, status and EOI come from the PRG's own case/generator tables, not
from treating the media's stored checksum or recorded PASS flag as an oracle.
All eight checkpoints must be validated before a follow-up changes any
image. On failure, preserve the original checkpoint and data for diagnosis.
Close all checkpoint writers and verify readback before requesting a reset.

#### Automated case and data contract

The suite loops over every slot for single-image cases, every unit for
pair-wide cases and both copy directions for DUPLICATE. It must not sample
only one drive of each model. Distinct device/drive/case tags and deterministic
binary patterns expose slot aliasing, wrong-drive access and cross-unit state
leaks. Include empty/singleton files, sector/record boundaries, multiple
directory pages, append/replace, wildcard and error-precedence cases, resource
exhaustion/recovery and the complete M5 REL boundary cases. Generate full-disk
fixtures by writing them, rather than asking for prefilled media.

Full and quick NEW and sector DUPLICATE are destructive subtests, not manual
preparation steps. Recreate needed fixtures between them automatically.
Check NEW's exact header/BAM/free counts and complete sector-data effects
against independently generated expectations. Check DUPLICATE against its
source, including unused-sector patterns, and verify that the source is
unchanged. A duplicated target intentionally has its source's tags until the
checker reinstalls target-specific fixtures. Reseed and verify all eight
slot identities before writing reset checkpoints.

Interleaved tests must exercise both slots and all four devices, including
paused cursors, status selection, interrupted/resumed transfers and
independence from another unit's destructive command or drive reset.
Schedule logical files and secondary addresses within both the PET KERNAL
and native per-unit limits. Do not assume eight data files plus four status
files can all be open, or close command channel 15 while relying on that
unit's data channels surviving. Global PET reset is a separate phase boundary,
not a per-unit independence test.

Every case reports phase, case ID, device/drive, expected versus actual data
or status/EOI, and PASS/FAIL/BLOCKED. Print progress before potentially long
operations so a hang identifies the active case and slot. Use bounded loops
and the selected KERNAL's supported timeout behavior, with cleanup on failure.
An independent watchdog is still needed to bound a firmware or hardware hang
that prevents the PET's timeout path from running. Never turn a missing suffix,
timeout or unvisited slot into PASS. Summaries distinguish fully completed,
failed and blocked phases and preserve the first discrepancy across resets
where checkpoint writing remains safe. Missing or partial prior-phase evidence
cannot yield an overall PASS.

The tape suite cannot by ordinary DOS commands change host file permissions,
replace a slot with an unlike container, inject an SD write/flush failure or
measure unobservable MCU headroom/electrical handshake phases. Automate those
matrix cases with explicit test-control/instrumentation support where available
and link the resulting evidence. Until then, report the corresponding final
gates as blocked, not covered by a successful eight-writable-image run. This
does not add manual per-drive setup steps to the tape suite or waive the
original-controller reference and physical acceptance requirements.

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
  Explicit operator-approved deferrals may carry a case to a later milestone
  without marking it passed.
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
