# Production ROM-backed IEEE drives

## Status and decision

This is the implementation plan, not a claim that the production system or
8050/8250 emulation already works. The 4040 proof of concept establishes that
an unmodified main DOS ROM can own filesystem, command, channel and REL
behavior while C implements its controller-facing hardware contract.
RP2040 throughput with DVI remains an unmeasured acceptance gate.

Preserve the experiment as an independently reproducible reference:

- The tested POC is committed and pushed on `ieee-6502` at
  `8e49617c71f2f853f040aad24e3eb7e486fd63ce`.
- `drive-6502-prod` starts from the fetched head of `main`,
  `5ed74c4335047e27b950af5dd976fce0270ea2bc`, not from the POC. Its initial
  change is this plan only. The existing production code and tests remain
  intact until deliberately migrated or replaced.
- Keep `ieee-6502` checked out in a separate reference worktree at
  `.tmp/ai/ieee-6502-reference/`. It shares Git objects, not source edits or
  build outputs, with the production worktree. Keep both worktrees' builds
  separate and initialize required submodules independently.
- The pre-POC baseline `d5c1804` explains the experiment's lineage.
  Prefer the current `main` geometry, storage lifecycle code and tests,
  including changes since that baseline.
- [The pinned POC report](https://github.com/DLehenbauer/econopet/blob/8e49617c71f2f853f040aad24e3eb7e486fd63ce/docs/dev/ieee-6502.md)
  records its experiments and limitations.
  Keep that evidence historical. Record new model-specific evidence here or
  in the production test reference as implementation proceeds.

Do not merge or cherry-pick the wholesale POC replacement commit into
production. Port selected mechanisms and tests from the reference worktree
into the planned module boundaries. This avoids inheriting its experimental
deletions, device-8 restrictions and incidental configuration changes.

The POC checkpoint passes 181 firmware checks (including 12 ROM-free 4040
regressions), 591 system tests, all eight configured independent-reference
and integrity checks, both IEEE benches under Icarus and Verilator, native
ROM/FIFO probes, and the production ARM build. Those counts describe that
checkpoint, not a required future test inventory or complete compatibility.

## Scope and compatibility target

Support four independently stateful IEEE devices, 8 through 11. Each device
has two media positions, drive 0 and drive 1, and runs exactly one selected
native model. Different devices may use different models simultaneously.

| Model | Selected target | Media per populated position | Raw bytes | Tracks / sectors |
| --- | --- | --- | --- | --- |
| 4040 | DOS 2.0 revision 2, schematic 320806 | D64 | 174848 | 35 / 683 |
| 8050 | Full-size DOS 2.7 MPI, PCB 8050002-05 | D80 | 533248 | 77 / 2083 |
| 8250 | Full-size DOS 2.7 MPI, PCB 8050002-06 | D82 | 1066496 | 154 / 4166 |

Model selection follows the complete pair of mounted formats. A missing
partner stays absent and must never redirect to the other position. Two
populated positions must have the same format, including D80 versus D82.
An all-empty, unconfigured device is disabled and releases its bus
contribution. No new configuration syntax for an empty-but-powered drive
model is required in this implementation.

Keep the existing configuration concepts: `device`, `drive`, and a filename
relative to `/disks`. Restore devices 8-11 and the public slot mapping
`slot = (device - 8) * 2 + drive` where existing callers use slots. Validate
that conversion once at the facade boundary. Internally distinguish a
logical IEEE device, a native backend instance and a media position.

Explicit exclusions:

- The old C DOS implementation, filesystem adapters and flat `.hdd` format.
- Mixed media within one device, including a D80/D82 pair.
- Arbitrary downloaded 6504 programs. Recognized native startup and formatter
  payloads remain required, not optional custom-code support.
- DOS 2.5, 8250LP, SFD-1001, third-party controller revisions and raw GCR media.
- Appended error maps, extended-track D64s and other container variants until
  separately specified and tested.
- Rotational scheduling, artificial seek delays and matching original-drive
  execution speed.
- New support for physical-drive coexistence on an enabled virtual bus.
  The current transport masks external IEEE devices when enabled. Retain
  and document that restriction rather than quietly claiming coexistence.

Execute the selected main ROMs without patches or command interception.
Hardware emulation must preserve bytes, EOI, status, ordering, memory effects
and native controller results within the supported raw-media contract.
ROM reuse is a strong simplification, not proof of universal parity:
controller replacement, virtual handshake ordering and lifecycle policy
still need independent evidence.

## Architectural rules

The ownership split is the central design decision:

1. The native ROM owns DOS policy: names, parsing, BAMs, directories, channels,
   buffering, allocation, status, EOF and REL.
2. The emulated machine owns CPU-visible hardware: address decoding, physical
   RAM, RIOT register semantics, board wiring and interrupt composition.
3. The replacement controller owns the omitted processor's observable
   protocol: startup, job dispatch, shared state and raw-sector results.
4. The media layer owns validated backing bindings, exact I/O, generations
   and write/flush lifetime. It does not interpret a filesystem.
5. The transport owns the shared FPGA endpoints, ordered event delivery and
   consumption attribution. It does not implement DOS commands.
6. The public facade owns configuration and lifecycle transactions.

Use explicit structs and caller-owned storage, not independent module-global
singletons. The production facade may contain one fixed bank of four native
instances. Lower layers must also be constructible independently in tests.
ROM images are borrowed immutable data, while RAM, controller state, FIFO
phases and fault state are private to each native instance.

Do not introduce a generic plugin framework, arbitrary model registration or
an interface for every helper. Use a small model enum and immutable selected
profile. Share proven mechanisms, but keep the two controller families'
semantics visibly separate. Keep per-cycle paths direct and small. Do not
replace a simple decoder branch with a chain of heap objects or indirect
calls without measuring the cost.

### File and module map

All firmware modules below are under `fw/src/ieee/`. Names are the intended
production names, replacing the POC's model-specific names where appropriate.

| File | Owns | Must not own |
| --- | --- | --- |
| `ieee_drive.c/.h` | Existing application entry points, four logical-device bindings, slot conversion, mount/reset/unmount orchestration and user-facing diagnostics | CPU instruction dispatch, DOS commands, filesystem interpretation |
| `drive_unit.c/.h` | One native backend: machine, controller and virtual peer, startup/step/quiesce/fault state and per-instance metrics | Global SPI queues, FILE ownership, logical command routing |
| `drive_machine.c/.h` | `m6502`, CPU pins, native RAM, model-selected memory decoder, board pin composition, RIOT1 IRQ wiring and bus-cycle job observation | FILE/FatFs, SPI, format loops, filename/channel semantics |
| `drive_riot.c/.h` | One MOS6532's ports/DDRs, PA7 detector, timer, flags and read/write side effects | Device address, IEEE polarity, board-specific wired inputs |
| `drive_model.c/.h` | Immutable supported profiles: image type, ROM layout, decoder family, controller family and startup/payload metadata | Mutable state, media ownership, a table-driven C implementation of DOS |
| `drive_rom.c/.h` | Loading, validating and sharing the two immutable main-ROM sets, publication and borrowing rules | Per-unit ROM copies, patched ROMs, media selection |
| `drive_controller.c/.h` | Common request/completion contract, bootstrap/job observation and bounded stepping, shared result types where actually common | A geometry-only approximation of every controller revision |
| `drive_controller_4040.c` | Verified 4040 startup, jobs, physical-header effects and stock formatter | DOS 2.7 assumptions |
| `drive_controller_8050.c` | Selected MPI DOS 2.7 startup, jobs, mode/side and shared-control effects for 8050 and 8250 | A second main-DOS implementation |
| `drive_fifo.c/.h` | One native peer's READY/ACCEPT/RELEASE and offered/consumed/abort phases, physical line levels | SPI accesses, unit selection, PET command parsing |
| `drive_transport.c/.h` | One shared FPGA RX/TX/TXS owner, ordered fan-out, pause/reconcile/flush/resume, owner epochs and runnable-unit scheduling | ROM private variables, raw disk access, filename routing |
| `drive_media.c/.h` | Raw media descriptors, homogeneous-pair validation, generation-bound references, range/protection checks, dirty tracking and flush-before-retire | FILE opening/closing, BAM allocation, DOS error strings |
| `drive_file.c/.h` | Retained FILE handles, SD path opening, exact seek/read/write/flush, size probing and close-error reporting | Model-specific native result selection or channel state |
| `diskimage.c/.h` | Pure D64/D80/D82 geometry, exact-size classification and sector-to-offset translation | Directory walking, file lookup, streams, chains, REL or `.hdd` |

Keep helpers private unless another module needs them. In particular, do not
export private memory handlers for tests. The CPU-bus fixtures remain the
integration contract for address decoding and side effects.

`drive_controller.c` is a thin coordinator, not another abstraction stack:
model-specific operations live in the two family files. Do not copy the same
controller three times merely because there are three advertised models.
8050 and 8250 share approved DOS/controller bytes, but their mode and media
geometry remain explicit inputs with separate evidence.

The logical-device facade initially maps each device to one `drive_unit`.
Keep that binding inside `ieee_drive.c` until actual routing behavior
justifies another module. Do not create a speculative hybrid parser now.

### Dependency direction and data flow

```text
configuration / menu / pet_reset / CLI
                 |
             ieee_drive
             /        \
    drive_file/media   drive_transport (single FPGA endpoint owner)
             \        /            |
              drive_unit <---------+
              /    |    \
       drive_machine  drive_fifo  drive_controller
           |                       |       |
       drive_riot              family code  drive_media
           |
    selected drive_model + borrowed drive_rom bytes

drive_media -> diskimage geometry
```

The diagram expresses ownership and calls, not file-include permission.
Pass non-owning callbacks or narrow borrowed views where necessary to avoid
header cycles. A controller request identifies its backend, media generation,
slot, job, header and buffer. The controller does not search for an owning
unit through global variables.

The CPU bus stores a submitted job, the replacement controller performs the
supported hardware operation, then publishes native completion only after
the operation's required buffer/header/media effects. The main ROM observes
the result by its native shared-memory polling. Do not invent a completion
IRQ or NMI.

## Reuse decisions

Do not start from scratch, and do not restore the old files wholesale.

| Source | Reuse | Remove or redesign |
| --- | --- | --- |
| POC `rom4040.*` | Pinned CPU core, cycle stepping, tested physical RAM aliases, board logic and RIOT behavior | Split peripheral mechanism from board wiring, remove hard-coded device 8, add the distinct DOS 2.7 decoder and startup observations |
| POC `fifo4040.*` | Handshake state machines and one-outstanding-byte contract | Rename, remove concrete `rom4040_t` coupling through the native-unit boundary, strengthen required invariants |
| POC `controller4040.*` | Independently verified job/header/error semantics and raw callback approach | Complete payload recognition, explicit per-instance/media lifetime, bounded formatter progress and family separation |
| POC `ieee_drive.*` | Application entry points, metrics, consumption-before-ATN ordering, ROM identity checks and exact stdio I/O | Replace singleton state, broad boot-on-mount behavior and embedded SPI scheduling with owned modules |
| Current `main` `diskimage.*` | Native zone tables, D82 side folding, exact raw sizes and geometry tests | Retire `diskimage_find`, directory entries, file streams, chain caches and HDD APIs at replacement cutover |
| Current `main` `dos_media.*` | Pair validation, borrowed descriptors, unit-local generation invalidation, pre-write dirty marking, flush failure retention and their tests | Rename to `drive_media`, decouple from the old rich `diskimage_t`, remove DOS-specific naming and unused BAM convenience access |
| Current `main` `ieee_drive.c` | Slot mapping and transport-owner history as reference | Retire the C status formatter, command interpreter, channel/REL tables, allocation and read-ahead caches at replacement cutover |

There is no current `drive_media.c` to copy. The reusable foundation is
the production worktree's `dos_media.c`, documented in
[drive-media.md](drive-media.md). Preserve its generation and failure-retention
contracts while separating them from the DOS adaptation. The POC removed
that foundation, but those deletions are not part of this branch's starting
point. Update the foundation document when its implementation is migrated.

Keep physical-image identity acquisition at the controller/model boundary.
Raw containers encode an initial physical disk ID in model-specific header
metadata. Reading those bytes to construct physical header observations is
not permission for media code to maintain a DOS BAM cache. Ordinary sector
writes must not silently replace the controller's remembered physical ID
with newly written BAM/header bytes. Define format, remount and failed-format
ID changes from the selected controller evidence. Physical identity belongs
to the mounted media generation and survives a CPU/native-device reset.
Do not rebuild it from potentially modified filesystem bytes on every reset.

Retain the CPU core's existing submodule pin and license notice. Do not
import GPL implementation code from emulator references. Those programs
execute out of process as references.

## Model contracts that must be established first

The existing approved manifest selects:

| Runtime ROM set | Files in `/roms` | Layout |
| --- | --- | --- |
| 4040 revision 2 | `901468-14.bin`, `901468-15.bin`, `901468-16.bin` | Three 4 KiB chips at `$D000-$FFFF` |
| 8050/8250 DOS 2.7 MPI | `901887-01.bin`, `901888-01.bin` | Two 8 KiB chips at `$C000-$FFFF` |

The 8050 and 8250 share the second set, not their machine state. Hardware
mode determines the single/double-sided configuration. Both families have
4 KiB shared RAM and 256 bytes of main-CPU RIOT RAM, but their decode cannot
be unified by moving a ROM base constant alone.

### Address decode and IEEE wiring

The POC 4040 decoder masks A15. The DOS 2.7 map must not do that: `$C000`
is ROM while `$4000` is shared RAM. Applying the POC mask would alias one
onto the other. Preserve each selected board's ROM, shared-block, RIOT and
stack mirrors and documented unmapped-bus behavior explicitly.

Extract the common 6532 mechanism once. Keep DDR composition, pullups,
ATN inversion, DIO wired feedback, device-address straps and LED pins in
the board layer. RIOT0 and RIOT1 have independent PA7 and timer state.
Only the selected board's wired RIOT IRQ reaches the main CPU (RIOT1 for
these profiles).

Use the native address straps for devices 8-11. Do not patch `LSNADR`,
`TLKADR`, ROM operands or received primary commands to impersonate another
device. Verify each model boots with the proper RIOT1 PB address input.

### DOS 2.7 startup is not the 4040 handshake

Primary `DOS_8250/dskint` shows several obligations absent from the POC:

- A reset payload at `$1100` sends the controller to its reset entry.
- Main-CPU ROM and shared-RAM diagnostics run before controller readiness
  is consumed.
- The main CPU expects `$1000 == 1`, copies `CNTLRI` to the shared buffer,
  clears `$1000` to request execution, then waits for a new nonzero result.
- The recognized initialization payload publishes controller variables,
  hardware mode and readiness. Diagnostic results at or above `$FD` are
  errors, not successful boot.

Implement a model-specific startup state machine reacting to actual CPU
bus stores. It must not depend solely on stores to the fifteen job bytes.
Do not reuse the POC's final-RAM-clear hook and controller ID `$0F` as the
DOS 2.7 readiness protocol.

Trace and verify the exact selected binary for controller-produced `DOS`,
`NSIDES`, `WPSW`, `LWPT`, geometry/control variables, wait flags and any
other location the main CPU reads before writing. The primary RAM map
names them, but the existence of a symbol alone does not determine its
initial value, owner or update order.

Native initialization derives single/double-sided behavior from the omitted
controller-side MIOT PB6. Corroboration has PB6 high for 8050 and low for
8250. C must publish the resulting shared state in the native order, not
pretend that this is a main-CPU RIOT strap.

### Controller jobs and downloaded native payloads

Inventory every producer/consumer interaction for each selected binary:
all job-byte variants, all fifteen headers/buffers, startup flags,
controller variables, disk-change/write-protect transitions, resets and
downloaded payload entry points. `DOS_8250/equate` includes `SECSEK` `$B8`
in addition to the POC's operations. Determine its actual use and observable
effects before deciding whether it shares code with SEEK.

For each interaction record the trigger, valid phase, observed inputs,
written bytes, completion condition, native result and recovery behavior.
Execute CPU-bus tests for non-job shared-memory triggers too. Do not declare
DOS 2.7 supported after only a banner or ordinary sector read.

Recognize the complete executable extent of each approved startup/format
payload and validate its permitted parameter region separately. The POC's
512-byte formatter prefix check is not sufficient production authorization
for an entire downloaded program. Tests must mutate every executable byte,
including bytes beyond that prefix, and reject unknown payloads before
their first media effect. Compare against borrowed approved ROM ranges or
verified identities, not redistributed embedded copies.

Keep normal READ/WRITE/VERIFY callbacks exact and synchronous initially.
They still require explicit I/O and flush results. Make a whole-disk
formatter resumable by bounded raw-sector work so one device cannot block
all other virtual devices for its entire image. Keep the native job pending
until its required effects complete. This is cooperative work partitioning,
not simulated mechanics. Capture the media identity and job operands before
yielding and revalidate them before subsequent work.

Never claim rollback after a partial write or format. Define and test the
resulting header, buffer, physical ID and media state, distinguishing a host
I/O error contract from independent GCR error/retry equivalence.

## Shared FPGA transport and four-device scheduling

There is one physical RX FIFO and one data/status TX pair, not four
independent SPI transports. The RTL already contains unit listener masks,
per-unit secondary addresses, unit/channel TX ownership, ATN-end tokens,
listener checkpoints and talk-progress events. The POC deliberately ignores
much of that multi-unit surface.

Preserve `main`'s address admission for 8-11 when integrating the ROM-backed
transport. Do not copy the POC's device-8-only comparator. Audit the existing
metadata rather than assuming address admission alone proves composition.
The current control register has a global enable, not a configured-unit
enable mask. Add an explicit four-bit admission mask so unconfigured,
booting or faulted native devices are not acknowledged by the fabric as
live listeners/talkers. Mask changes require the same ordered quiescence
rules as backend replacement. Keep the global disable fully passive.
Document each RX token's meaning and ordering in the shared hardware
contract. Change the register layout only if an actual ambiguity is
demonstrated, updating firmware, RTL, generated/shared definitions, mocks
and tests together. The missing admission control is an identified need,
not a reason to redesign unrelated FIFO registers.

The production transport must:

1. Observe the actual TX/TXS consumption counters before ATN, owner changes,
   flushes, reset or mount retirement. Pause future DAVs and resample after
   the active-byte protection boundary.
2. Deliver every accepted ATN command in order to the native backends that
   must observe it. Native ROMs perform their own address decoding.
   Never leak payload data to an unselected listener.
3. Capture the intended recipient set for each data byte before later
   commands can change selection. Advance a recipient at most once.
   Retire the shared FIFO head only when all required recipients complete
   their synthesized exchange.
4. Preserve ATN-release, listener-checkpoint and talk-progress tokens as
   distinct events. The POC's `if (byte == 0)` handling is not a sufficient
   interpretation of all ATN-end/checkpoint tokens.
5. Retain the source binding as logical device, native backend, secondary
   channel and owner epoch. GET belongs to that binding, not to whichever
   device/channel happens to be selected when firmware later runs.
6. Enqueue at most one outstanding native source byte globally at first.
   Set valid unit/channel ownership for both data and status. Reconcile
   and finish or abort that offer before replacing its binding.
7. Release paused transport only after the ordered event is safe. Resume
   the retained owner, not a fresh guess based on a mutable SA register.
8. Schedule runnable native instances fairly. One stalled listener, talker
   or formatter must not consume unbounded work or starve unrelated units.

Broadcast LISTEN and device-to-device transfers deserve explicit composed
tests, not an assumption that four independent single-unit cases cover
them. A selected talker may also have peer virtual listeners. The data
source, logical byte and EOI must traverse those listeners without duplicate
GET or listener retirement.

### Idle service contract

The POC still executes CPU slices while generally idle. Do not copy that as
the production policy or assume an empty RX FIFO means DOS is idle.

Target: once all native backends are parked at proven quiescent boundaries,
`ieee_drive_task()` does one bounded shared transport poll and returns with
zero emulated CPU cycles if nothing changed. It must not scan four running
6502s on every main-loop iteration.

Derive quiescence certificates from the exact selected ROMs. Parking is
allowed only at a known instruction-fetch boundary with no pending command,
job, startup/format step, IRQ/NMI/SO work, RX phase, source acknowledgement
or other autonomous observable work. A timer that can change control flow
or observable hardware prevents claiming quiescence until its treatment is
proved. Do not turn off timers or clear flags to manufacture idle.

RX/ATN, real GET/unread abort, media/reset lifecycle and pending controller
work wake the appropriate unit. Resume its exact saved CPU state before
offering another byte. Main-CPU RAM execution or changed indirect vectors
must invalidate any assumption that a familiar PC alone proves idle.
Unknown states receive bounded execution, never a speculative sleep.

Emulated timers continue to use actual executed CPU cycles. Do not use
wall-clock elapsed time to add mechanical delays or equate native/reference
phases. Any later fast-forward optimization needs its own side-effect proof.

Retain deterministic cycle/work quotas for active work and a separate
platform cooperative budget if necessary. Metrics and tests must expose
idle cycles, runnable-unit progress, longest service gap and non-preemptible
sector I/O. Choose final active quotas using manual RP2040 measurements,
not the POC's host rate or arbitrary hardware-speed emulation.

## Media and lifecycle transactions

### Prepare, quiesce, publish, retire

Mounting is an explicit host lifecycle operation, not a native DOS command.
Define successful replacement as a cold restart of the affected native
device, including its unchanged partner. Other devices keep their CPU,
RAM, channels, media generations and source ownership unless shared-bus
ordering requires a short pause. Document this policy rather than promising
native hot-insertion semantics.

Prepare the entire proposed pair before publication:

1. Open candidates with retained owners, check exact raw size and supported
   extension/type agreement, inspect required physical-ID source bytes,
   determine write capability and validate pair homogeneity.
2. Ensure the selected ROM set is fully loaded and valid, resource capacity
   exists, and the candidate native instance can pass bounded boot.
   Do not destroy the live instance merely to discover a bad ROM or boot.
3. Stop new work for the affected binding, reconcile actual consumption,
   resolve or explicitly reject in-flight work, and flush the old pair.
   Do not silently drop ROM-resident pending writes or simulate native CLOSE.
4. Publish the new pair, native state and binding generation as one
   serialized operation. Invalidate retained work for both positions in that
   device, not for other devices.
5. Retire only old FILE owners not retained by the new pair and release the
   transport in a state consistent with its next event.

Use a bounded staging native instance or equivalent non-destructive preflight.
Include that temporary memory in the resource budget. If preserving active
native work is unsafe, reject the replacement before changing it rather
than claiming a successful transparent remount.

Prefer four live bindings into a fixed pool with one additional staging
instance. Publish a binding to the successfully prepared instance and
recycle the retired instance, rather than shallow-copying structs whose
FIFO/controller callbacks point into the old instance. Native addresses
remain stable throughout their borrowed lifetime. Preflight stops at the
verified diagnostics/readiness boundary and must not perform destructive
work against an unchanged live partner.

The FPGA RX records do not carry host media generations. A generation check
alone therefore cannot prevent an old queued command from being dispatched
against a newly mounted disk. Reach and verify a safe ordered RX boundary
before publication, or reject/defer replacement. A unit-local change must
not globally flush another unit's accepted events or outstanding source.
Global flush is reserved for explicitly all-unit operations after their
consumption has been reconciled.

Failure before publication retains the old bindings, generation and valid
owners. A partial write or successful early flush is irreversible progress,
not a rolled-back operation. `fclose` consumes a stream even when it reports
an error. A post-publication close failure must be surfaced as a retirement
error, never leave a dangling FILE pointer as a supposedly restored mount.
Separate that case from flush-before-publication failure in APIs and tests.

Do not mount the same writable backing file twice under independently cached
native drives. Reject detectable duplicate writable ownership across the
eight positions (including path aliases where the storage API can identify
them). Read-only sharing is safe. Detection limits for FAT aliases must be
documented rather than hidden behind string comparison alone.

### Identity and flush rules

References carry native backend/position and a monotonic media generation.
Zero means absent. Generations survive native reset and unmount/remount
cycles. Refuse exhaustion before mutation or I/O, never wrap to a live old
identity. The media layer checks identity, geometry and protection before
calling backing storage.

Mark dirty before invoking a writer, because a failed callback may already
have changed bytes. Preserve dirty state after failed writes/flushes. A
successful native WRITE result requires the defined write-and-flush boundary,
not merely copying bytes into a libc buffer. It is not a promise of
power-loss atomicity.

Bulk unmount must flush all affected media before detaching any binding.
On failure retain bindings and all remaining dirty state, while recording
successful earlier flushes. Do not close an unchanged partner merely because
a copied descriptor appears in a new pair.

### Reset and configuration are separate operations

| Trigger | Required behavior |
| --- | --- |
| Native `I`, `UI`, `U9` and other ROM commands | Execute the selected ROM and its recognized hardware protocol. No host remount or blanket RAM clearing |
| `ieee_drive_reset()` / coordinated PET reset | Reconcile/abort transport consistently, cold-start configured native instances, retain image owners and physical media bytes |
| Single-device mount/replacement | Transaction above, restarting only the affected native device |
| Full configuration switch | Preserve the existing halted-PET, unmount-then-apply boundary, with explicit teardown failure handling |
| Independently driven electrical IFC/SO/reset | Remains unsupported unless separately implemented and exercised. Do not describe coordinated reset tests as evidence for it |

Configuration callbacks currently mount sequentially while the PET is halted.
Collect a proposed pair or provide a bounded configuration begin/finish
boundary so the native device is not repeatedly booted and exposed between
the first and second mount. Reject a mixed pair before enabling it.
Do not promise rollback of an entire failed configuration switch: the
existing flow intentionally retires the previous configuration first.

## Future hybrid seam (not implemented now)

Reserve the distinction between logical device and native backend without
introducing a hybrid command interpreter.

A future hybrid binding could own two native backends with the same logical
IEEE address: one populated only at position 0, the other only at position 1.
The facade would route explicitly disk-targeted transactions to the
corresponding backend and non-targeted transactions to the most recently
accessed backend, as requested. Each backend keeps separate DOS RAM,
channels, status, physical IDs and controller state.

Prepare for that by keeping:

- External device identity and internal backend identity separate in the
  facade, transport ownership and media references.
- Raw receive/selection events above the native-unit boundary.
- Native machine/controller code unaware of filename routing.
- No shared global "current disk", status channel or DOS RAM.

Do not implement media-mixing acceptance, prefix rewriting, dual backend
allocation, command replay or most-recent tracking in this production phase.
Homogeneous-pair rejection remains a tested public contract.

The future routing project must specify channel lifetime after a disk is
selected, status consumption, binary commands without textual disk prefixes,
relative P positioning, memory/user commands, reset, untargeted operations
and commands touching both disks (copy/duplicate/rename). Routing an OPEN is
not enough: later TALK/WRITE/CLOSE must stay bound to the owning backend.
The current plan makes those policies possible without deciding them or
accidentally reintroducing a C DOS implementation.

## ROM acquisition and packaging

The actual repository script is
[`.devcontainer/download.sh`](../../.devcontainer/download.sh), not
`downloads.sh`. Extend that existing path and its cache/fallback/extraction
mechanism rather than adding a second downloader.

Install the five main chips listed above into the same
`$ECONOPET_MEDIA_DIR/roms/` directory as the PET ROMs, with those exact
basenames. No separate `drive-roms/` runtime directory and no firmware
embedding.

Also acquire reference-only inputs into `roms/`:

| Input | Purpose |
| --- | --- |
| `901466-04.bin` | Original 4040 controller reference |
| `901869-01.bin` | Selected 8050/8250 MPI controller reference |
| `901467.uk6` | 4040 hardware GCR reference PROM |

Determine and record any additional lookup/device ROM required by the
selected independent 8050/8250 reference. Do not assume the 4040 GCR PROM
is used by the different DOS 2.7 electronics. These reference inputs are
not required to boot production, which does not execute a 6504.

Use [the approved manifest](../../tools/drive-roms/drive-emulation-roms.json)
as the identity authority for all seven drive chips. Drive payload
verification must include exact length and SHA-256, after extraction or
splitting, not merely an archive checksum or successful HTTP response.
Keep existing PET MD5 rows working when extending the verifier's record
format. Do not downgrade drive identities to match a convenient download.

Implementation work includes:

1. Add explicit drive records and verified primary/fallback sources. Prefer
   stable, revision-pinned fallback assets. Test individual chip/archive
   names rather than assuming a combined VICE image is three chip files.
2. Reuse staged `.part` writes, payload verification, invalid-cache eviction,
   retry/fallback and atomic rename. Interrupted, truncated, HTML-instead-of-
   ROM and wrong-revision responses must fail without replacing valid media.
3. Keep runtime descriptors compact: exact part/size/approved checksum,
   ROM address and set membership. Add a consistency test against the
   identity manifest so downloader, firmware catalog and reference tools
   cannot drift silently. Introduce a generator only if this becomes simpler
   than checking the small fixed catalog.
4. Load and validate an entire immutable ROM set before publishing it.
   Report missing/invalid part names and selected model explicitly. A
   failure must not invalidate ROM bytes already borrowed by other units.
5. Update the Dockerfile's narrow `.devcontainer` build-context COPY rules
   if verification needs another file. A script that works only from a full
   repository checkout is not sufficient.
6. Verify `sdcard/CMakeLists.txt` stages the files under SD `/roms`, dependency
   changes refresh staged media on an incremental build, and the resulting
   package can boot each configured model. Restore relevant D80 sample
   mounts in `sdcard/config.yaml` only when DOS 2.7 is actually enabled.
7. Test downloads using local synthetic fixtures and mocked network tools,
   not a network request on every unit-test run. Include first download,
   valid/invalid cache, fallback, extraction, checksum/size failures,
   interrupted transfer, rerun and Docker-context availability.

No ROM bytes, archives, disassemblies or reference disk captures belong in
Git. Respect the existing binary redistribution restrictions separately
from local acquisition. The SD packaging and CI image/artifact pipeline
currently copy external ROM directories. Explicitly decide and enforce
which private/local outputs may contain them, and prevent unapproved public
release or container publication from being mistaken for permission.
Updating a hash manifest does not grant redistribution rights.

## Resource and failure budgets

Use one shared 12 KiB 4040 ROM image and one shared 16 KiB DOS 2.7 image.
Four copies of the 4352-byte native RAM contribute 17408 bytes. Those ROMs
and native RAM alone total 46080 bytes (45 KiB), before CPU/peripheral
state, adapters, metrics, eight FILE/FatFs owners, stack, staging, DVI,
USB, filesystem buffers or other firmware.

The POC ARM build observed 305332 bytes of text and 42160 bytes of BSS.
Do not add 45 KiB to or subtract it from that BSS figure blindly: the
checkpoint already includes one ROM set and one native instance.

Require linker/map accounting for one/four active devices, both ROM sets,
the temporary mount/boot instance, and worst formatter/VERIFY stack depth.
Use compile-time checks for fixed capacities, valid model ranges, header/
buffer bounds, SRAM reservations and representable offsets. Do not allocate
whole disk images, directory chains or per-file REL caches in production.
Avoid a maximum-ROM-sized array inside every unit.

Classify errors deliberately:

| Class | Handling |
| --- | --- |
| Invalid configuration, mixed media, unsupported file size, missing ROM | Logged/notified preparation failure, preserve live device if replacement was not published |
| Native media errors | Family-specific native controller result and required header/buffer effects |
| Host seek/read/write/flush/close failures | Explicit structured storage/lifecycle error, mapped only where native mapping is defined, retain diagnostic cause |
| Unsupported downloaded controller code or unrecognized hardware protocol | Fault the affected backend explicitly, stop its CPU, safely release its transport contribution, retain diagnostic context and do not fabricate successful DOS status |
| Broken internal ownership/capacity invariant | Production `vet()` failure with device/backend/PC/job/owner context as applicable |

Do not use `vet()` to crash on a documented user/media error that the API
must reject. Conversely, do not compile away required ownership checks.
Use `assert()` only for paranoid internal checks already protected by the
production contract, and framework assertions for test expectations.

### Invariant and enforcement checklist

| Invariant | Owner and enforcement | Required negative/boundary test |
| --- | --- | --- |
| Each logical device has one native backend now and one homogeneous pair | Facade/media validation before mutation | Every ordered format pair, either empty partner, D80/D82 rejection |
| ROM bytes never change or disappear while borrowed | ROM loader publication/borrow contract | Failed second-set load, shared users, reset/rebind |
| Native RAM and flags do not cross units | Native-unit storage, static bounds | Same address/SA on devices 8-11 with distinct contents |
| Board decoder is selected before CPU RESET | Machine profile validation | 4040 A15 aliases versus DOS 2.7 `$4000/$C000` distinction |
| Every job's header and buffer remain in its instance | Static layout checks plus `vet` at callback boundary | All fifteen slots, mirrors, buffer ends and malicious RAM stores |
| Native completion follows all required effects exactly once | Controller state transition checks | Poll/read/rewrite history, interrupted format, callback failure |
| RX head is retired only after all intended recipients finish | Transport recipient mask and `vet` on retirement | Two/four listeners with unequal service gaps and ATN interruption |
| Offer is not GET, and GET is attributed once to its original owner | Transport source binding/epoch, `vet` on modulo delta and consume | Private reads, both abort sides, wrap, owner/SA switch and reset collision |
| Presentation count equals accepted plus unread-aborted plus outstanding | Adapter/transport counters and test oracle | Seeded persistent histories at each transition |
| No job can write replacement media through an old reference | Media generation check before callbacks | Rebind during yielded work, unrelated unit survives, exhaustion |
| Failed writes remain dirty and failed flushes retain bindings | Media state machine | Partial writer, second-slot flush failure, bulk teardown |
| Closed FILE owners are never reused | File/facade ownership and retirement result | `fclose` failure, unchanged partner, rejected candidate |
| Parked state has no unserviced autonomous work | Unit quiescence certificate with invalidation | Timers/IRQs, RAM code/vector changes, pending command and handshake |
| Shared endpoint state changes only after consumption reconciliation | Single transport owner and transition checks | ATN/flush/pause/GET on the same boundary |
| No external failure is silently successful | Typed error/result paths and required logging | Missing ROM, unknown payload, I/O failure and malformed reference output |

Validate public indexes and packet shapes normally. Use `vet()` on
internally impossible states after validation, including a second outstanding
source or an impossible counter delta. Checks must not depend on Debug-only
behavior.

## Test reuse and expansion

Keep the POC tests as regression assets, not a second production
implementation. Move or parameterize fixtures only when the replacement
tests retain the same observations and demonstrate they detect the same
failures.

| Layer | Reuse from POC / baseline | Production additions |
| --- | --- | --- |
| Ordinary firmware Check | All 12 `ieee4040` cases, selected baseline geometry and `dos_media` cases | Peripheral/module-focused suites, three geometries, four instances, staged mount/flush/close failures, idle work accounting |
| Ordinary gateware | `ieee_tb` and `ieee_sys_tb`, both engines, GET/abort/private-read/pause/flush/wrap cases | Address 8-11 admission and configured-unit mask, explicit owners, multi-listener masks/checkpoints, data/status isolation and simultaneous event cases |
| CPU-bus hardware contracts | `hardware4040_test.cpp` and its safe instruction-fetch patch boundary | Separate decoder/startup profiles, all DOS 2.7 shared-control interactions, native address straps and persistent four-instance histories |
| Native ROM/full board | `rom4040_test.cpp`, PET KERNAL 2/4, production top and physical SPI | Model x address, two media positions, mixed models across devices, broadcast LISTEN, peer transfers, resets and lifecycle |
| Independent peripheral | RIOT MAME replay and corruption checks | Retain phase-correct comparisons and add any newly used semantic surface |
| Independent controller | Five native 4040 scenarios, strict capture parsing and exact final images | Both DOS 2.7 hardware modes, startup/shared variables/SECSEK, side boundaries, protection/change state and formatter effects |
| Independent full wire | Existing pinned instrumented MAME capture/replay toolkit | Execute selected production scenarios with exact model/device identities, not just build the tools |
| Direct ROM/FIFO probes | Bounded original-ROM read/write/REL and standalone RTL probe | Retarget production entry points, add selected DOS 2.7 smoke coverage without a private fork of emulator logic |

Keep exhaustive finite geometry/peripheral/job enumeration in fast tests.
System tests should prove composition, not duplicate the whole DOS
conformance space on the slowest board model. Expected geometry, media
patterns and protocol histories must come from independent fixture models,
not the production helper under test.

### Essential new test sets

**Geometry and media:** every valid sector in all three formats maps once,
with correct first/last bytes and no overlap. D82 track 78 starts at byte
533248 and track 154 remains bounded. Enumerate invalid coordinates and
exact-size rejection. Exercise every pair combination, all eight media
positions, read-only sharing, duplicate writers, generation exhaustion,
stale references, partial writes and flush/close failures.

**Native hardware:** retain the POC's exhaustive RIOT register/mirror,
DDR/input, PA7, timer boundary, read-side-effect and IRQ checks. Test
both model decoders and address straps. For DOS 2.7, test startup progress
without mailbox stores, all producer/consumer shared flags, payload
mutations, controller completion effects and reset/recovery.

**Transport:** reuse deterministic seeds `1`, `$4040` and `$C0FFEE`, then
vary unit, model, channel, service gaps and receiver fan-out. Check exact
wire bytes/EOI plus independent accepted-byte accounting. Cover data/status
owners, all counter wrap boundaries, stale-owner rejection, ATN immediately
before/after GET, active-byte pause, queued checkpoint tokens, RX saturation,
reset and remount at each pending phase.

**Native DOS smoke:** boot/status, ordinary file create/write/CLOSE/read,
directory, format, block/memory operations and representative REL positioning
for each model. Add a meaningful 8250 large-REL/super-side-sector boundary,
not just a short 4040-style REL file. Inspect persisted media independently.
Retarget the existing production Commodore/Waterloo composition tests to the
new facade, updating expectations only with model/ROM evidence. Preserve
tests and fixes added to `main` after the POC branched. In
particular, regain the supported D80 SuperPET workload without HDD shortcuts.

**Lifecycle and idle:** reset while RX/TX/ATN/controller work is pending,
replace/remove one device while another has open channels, failed replacement
without losing the old device, all-unit teardown failure and retry, temporary
boot-instance pointer lifetime, stale queued RX across publication, masked
empty/faulted devices and repeated system-fixture destruction. Assert zero
emulated cycles across repeated certified-idle service calls, and demonstrate
that each legitimate wake source makes bounded progress.

### Independent evidence discipline

Keep three different claims separate: static ROM-byte correspondence,
original-controller mailbox/media agreement, and composed IEEE wire
agreement. A pass at one layer does not stand in for the others.

The POC's packaged MAME 0.264 reference is not the separately pinned,
instrumented full-wire build. Retain their binary/script/ROM hashes and
fail-closed completion/shape/order checks. Missing tools or ROMs may skip
developer convenience cases, but production acceptance must require the
actual comparisons with `--no-tests=error`, not a green partial inventory.
Also check the required test-name/profile inventory explicitly: matching one
reference or integrity test does not establish that all three models'
required scenarios were registered and executed.

Keep the documented MAME D64 first-disk-ID limitation and narrow SEEK sector/
checksum normalization visible. Do not broaden normalization to make new
models pass. Verify the DOS 2.7 reference's differing-ID and side behavior
before using it as an oracle. Reference bugs need independent corroboration,
not adjustments that copy production output into expected captures.

No rotational or percentage-speed comparison is part of functional
acceptance. Preserve exact semantic results while allowing C jobs to
complete sooner. Host I/O failures remain explicitly modeled host failures,
not a claim to reproduce every analog GCR fault.

Every production defect found by a system test, reference run or probe must
also close its corresponding ordinary fw/gw coverage hole in the same fix.
Simulator lifetime and scheduling-harness defects stay in the tests owning
those mechanisms.

## Implementation sequence and gates

Each phase should produce reviewable commits. Keep existing production tests
and behavior until the replacement owns the corresponding surface, then
retire the adaptation and its implementation-specific tests deliberately.
Develop native modules independently before facade cutover, not as a second
user-selectable DOS implementation. After cutover, preserve the POC's passing
4040 observations and the supported production workloads.

### P0: preserve the evidence (complete)

Publish the POC checkpoint, check it out in the separate reference worktree,
and base production on the head of `main`, as recorded above. Keep only the
plan on top initially. Keep private ROMs/artifacts outside Git. Save commands
and accepted observations, not just test counts. Never use a POC merge as
the production bootstrap.

### P1: lock profiles, protocol inventory and ROM delivery

Confirm approved main/controller identities and obtain DOS 2.7 reference
inputs. Inventory main-CPU-visible protocols against exact binary mappings,
primary source and board evidence. Extend download verification and
packaging contracts, including the publication restriction.

Exit gate: all required inputs are obtainable and independently verified,
the DOS 2.7 contract matrix names every interaction, and download failure/
cache tests pass. No 8050/8250 compatibility claim yet.

### P2: refine the existing raw storage foundation

Refactor the geometry and media-lifetime mechanisms already present on
`main`, with new narrow descriptors and ownership boundaries. Add FILE fault
injection and staged pair preparation. Keep the active facade working until
the replacement is ready, without importing POC deletions.

Exit gate: exhaustive three-format geometry, pair validation, stale identity,
write/flush/close and resource checks pass in ordinary firmware suites.

### P3: modularize the proven 4040 path

Port the proven machine/RIOT/controller/peer mechanisms from the reference
worktree into the planned production modules, not the experimental facade
wholesale. Introduce instance storage and shared immutable ROM borrowing.
Use isolated fixtures for one native device before production cutover.

Exit gate: all POC observations and independent 4040 comparisons still pass
through production modules, no alternate POC emulator is linked into tests,
and new module invariants fail in negative tests.

### P4: implement selected DOS 2.7 hardware and controller contracts

Add the correct decoder, startup handshake, control-state publication,
job variants and complete native payload recognition. Bring up 8050/D80
first, then 8250/D82 mode and second-side behavior.

Exit gate: real ROM diagnostics pass, independent startup/job/header/media
agreement passes for both modes, formatter and large-REL boundary cases
pass, and every discovered defect has ordinary firmware coverage.

### P5: integrate devices 8-11 and shared-endpoint ownership

Preserve gateware admission and implement native address straps, ordered event
fan-out and explicit TX ownership, then mixed-model operation across four
homogeneous devices. P5 transport work may start after P3 independently of
P4, but acceptance needs both.

Exit gate: multi-device/channel isolation, broadcast listener and peer
transfer tests pass through the production FPGA and physical SPI.
Gateware edge/collision tests pass under both engines.

### P6: finish production lifecycle, idle and cooperative service

Wire staged mounting/configuration, retirement errors, unit-local reset,
resumable formatter work, certified idle parking, metrics and fault
containment. Finalize menu/CLI/configuration behavior and user documentation.

Exit gate: lifecycle failure matrices pass, all idle calls meet the
zero-emulated-work contract, active work is bounded/fair, and worst-case
memory accounting includes staging plus DVI/USB/SD.

### P7: simulation-only production acceptance

Run ordinary fw/gw suites, all configured system tests, native references,
full-wire selected comparisons and ROM probes with no acceptance exclusions.
Build production `RelWithDebInfo`, tests/framework/firmware-for-tests
`Debug`, and separately optimized Verilator models/runtime with assertions
retained, using root presets. Do not introduce Release matrices.

Exercise incremental configure/build/media replacement as well as a fresh
build. Record exact model/ROM/tool identities and supported versus excluded
behavior. Updating the branch is not evidence that these future gates passed.

### P8: manual RP2040 acceptance (last)

Only after simulation gates pass, measure real core-0 operation with core 1
dedicated to DVI, USB and SD active. Include one/four configured devices,
mixed models across devices, long LOAD/SAVE, formatting, native REL and the
D80 SuperPET workload.

Measure useful payload throughput, controller I/O time, idle service cost,
maximum service gaps, CPU/stack/SRAM margin and PET controller timeout
behavior. Do not use host emulation rate as a substitute. Keep one
outstanding byte unless any pipeline optimization is separately proved
consumption-correct.

If hardware throughput fails, optimize the measured bottleneck without
speculative GET, ROM patches, artificial mechanics or moving work onto the
DVI core. Do not declare production feasible until this manual gate passes.

## Evidence anchors and unresolved risks

Primary source: `mist64/cbmsrc` at
`01bd60f162ef92212ef0cb67546ae8f42be34168`.
Use approved revision-2 overlays/binary correspondence for 4040, not the
older archived source alone.

- [`DOS_8250/dskint`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/dskint)
  supplies main-CPU startup/reset/readiness sequencing.
- [`ramvar`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ramvar),
  [`equate`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/equate),
  [`i_odef`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/i_odef)
  and [`begin`](https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/begin)
  identify shared control state, job variants, native wiring and mode-derived
  geometry. Source contains unused/historical units too. Follow the selected
  composition and verify against approved bytes rather than choosing a
  similarly named initialization file.
- [The approval/mapping manifest](../../tools/drive-roms/drive-emulation-roms.json)
  identifies both main-ROM sets, selected controller revisions and mapped
  DOS 2.7 initialization/formatter payloads.
- [Pinned MAME `c8050.cpp`](https://github.com/mamedev/mame/blob/9d8a6727a206fc822b8b69d67a51e6fd7dbf4d4b/src/devices/bus/ieee488/c8050.cpp)
  corroborates the non-A15-mirrored DOS 2.7 map, address straps, shared main
  ROMs, RIOT IRQ wiring and controller-side PB6 distinction.
- POC firmware and system contracts are in the separate reference worktree
  at the pinned commit above. They are implementation evidence, not
  authorities overriding the native model. Production paths with similar
  names refer to `main`'s code until explicitly migrated.
- Current `fw/src/ieee/dos_media.*` and geometry tests retain the raw-storage
  failure/lifetime design worth reusing. `d5c1804` is a historical reference,
  not a substitute for preserving newer production fixes and tests.

Highest risks still requiring implementation evidence are DOS 2.7's
non-mailbox controller interactions, complete payload recognition,
multi-listener event ownership, safe idle certification, failure-preserving
replacement and real RP2040 per-byte cost. The plan addresses each with an
owner, an invariant and a release gate rather than treating the successful
4040 experiment as proof that the rest is automatic.
