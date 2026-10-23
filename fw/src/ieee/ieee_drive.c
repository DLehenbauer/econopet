// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "ieee_drive.h"

#include <stdio.h>
#include <string.h>

#if defined(PICO_RP2040)
#include "pico/time.h"
#endif

#include "diag/log/log.h"
#include "diskimage.h"
#include "driver.h"
#include "fatal.h"
#include "hardware_contract.h"
#include "ieee_protocol.h"
#include "sd/sd.h"

#define DEV_ADDR 8

// The fabric answers devices 8 through 11, each a dual-drive unit. Mount
// slot = unit * DRIVES_PER_UNIT + drive, so slots 0/1 map to device 8 and
// slots 6/7 map to device 11.
#define NUM_UNITS       4
#define DRIVES_PER_UNIT 2
#define NUM_DRIVES      (NUM_UNITS * DRIVES_PER_UNIT)
#define COMMAND_VALIDATION_LIMIT 58
#define COMMAND_4040_RECEIVE_CAPACITY 41
#define COMMAND_RECEIVE_CAPACITY (COMMAND_VALIDATION_LIMIT + 1)
#define DEFAULT_CHANNEL 0

// Write fabric control without changing firmware channel state.
static void ieee_ctrl_write(uint8_t value) {
    spi_write_at(ECONOPET_WB_IEEE_CTRL_ADDR, value);
}

// ----------------------------------------------------------------------------
// Disk images
// ----------------------------------------------------------------------------

typedef struct {
    FILE* file;
    diskimage_t image;
    bool present;
    char name[SD_PATH_MAX];  // image path currently "inserted"
} drive_t;

static drive_t drives[NUM_DRIVES];
static bool emulation_enabled = false;

typedef enum {
    drive_model_4040,
    drive_model_8050,
} drive_model_t;

// ----------------------------------------------------------------------------
// REL chain cache (built at mount time)
// ----------------------------------------------------------------------------
// The Super-OS/9 loader bounds drive latency with a counted DAV poll
// (~1.25s at 1MHz), so chains are prebuilt at mount. Links never change
// after mount (record writes reuse existing sectors).
#define CHAIN_CACHE_SLOTS 4

typedef struct {
    bool valid;
    uint8_t track, sector;              // chain start track/sector = cache key
    diskchain_t chain;
} chain_cache_entry_t;

static chain_cache_entry_t chain_cache[NUM_DRIVES][CHAIN_CACHE_SLOTS];

static void chain_cache_build(unsigned int slot) {
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    unsigned int cache_slot = 0;
    diskimage_entry_t e;

    for (unsigned int i = 0; i < CHAIN_CACHE_SLOTS; i++)
        chain_cache[slot][i].valid = false;

    for (unsigned int i = 0; diskimage_entry(&drives[slot].image, i, &e); i++) {
        if (e.file_type != DISKIMAGE_FTYPE_REL) continue;
        if (cache_slot >= CHAIN_CACHE_SLOTS) {
            log_info("ieee: drive %u: more REL files than %u cache slots",
                     slot, CHAIN_CACHE_SLOTS);
            break;
        }
        chain_cache_entry_t* c = &chain_cache[slot][cache_slot];
        if (!diskchain_build(&c->chain, &drives[slot].image,
                             e.start_track, e.start_sector))
            continue;
        c->track = e.start_track;
        c->sector = e.start_sector;
        c->valid = true;
        cache_slot++;
    }
    log_info("ieee: drive %u: %u REL chain(s) cached in %lu ms",
             slot, cache_slot,
             (unsigned long) (to_ms_since_boot(get_absolute_time()) - t0));
}

static const diskchain_t* chain_cache_find(unsigned int slot, uint8_t track,
                                           uint8_t sector) {
    for (unsigned int i = 0; i < CHAIN_CACHE_SLOTS; i++) {
        chain_cache_entry_t* c = &chain_cache[slot][i];
        if (c->valid && c->track == track && c->sector == sector) return &c->chain;
    }
    return NULL;
}

static bool file_read(void* ctx, uint32_t offset, void* buf, size_t len) {
    FILE* f = (FILE*) ctx;
    if (fseek(f, (long) offset, SEEK_SET) != 0) return false;
    return fread(buf, 1, len, f) == len;
}

static bool file_write(void* ctx, uint32_t offset, const void* buf, size_t len) {
    FILE* f = (FILE*) ctx;
    if (fseek(f, (long) offset, SEEK_SET) != 0) return false;
    if (fwrite(buf, 1, len, f) != len) return false;
    return fflush(f) == 0;      // persist promptly: power-off safety
}

// Mounts a path relative to /disks into a global drive slot.
static bool mount_image(unsigned int slot, const char* filename) {

    char path[SD_PATH_MAX];
    sd_make_path(path, SD_DIR_DISKS, filename);
    // Read-write when the card allows it (REL record writes, OS-9 format);
    // fall back to read-only rather than failing the mount.
    bool writable = true;
    FILE* f = fopen(path, "r+b");
    if (f == NULL) {
        writable = false;
        f = fopen(path, "rb");
    }
    if (f == NULL) return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);

    // .hdd = flat hard-disk REL container (any 258-multiple size); everything
    // else opens by the exact d64/d80 container sizes.
    const char* dot = strrchr(filename, '.');
    bool is_hdd = (dot != NULL && strcasecmp(dot, ".hdd") == 0);

    diskimage_t img;
    bool opened = is_hdd ? diskimage_open_hdd(&img, file_read, f, (uint32_t) size)
                         : diskimage_open(&img, file_read, f, (uint32_t) size);
    if (!opened) {
        log_info("ieee: %s has unsupported size %ld, ignored", path, size);
        fclose(f);
        return false;
    }

    if (drives[slot].file != NULL) fclose(drives[slot].file);
    drives[slot].file = f;
    drives[slot].image = img;
    drives[slot].image.ctx = f;
    drives[slot].image.write = writable ? file_write : NULL;
    drives[slot].present = true;
    snprintf(drives[slot].name, sizeof(drives[slot].name), "%s", filename);
    log_info("ieee: drive %u = %s (%ld bytes)", slot, path, size);
    chain_cache_build(slot);
    return true;
}

// ----------------------------------------------------------------------------
// DOS state
// ----------------------------------------------------------------------------

typedef enum {
    st_code_ok = 0,
    st_code_read_error = 23,
    st_code_write_protect = 26,
    st_code_long_line = 32,
    st_code_record_missing = 50,
    st_code_record_overflow = 51,
    st_code_file_not_found = 62,
    st_code_file_type_mismatch = 64,
    st_code_no_channel = 70,
    st_code_power_on = 73,
} status_code_t;

typedef struct {
    status_code_t code;
    uint8_t track;
    uint8_t sector;
    uint8_t drive;
    unsigned int read_pos;
    uint32_t generation;
    bool formatted;
    drive_model_t model;
} drive_status_t;

// Per-unit DOS status: each IEEE unit has its own channel-15 command channel,
// so status is tracked and served per unit.
static drive_status_t drive_status[NUM_UNITS] = {
    { .code = st_code_power_on },
    { .code = st_code_power_on },
    { .code = st_code_power_on },
    { .code = st_code_power_on },
};
static uint8_t status_tx_unit = NUM_UNITS;
static uint8_t status_tx_prefetched;
static unsigned int status_tx_remaining;
static uint32_t status_tx_generation;
static bool status_tx_pending;

static void set_status(unsigned int slot, status_code_t code, uint8_t track,
                       uint8_t sector) {
    drive_status_t* const status = &drive_status[slot / DRIVES_PER_UNIT];
    const uint32_t generation = status->generation + 1;
    *status = (drive_status_t) {
        .code = code,
        .track = track,
        .sector = sector,
        .drive = (uint8_t) (slot % DRIVES_PER_UNIT),
        .generation = generation,
    };
}

// Reconcile native GET progress, which survives FIFO replacement and wraps modulo 256.
static uint8_t tx_progress(uint32_t address, uint8_t* previous, unsigned int remaining) {
    const uint8_t current = spi_read_at(address);
    const uint8_t advanced = (uint8_t) (current - *previous);
    vet(advanced <= remaining, "IEEE prefetch exceeds its pending batch");
    *previous = current;
    return advanced;
}

// Advance the status owner only on GET, acknowledging the error at terminal CR.
static void status_tx_sync(void) {
    if (status_tx_unit == NUM_UNITS) return;
    drive_status_t* const status = &drive_status[status_tx_unit];
    const uint8_t advanced = tx_progress(ECONOPET_WB_IEEE_TXS_ADDR,
        &status_tx_prefetched, status_tx_remaining);
    status_tx_remaining -= advanced;
    if (status->generation == status_tx_generation) {
        status->read_pos += advanced;
        if (status_tx_remaining == 0)
            set_status(status_tx_unit * DRIVES_PER_UNIT + status->drive, st_code_ok, 0, 0);
    }
    if (status_tx_remaining == 0) status_tx_unit = NUM_UNITS;
}

static bool mcu_talking = false;

// Only the most recently primary-addressed unit accepts a secondary address.
static uint8_t addressed_unit = NUM_UNITS;
static uint8_t talk_unit = 0;
static uint8_t file_unit = 0;    // unit of the open sequential file

static uint8_t stream_slot = 0;

typedef enum {
    receive_idle,
    receive_name,
    receive_command,
} receive_mode_t;

// Native OPEN names and commands share one counted input buffer per peripheral.
typedef struct {
    bool listening;
    receive_mode_t mode;
    uint8_t channel;
    uint8_t secondary;
    uint8_t pending;
    unsigned int length;
    uint8_t bytes[COMMAND_RECEIVE_CAPACITY + 1]; // Spare byte for filename termination.
} receive_state_t;
static receive_state_t receive_states[NUM_UNITS];

// One data channel at a time (all the Waterloo loader needs).
typedef enum {
    file_channel_closed,
    file_channel_readable,
    file_channel_exhausted,
    file_channel_consumed,
} file_channel_state_t;

static file_channel_state_t file_state = file_channel_closed;
static uint8_t file_chan = 0;
static diskstream_t stream;
static bool streaming = false; // addressed TX top-up, independent of channel lifetime
static uint32_t streamed_bytes = 0;

// ---------------------------------------------------------------------------
// CBM RELative-file channels (Super-OS/9 keeps its filesystems inside REL
// files, reclen 129, and reads them with ch15 'P' position commands).
// ---------------------------------------------------------------------------
#define MAX_REL_CHANNELS 4

typedef struct {
    bool in_use;
    uint8_t chan;           // secondary address 0-14 (unique per unit, not globally)
    uint8_t slot;           // global mount slot backing this channel
    uint8_t reclen;
    // Absolute byte offsets into the record stream (vdrive-rel.c semantics).
    // Native trimming retains the selected byte when the whole record is NUL.
    // The flat extension keeps its existing transparent-empty-record behavior.
    uint32_t cur_record;    // 1-based, per CBM convention
    uint32_t record_base;   // physical payload offset, including native pointer wrap
    uint32_t bufptr;
    int32_t  length;
    bool     missing;       // positioned past the last record
    uint16_t pending_size;
    uint16_t pending_pos;
    bool pending_record;
    uint8_t pending[DISKIMAGE_REL_MAX_RECORD_LENGTH];
    // Record-write staging (EOI completes a record without ending LISTEN):
    bool     wr_active;
    bool     wr_overflow;
    uint16_t wr_pos;        // start position within the record (from P)
    uint16_t wr_count;
    uint8_t  wr_buf[DISKIMAGE_REL_MAX_RECORD_LENGTH];
    diskchain_t chain;
} rel_channel_t;

static rel_channel_t rel_chans[MAX_REL_CHANNELS];
static rel_channel_t* data_tx_channel;
static uint8_t data_tx_prefetched;
static uint8_t data_tx_owner; // Retained queue binding, not the transient hardware pause.
static void rel_next_record(rel_channel_t* rc);
static void rel_prepare(rel_channel_t* rc);

// Reconcile the sole bounded REL batch with actual native GETs, not queue writes.
// Fabric resets must be MCU-coordinated through ieee_drive_reset(), which
// invalidates this batch. Independent physical CPU/fabric resets are unsupported.
static void data_tx_sync(void) {
    if (data_tx_channel == NULL) return;
    rel_channel_t* const rc = data_tx_channel;
    const uint8_t advanced = tx_progress(ECONOPET_WB_IEEE_TX_ADDR,
        &data_tx_prefetched, rc->pending_size - rc->pending_pos);
    rc->pending_pos += advanced;
    rc->bufptr += advanced;
    if (advanced != 0 && rc->pending_record && rc->pending_pos == rc->pending_size) {
        // GET of the EOI byte prepares the following record before NDAC.
        // Detach the drained batch before positioning can invalidate a queue.
        data_tx_channel = NULL;
        rel_next_record(rc);
        if (!rc->chain.flat && !rc->missing) rel_prepare(rc);
    }
}

// Stop either source without withdrawing a byte awaiting NDAC.
static void tx_pause(void) {
    // Pause even without a tracked REL batch: full flush leaves TX unbound,
    // and hardware may already be serving a later queued channel selection.
    spi_write_at(ECONOPET_WB_IEEE_SA_ADDR,
        ECONOPET_IEEE_TX_OWNER_VALID_MASK | ECONOPET_IEEE_TX_OWNER_PAUSE_MASK);
}

// Restore a retained queue owner after a temporary RX-service pause.
static void tx_resume(void) {
    if (mcu_talking
        && IEEE_CMD_CHANNEL(receive_states[talk_unit].secondary) == IEEE_COMMAND_CHANNEL) {
        spi_write_at(ECONOPET_WB_IEEE_SA_ADDR, ECONOPET_IEEE_TX_OWNER_VALID_MASK
            | (talk_unit << ECONOPET_IEEE_TX_OWNER_UNIT_SHIFT) | IEEE_COMMAND_CHANNEL);
    } else if (data_tx_owner & ECONOPET_IEEE_TX_OWNER_VALID_MASK) {
        spi_write_at(ECONOPET_WB_IEEE_SA_ADDR, data_tx_owner);
    }
}

// Save the unread tail before firmware or fabric reuses the shared data FIFO.
// Ordinary RX service has already waited for the active presentation to finish.
// Explicit reset/remount instead deliberately invalidates the old transfer.
static void data_tx_park(void) {
    tx_pause();
    data_tx_sync();
    data_tx_channel = NULL;
    data_tx_owner = 0;
}

// Retain the queued owner, releasing it only after ordered RX processing finishes.
static void data_tx_bind(uint8_t unit, uint8_t chan) {
    data_tx_owner = ECONOPET_IEEE_TX_OWNER_VALID_MASK
        | (unit << ECONOPET_IEEE_TX_OWNER_UNIT_SHIFT) | chan;
}

// Channels are keyed by (unit, chan): the d8d9 descriptor table reuses
// secondary addresses 2/3 on BOTH units, so the channel number alone is
// ambiguous.
static rel_channel_t* rel_find(uint8_t unit, uint8_t chan) {
    for (int i = 0; i < MAX_REL_CHANNELS; i++)
        if (rel_chans[i].in_use && rel_chans[i].chan == chan
            && rel_chans[i].slot / DRIVES_PER_UNIT == unit) return &rel_chans[i];
    return NULL;
}

// Resets DOS and protocol state that must not outlive initialization.
static void reset_protocol_state(void) {
    for (unsigned int unit = 0; unit < NUM_UNITS; unit++) {
        set_status(unit * DRIVES_PER_UNIT, st_code_power_on, 0, 0);
    }

    mcu_talking = false;
    addressed_unit = NUM_UNITS;
    talk_unit = 0;
    file_unit = 0;
    stream_slot = 0;
    memset(receive_states, 0, sizeof(receive_states));
    file_state = file_channel_closed;
    file_chan = 0;
    stream = (diskstream_t) { 0 };
    streaming = false;
    streamed_bytes = 0;
    data_tx_channel = NULL;
    data_tx_owner = 0;
    status_tx_unit = NUM_UNITS;
    status_tx_remaining = 0;
    status_tx_pending = false;
    memset(rel_chans, 0, sizeof(rel_chans));
}

static drive_model_t get_drive_model(unsigned int slot) {
    // First check for a disk present in the requested drive.  If that drive is
    // empty, try the other drive in the same unit.  If both drives are empty,
    // return the default drive model (4040).
    if (!drives[slot].present) slot ^= 1u;
    if (!drives[slot].present) return drive_model_4040;

    // Determine the drive model based on the type of the mounted disk image.
    const diskimage_type_t type = drives[slot].image.type;
    return type == diskimage_type_d80 || type == diskimage_type_hdd
        ? drive_model_8050
        : drive_model_4040;
}

// Position the engine at record 'rec', byte 'pos' within it, and compute
// the trimmed length (last non-zero byte), like vdrive_rel_position.
static void rel_position_at(rel_channel_t* rc, uint32_t rec, uint32_t base, uint8_t pos) {
    uint8_t buf[DISKIMAGE_REL_MAX_RECORD_LENGTH];
    if (data_tx_channel == rc) {
        data_tx_park();
        ieee_ctrl_write(ECONOPET_IEEE_CTRL_ENABLE_MASK | ECONOPET_IEEE_CTRL_DATA_FLUSH_MASK);
    }
    rc->pending_size = 0;
    rc->pending_pos = 0;
    rc->pending_record = false;
    rc->cur_record = rec;
    rc->record_base = base;
    rc->missing = (rec == 0)
        || base + rc->reclen > diskchain_size(&rc->chain);
    if (rc->missing) return;

    rc->bufptr = base + pos;
    rc->length = (int32_t) (base + rc->reclen - 1);
    if (!diskchain_read(&rc->chain, base, buf, rc->reclen)) {
        rc->missing = true;
        return;
    }
    // Native FNDLST scans the whole record, independently of the P byte offset.
    const int32_t trim_start = (int32_t) (rc->chain.flat ? rc->bufptr : base);
    while (rc->length >= trim_start
           && buf[rc->length - base] == 0) {
        rc->length--;
    }
    // SETLST falls back to GETPNT only if the entire native record is empty.
    if (!rc->chain.flat && pos < rc->reclen && rc->length < (int32_t) base)
        rc->length = (int32_t) rc->bufptr;
    if (!rc->chain.flat && rc->length < (int32_t) rc->bufptr) {
        const uint32_t block = rc->bufptr / DISKIMAGE_SECTOR_PAYLOAD_SIZE;
        const uint32_t last_block = (uint32_t) rc->length / DISKIMAGE_SECTOR_PAYLOAD_SIZE;
        if (block != last_block) {
            // SETLST cannot search the previous buffer after P selected its successor.
            rc->length = (int32_t) rc->bufptr;
        } else {
            // RDREL compares physical byte pointers, not logical record offsets.
            // A last-character pointer behind P is reached in the next buffer.
            rc->length += DISKIMAGE_SECTOR_PAYLOAD_SIZE;
        }
    }
}

// Explicit P selects a logical record, independently of the current buffer.
static void rel_position(rel_channel_t* rc, uint32_t rec, uint8_t pos) {
    rel_position_at(rc, rec, rec == 0 ? 0 : (rec - 1) * (uint32_t) rc->reclen, pos);
}

// NXTREC retains the active physical buffer when NR is ahead of its pointer.
static void rel_next_record(rel_channel_t* rc) {
    uint32_t base = rc->record_base + rc->reclen;
    if (!rc->chain.flat) {
        base = rc->bufptr / DISKIMAGE_SECTOR_PAYLOAD_SIZE * DISKIMAGE_SECTOR_PAYLOAD_SIZE
            + base % DISKIMAGE_SECTOR_PAYLOAD_SIZE;
        // bufptr is one past the prefetched record's final character.
        if (base < rc->bufptr) base += DISKIMAGE_SECTOR_PAYLOAD_SIZE;
    }
    rel_position_at(rc, rc->cur_record + 1, base, 0);
}

// Save prepared record data independently of shared FIFO ownership (native RD25).
static void rel_prepare(rel_channel_t* rc) {
    if (rc->pending_pos < rc->pending_size) return;
    while (!rc->missing && (int32_t) rc->bufptr > rc->length)
        rel_next_record(rc);
    rc->pending_pos = 0;
    rc->pending_record = false;
    if (rc->missing) {
        set_status(rc->slot, st_code_record_missing, 0, 0);
        rc->pending[0] = 0x0D;
        rc->pending_size = 1;
    } else {
        rc->pending_size = (uint16_t) (rc->length - rc->bufptr + 1);
        vet(rc->pending_size > 0 && rc->pending_size <= sizeof(rc->pending),
            "IEEE REL response exceeds its bounded pending buffer");
        if (!diskchain_read(&rc->chain, rc->bufptr, rc->pending, rc->pending_size)) {
            set_status(rc->slot, st_code_read_error, 0, 0);
            rc->pending[0] = 0x0D;
            rc->pending_size = 1;
        } else {
            rc->pending_record = true;
        }
    }
}

// Restore the immutable unread response, with EOI on its last byte.
static void rel_serve(rel_channel_t* rc) {
    _Static_assert(DISKIMAGE_REL_MAX_RECORD_LENGTH < UINT8_MAX,
        "REL batch must be smaller than the prefetch counter modulus");
    data_tx_sync();
    if (data_tx_channel == rc && rc->pending_pos < rc->pending_size) {
        data_tx_bind(rc->slot / DRIVES_PER_UNIT, rc->chan);
        return;
    }

    data_tx_park();
    ieee_ctrl_write(ECONOPET_IEEE_CTRL_ENABLE_MASK | ECONOPET_IEEE_CTRL_DATA_FLUSH_MASK);
    rel_prepare(rc);

    const uint16_t n = rc->pending_size - rc->pending_pos;
    data_tx_prefetched = spi_read_at(ECONOPET_WB_IEEE_TX_ADDR);
    data_tx_channel = rc;
    // Keep presentation paused until the immutable unread suffix is restored.
    if (n > 1)
        spi_write_same_block(ECONOPET_WB_IEEE_TX_ADDR, rc->pending + rc->pending_pos, n - 1u);
    spi_write_at(ECONOPET_WB_IEEE_TX_LAST_ADDR, rc->pending[rc->pending_size - 1]);
    data_tx_bind(rc->slot / DRIVES_PER_UNIT, rc->chan);
    streamed_bytes += n;   // REL read, attributed to this channel's slot
}

// Arm write staging without replacing the successor prepared by native RD40.
static void rel_write_arm(rel_channel_t* rc) {
    const uint32_t base = rc->record_base;
    rc->wr_active = true;
    rc->wr_overflow = false;
    rc->wr_count = 0;
    if (rc->missing || rc->bufptr < base || rc->bufptr >= base + rc->reclen) {
        rc->wr_pos = 0;         // fresh record start
    } else {
        rc->wr_pos = (uint16_t) (rc->bufptr - base);
    }
}

// Enter LISTEN on this channel, replacing its old read response.
static void rel_write_begin(rel_channel_t* rc) {
    data_tx_sync();
    if (data_tx_channel == rc) {
        data_tx_park();
        ieee_ctrl_write(ECONOPET_IEEE_CTRL_ENABLE_MASK | ECONOPET_IEEE_CTRL_DATA_FLUSH_MASK);
    }
    rc->pending_size = 0;
    rc->pending_pos = 0;
    rel_write_arm(rc);
}

static void rel_write_byte(rel_channel_t* rc, uint8_t byte) {
    if (rc->wr_pos + rc->wr_count >= rc->reclen) {
        rc->wr_overflow = true;
        set_status(rc->slot, st_code_record_overflow, 0, 0);
        return;
    }
    rc->wr_buf[rc->wr_count++] = byte;
}

// Complete a staged record, zero-fill its tail and prepare its successor.
static bool rel_write_commit(rel_channel_t* rc) {
    uint8_t rec[DISKIMAGE_REL_MAX_RECORD_LENGTH];
    rc->wr_active = false;
    if (rc->missing) {
        set_status(rc->slot, st_code_record_missing, 0, 0);
        return false;
    }
    uint32_t base = rc->record_base;
    if (!diskchain_read(&rc->chain, base, rec, rc->reclen)) {
        set_status(rc->slot, st_code_read_error, 0, 0);
        return false;
    }
    memcpy(rec + rc->wr_pos, rc->wr_buf, rc->wr_count);
    memset(rec + rc->wr_pos + rc->wr_count, 0,
           rc->reclen - rc->wr_pos - rc->wr_count);
    if (!diskchain_write(&rc->chain, base, rec, rc->reclen)) {
        set_status(rc->slot, st_code_write_protect, 0, 0);
        log_info("ieee: REL write failed (read-only?) rec %lu",
                 (unsigned long) rc->cur_record);
        return false;
    }
    // WR40 reaches RD40/NXTREC with the physical buffer pointer, not a fresh P.
    rel_position_at(rc, rc->cur_record + 1, base + rc->reclen, 0);
    if (!rc->chain.flat && !rc->missing) rel_prepare(rc);
    return true;
}

// Release one unit's data channel without discarding a peer's selected source.
static void close_channel(uint8_t unit, uint8_t channel) {
    const uint8_t owner = ECONOPET_IEEE_TX_OWNER_VALID_MASK
        | (unit << ECONOPET_IEEE_TX_OWNER_UNIT_SHIFT) | channel;
    if (data_tx_owner == owner) {
        data_tx_park();
        ieee_ctrl_write(ECONOPET_IEEE_CTRL_ENABLE_MASK | ECONOPET_IEEE_CTRL_DATA_FLUSH_MASK);
    }
    rel_channel_t* const rc = rel_find(unit, channel);
    if (rc != NULL) {
        rc->in_use = false;
        rc->wr_active = false;
    }
    if (file_unit == unit && file_chan == channel) {
        file_state = file_channel_closed;
        streaming = false;
    }
}

// Starts counted command collection for both OPEN 15 and secondary address 15.
static void ch15_begin(uint8_t unit) {
    receive_states[unit].mode = receive_command;
    receive_states[unit].channel = IEEE_COMMAND_CHANNEL;
}

// Execute the collected command after native IDLE dispatch, not on receipt of EOI.
static void ch15_execute(uint8_t ch15_unit) {
    receive_state_t* const command = &receive_states[ch15_unit];
    const uint8_t* const ch15_cmd = command->bytes;
    const unsigned int ch15_cmd_len = command->length;
    command->length = 0;
    if (ch15_cmd_len == 0) return;

    if (ch15_cmd[0] == 'P' && ch15_cmd_len >= 4) {
        // P + channel byte ($60 | chan) + record lo + record hi [+ position]
        rel_channel_t* rc = rel_find(ch15_unit, ch15_cmd[1] & 0x0F);
        uint32_t rec = ch15_cmd[2] | ((uint32_t) ch15_cmd[3] << 8);
        uint8_t pos = (ch15_cmd_len >= 5 && ch15_cmd[4] >= 1) ? (uint8_t) (ch15_cmd[4] - 1) : 0;
        if (rc == NULL) {
            const uint8_t channel = ch15_cmd[1] & IEEE_CMD_CHANNEL_MASK;
            const bool sequential = channel == IEEE_CMD_CHANNEL_MASK
                || (file_state != file_channel_closed
                    && file_unit == ch15_unit && file_chan == channel);
            set_status(ch15_unit * DRIVES_PER_UNIT
                       + drive_status[ch15_unit].drive,
                       sequential ? st_code_file_type_mismatch : st_code_no_channel, 0, 0);
        } else {
            const bool overflow = pos >= rc->reclen;
            if (overflow) pos = 0;
            rel_position(rc, rec, pos);
            set_status(rc->slot,
                       rc->missing ? st_code_record_missing
                           : overflow ? st_code_record_overflow : st_code_ok, 0, 0);
        }
    } else if (ch15_cmd[0] == 'I' || ch15_cmd[0] == 'V') {
        uint8_t drive = drive_status[ch15_unit].drive;
        for (unsigned int index = 1; index < ch15_cmd_len; index++) {
            if (ch15_cmd[index] == '0' || ch15_cmd[index] == '1') {
                drive = (uint8_t) (ch15_cmd[index] - '0');
                break;
            }
        }
        set_status(ch15_unit * DRIVES_PER_UNIT + drive, st_code_ok, 0, 0);
    } else {
        log_info("ieee: ch15 command %02x len %u (ignored)", ch15_cmd[0], ch15_cmd_len);
    }
}

// Consume a pending OPEN from the shared buffer when the peripheral reaches IDLE.
static void resolve_open(uint8_t open_unit) {
    receive_state_t* const input = &receive_states[open_unit];
    char* const open_name = (char*) input->bytes;
    const uint8_t open_chan = input->channel;
    open_name[input->length] = '\0';
    input->length = 0;

    // Split off the ",PRG"/",SEQ" type suffix.
    char* comma = strchr(open_name, ',');
    if (comma != NULL) *comma = '\0';

    rel_channel_t* const previous = rel_find(open_unit, open_chan & 0x0F);
    if (previous != NULL) previous->in_use = false;

    // Slot = the addressed unit's pair, plus the drive number from the
    // "0:"/"1:" (or "0."/"1.") prefix; default drive 0 of that unit.
    unsigned int slot = open_unit * DRIVES_PER_UNIT;
    if ((open_name[0] == '0' || open_name[0] == '1')
        && (open_name[1] == ':' || open_name[1] == '.')) {
        slot = open_unit * DRIVES_PER_UNIT
             + (unsigned int) (open_name[0] - '0');
    }
    // Fall back to the unit's other drive only for ordinary files: OS-9's
    // per-drive REL containers are drive-specific, and aliasing them can
    // corrupt the mounted system disk.
    if (!drives[slot].present && drives[slot ^ 1u].present
        && drives[slot ^ 1u].image.type != diskimage_type_hdd) {
        log_info("ieee: drive %u empty, using drive %u", slot, slot ^ 1u);
        slot ^= 1u;
    }

    file_state = file_channel_closed;
    streaming = false;
    // New file: discard any stale queued data from a previous channel.
    data_tx_park();
    ieee_ctrl_write(ECONOPET_IEEE_CTRL_ENABLE_MASK | ECONOPET_IEEE_CTRL_DATA_FLUSH_MASK);

    if (!drives[slot].present) {
        set_status(slot, st_code_file_not_found, 0, 0);
        log_info("ieee: OPEN '%s': no disk image mounted", open_name);
        return;
    }

    diskimage_entry_t entry;
    if (!diskimage_find(&drives[slot].image, open_name, &entry)) {
        set_status(slot, st_code_file_not_found, 0, 0);
        log_info("ieee: OPEN '%s': not found", open_name);
        return;
    }

    if (entry.file_type == DISKIMAGE_FTYPE_REL) {
        // Relative file: record-addressed access on this channel (used by
        // Super-OS/9 for its embedded filesystems). Reuse the slot if this
        // channel was already open, else take a free one.
        rel_channel_t* rc = rel_find(open_unit, open_chan & 0x0F);
        if (rc == NULL) {
            for (int i = 0; i < MAX_REL_CHANNELS; i++)
                if (!rel_chans[i].in_use) { rc = &rel_chans[i]; break; }
        }
        // Use the mount-time chain cache; live build only for chains it
        // doesn't know (e.g. more REL files than cache slots).
        const diskchain_t* cached = (rc != NULL)
            ? chain_cache_find(slot, entry.start_track, entry.start_sector)
            : NULL;
        if (cached != NULL) {
            rc->chain = *cached;
        }
        if (rc == NULL
            || (cached == NULL
                && !diskchain_build(&rc->chain, &drives[slot].image,
                                    entry.start_track, entry.start_sector))) {
            set_status(slot, st_code_file_not_found, 0, 0);
            log_info("ieee: REL OPEN '%s' failed", open_name);
            return;
        }
        if (entry.record_len > DISKIMAGE_REL_MAX_RECORD_LENGTH) {
            set_status(slot, st_code_file_not_found, 0, 0);
            log_info("ieee: REL OPEN '%s' bad reclen %u", open_name, entry.record_len);
            return;
        }
        rc->in_use = true;
        rc->wr_active = false;
        rc->slot = (uint8_t) slot;
        rc->chan = open_chan & 0x0F;
        rc->reclen = entry.record_len >= DISKIMAGE_REL_MIN_RECORD_LENGTH
            ? entry.record_len : 129;
        rel_position(rc, 1, 0);
        set_status(slot, st_code_ok, 0, 0);
        log_info("ieee: REL OPEN '%s' ok (chan %u, reclen %u, %lu bytes)",
                 open_name, rc->chan, rc->reclen,
                 (unsigned long) diskchain_size(&rc->chain));
        return;
    }

    if (!diskstream_open(&stream, &drives[slot].image,
                         entry.start_track, entry.start_sector)) {
        set_status(slot, st_code_file_not_found, 0, 0);
        log_info("ieee: OPEN '%s': stream open failed", open_name);
        return;
    }

    file_state = file_channel_readable;
    stream_slot = (uint8_t) slot;
    file_chan = open_chan;
    file_unit = open_unit;
    set_status(slot, st_code_ok, 0, 0);
    streamed_bytes = 0;
    log_info("ieee: OPEN '%s' ok (chan %u)", open_name, file_chan);
}

// Native TALK/TALK1 returns through NOTLK for unallocated or consumed SEQ data.
static bool talk_source_ready(void) {
    if (!mcu_talking) return false;
    const uint8_t channel = IEEE_CMD_CHANNEL(receive_states[talk_unit].secondary);
    return channel == IEEE_COMMAND_CHANNEL || rel_find(talk_unit, channel) != NULL
        || ((file_state == file_channel_readable || file_state == file_channel_exhausted)
            && file_unit == talk_unit && file_chan == channel);
}

// Dispatch at IDLE/VIDLE. An explicit routine exit bypasses retained role ownership.
static void dispatch_pending(uint8_t idle_unit) {
    for (uint8_t unit = 0; unit < NUM_UNITS; ++unit) {
        receive_state_t* const input = &receive_states[unit];
        if (!input->pending || (input->listening && unit != idle_unit)
            || (talk_unit == unit && unit != idle_unit && talk_source_ready())) continue;
        input->pending = 0;
        unsigned int length = input->length;
        if (length > 1) {
            if (input->bytes[length - 1] == '\r') --length;
            else if (input->bytes[length - 2] == '\r') length -= 2;
        }
        if (length > COMMAND_VALIDATION_LIMIT) {
            input->length = 0;
            set_status(unit * DRIVES_PER_UNIT + drive_status[unit].drive, st_code_long_line, 0, 0);
        } else if (IEEE_CMD_IS_OPEN(input->secondary)
            && IEEE_CMD_CHANNEL(input->secondary) != IEEE_COMMAND_CHANNEL) {
            input->length = length;
            resolve_open(unit);
        } else {
            // RECORD reads raw binary fields outside CMDSIZ. Text commands do not.
            if (input->bytes[0] != 'P') input->length = length;
            ch15_execute(unit);
        }
    }
}

// Queue only the unread status suffix, retaining DOS lifetime until its CR GET.
static void push_status(unsigned int unit) {
    status_tx_sync();
    ieee_ctrl_write(ECONOPET_IEEE_CTRL_ENABLE_MASK | ECONOPET_IEEE_CTRL_STATUS_FLUSH_MASK);
    drive_status_t* const retained = &drive_status[unit];
    if (!retained->formatted) {
        retained->model = get_drive_model(unit * DRIVES_PER_UNIT + retained->drive);
        retained->formatted = true;
    }
    const char* text;
    char line[IEEE_STATUS_LINE_CAPACITY];
    const drive_status_t status = drive_status[unit];
    const drive_model_t model = status.model;

    switch (status.code) {
        case st_code_ok:              text = " OK"; break;
        case st_code_read_error:      text = "READ ERROR"; break;
        case st_code_write_protect:   text = "WRITE PROTECT ON"; break;
        case st_code_long_line:       text = "SYNTAX ERROR"; break;
        case st_code_record_missing:  text = "RECORD NOT PRESENT"; break;
        case st_code_record_overflow: text = "OVERFLOW IN RECORD"; break;
        case st_code_file_not_found:  text = " FILE NOT FOUND"; break;
        case st_code_file_type_mismatch: text = " FILE TYPE MISMATCH"; break;
        case st_code_no_channel:      text = "NO CHANNEL"; break;
        case st_code_power_on:
            text = model == drive_model_8050
                ? "CBM DOS V2.7"
                : "CBM DOS V2";
            break;
        default:                      text = ""; break;
    }
    const int n = model == drive_model_8050
        ? snprintf(line, sizeof(line), "%02u,%s,%02u,%02u,%u",
                   (unsigned int) status.code, text, status.track, status.sector,
                   status.drive)
        : snprintf(line, sizeof(line), "%02u,%s,%02u,%02u",
                   (unsigned int) status.code, text, status.track, status.sector);

    vet(n >= 0 && (size_t) n <= IEEE_STATUS_TEXT_MAX,
        "IEEE status text exceeds its %u-byte capacity", IEEE_STATUS_TEXT_MAX);
    vet(status.read_pos <= (unsigned int) n, "IEEE status progress exceeds its text");
    status_tx_unit = (uint8_t) unit;
    status_tx_generation = status.generation;
    status_tx_remaining = (unsigned int) n + 1 - status.read_pos;
    status_tx_prefetched = spi_read_at(ECONOPET_WB_IEEE_TXS_ADDR);
    for (unsigned int i = status.read_pos; i < (unsigned int) n; i++)
        spi_write_at(ECONOPET_WB_IEEE_TXS_ADDR, (uint8_t) line[i]);
    spi_write_at(ECONOPET_WB_IEEE_TXS_LAST_ADDR, 0x0D);
}

// Fill the TX FIFO from the open stream until the fabric reports no room
// for another chunk.
static void service_tx(void) {
    _Static_assert(ECONOPET_IEEE_TX_BURST_CHUNK >= DISKIMAGE_SECTOR_LINK_SIZE + 2,
        "native one-byte EOF response must fit a TX burst");
    // An underrun mid-record times out the kernel's counted read.
    uint8_t buf[ECONOPET_IEEE_TX_BURST_CHUNK];

    for (;;) {
        uint8_t ctrl = spi_read_at(ECONOPET_WB_IEEE_CTRL_ADDR);
        if (!(ctrl & ECONOPET_IEEE_CTRL_RD_TX_ROOM_MASK)) return;

        size_t n = 0;
        bool last = false;
        uint8_t last_byte = 0;

        while (n < ECONOPET_IEEE_TX_BURST_CHUNK) {
            uint8_t byte;
            bool is_last;
            if (!diskstream_next(&stream, &byte, &is_last)) {
                // Preserve the valid prefix, but release the failed channel
                // without inventing an EOF byte, as native TLKERR/FRECHN do.
                if (n > 0) spi_write_same_block(ECONOPET_WB_IEEE_TX_ADDR, buf, n);
                streamed_bytes += n;
                log_info("ieee: read error after %lu bytes", (unsigned long) streamed_bytes);
                set_status(stream_slot, st_code_read_error, 0, 0);
                file_state = file_channel_closed;
                streaming = false;
                return;
            }
            if (is_last) {
                // REL reads share byte accounting, not this stream's cursor.
                if (stream.sectors == 1 && stream.pos == DISKIMAGE_SECTOR_LINK_SIZE + 1
                    && (stream.img->type == diskimage_type_d64
                        || stream.img->type == diskimage_type_d80)) {
                    // Native OR30 loses the sole byte's initial EOF. The wrapped
                    // buffer pointer leaks both link bytes before repeating it.
                    buf[n++] = byte;
                    for (size_t i = 0; i < DISKIMAGE_SECTOR_LINK_SIZE; ++i)
                        buf[n++] = stream.buf[i];
                }
                last = true;
                last_byte = byte;
                break;
            }
            buf[n++] = byte;
        }

        if (n > 0) spi_write_same_block(ECONOPET_WB_IEEE_TX_ADDR, buf, n);
        streamed_bytes += n;

        if (last) {
            // Final byte carries EOI: separate address, so not part of the burst.
            spi_write_at(ECONOPET_WB_IEEE_TX_LAST_ADDR, last_byte);
            streamed_bytes++;
            log_info("ieee: stream complete, %lu bytes", (unsigned long) streamed_bytes);
            streaming = false;
            file_state = file_channel_exhausted;
            return;
        }
    }
}

static void handle_command(uint8_t cmd) {
    switch (IEEE_CMD_CLASS(cmd)) {
        case IEEE_CMD_LISTEN_BASE:  // LISTEN / UNLISTEN
            addressed_unit = NUM_UNITS;
            if (cmd == IEEE_CMD_UNLISTEN) {
                for (unsigned int unit = 0; unit < NUM_UNITS; ++unit) {
                    receive_states[unit].listening = false;
                    receive_states[unit].mode = receive_idle;
                }
                for (int i = 0; i < MAX_REL_CHANNELS; i++)
                    if (rel_chans[i].in_use && rel_chans[i].wr_active) {
                        if (rel_chans[i].wr_count != 0) rel_write_commit(&rel_chans[i]);
                        else rel_chans[i].wr_active = false;
                    }
            } else {
                const uint8_t a = IEEE_CMD_ADDRESS(cmd);
                if (a >= DEV_ADDR && a < DEV_ADDR + NUM_UNITS) {
                    addressed_unit = (uint8_t) (a - DEV_ADDR);
                    receive_states[addressed_unit].listening = true;
                    receive_states[addressed_unit].mode = receive_idle;
                    receive_states[addressed_unit].channel = DEFAULT_CHANNEL;
                    receive_states[addressed_unit].secondary = IEEE_CMD_LISTEN_BASE;
                    if (mcu_talking && talk_unit == addressed_unit) {
                        mcu_talking = false;
                        streaming = false;
                    }
                }
            }
            break;

        case IEEE_CMD_TALK_BASE:  // TALK / UNTALK
            addressed_unit = NUM_UNITS;
            streaming = false;
            if (cmd == IEEE_CMD_UNTALK) {
                mcu_talking = false;
                // Channel data persists across UNTALK (continuation) -- just
                // pause the top-up until the next TALK on the data channel.
                streaming = false;
            } else {
                uint8_t a = IEEE_CMD_ADDRESS(cmd);
                mcu_talking = (a >= DEV_ADDR && a < DEV_ADDR + NUM_UNITS);
                if (mcu_talking) {
                    talk_unit = (uint8_t) (a - DEV_ADDR);
                    addressed_unit = talk_unit;
                    receive_states[talk_unit].listening = false;
                    receive_states[talk_unit].mode = receive_idle;
                    receive_states[talk_unit].channel = DEFAULT_CHANNEL;
                    receive_states[talk_unit].secondary = IEEE_CMD_LISTEN_BASE;
                }
            }
            break;

        case IEEE_CMD_SECONDARY_BASE:  // secondary address (data channel)
            if (addressed_unit == NUM_UNITS) break;
            receive_states[addressed_unit].secondary = cmd;
            if (receive_states[addressed_unit].listening) {
                rel_channel_t* wrc;
                const uint8_t listen_unit = addressed_unit;
                const uint8_t listen_chan = IEEE_CMD_CHANNEL(cmd);
                receive_states[listen_unit].channel = listen_chan;
                receive_states[listen_unit].mode = receive_idle;
                if (listen_chan == 15) {
                    ch15_begin(listen_unit);
                } else if ((wrc = rel_find(listen_unit, listen_chan)) != NULL) {
                    rel_write_begin(wrc);
                }
            }
            if (mcu_talking && talk_unit == addressed_unit) {
                streaming = false;
                uint8_t chan = IEEE_CMD_CHANNEL(cmd);
                rel_channel_t* rc;
                if (chan == 15) {
                    status_tx_pending = true;
                } else if ((rc = rel_find(talk_unit, chan)) != NULL) {
                    rel_serve(rc);
                } else if ((file_state == file_channel_readable
                            || file_state == file_channel_exhausted)
                    && talk_unit == file_unit && chan == file_chan) {
                    if (data_tx_channel != NULL) {
                        data_tx_park();
                        ieee_ctrl_write(ECONOPET_IEEE_CTRL_ENABLE_MASK | ECONOPET_IEEE_CTRL_DATA_FLUSH_MASK);
                    }
                    data_tx_bind(file_unit, file_chan);
                    streaming = file_state == file_channel_readable;
                }
            }
            break;

        case IEEE_CMD_CLOSE_BASE:  // CLOSE ($Ex) / OPEN ($Fx)
            if (addressed_unit == NUM_UNITS
                || !receive_states[addressed_unit].listening) break;
            receive_states[addressed_unit].secondary = cmd;
            // Fabric has already flushed ordinary OPEN/CLOSE, but its GET
            // counter still records genuine progress before that discard.
            if (IEEE_CMD_CHANNEL(cmd) != 15) data_tx_park();
            if (IEEE_CMD_IS_OPEN(cmd)) {
                receive_states[addressed_unit].channel = IEEE_CMD_CHANNEL(cmd);
                if (IEEE_CMD_CHANNEL(cmd) == 15) {
                    ch15_begin(addressed_unit);
                } else {
                    receive_states[addressed_unit].mode = receive_name;
                }
            } else {
                const uint8_t channel = IEEE_CMD_CHANNEL(cmd);
                if (channel == IEEE_COMMAND_CHANNEL) {
                    for (int index = IEEE_COMMAND_CHANNEL - 1; index >= 0; --index)
                        close_channel(addressed_unit, (uint8_t) index);
                    set_status(addressed_unit * DRIVES_PER_UNIT
                        + drive_status[addressed_unit].drive, st_code_ok, 0, 0);
                } else {
                    close_channel(addressed_unit, channel);
                    ieee_ctrl_write(ECONOPET_IEEE_CTRL_ENABLE_MASK | ECONOPET_IEEE_CTRL_DATA_FLUSH_MASK);
                }
            }
            break;

        default:
            break;
    }
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

// Apply mounted-media enablement after callers have parked or reset DOS state.
static void sync_emulation_enabled(void) {
    bool has_mounted_drive = false;
    for (unsigned int slot = 0; slot < NUM_DRIVES; slot++) {
        if (drives[slot].present) {
            has_mounted_drive = true;
            break;
        }
    }
    emulation_enabled = has_mounted_drive;
    if (emulation_enabled) {
        ieee_ctrl_write(ECONOPET_IEEE_CTRL_ENABLE_MASK | ECONOPET_IEEE_CTRL_FLUSH_MASK);
        log_info("ieee: virtual drive enabled");
    } else {
        ieee_ctrl_write(0);
        log_info("ieee: virtual drive off (real hardware bus)");
    }
}

void ieee_drive_init(void) {
    // Initialization always starts from a stock-PET bus and fresh DOS state.
    ieee_drive_unmount_all();
}

void ieee_drive_reset(void) {
    reset_protocol_state();
    if (emulation_enabled) ieee_ctrl_write(ECONOPET_IEEE_CTRL_ENABLE_MASK | ECONOPET_IEEE_CTRL_FLUSH_MASK);
}

void ieee_drive_unmount_all(void) {
    for (unsigned int slot = 0; slot < NUM_DRIVES; slot++) {
        if (drives[slot].file != NULL) fclose(drives[slot].file);
        drives[slot] = (drive_t) { 0 };
        for (unsigned int i = 0; i < CHAIN_CACHE_SLOTS; i++) {
            chain_cache[slot][i].valid = false;
        }
    }
    reset_protocol_state();
    sync_emulation_enabled();
}

bool ieee_drive_mount(unsigned int slot, const char* filename) {
    if (slot >= NUM_DRIVES) return false;

    bool mounted = mount_image(slot, filename);
    if (!mounted) log_info("ieee: could not mount disk image: /disks/%s", filename);
    if (mounted) {
        status_tx_sync();
        status_tx_unit = NUM_UNITS;
        status_tx_pending = false;
        data_tx_park();
        for (unsigned int index = 0; index < MAX_REL_CHANNELS; ++index)
            if (rel_chans[index].slot == slot) rel_chans[index].in_use = false;
        if (stream_slot == slot) {
            file_state = file_channel_closed;
            streaming = false;
        }
        sync_emulation_enabled();
    }
    return mounted;
}

void ieee_drive_task(void) {
    if (!emulation_enabled) return;
    data_tx_sync();
    status_tx_sync();

    // Drain the RX FIFO (commands and listener data).
    for (unsigned int i = 0; i < 64; i++) {
        uint8_t st = spi_read_at(ECONOPET_WB_IEEE_STATUS_ADDR);
        if (!(st & ECONOPET_IEEE_ST_RX_AVAIL_MASK)) break;

        // Hardware may already be serving a newer selection than this RX head.
        // Pause first, then yield until NDAC releases the latched byte. Never
        // block the MCU or flush/rebind its FIFO while a listener is sampling.
        tx_pause();
        status_tx_sync();
        const uint8_t ctrl = spi_read_at(ECONOPET_WB_IEEE_CTRL_ADDR);
        if (ctrl & (ECONOPET_IEEE_CTRL_RD_DATA_ACTIVE_MASK
            | ECONOPET_IEEE_CTRL_RD_STATUS_ACTIVE_MASK)) return;

        // Sample the head's ATN flag immediately before its paired RX read,
        // after the pause transactions (also preserving semantic RX tracing).
        st = spi_read_at(ECONOPET_WB_IEEE_STATUS_ADDR);
        bool is_atn = (st & ECONOPET_IEEE_ST_RX_ATN_MASK) != 0;
        uint8_t byte = spi_read_at(ECONOPET_WB_IEEE_RX_ADDR);
        spi_write_at(ECONOPET_WB_IEEE_RX_ADDR, 0);   // explicit pop (reads are side-effect-free)

        if (ctrl & ECONOPET_IEEE_CTRL_RD_RX_ATN_END_MASK) {
            if (byte == 0) {
                dispatch_pending(NUM_UNITS);
            } else {
                for (uint8_t unit = 0; unit < NUM_UNITS; ++unit) {
                    const receive_state_t* const input = &receive_states[unit];
                    // LSN25 keeps accepting SA1. OPEN/commands and writable REL
                    // stay in LSN30/PUT, even after this transport checkpoint.
                    if ((byte & (1u << unit)) && input->listening
                        && input->mode == receive_idle && input->channel != IEEE_SAVE_CHANNEL
                        && rel_find(unit, input->channel) == NULL)
                        dispatch_pending(unit);
                }
            }
            continue;
        }
        if (ctrl & ECONOPET_IEEE_CTRL_RD_RX_TALK_EVENT_MASK) {
            const uint8_t unit = (byte >> ECONOPET_IEEE_TX_OWNER_UNIT_SHIFT) & (NUM_UNITS - 1);
            const uint8_t channel = IEEE_CMD_CHANNEL(byte);
            const bool eoi_get = (byte & ECONOPET_IEEE_RX_TALK_EOI_GET_MASK) != 0;
            if (eoi_get && file_state == file_channel_exhausted
                && file_unit == unit && file_chan == channel)
                file_state = file_channel_consumed;
            // ATN-interrupted EOF records progress only. TALK1 rechecks readiness,
            // while unread TLK20 termination returns to IDLE even for a ready source.
            if (!eoi_get || (byte & ECONOPET_IEEE_RX_TALK_RECHECK_MASK))
                dispatch_pending(eoi_get ? NUM_UNITS : unit);
            continue;
        }
        if (is_atn) {
            handle_command(byte);
        } else {
            for (uint8_t unit = 0; unit < NUM_UNITS; ++unit) {
                receive_state_t* const input = &receive_states[unit];
                if (!input->listening) continue;
                if (input->mode != receive_idle) {
                    const unsigned int capacity =
                        get_drive_model(unit * DRIVES_PER_UNIT + drive_status[unit].drive)
                            == drive_model_4040
                        ? COMMAND_4040_RECEIVE_CAPACITY : COMMAND_RECEIVE_CAPACITY;
                    if (input->length < capacity)
                        input->bytes[input->length++] = byte;
                    if (ctrl & ECONOPET_IEEE_CTRL_RD_RX_EOI_MASK) {
                        ++input->pending;
                    }
                } else {
                    rel_channel_t* wrc = rel_find(unit, input->channel);
                    if (wrc != NULL && wrc->wr_active) {
                        rel_write_byte(wrc, byte);
                        if (ctrl & ECONOPET_IEEE_CTRL_RD_RX_EOI_MASK) {
                            const bool overflow = wrc->wr_overflow;
                            if (rel_write_commit(wrc) && !overflow) {
                                // DOS 2.7 WR45 clears status. DOS 2 WR51 does not.
                                if (get_drive_model(wrc->slot) == drive_model_8050)
                                    set_status(wrc->slot, st_code_ok, 0, 0);
                                rel_write_arm(wrc);
                            }
                        }
                    }
                }
            }
        }
    }

    // Native UNLISTEN/foreign LISTEN preserve TLKACT and its queued response.
    // Only resume a retained binding, never one invalidated by queue parking.
    if (spi_read_at(ECONOPET_WB_IEEE_STATUS_ADDR) & ECONOPET_IEEE_ST_RX_AVAIL_MASK) return;
    // Deferred peer OPEN/CLOSE may reuse TX after the final TALK selection.
    // Restore its immutable unread REL suffix before releasing the paused source.
    if (mcu_talking && !(data_tx_owner & ECONOPET_IEEE_TX_OWNER_VALID_MASK)) {
        rel_channel_t* const rc =
            rel_find(talk_unit, IEEE_CMD_CHANNEL(receive_states[talk_unit].secondary));
        if (rc != NULL) rel_serve(rc);
    }
    if (status_tx_pending) {
        if (mcu_talking
            && IEEE_CMD_CHANNEL(receive_states[talk_unit].secondary) == IEEE_COMMAND_CHANNEL)
            push_status(talk_unit);
        status_tx_pending = false;
    }
    // A secondary accepted during TXS filling may have discarded its prefix.
    // Keep sources paused until ordered RX processing rebuilds the response.
    if (spi_read_at(ECONOPET_WB_IEEE_STATUS_ADDR) & ECONOPET_IEEE_ST_RX_AVAIL_MASK) return;
    tx_resume();
    if (streaming) service_tx();
}
