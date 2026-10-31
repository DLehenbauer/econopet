// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"

#include "ieee_drive.h"

#include <stdio.h>
#include <string.h>

#include "diag/log/log.h"
#include "driver.h"
#include "fatal.h"
#include "hardware_contract.h"
#include "ieee/dos.h"
#include "ieee/ieee_protocol.h"
#include "sd/sd.h"

// Firmware transport facade for EconoPET's IEEE-488 drive support.
//
// gw/EconoPET/src/ieee.sv implements the electrical handshake and ordered
// receive/transmit FIFOs. driver.c carries register accesses over SPI, with
// hardware_contract.h defining the shared register, owner, and event encodings.
// This file turns those events into per-unit DOS requests and publishes DOS
// output back to the fabric. It does not implement electrical handshakes in C.
//
// dos.c owns request collection/dispatch, secondary semantics, directory lookup,
// logical channels, file cursors, EOF, and status text. dos_media.c guards
// raw-sector access and mounted-media generations,
// while diskimage.c detects formats and translates track/sector geometry.
// This facade owns the FILE handles, device presence, listener/talker routing,
// cooperative scheduling, and coordination of transport/DOS/media lifetimes.
//
// The C adaptation separates the ROM's intertwined bus and DOS routines:
// decoded command bytes replace the ROM's port sampling, bounded input records
// replace command-buffer collection, and staged FIFO suffixes replace direct
// CHNDAT presentation. Only observed NRFD GET counts advance DOS sources.
// Staging, presenting DAV, and receiving NDAC are not interchangeable progress
// boundaries. Pause/synchronize/discard ordering keeps unconsumed suffixes
// reversible when addressing, commands, or mounts change.
//
// ROM references use mist64/cbmsrc commit
// 01bd60f162ef92212ef0cb67546ae8f42be34168. Targets are 4040 DOS 2.0
// revision 2 (901468-14/-15/-16, with approved source overlays) and shared
// 8050/8250 DOS 2.7 (901887-01/901888-01). docs/dev/drive-emulation.md records
// revision correspondence and staged scope. Function-local references explain
// ROM behavior without claiming that host mounting or FIFO registers are native.
// Current DOS capabilities and known omissions are documented there rather
// than inferred from the transport's ability to route a request.

// Out-of-range sentinel for firmware addressing state, not an IEEE address.
#define NATIVE_UNIT_NONE DOS_MEDIA_UNIT_COUNT
// Bound ordered RX entries processed per main-loop pass, including metadata.
#define NATIVE_RX_BUDGET 64u
// Fabric TXS capacity, independent of the data FIFO's burst allowance.
#define NATIVE_STATUS_CAPACITY 32u
// Retain the first addressing exchange without logging every transfer.
#define NATIVE_COMMAND_LOG_BUDGET 8u

_Static_assert(ECONOPET_IEEE_TX_BURST_CHUNK <= DOS_STAGE_CAPACITY, "fabric staging exceeds the DOS preview bound");

// One staged suffix in a global fabric FIFO, tied to its native source owner.
// counter is the modulo-256 GET baseline, not the number of queued SPI writes.
typedef struct {
    unsigned int remaining;
    uint8_t unit;
    dos_source_t source;
    uint8_t counter;
} native_queue_t;

// Static storage keeps DOS/media identity independent of transient bus roles.
// files owns handles borrowed by media callbacks. Data and status FIFOs each
// have one global staged owner, whereas listener roles and DOS state are per unit.
static dos_media_bank_t media;
static dos_unit_t units[DOS_MEDIA_UNIT_COUNT];
static FILE* files[DOS_MEDIA_UNIT_COUNT * DOS_MEDIA_SLOTS_PER_UNIT];
static bool listening[DOS_MEDIA_UNIT_COUNT];
static native_queue_t data_queue, status_queue;
static uint8_t addressed = NATIVE_UNIT_NONE, talker = NATIVE_UNIT_NONE, secondary;
static uint8_t next_job;
static unsigned int command_logs_remaining;
static bool log_service_pending;

// Computes IEEE presence from complete unit models, never from another unit.
static uint8_t present_units(void) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/dskint
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/begin
    //
    // DIAGOK/BEGIN derive one device's primary address from hardware switches.
    // No direct ROM entry for this host presence policy: a configured pair
    // selects an emulated device even if only one slot is occupied.
    // Empty slots stay absent within that device.
    // Bit positions map units 0..3 to primary device addresses 8..11.
    uint8_t mask = 0;
    for (unsigned int unit = 0; unit < DOS_MEDIA_UNIT_COUNT; ++unit) {
        if (media.units[unit].model != DOS_MODEL_NONE) {
            mask |= (uint8_t)(1u << unit);
        }
    }
    return mask;
}

// Every native CTRL write selects the mounted emulated units, or disables
// emulation when none are present. Flush requests preserve that selection.
static void control(const uint8_t flags) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ieee
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ieee
    //
    // DCDE compares primary addresses with LSNADR/TLKADR. No direct ROM entry
    // for the host CTRL register: its mask enables those virtual responders
    // together, while flush flags affect fabric storage rather than DOS channels.
    // Every fabric CTRL write replaces the unit mask, including FIFO flushes.
    // Recompute it rather than accidentally disabling a peer with a bare flag.
    // Any selected unit enables interception globally, not selective pass-through
    // to physical devices at other addresses. An empty mask restores transparency.
    const uint8_t mask = present_units();
    spi_write_at(ECONOPET_WB_IEEE_CTRL_ADDR, (mask << ECONOPET_IEEE_CTRL_UNIT_MASK_SHIFT) | flags);
}

// Parks only new presentations. A byte already awaiting NDAC remains driven.
static void pause_tx(void) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ieee
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ieee
    //
    // TLK10 presents CHNDAT and TLK35 holds the byte until NDAC releases.
    // No direct ROM entry for this host fence. PAUSE gates only the start
    // of another presentation, not an already-driven byte or its GET accounting.
    // Fabric owner PAUSE blocks new presentations without withdrawing DAV from
    // an already active byte. It is a transport fence, not native GET progress
    // or evidence that the bus is idle. sync_tx/quiescent inspect that separately.
    spi_write_at(ECONOPET_WB_IEEE_SA_ADDR, ECONOPET_IEEE_TX_OWNER_VALID_MASK | ECONOPET_IEEE_TX_OWNER_PAUSE_MASK);
}

// Restores the current native source after ordered RX work.
static void resume_tx(void) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ieee
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ieee
    //
    // TALK/FNDRCH selects a ready logical source before TLK10 presents it.
    // No direct ROM entry for the host owner register. Rebind its unit/SA
    // only after ordered RX handling, without treating that write as native GET.
    // Bind the fabric source to the selected unit/secondary and clear PAUSE.
    // Data and status are distinguished by secondary 15 in the fabric.
    // With no selected talker, leave the owner register untouched rather than
    // inventing a source or releasing a presentation by rewriting its queue.
    if (talker != NATIVE_UNIT_NONE) {
        spi_write_at(ECONOPET_WB_IEEE_SA_ADDR,
                     ECONOPET_IEEE_TX_OWNER_VALID_MASK | (talker << ECONOPET_IEEE_TX_OWNER_UNIT_SHIFT) | secondary);
    }
}

// Commits only actual NRFD GET progress, including interrupted presentations.
static void sync_queue(native_queue_t* const queue, const bool status) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ieee
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ieee
    //
    // ROM TALK/TLK20/TLK30 calls GET when NRFD requests source progress,
    // before TLK35 finishes waiting for NDAC. The alternate TLK20 exit can
    // return to IDLE without GET. Thus neither queued SPI writes nor completed
    // DAV/NDAC handshakes alone determine how far the DOS cursor has advanced.
    // No direct ROM entry for this host FIFO/counter reconciliation.
    if (queue->remaining == 0) {
        return;
    }
    // The fabric counts actual GETs separately for data and status. Subtract
    // the saved counter modulo 256, which is unambiguous within the bounded
    // staging window. Reject progress beyond the owned suffix as a contract
    // violation, then retain the new baseline and outstanding length.
    const uint8_t current = spi_read_at(status ? ECONOPET_WB_IEEE_TXS_ADDR : ECONOPET_WB_IEEE_TX_ADDR);
    const uint8_t count = (uint8_t)(current - queue->counter);
    vet(count <= queue->remaining, "native transport GET exceeds its staging window");
    queue->counter = current;
    queue->remaining -= count;
    // Commit against the recorded owner, not today's talker. An intervening
    // selection or interrupted presentation must not charge a different unit.
    // Status GET may replace the retained record with OK on CR consumption.
    if (status) {
        const dos_status_progress_t progress = dos_status_progress(&units[queue->unit]);
        if (count != 0) {
            log_debug("IEEE u%u get p%u +%u q%u c%u", queue->unit, progress.position, count, queue->remaining, current);
            if (progress.position + count == progress.length) {
                log_info("IEEE u%u status complete n%u", queue->unit, progress.length);
            }
        }
    }
    dos_output_get(&units[queue->unit], &media, media.units[queue->unit].model, queue->source, count);
}

// Reconciles both independent FIFO owners before mutation or replacement.
static void sync_tx(void) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ieee
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ieee
    //
    // No direct ROM entry: reconcile the host's two queues against TLK30's
    // GET boundary. A source may still belong to an earlier bus selection.
    // Data and status sources have independent FIFO counters and native cursors.
    // Reconcile both before replacing either source or changing DOS state.
    sync_queue(&data_queue, false);
    sync_queue(&status_queue, true);
}

// Discards staged copies, not native progress, after pausing and synchronizing.
static void discard_queue(const bool status) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ieee
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ieee
    //
    // No direct ROM entry: native TALK presents CHNDAT without a copied FIFO
    // suffix. Drop only the host preview after reconciling TLK30 GET progress,
    // rather than closing its channel or restarting the native source cursor.
    // Caller has paused presentations and committed actual GET progress.
    // FIFO-only flushes preserve fabric GET counters. Drop only staged copies,
    // leaving native cursors available to rebuild an interrupted suffix.
    // control also preserves the mounted-device mask during this flush.
    control(status ? ECONOPET_IEEE_CTRL_STATUS_FLUSH_MASK : ECONOPET_IEEE_CTRL_DATA_FLUSH_MASK);
    if (status) {
        status_queue.remaining = 0;
    } else {
        data_queue.remaining = 0;
    }
}

// Publishes at most one bounded native suffix, leaving GET cursors unchanged.
static void fill_tx(void) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ieee
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ieee
    //
    // ROM TALK/TALK1 checks channel readiness and TLK10 presents CHNDAT with
    // its readiness/EOI flags. The FIFO adaptation previews a bounded suffix
    // through dos_output_stage instead of advancing GET on enqueue.
    // There is no direct ROM entry for this host FIFO staging operation.
    // Do not replace a still-owned suffix or fabricate an unopened file source.
    if (talker == NATIVE_UNIT_NONE) {
        return;
    }
    const bool status = secondary == IEEE_COMMAND_CHANNEL;
    native_queue_t* const queue = status ? &status_queue : &data_queue;
    if (queue->remaining != 0) {
        return;
    }
    uint8_t bytes[ECONOPET_IEEE_TX_BURST_CHUNK];
    bool eoi;
    // TXS has 32 entries. Native formatted status fits in that bounded queue.
    const unsigned int capacity = status ? NATIVE_STATUS_CAPACITY : sizeof(bytes);
    dos_source_t source;
    const unsigned int count = dos_output_stage(&units[talker], &media, secondary, bytes, capacity, &eoi, &source);
    if (count == 0) {
        return;
    }
    // Capture the source owner and current GET baseline before publication.
    // Later sync_queue commits only the observed consumed prefix of this copy.
    queue->unit = talker;
    queue->source = source;
    queue->counter = spi_read_at(status ? ECONOPET_WB_IEEE_TXS_ADDR : ECONOPET_WB_IEEE_TX_ADDR);
    queue->remaining = count;
    const uint32_t regular = status ? ECONOPET_WB_IEEE_TXS_ADDR : ECONOPET_WB_IEEE_TX_ADDR;
    const uint32_t last = status ? ECONOPET_WB_IEEE_TXS_LAST_ADDR : ECONOPET_WB_IEEE_TX_LAST_ADDR;
    const unsigned int regular_count = count - (unsigned int)eoi;
    // Regular writes enqueue nonfinal bytes. Only the dedicated LAST register
    // tags the final byte with EOI, so a short non-EOF chunk is not terminated.
    // Diagnostic only: isolate status WRITE_SAME bursts without slowing file data.
    if (status) {
        for (unsigned int index = 0; index < regular_count; ++index) {
            spi_write_at(regular, bytes[index]);
        }
    } else if (regular_count != 0) {
        spi_write_same_block(regular, bytes, regular_count);
    }
    if (eoi) {
        spi_write_at(last, bytes[count - 1]);
    }
    if (status) {
        // INFO retains initial lengths when per-GET DEBUG records wrap.
        const dos_status_progress_t progress = dos_status_progress(&units[talker]);
        if (progress.position == 0) {
            log_info("IEEE u%u status n%u e%u t%02x abs", talker, count, eoi, bytes[count - 1]);
        }
        log_debug("IEEE u%u put p%u n%u e%u t%02x [%.*s]", talker, progress.position, count, eoi, bytes[count - 1],
                  (int)(count - (unsigned int)eoi), (const char*)bytes);
    }
}

// Retire this unit's staged copies before permitting ordered DOS dispatch.
static void dispatch(const uint8_t unit) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/idle
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/idle
    //
    // PUT:L45 marks CMDWAT after EOI, then IDLE/VIDLE calls PARSXQ.
    // No direct ROM entry for this host transport fence. Dispatch a completed
    // DOS record only after withdrawing copies that its execution may replace.
    // The caller has paused and synchronized actual GETs before reaching this
    // ordered boundary. DOS owns readiness, request interpretation, and errors.
    if (!dos_input_ready(&units[unit])) {
        return;
    }
    // Remove stale copies for this unit before dos_open/status can replace
    // their underlying source state. Other units' status suffixes remain owned.
    if (data_queue.remaining != 0 && data_queue.unit == unit) {
        discard_queue(false);
    }
    if (status_queue.remaining != 0 && status_queue.unit == unit) {
        discard_queue(true);
    }
    dos_input_dispatch(&units[unit], &media, unit);
}

// Decodes IEEE roles separately from per-unit command and channel ownership.
static void command(const uint8_t byte) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ieee
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ieee
    //
    // ROM ATNIRQ/ATN30 samples a command and DCDE decodes primary/secondary
    // addresses and OPEN/CLOSE. ATN50/ATN60 select listener or talker execution.
    // The fabric already sampled the logical byte. Track firmware roles and
    // DOS ownership here without duplicating the port-level handshake.
    if (command_logs_remaining != 0) {
        --command_logs_remaining;
        log_info("IEEE cmd %02x mask%02x addr%u talk%u", byte, present_units(), addressed, talker);
    }
    const uint8_t address = IEEE_CMD_ADDRESS(byte);
    const uint8_t unit = address >= 8 && address < 8 + DOS_MEDIA_UNIT_COUNT && (present_units() & (1u << (address - 8)))
                             ? (unsigned int)(address - 8)
                             : NATIVE_UNIT_NONE;
    switch (IEEE_CMD_CLASS(byte)) {
    case IEEE_CMD_LISTEN_BASE:
        // Primary LISTEN selects a mounted device. UNLISTEN clears every
        // listener and offers each completed record for deferred dispatch.
        // Per-unit listener flags allow data fan-out to multiple devices.
        addressed = unit;
        if (byte == IEEE_CMD_UNLISTEN) {
            addressed = NATIVE_UNIT_NONE;
            for (uint8_t index = 0; index < DOS_MEDIA_UNIT_COUNT; ++index) {
                listening[index] = false;
                dispatch(index);
            }
        } else if (unit != NATIVE_UNIT_NONE) {
            listening[unit] = true;
            if (talker == unit) {
                talker = NATIVE_UNIT_NONE;
            }
        }
        break;
    case IEEE_CMD_TALK_BASE:
        // TALK selects at most one source and clears that unit's listener
        // role. UNTALK removes the talker without freeing its DOS channel.
        addressed = unit;
        if (byte == IEEE_CMD_UNTALK) {
            talker = NATIVE_UNIT_NONE;
        } else {
            talker = unit;
            if (unit != NATIVE_UNIT_NONE) {
                listening[unit] = false;
            }
        }
        break;
    case IEEE_CMD_SECONDARY_BASE:
        // A secondary applies only to the last valid primary selection.
        // TALK chooses a retained source. DOS decides whether a listening
        // secondary starts input collection.
        if (addressed == NATIVE_UNIT_NONE) {
            break;
        }
        dos_input_address(&units[addressed], &media, media.units[addressed].model, byte,
                          talker != addressed && listening[addressed]);
        if (talker == addressed) {
            secondary = IEEE_CMD_CHANNEL(byte);
            // A selected source is rebuilt from retained native GET progress.
            discard_queue(secondary == IEEE_COMMAND_CHANNEL);
        }
        break;
    case IEEE_CMD_CLOSE_BASE:
        // Commodore OPEN/CLOSE extensions require a selected listener.
        // OPEN starts a fresh name record. CLOSE releases DOS resources
        // after outstanding GET progress was synchronized by the caller.
        if (addressed == NATIVE_UNIT_NONE || !listening[addressed]) {
            break;
        }
        // Fabric OPEN/CLOSE flushes data globally. Rebuild any peer suffix
        // from its retained GET cursor rather than retaining a phantom queue.
        discard_queue(false);
        if (!IEEE_CMD_IS_OPEN(byte)) {
            if (IEEE_CMD_CHANNEL(byte) == IEEE_COMMAND_CHANNEL) {
                const dos_status_progress_t progress = dos_status_progress(&units[addressed]);
                log_info("IEEE u%u close status p%u/%u", addressed, progress.position, progress.length);
            }
            if (data_queue.remaining != 0 && data_queue.unit == addressed) {
                discard_queue(false);
            }
            if (status_queue.remaining != 0 && status_queue.unit == addressed) {
                discard_queue(true);
            }
        }
        dos_input_address(&units[addressed], &media, media.units[addressed].model, byte, true);
        break;
    default:
        break;
    }
}

// Service bounded DOS work and ordered fabric events once per main-loop pass.
// Preserve active presentations and resume output only after pending RX ordering.
void ieee_drive_task(void) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ieee
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ieee
    //
    // ROM ATNIRQ interrupts normal DOS work and IEEE LISTEN/TALK exchange bytes
    // through PUT/GET. This adaptation instead services ordered fabric entries
    // cooperatively, separating source progress, command decoding, and DOS work.
    // There is no direct ROM entry for this main-loop scheduler.
    // No configured model means no emulated transport work to service.
    if (present_units() == 0) {
        return;
    }
    sync_tx();
    // Round-robin selection advances at most one unit with pending DOS work.
    // If any command or channel job remains runnable, service output but leave
    // subsequent RX entries queued so they cannot overtake that operation.
    // A directory chain must yield to the main loop, even on corrupt media.
    for (unsigned int step = 0; step < DOS_MEDIA_UNIT_COUNT; ++step) {
        const uint8_t unit = next_job;
        next_job = (next_job + 1) % DOS_MEDIA_UNIT_COUNT;
        if (dos_work_pending(&units[unit])) {
            dos_work(&units[unit], &media);
            break;
        }
    }
    for (unsigned int unit = 0; unit < DOS_MEDIA_UNIT_COUNT; ++unit) {
        if (dos_work_pending(&units[unit])) {
            fill_tx();
            resume_tx();
            return;
        }
    }
    for (unsigned int work = 0; work < NATIVE_RX_BUDGET; ++work) {
        // Check for queued work, pause new output, and reconcile GETs before
        // examining the head. An active DAV must finish before this event is
        // popped or source state is changed. Leave PAUSE set when returning.
        uint8_t flags = spi_read_at(ECONOPET_WB_IEEE_STATUS_ADDR);
        if (log_service_pending) {
            log_service_pending = false;
            log_info("IEEE service mask%02x st%02x", present_units(), flags);
        }
        if (!(flags & ECONOPET_IEEE_ST_RX_AVAIL_MASK)) {
            break;
        }
        pause_tx();
        sync_tx();
        const uint8_t state = spi_read_at(ECONOPET_WB_IEEE_CTRL_ADDR);
        if (state & (ECONOPET_IEEE_CTRL_RD_DATA_ACTIVE_MASK | ECONOPET_IEEE_CTRL_RD_STATUS_ACTIVE_MASK)) {
            return;
        }
        flags = spi_read_at(ECONOPET_WB_IEEE_STATUS_ADDR);
        // CTRL carries RX-head metadata. Read the head byte, then explicitly
        // pop it through RX. Ordered metadata entries are not controller bytes.
        const uint8_t byte = spi_read_at(ECONOPET_WB_IEEE_RX_ADDR);
        spi_write_at(ECONOPET_WB_IEEE_RX_ADDR, 0);
        if (state & ECONOPET_IEEE_CTRL_RD_RX_ATN_END_MASK) {
            // Zero boundary payload denotes ATN release and permits dispatch.
            // Nonzero listener-boundary metadata is consumed without dispatch.
            if (byte == 0) {
                for (uint8_t unit = 0; unit < DOS_MEDIA_UNIT_COUNT; ++unit) {
                    dispatch(unit);
                }
            }
        } else if (!(state & ECONOPET_IEEE_CTRL_RD_RX_TALK_EVENT_MASK)) {
            // TALK-event metadata is consumed without entering the parser.
            // Actual source advancement is already reflected in GET counters.
            // ATN bytes update roles, while ordinary bytes fan out only to
            // listeners. DOS handles collection limits and EOI completion
            // without dispatching mid-entry.
            if (flags & ECONOPET_IEEE_ST_RX_ATN_MASK) {
                command(byte);
            } else {
                for (uint8_t unit = 0; unit < DOS_MEDIA_UNIT_COUNT; ++unit) {
                    if (listening[unit]) {
                        // Ordinary PUT may publish a storage error, retiring its old status copy first.
                        if (status_queue.remaining != 0 && status_queue.unit == unit) {
                            discard_queue(true);
                        }
                        dos_listen_put(&units[unit], &media, byte, (state & ECONOPET_IEEE_CTRL_RD_RX_EOI_MASK) != 0);
                    }
                }
            }
        }
        // Preserve later ordered entries until the current DOS work completes.
        bool pending = false;
        for (unsigned int unit = 0; unit < DOS_MEDIA_UNIT_COUNT; ++unit) {
            pending |= dos_work_pending(&units[unit]);
        }
        if (pending) {
            break;
        }
    }
    if (spi_read_at(ECONOPET_WB_IEEE_STATUS_ADDR) & ECONOPET_IEEE_ST_RX_AVAIL_MASK) {
        return;
    }
    // Do not publish/resume a source ahead of unprocessed ordered RX work.
    // Recheck after publication as SPI writes can overlap a new bus selection.
    fill_tx();
    // SPI publication may overlap a newer selection. Process it before resuming.
    if (spi_read_at(ECONOPET_WB_IEEE_STATUS_ADDR) & ECONOPET_IEEE_ST_RX_AVAIL_MASK) {
        return;
    }
    resume_tx();
}

// Executes exact-size, bounded reads against a retained FILE owner.
static bool file_read(void* const context, const uint32_t offset, void* const output, const size_t length) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tsutil
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tsutil
    //
    // RDAB/RDIN/RDSS submit native sector jobs. No direct ROM entry for this
    // FILE callback: diskimage/dos_media translate and validate those requests
    // before reaching host byte offsets, without emulating controller retries.
    // Host backing-store adapter, not a ROM read job. dos_media resolves a
    // current media identity and valid sector before invoking this callback.
    // Seek each request independently and require the full requested length.
    // A short read may modify output before false is returned. The FILE owner
    // remains this facade, which must not close it while a binding can use it.
    FILE* const file = context;
    clearerr(file);
    return fseek(file, (long)offset, SEEK_SET) == 0 && fread(output, 1, length, file) == length;
}

// Completes exact-size sector updates without retrying a possibly partial write.
static bool file_write(void* const context, const uint32_t offset, const void* const input, const size_t length) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tsutil
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tsutil
    //
    // WRTOUT/WRTAB/WRTSS submit native WRITE jobs. No direct ROM entry for
    // this host callback. A failed fwrite can modify storage partially, so
    // return failure without replaying it or closing the facade-owned FILE.
    FILE* const file = context;
    clearerr(file);
    return fseek(file, (long)offset, SEEK_SET) == 0 && fwrite(input, 1, length, file) == length;
}

// Flushes the retained owner without closing it on failure.
static bool file_flush(void* const context) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/close
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/close
    //
    // CLSCHN writes native metadata through CLSDIR/MAPOUT. No direct ROM
    // entry for fflush: this extra host persistence boundary neither performs
    // those DOS writes nor acknowledges a bus byte. Failure leaves ownership.
    FILE* const file = context;
    clearerr(file);
    return fflush(file) == 0;
}

// Rejects a busy mutation without withdrawing active DAV or consuming RX work.
static bool quiescent(void) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ieee
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ieee
    //
    // TLK30 commits GET before TLK35 can finish its presentation. No direct
    // ROM entry for this host mutation guard: reconcile progress, then require
    // no driven byte and no queued RX event before allowing a binding change.
    // Host mutation guard: stop new presentations and commit observed GETs.
    // Neither PAUSE nor empty software queues alone proves a safe boundary.
    pause_tx();
    sync_tx();
    const uint8_t state = spi_read_at(ECONOPET_WB_IEEE_CTRL_ADDR);
    const uint8_t flags = spi_read_at(ECONOPET_WB_IEEE_STATUS_ADDR);
    if ((state & (ECONOPET_IEEE_CTRL_RD_DATA_ACTIVE_MASK | ECONOPET_IEEE_CTRL_RD_STATUS_ACTIVE_MASK)) ||
        (flags & ECONOPET_IEEE_ST_RX_AVAIL_MASK)) {
        resume_tx();
        return false;
    }
    // Success leaves new presentations paused for the caller's binding change.
    // This checks transport activity, not absence of allocated DOS channels.
    return true;
}

// Opens a complete candidate without disturbing a live binding or channel.
static bool open_candidate(const char* const filename, FILE** const file, dos_media_io_t* const replacement) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
    //
    // INITDR seeks and reads native disk metadata. No direct ROM entry for
    // preparing a host image candidate: opening a FILE and detecting geometry
    // here must not initialize a drive or replace an existing DOS binding.
    // Host filesystem preparation has no native ROM counterpart. Start with
    // empty outputs so NULL denotes removal without opening any storage.
    *file = NULL;
    *replacement = (dos_media_io_t){0};
    if (filename != NULL) {
        // Prefer writable media, falling back to read-only. Detect exact raw
        // image size without reading directory/BAM contents or changing a live
        // binding. Every failure after fopen closes only the candidate handle.
        char path[SD_PATH_MAX];
        sd_make_path(path, SD_DIR_DISKS, filename);
        FILE* candidate = fopen(path, "r+b");
        const bool writable = candidate != NULL;
        if (candidate == NULL) {
            candidate = fopen(path, "rb");
        }
        if (candidate == NULL) {
            return false;
        }
        // Sector/BAM caches belong to DOS, not independently buffered FILE aliases.
        if (setvbuf(candidate, NULL, _IONBF, 0) != 0) {
            fclose(candidate);
            return false;
        }
        if (fseek(candidate, 0, SEEK_END) != 0) {
            fclose(candidate);
            return false;
        }
        const long size = ftell(candidate);
        if (size < 0 || (unsigned long)size > UINT32_MAX ||
            !diskimage_open(&replacement->image, file_read, candidate, (uint32_t)size)) {
            fclose(candidate);
            return false;
        }
        if (writable) {
            replacement->image.write = file_write;
            replacement->flush = file_flush;
        }
        *file = candidate;
    }
    return true;
}

// Mount a disks-relative file or remove one slot for NULL, preserving peers.
// Reject invalid candidates or busy transport without replacing the live mount.
bool ieee_drive_mount(const unsigned int slot, const char* const filename) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/close
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/close
    //
    // No direct ROM entry: host rebinding is neither INITDR nor a DOS command.
    // Finish old writers through the modeled CLOSE paths before publishing a
    // new media generation. The unit shares DOS state across its two slots.
    // Host lifecycle policy: map the facade's flat slot index to unit/drive.
    // Prepare a separate candidate and validate the complete proposed pair
    // before asking the transport to pause. files owns the handles that media
    // descriptors borrow, so a failed proposal must close only the candidate.
    if (slot >= count_of(files)) {
        return false;
    }
    const unsigned int unit = slot / DOS_MEDIA_SLOTS_PER_UNIT;
    const unsigned int drive = slot % DOS_MEDIA_SLOTS_PER_UNIT;
    FILE* candidate;
    dos_media_io_t replacement;
    if (!open_candidate(filename, &candidate, &replacement)) {
        return false;
    }
    const dos_media_io_t pair[] = {
        drive == 0 ? replacement : media.units[unit].slots[0].io,
        drive == 1 ? replacement : media.units[unit].slots[1].io,
    };
    dos_model_t model;
    if (dos_media_validate_pair(pair, &model) != DOS_MEDIA_OK || !quiescent()) {
        if (candidate != NULL) {
            fclose(candidate);
        }
        return false;
    }
    // Finalization can replace status even when rebinding subsequently fails.
    // Earlier persistence is not rolled back, but old FILE bindings stay owned.
    if (status_queue.remaining != 0 && status_queue.unit == unit) {
        discard_queue(true);
    }
    if (dos_work_pending(&units[unit]) || !dos_finalize(&units[unit], &media) ||
        dos_media_set_unit(&media, unit, pair) != DOS_MEDIA_OK) {
        if (candidate != NULL) {
            fclose(candidate);
        }
        resume_tx();
        return false;
    }
    // Media publication succeeded and old unit references are now stale.
    // Temporarily withdraw this device from the fabric mask, preserving peers,
    // then discard its staged copies before resetting their native sources.
    const uint8_t peers = present_units() & (uint8_t)~(1u << unit);
    spi_write_at(ECONOPET_WB_IEEE_CTRL_ADDR, peers << ECONOPET_IEEE_CTRL_UNIT_MASK_SHIFT);
    if (data_queue.remaining != 0 && data_queue.unit == unit) {
        discard_queue(false);
    }
    if (status_queue.remaining != 0 && status_queue.unit == unit) {
        discard_queue(true);
    }
    // Close the retired slot handle only after successful binding replacement.
    // Reset both drives' shared DOS state, but retain the unchanged partner's
    // FILE and other units' DOS/input state. Clear bus selections of this unit.
    if (files[slot] != NULL) {
        fclose(files[slot]);
    }
    files[slot] = candidate;
    dos_reset(&units[unit], model);
    listening[unit] = false;
    if (talker == unit) {
        talker = NATIVE_UNIT_NONE;
    }
    if (addressed == unit) {
        addressed = NATIVE_UNIT_NONE;
    }
    // Restore the complete presence mask and resume any surviving peer talker.
    control(0);
    resume_tx();
    log_info("IEEE mount u%u d%u rw%u %.24s", unit, drive, replacement.image.write != NULL,
             filename != NULL ? filename : "(removed)");
    return true;
}

// Remove every mounted image after quiescence and successful media flushing.
// Retain handles and bindings if the transition cannot be completed.
bool ieee_drive_unmount_all(void) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/close
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/close
    //
    // No direct ROM entry: native CLOSE/CLSALL releases channels, not FILE
    // handles or mounted images. Host teardown finalizes every unit before
    // detaching any binding. Successful earlier finalization is not rolled back.
    // Host teardown is distinct from native DOS reset. Reject active transport
    // without detaching handles. dos_media flushes all units before removing
    // any binding, retaining them on a failure and allowing a later retry.
    if (present_units() != 0 && !quiescent()) {
        return false;
    }
    for (unsigned int unit = 0; unit < DOS_MEDIA_UNIT_COUNT; ++unit) {
        if (status_queue.remaining != 0 && status_queue.unit == unit) {
            discard_queue(true);
        }
        if (dos_work_pending(&units[unit]) || !dos_finalize(&units[unit], &media)) {
            resume_tx();
            return false;
        }
    }
    if (dos_media_unmount_all(&media) != DOS_MEDIA_OK) {
        resume_tx();
        return false;
    }
    // No live binding can invoke these callbacks after successful detachment.
    // Release every owned FILE, then reset DOS and fabric state with no devices
    // selected. The media bank's generation survives this reset/unmount cycle.
    for (unsigned int slot = 0; slot < count_of(files); ++slot) {
        if (files[slot] != NULL) {
            fclose(files[slot]);
        }
        files[slot] = NULL;
    }
    // Detached generations cannot finalize cached BAM references again.
    for (unsigned int unit = 0; unit < DOS_MEDIA_UNIT_COUNT; ++unit) {
        dos_reset(&units[unit], DOS_MODEL_NONE);
    }
    ieee_drive_reset();
    return true;
}

// Reset transport and successfully finalized DOS units without removing media.
// Failed owners retain their DOS state. Coordinate with the PET/fabric reset path.
void ieee_drive_reset(void) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/dskint
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/begin
    //
    // DIAGOK/BEGIN bootstraps channel tables and command/error readiness.
    // No direct ROM entry for this coordinated host reset. It is not U9/UI,
    // and mounted FILEs and media generations survive it.
    // This is a coordinated firmware reset, not a port of ROM power-on
    // diagnostics, head initialization, or command-channel reset execution.
    // Reinitialize only finalized units using their retained models, dropping
    // their partial input. Clear bus listener roles even for a failed owner.
    for (unsigned int unit = 0; unit < DOS_MEDIA_UNIT_COUNT; ++unit) {
        // Failed finalization must survive a PET reset with its owner and error.
        if (dos_finalize(&units[unit], &media)) {
            dos_reset(&units[unit], media.units[unit].model);
        }
        listening[unit] = false;
    }
    // Global fabric flush resets FIFO GET counters, so abandon both staged
    // owners/baselines as well as addressing and round-robin scheduling state.
    // control preserves presence from mounted media while requesting the flush.
    data_queue = (native_queue_t){0};
    status_queue = (native_queue_t){0};
    addressed = talker = NATIVE_UNIT_NONE;
    secondary = 0;
    next_job = 0;
    control(ECONOPET_IEEE_CTRL_FLUSH_MASK);
    command_logs_remaining = NATIVE_COMMAND_LOG_BUDGET;
    log_service_pending = true;
    const uint8_t state = spi_read_at(ECONOPET_WB_IEEE_CTRL_ADDR);
    const uint8_t flags = spi_read_at(ECONOPET_WB_IEEE_STATUS_ADDR);
    log_info("IEEE reset mask%02x ctrl%02x st%02x", present_units(), state, flags);
}

// Initialize the facade with no mounted media and fresh DOS/transport state.
void ieee_drive_init(void) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/dskint
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/begin
    //
    // DIAGOK/BEGIN establishes native DOS channel state. No direct ROM entry
    // for this host facade initializer: establish an empty media bank through
    // normal teardown, not by zeroing live handles or reusing old identities.
    // Static state starts empty. Reuse normal teardown so repeated init cannot
    // silently leak handles or discard unflushed media. Failure to establish
    // the required quiescent/unmounted state is a fatal initialization error.
    vet(ieee_drive_unmount_all(), "native initialization requires quiescent media");
}
