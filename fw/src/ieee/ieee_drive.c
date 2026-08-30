// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "ieee_drive.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "pico/time.h"

#include "diag/log/log.h"
#include "diskimage.h"
#include "driver.h"

// FPGA register block (see gw common_pkg.sv WB_IEEE_BASE = 5'b01110 and
// ieee.sv for semantics).
#define ADDR_IEEE (0b01110 << 15)

#define IEEE_REG_CTRL    (ADDR_IEEE + 0)
#define IEEE_REG_STATUS  (ADDR_IEEE + 1)
#define IEEE_REG_RX      (ADDR_IEEE + 2)
#define IEEE_REG_TX      (ADDR_IEEE + 3)
#define IEEE_REG_TX_LAST (ADDR_IEEE + 4)
#define IEEE_REG_SA      (ADDR_IEEE + 5)
#define IEEE_REG_TXS      (ADDR_IEEE + 6)   // status-channel TX
#define IEEE_REG_TXS_LAST (ADDR_IEEE + 7)   // status-channel TX, final byte (EOI)

#define IEEE_CTRL_ENABLE     0x01   // write bit
#define IEEE_CTRL_FLUSH      0x02   // write bit
#define IEEE_CTRL_DATA_FLUSH 0x04   // write bit: flush the data TX FIFO only

// CTRL register READ-back bits (asymmetric with the write bits above).
#define IEEE_CTRL_RD_TX_ROOM 0x02   // data TX FIFO has room for >= TX_BURST_CHUNK

// Data TX FIFO burst-fill chunk. Must match TX_BURST_CHUNK in ieee.sv: the
// fabric's tx_room watermark guarantees space for this many bytes, so a burst
// of up to this size never overflows.
#define TX_BURST_CHUNK 64

#define IEEE_ST_RX_AVAIL     0x01
#define IEEE_ST_RX_ATN       0x02
#define IEEE_ST_TX_FULL      0x04
#define IEEE_ST_TX_EMPTY     0x08
#define IEEE_ST_ATN          0x10
#define IEEE_ST_LISTENING    0x20
#define IEEE_ST_TALK_STARVED 0x80
#define IEEE_ST_TALKING      0x40

#define DEV_ADDR 8

// The fabric answers devices DEV_ADDR and DEV_ADDR+1 (units 8 and 9), each a
// dual-drive unit -- Super-OS/9's d8d9 configuration uses all four drives.
// Mount slot = unit * 2 + drive, so slots 0/1 are device 8's "0:"/"1:" drives
// and slots 2/3 are device 9's.
#define NUM_UNITS  2
#define NUM_DRIVES 4

// ----------------------------------------------------------------------------
// Disk images
// ----------------------------------------------------------------------------

typedef struct {
    FILE* file;
    diskimage_t image;
    bool present;
    char name[32];              // image filename currently "inserted"
    int library_index;          // index into the image library, or -1
} drive_t;

static drive_t drives[NUM_DRIVES];
static bool emulation_enabled = false;

// Bumped on every successful mount; the HUD overlay flashes on change.
static unsigned mount_generation = 0;

unsigned ieee_drive_mount_generation(void) {
    return mount_generation;
}

// Library of images found in /disks at boot.
#define MAX_LIBRARY 24
static char library[MAX_LIBRARY][32];
static unsigned int library_count = 0;

static char toast[48];
static uint32_t toast_until_ms = 0;
static bool overlay_debug_on = false;  // F8 toggles the status line

static void scan_library(void) {
    DIR* dir = opendir("/disks");
    if (dir == NULL) return;

    struct dirent* e;
    while ((e = readdir(dir)) != NULL && library_count < MAX_LIBRARY) {
        const char* dot = strrchr(e->d_name, '.');
        if (dot == NULL) continue;
        if (strcasecmp(dot, ".d80") != 0 && strcasecmp(dot, ".d64") != 0) continue;
        if (strlen(e->d_name) >= sizeof(library[0])) continue;
        strcpy(library[library_count++], e->d_name);
    }
    closedir(dir);
    log_info("ieee: %u disk image(s) in /disks", library_count);
}

static void show_toast(const char* msg) {
    snprintf(toast, sizeof(toast), "%s", msg);
    toast_until_ms = to_ms_since_boot(get_absolute_time()) + 3000;
}

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

static void chain_cache_build(unsigned int n) {
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    unsigned int slot = 0;
    diskimage_entry_t e;

    for (unsigned int i = 0; i < CHAIN_CACHE_SLOTS; i++) chain_cache[n][i].valid = false;

    for (unsigned int i = 0; diskimage_entry(&drives[n].image, i, &e); i++) {
        if (e.file_type != DISKIMAGE_FTYPE_REL) continue;
        if (slot >= CHAIN_CACHE_SLOTS) {
            log_info("ieee: drive %u: more REL files than %u cache slots", n, CHAIN_CACHE_SLOTS);
            break;
        }
        chain_cache_entry_t* c = &chain_cache[n][slot];
        if (!diskchain_build(&c->chain, &drives[n].image, e.start_track, e.start_sector))
            continue;
        c->track = e.start_track;
        c->sector = e.start_sector;
        c->valid = true;
        slot++;
    }
    log_info("ieee: drive %u: %u REL chain(s) cached in %lu ms", n, slot,
             (unsigned long) (to_ms_since_boot(get_absolute_time()) - t0));
}

static const diskchain_t* chain_cache_find(unsigned int n, uint8_t track, uint8_t sector) {
    for (unsigned int i = 0; i < CHAIN_CACHE_SLOTS; i++) {
        chain_cache_entry_t* c = &chain_cache[n][i];
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

// Mounts library entry 'idx' into drive 'n'. Returns true on success.
static bool mount_image(unsigned int n, int idx) {
    if (idx < 0 || (unsigned int) idx >= library_count) return false;

    char path[48];
    snprintf(path, sizeof(path), "/disks/%s", library[idx]);
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

    diskimage_t img;
    if (!diskimage_open(&img, file_read, f, (uint32_t) size)) {
        log_info("ieee: %s has unsupported size %ld, ignored", path, size);
        fclose(f);
        return false;
    }

    if (drives[n].file != NULL) fclose(drives[n].file);
    drives[n].file = f;
    drives[n].image = img;
    drives[n].image.ctx = f;
    drives[n].image.write = writable ? file_write : NULL;
    drives[n].present = true;
    drives[n].library_index = idx;
    snprintf(drives[n].name, sizeof(drives[n].name), "%s", library[idx]);
    mount_generation++;
    log_info("ieee: drive %u = %s (%ld bytes)", n, path, size);
    chain_cache_build(n);
    return true;
}

static int library_find(const char* name) {
    for (unsigned int i = 0; i < library_count; i++) {
        if (strcasecmp(library[i], name) == 0) return (int) i;
    }
    return -1;
}

static void mount_drive(unsigned int n) {
    // Prefer the conventional name (drive0.d80 etc.), else the nth image.
    // Unit-9 slots (2/3) mount by conventional name only; the nth-image
    // fallback would populate a second unit unasked.
    char preferred[16];
    drives[n].library_index = -1;

    for (unsigned int ext = 0; ext < 2; ext++) {
        snprintf(preferred, sizeof(preferred), "drive%u.%s", n, ext ? "d64" : "d80");
        int idx = library_find(preferred);
        if (idx >= 0 && mount_image(n, idx)) return;
    }
    if (n < 2 && n < library_count) mount_image(n, (int) n);
}

// ----------------------------------------------------------------------------
// DOS state
// ----------------------------------------------------------------------------

typedef enum {
    st_code_ok = 0,
    st_code_read_error = 23,
    st_code_write_protect = 26,
    st_code_record_missing = 50,
    st_code_record_overflow = 51,
    st_code_file_not_found = 62,
    st_code_power_on = 73,
} status_code_t;

// Per-unit DOS status: each IEEE unit has its own channel-15 command channel,
// so status is tracked and served per unit.
static status_code_t status_code[NUM_UNITS] = {st_code_power_on, st_code_power_on};

static bool mcu_listening = false;  // mirrors the FPGA's addressed state
static bool mcu_talking = false;

// Which unit (0 = device 8, 1 = device 9) the current LISTEN/TALK addressed.
// The secondary address that follows binds channels/status to that unit.
static uint8_t listen_unit = 0;
static uint8_t talk_unit = 0;
static uint8_t ch15_unit = 0;    // unit whose ch15 command is being collected
static uint8_t open_unit = 0;    // unit of the OPEN name being collected
static uint8_t file_unit = 0;    // unit of the open sequential file

// Debug counters for the HDMI overlay.
static uint32_t dbg_cmds = 0;
static uint32_t dbg_data = 0;
static uint8_t dbg_last_cmd = 0;
static char dbg_open_result = '-';   // '-' none, 'Y' found, 'N' not found

// Per-slot activity for the HUD's read/write markers, attributed to the slot
// of the exact channel each byte flows on (sequential via 'stream_drive', REL
// via the channel's 'drive' field). Counts file data only.
static uint32_t hud_read[NUM_DRIVES];
static uint32_t hud_write[NUM_DRIVES];
static uint8_t  stream_drive = 0;   // slot of the current sequential read stream

static bool collecting_name = false;
static char open_name[48];
static unsigned int open_name_len = 0;
static uint8_t open_chan = 0;

// One data channel at a time (all the Waterloo loader needs).
static bool file_open_ok = false;
static uint8_t file_chan = 0;
static diskstream_t stream;
static bool streaming = false;      // TX top-up in progress
static bool stream_finished = false; // final byte (EOI) already queued
static uint32_t streamed_bytes = 0;

// ---------------------------------------------------------------------------
// CBM RELative-file channels (Super-OS/9 keeps its filesystems inside REL
// files, reclen 129, and reads them with ch15 'P' position commands).
// ---------------------------------------------------------------------------
#define MAX_REL_CHANNELS 4

typedef struct {
    bool in_use;
    uint8_t chan;           // secondary address 0-14 (unique per unit, not globally)
    uint8_t drive;          // mount slot 0-3 this channel is on (unit = drive >> 1)
    uint8_t reclen;
    // Absolute byte offsets into the record stream (vdrive-rel.c semantics).
    // length = offset of the last non-zero byte; CBM DOS trims trailing
    // nulls and Super-OS/9 depends on that.
    uint32_t cur_record;    // 1-based, per CBM convention
    uint32_t bufptr;
    int32_t  length;
    bool     missing;       // positioned past the last record
    // Record-write staging (LISTEN data, committed at UNLISTEN):
    bool     wr_active;
    uint16_t wr_pos;        // start position within the record (from P)
    uint16_t wr_count;
    uint8_t  wr_buf[254];
    diskchain_t chain;
} rel_channel_t;

static rel_channel_t rel_chans[MAX_REL_CHANNELS];

// Channels are keyed by (unit, chan): the d8d9 descriptor table reuses
// secondary addresses 2/3 on BOTH units, so the channel number alone is
// ambiguous.
static rel_channel_t* rel_find(uint8_t unit, uint8_t chan) {
    for (int i = 0; i < MAX_REL_CHANNELS; i++)
        if (rel_chans[i].in_use && rel_chans[i].chan == chan
            && (rel_chans[i].drive >> 1) == unit) return &rel_chans[i];
    return NULL;
}

// Channel-15 DOS command collection (LISTEN + secondary $6F, data, UNLISTEN)
static uint8_t ch15_cmd[40];
static unsigned int ch15_cmd_len = 0;
static bool collecting_ch15 = false;
static uint8_t listen_chan = 0xFF;  // active LISTEN data channel, $FF = none

// Position the engine at record 'rec', byte 'pos' within it, and compute
// the trimmed length (last non-zero byte), like vdrive_rel_position.
static void rel_position(rel_channel_t* rc, uint32_t rec, uint8_t pos) {
    uint8_t buf[254];
    rc->cur_record = rec;
    rc->missing = (rec == 0)
        || rec > diskchain_size(&rc->chain) / rc->reclen;
    if (rc->missing) return;

    uint32_t base = (rec - 1) * (uint32_t) rc->reclen;
    rc->bufptr = base + pos;
    rc->length = (int32_t) (base + rc->reclen - 1);
    if (!diskchain_read(&rc->chain, base, buf, rc->reclen)) {
        rc->missing = true;
        return;
    }
    while (rc->length >= (int32_t) rc->bufptr
           && buf[rc->length - base] == 0) {
        rc->length--;
    }
}

// Serve the current (trimmed) record into the fabric TX FIFO, EOI on its
// last byte. Empty records are transparent: reads flow into the next
// record, exactly like vdrive_rel_read.
static void rel_serve(rel_channel_t* rc) {
    uint8_t buf[254];

    spi_write_at(IEEE_REG_CTRL, IEEE_CTRL_ENABLE | IEEE_CTRL_DATA_FLUSH);

    while (!rc->missing && (int32_t) rc->bufptr > rc->length) {
        rel_position(rc, rc->cur_record + 1, 0);
    }
    if (rc->missing) {
        status_code[rc->drive >> 1] = st_code_record_missing;
        spi_write_at(IEEE_REG_TX_LAST, 0x0D);
        return;
    }

    uint16_t n = (uint16_t) (rc->length - rc->bufptr + 1);
    if (!diskchain_read(&rc->chain, rc->bufptr, buf, n)) {
        status_code[rc->drive >> 1] = st_code_read_error;
        spi_write_at(IEEE_REG_TX_LAST, 0x0D);
        return;
    }
    // Burst, not per-byte: per-byte SPI can't outrun the CPU's drain. The
    // FIFO was just flushed and a record is <= 254 bytes, so it can't
    // overflow.
    if (n > 1) spi_write_same_block(IEEE_REG_TX, buf, n - 1u);
    spi_write_at(IEEE_REG_TX_LAST, buf[n - 1]);
    rc->bufptr += n;
    streamed_bytes += n;
    hud_read[rc->drive & 3] += n;   // REL read, attributed to this channel's slot
}

// Begin collecting a record write on this channel (kernel did LISTEN +
// secondary): bytes land at the position set by the last P command.
static void rel_write_begin(rel_channel_t* rc) {
    uint32_t base = (rc->cur_record - 1) * (uint32_t) rc->reclen;
    rc->wr_active = true;
    rc->wr_count = 0;
    if (rc->missing || rc->bufptr < base || rc->bufptr >= base + rc->reclen) {
        rc->wr_pos = 0;         // fresh record start
    } else {
        rc->wr_pos = (uint16_t) (rc->bufptr - base);
    }
}

static void rel_write_byte(rel_channel_t* rc, uint8_t byte) {
    if (rc->wr_pos + rc->wr_count >= rc->reclen) {
        status_code[rc->drive >> 1] = st_code_record_overflow;   // 51: drop the excess
        return;
    }
    rc->wr_buf[rc->wr_count++] = byte;
}

// Commit at UNLISTEN, CBM-style: bytes land at the position, the record
// tail is zero-filled (what the read side's null-trim recovers), and the
// engine advances.
static void rel_write_commit(rel_channel_t* rc) {
    uint8_t rec[254];
    rc->wr_active = false;
    if (rc->missing) {
        status_code[rc->drive >> 1] = st_code_record_missing;
        return;
    }
    uint32_t base = (rc->cur_record - 1) * (uint32_t) rc->reclen;
    if (!diskchain_read(&rc->chain, base, rec, rc->reclen)) {
        status_code[rc->drive >> 1] = st_code_read_error;
        return;
    }
    memcpy(rec + rc->wr_pos, rc->wr_buf, rc->wr_count);
    memset(rec + rc->wr_pos + rc->wr_count, 0,
           rc->reclen - rc->wr_pos - rc->wr_count);
    if (!diskchain_write(&rc->chain, base, rec, rc->reclen)) {
        status_code[rc->drive >> 1] = st_code_write_protect;     // 26: read-only image
        log_info("ieee: REL write failed (read-only?) rec %lu",
                 (unsigned long) rc->cur_record);
        return;
    }
    rel_position(rc, rc->cur_record + 1, 0);     // advance like a real drive
}

// Execute a completed channel-15 command. Only 'P' (position) acts;
// 'I'/'V' succeed silently like a real drive.
static void ch15_execute(void) {
    if (ch15_cmd_len == 0) return;

    if (ch15_cmd[0] == 'P' && ch15_cmd_len >= 4) {
        // P + channel byte ($60 | chan) + record lo + record hi [+ position]
        rel_channel_t* rc = rel_find(ch15_unit, ch15_cmd[1] & 0x0F);
        uint32_t rec = ch15_cmd[2] | ((uint32_t) ch15_cmd[3] << 8);
        uint8_t pos = (ch15_cmd_len >= 5 && ch15_cmd[4] >= 1) ? (uint8_t) (ch15_cmd[4] - 1) : 0;
        if (rc == NULL) {
            status_code[ch15_unit] = st_code_file_not_found;
        } else {
            rel_position(rc, rec, pos);
            status_code[ch15_unit] = rc->missing ? st_code_record_missing : st_code_ok;
        }
    } else if (ch15_cmd[0] == 'I' || ch15_cmd[0] == 'V') {
        status_code[ch15_unit] = st_code_ok;
    } else {
        log_info("ieee: ch15 command %02x len %u (ignored)", ch15_cmd[0], ch15_cmd_len);
    }
    ch15_cmd_len = 0;
}

static void resolve_open(void) {
    collecting_name = false;
    open_name[open_name_len] = '\0';

    // Split off the ",PRG"/",SEQ" type suffix.
    char* comma = strchr(open_name, ',');
    if (comma != NULL) *comma = '\0';

    // Channel 15 is the command channel: its "filename" is a DOS command
    // string, never a file open. Don't disturb the active data stream.
    if ((open_chan & 0x0F) == 0x0F) {
        log_info("ieee: DOS command '%s' (ignored)", open_name);
        return;
    }

    // Slot = the addressed unit's pair, plus the drive number from the
    // "0:"/"1:" (or "0."/"1.") prefix; default drive 0 of that unit.
    unsigned int n = open_unit * 2u;
    if ((open_name[0] == '0' || open_name[0] == '1')
        && (open_name[1] == ':' || open_name[1] == '.')) {
        n = open_unit * 2u + (unsigned int) (open_name[0] - '0');
    }
    // Fall back to the unit's other drive only for ordinary files: OS-9's
    // per-drive REL containers are drive-specific, and aliasing them can
    // corrupt the mounted system disk.
    bool is_os9_vol = (strstr(open_name, "OS9 DRIVE") != NULL)
                   || (strstr(open_name, "os9 drive") != NULL);
    if (!drives[n].present && drives[n ^ 1u].present && !is_os9_vol) {
        log_info("ieee: drive %u empty, using drive %u", n, n ^ 1u);
        n ^= 1u;
    }

    file_open_ok = false;
    streaming = false;
    stream_finished = false;
    // New file: discard any stale queued data from a previous channel.
    spi_write_at(IEEE_REG_CTRL, IEEE_CTRL_ENABLE | IEEE_CTRL_DATA_FLUSH);

    if (!drives[n].present) {
        status_code[open_unit] = st_code_file_not_found;
        log_info("ieee: OPEN '%s': no disk image mounted", open_name);
        return;
    }

    diskimage_entry_t entry;
    if (!diskimage_find(&drives[n].image, open_name, &entry)) {
        status_code[open_unit] = st_code_file_not_found;
        dbg_open_result = 'N';
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
            ? chain_cache_find(n, entry.start_track, entry.start_sector)
            : NULL;
        if (cached != NULL) {
            rc->chain = *cached;
        }
        if (rc == NULL
            || (cached == NULL
                && !diskchain_build(&rc->chain, &drives[n].image,
                                    entry.start_track, entry.start_sector))) {
            status_code[open_unit] = st_code_file_not_found;
            dbg_open_result = 'N';
            log_info("ieee: REL OPEN '%s' failed", open_name);
            return;
        }
        if (entry.record_len > 254) {   // CBM max; larger = corrupt image, and
            status_code[open_unit] = st_code_file_not_found;   // would overrun
            log_info("ieee: REL OPEN '%s' bad reclen %u", open_name, entry.record_len);
            return;                     // the 254-byte record buffers
        }
        rc->in_use = true;
        rc->drive = (uint8_t) n;
        rc->chan = open_chan & 0x0F;
        rc->reclen = entry.record_len ? entry.record_len : 129;
        rel_position(rc, 1, 0);
        status_code[open_unit] = st_code_ok;
        dbg_open_result = 'R';
        log_info("ieee: REL OPEN '%s' ok (chan %u, reclen %u, %lu bytes)",
                 open_name, rc->chan, rc->reclen,
                 (unsigned long) diskchain_size(&rc->chain));
        return;
    }

    if (!diskstream_open(&stream, &drives[n].image, entry.start_track, entry.start_sector)) {
        status_code[open_unit] = st_code_file_not_found;
        dbg_open_result = 'N';
        log_info("ieee: OPEN '%s': stream open failed", open_name);
        return;
    }

    file_open_ok = true;
    stream_drive = (uint8_t) n;
    stream_finished = false;
    file_chan = open_chan;
    file_unit = open_unit;
    status_code[open_unit] = st_code_ok;
    streamed_bytes = 0;
    dbg_open_result = 'Y';
    log_info("ieee: OPEN '%s' ok (chan %u)", open_name, file_chan);
}

static void push_status(unsigned int unit) {
    const char* text;
    char line[40];

    switch (status_code[unit]) {
        case st_code_ok:             text = " OK"; break;
        case st_code_read_error:     text = "READ ERROR"; break;
        case st_code_write_protect:  text = "WRITE PROTECT ON"; break;
        case st_code_record_missing: text = "RECORD NOT PRESENT"; break;
        case st_code_record_overflow: text = "OVERFLOW IN RECORD"; break;
        case st_code_file_not_found: text = "FILE NOT FOUND"; break;
        case st_code_power_on:       text = "ECONOPET IEEE"; break;
        default:                     text = ""; break;
    }
    int n = snprintf(line, sizeof(line), "%02u,%s,00,00", (unsigned int) status_code[unit], text);

    for (int i = 0; i < n; i++) spi_write_at(IEEE_REG_TXS, (uint8_t) line[i]);
    spi_write_at(IEEE_REG_TXS_LAST, 0x0D);

    // Reading the status channel resets it, like a real drive.
    status_code[unit] = st_code_ok;
}

// Fill the TX FIFO from the open stream until the fabric reports no room
// for another chunk.
static uint32_t starved_events = 0;


static void service_tx(void) {
    // An underrun mid-record times out the kernel's counted read.
    uint8_t buf[TX_BURST_CHUNK];

    for (;;) {
        uint8_t st = spi_read_at(IEEE_REG_STATUS);
        if (st & IEEE_ST_TALK_STARVED) starved_events++;

        uint8_t ctrl = spi_read_at(IEEE_REG_CTRL);
        if (!(ctrl & IEEE_CTRL_RD_TX_ROOM)) return;

        size_t n = 0;
        bool last = false;
        uint8_t last_byte = 0;

        while (n < TX_BURST_CHUNK) {
            uint8_t byte;
            bool is_last;
            if (!diskstream_next(&stream, &byte, &is_last)) {
                // Natural EOF exits via 'is_last'; reaching here mid-file means
                // an SD read error. Flush what we gathered, then close with an
                // EOI'd filler so the kernel sees a clean (if short) end.
                if (n > 0) spi_write_same_block(IEEE_REG_TX, buf, n);
                streamed_bytes += n;
                hud_read[stream_drive & 3] += n;
                log_info("ieee: read error after %lu bytes", (unsigned long) streamed_bytes);
                status_code[stream_drive >> 1] = st_code_read_error;
                spi_write_at(IEEE_REG_TX_LAST, 0x0D);
                streaming = false;
                return;
            }
            if (is_last) { last = true; last_byte = byte; break; }
            buf[n++] = byte;
        }

        if (n > 0) spi_write_same_block(IEEE_REG_TX, buf, n);
        streamed_bytes += n;
        hud_read[stream_drive & 3] += n;

        if (last) {
            // Final byte carries EOI: separate address, so not part of the burst.
            spi_write_at(IEEE_REG_TX_LAST, last_byte);
            streamed_bytes++;
            hud_read[stream_drive & 3]++;
            log_info("ieee: stream complete, %lu bytes", (unsigned long) streamed_bytes);
            streaming = false;
            stream_finished = true;
            return;
        }
    }
}

static void handle_command(uint8_t cmd) {
    dbg_cmds++;
    dbg_last_cmd = cmd;
    switch (cmd & 0xE0) {
        case 0x20:  // LISTEN / UNLISTEN
            if (cmd == 0x3F) {
                if (collecting_name) resolve_open();
                if (collecting_ch15) { collecting_ch15 = false; ch15_execute(); }
                for (int i = 0; i < MAX_REL_CHANNELS; i++)
                    if (rel_chans[i].in_use && rel_chans[i].wr_active)
                        rel_write_commit(&rel_chans[i]);
                mcu_listening = false;
                listen_chan = 0xFF;
            } else {
                uint8_t a = cmd & 0x1F;
                mcu_listening = (a == DEV_ADDR || a == DEV_ADDR + 1);
                if (mcu_listening) listen_unit = (uint8_t) (a - DEV_ADDR);
            }
            break;

        case 0x40:  // TALK / UNTALK
            if (cmd == 0x5F) {
                mcu_talking = false;
                // Channel data persists across UNTALK (continuation) -- just
                // pause the top-up until the next TALK on the data channel.
                streaming = false;
            } else {
                uint8_t a = cmd & 0x1F;
                mcu_talking = (a == DEV_ADDR || a == DEV_ADDR + 1);
                if (mcu_talking) talk_unit = (uint8_t) (a - DEV_ADDR);
            }
            break;

        case 0x60:  // secondary address (data channel)
            if (mcu_listening) {
                rel_channel_t* wrc;
                listen_chan = cmd & 0x0F;
                if (listen_chan == 15) {
                    collecting_ch15 = true;
                    ch15_unit = listen_unit;
                    ch15_cmd_len = 0;
                } else if ((wrc = rel_find(listen_unit, listen_chan)) != NULL) {
                    rel_write_begin(wrc);
                }
            }
            if (mcu_talking) {
                uint8_t chan = cmd & 0x0F;
                rel_channel_t* rc;
                if (chan == 15) {
                    push_status(talk_unit);
                } else if ((rc = rel_find(talk_unit, chan)) != NULL) {
                    rel_serve(rc);
                } else if (file_open_ok && talk_unit == file_unit && chan == file_chan) {
                    if (stream_finished) {
                        // Read past EOF: real CBM DOS answers a lone EOI'd
                        // CR with clean status, not a read error.
                        spi_write_at(IEEE_REG_TX_LAST, 0x0D);
                    } else {
                        streaming = true;
                    }
                }
            }
            break;

        case 0xE0:  // CLOSE ($Ex) / OPEN ($Fx)
            if ((cmd & 0xF0) == 0xF0) {
                if (mcu_listening) {
                    open_chan = cmd & 0x0F;
                    open_unit = listen_unit;
                    collecting_name = true;
                    open_name_len = 0;
                }
            } else {
                if (mcu_listening) {
                    rel_channel_t* rc = rel_find(listen_unit, cmd & 0x0F);
                    if (rc != NULL) {
                        rc->in_use = false;
                        spi_write_at(IEEE_REG_CTRL, IEEE_CTRL_ENABLE | IEEE_CTRL_DATA_FLUSH);
                    } else if (listen_unit == file_unit && (cmd & 0x0F) == file_chan) {
                        file_open_ok = false;
                        streaming = false;
                        stream_finished = false;
                        spi_write_at(IEEE_REG_CTRL, IEEE_CTRL_ENABLE | IEEE_CTRL_DATA_FLUSH);
                    }
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

void ieee_drive_init(void) {
    scan_library();
    for (unsigned int n = 0; n < NUM_DRIVES; n++) mount_drive(n);

    // Virtual drive is OFF by default: the machine behaves like a stock PET
    // with real hardware drives on the IEEE-488 bus (the fabric stays
    // transparent). A config that opts in with 'ieee-drive: on' turns it on
    // via ieee_drive_set_enabled() during menu apply.
    emulation_enabled = false;
    spi_write_at(IEEE_REG_CTRL, 0);   // ensure fabric transparent at boot
}

// Enable/disable the virtual drive (only if an image is mounted).
void ieee_drive_set_enabled(bool en) {
    emulation_enabled = en && (drives[0].present || drives[1].present
                               || drives[2].present || drives[3].present);
    if (emulation_enabled) {
        spi_write_at(IEEE_REG_CTRL, IEEE_CTRL_ENABLE | IEEE_CTRL_FLUSH);
        log_info("ieee: virtual drive enabled (device %u)", DEV_ADDR);
    } else {
        spi_write_at(IEEE_REG_CTRL, 0);   // transparent -> real hardware bus
        log_info("ieee: virtual drive off (real hardware bus)");
    }
}

// Lazy mount: a cycle keypress only advances the selection; the mount
// commits in lazy_mount_task once the taps stop, so cycling stays instant.
#define LAZY_MOUNT_MS 1500
static int      pending_idx[NUM_DRIVES] = {-1, -1, -1, -1};
static uint32_t pending_ms[NUM_DRIVES];

bool ieee_drive_cycle(unsigned int n) {
    // Advance slot n's selection; the mount itself is deferred (see
    // lazy_mount_task).
    char msg[48];

    if (n >= NUM_DRIVES) return false;
    if (library_count == 0) {
        show_toast("NO IMAGES IN /DISKS");
        return false;
    }
    if (streaming || file_open_ok) {
        show_toast("DRIVE BUSY");
        return false;
    }

    int cur = (pending_idx[n] >= 0) ? pending_idx[n] : drives[n].library_index;
    int next = (cur + 1) % (int) library_count;
    pending_idx[n] = next;
    pending_ms[n] = to_ms_since_boot(get_absolute_time());
    snprintf(msg, sizeof(msg), "D%u: %s", n, library[next]);
    show_toast(msg);
    return true;
}

// Commit a settled selection; at most one mount per pass.
static void lazy_mount_task(void) {
    const uint32_t now = to_ms_since_boot(get_absolute_time());
    char msg[48];

    for (unsigned int n = 0; n < NUM_DRIVES; n++) {
        if (pending_idx[n] < 0) continue;
        if ((now - pending_ms[n]) < LAZY_MOUNT_MS) continue;

        int want = pending_idx[n];
        pending_idx[n] = -1;

        // A transfer started during the debounce: swapping the disk under it
        // now would be a surprise -- drop the selection instead.
        if (streaming || file_open_ok) {
            show_toast("DRIVE BUSY");
            continue;
        }
        // Cycled full circle back to the image already mounted: nothing to do.
        if (drives[n].present && want == drives[n].library_index) continue;

        // Try each candidate once from the selection onward; skip images that
        // fail to mount so one bad file can't wedge the rotation.
        for (unsigned int tries = 0; tries < library_count; tries++) {
            int idx = (want + (int) tries) % (int) library_count;
            if (mount_image(n, idx)) {
                file_open_ok = false;   // any open file was on the old disk
                stream_finished = false;
                snprintf(msg, sizeof(msg), "D%u: %s", n, drives[n].name);
                show_toast(msg);
                return;                 // one mount per pass
            }
        }
        show_toast("NO MOUNTABLE IMAGE");
    }
}

bool ieee_drive_hotkey(unsigned char hid_keycode) {
    // F1-F4 (0x3A-0x3D) cycle the four slots, F8 (0x41) toggles the status
    // line, F11 (0x44) lists mounts. The PET has no function keys, so these
    // shadow nothing in the keymap.
    char msg[48];

    switch (hid_keycode) {
        case 0x41:   // F8: toggle the bottom-right debug status line
            overlay_debug_on = !overlay_debug_on;
            show_toast(overlay_debug_on ? "DEBUG OVERLAY ON" : "DEBUG OVERLAY OFF");
            return true;
        case 0x3A: ieee_drive_cycle(0); return true;   // F1
        case 0x3B: ieee_drive_cycle(1); return true;   // F2
        case 0x3C: ieee_drive_cycle(2); return true;   // F3
        case 0x3D: ieee_drive_cycle(3); return true;   // F4
        case 0x44: {
            // Unit-9 slots included only when mounted (toast stays short for
            // the common two-drive setup; long name sets truncate).
            unsigned int pos = 0;
            for (unsigned int i = 0; i < NUM_DRIVES && pos < sizeof(msg); i++) {
                if (i >= 2 && !drives[i].present) continue;
                pos += (unsigned int) snprintf(msg + pos, sizeof(msg) - pos,
                                               "%sD%u:%s", i ? " " : "", i,
                                               drives[i].present ? drives[i].name : "-");
            }
            show_toast(msg);
            return true;
        }
        default:
            return false;
    }
}

void ieee_drive_overlay_text(char* out, unsigned int len) {
    if (toast_until_ms != 0
        && to_ms_since_boot(get_absolute_time()) < toast_until_ms) {
        snprintf(out, len, "%s", toast);   // toasts always show (transient)
        return;
    }
    if (!overlay_debug_on) { out[0] = '\0'; return; }   // F8 hid the status line
    ieee_drive_debug_status(out, len);
}

void ieee_drive_hud_status(char* out, unsigned int len) {
    // Disk-status line for the on-CRT HUD: mounted images plus a per-drive
    // read/write marker. Unit-9 slots (D2/D3) appear only while mounted.
    static uint32_t prev_read[NUM_DRIVES], prev_write[NUM_DRIVES];
    static uint32_t act_ms[NUM_DRIVES];              // last-activity time per slot
    static char     act_type[NUM_DRIVES] = {' ', ' ', ' ', ' '};

    const uint32_t now = to_ms_since_boot(get_absolute_time());

    for (int i = 0; i < NUM_DRIVES; i++) {
        if (hud_read[i] != prev_read[i]) {
            prev_read[i] = hud_read[i];
            act_ms[i] = now;
            act_type[i] = 'R';
        }
        if (hud_write[i] != prev_write[i]) {
            prev_write[i] = hud_write[i];
            act_ms[i] = now;
            act_type[i] = 'W';
        }
    }
    // Keep the read marker lit through a continuous sequential stream even if
    // the byte delta between calls is missed ('streaming' is only true then).
    if (streaming) {
        act_ms[stream_drive & 3] = now;
        act_type[stream_drive & 3] = 'R';
    }

    const bool blink = ((now / 250) & 1);   // ~2 Hz
    unsigned int pos = 0;
    for (int i = 0; i < NUM_DRIVES && pos < len; i++) {
        char m;
        if (i >= 2 && !drives[i].present && pending_idx[i] < 0) continue;
        m = ((now - act_ms[i]) < 400 && blink) ? act_type[i] : ' ';
        // A pending (not yet committed) selection shows immediately with a
        // '*' so cycling gives instant feedback; the lazy mount and its REL
        // preload land a moment later.
        if (pending_idx[i] >= 0) {
            pos += (unsigned int) snprintf(out + pos, len - pos, "%sD%d:%s*",
                                           i ? " " : "", i, library[pending_idx[i]]);
        } else {
            pos += (unsigned int) snprintf(out + pos, len - pos, "%sD%d:%s%c",
                                           i ? " " : "", i,
                                           drives[i].present ? drives[i].name : "-", m);
        }
    }
}

void ieee_drive_debug_status(char* out, unsigned int len) {
    if (!emulation_enabled) {
        snprintf(out, len, "IEEE OFF D0:%c D1:%c",
                 drives[0].present ? 'Y' : 'N', drives[1].present ? 'Y' : 'N');
        return;
    }

    uint8_t st = spi_read_at(IEEE_REG_STATUS);

    // ST=FPGA status  K=last cmd  C/D=cmd/data counts
    // S=streamed bytes  V=starved events  O=open result + OPEN name
    snprintf(out, len, "ST%02X K%02X C%lu D%lu S%lu V%lu O%c %s",
             st, dbg_last_cmd,
             (unsigned long) dbg_cmds, (unsigned long) dbg_data,
             (unsigned long) streamed_bytes, (unsigned long) starved_events,
             dbg_open_result, open_name);
}

void ieee_drive_task(void) {
    lazy_mount_task();   // deferred disk swaps run even with emulation off

    if (!emulation_enabled) return;

    // Drain the RX FIFO (commands and listener data).
    for (unsigned int i = 0; i < 64; i++) {
        uint8_t st = spi_read_at(IEEE_REG_STATUS);
        if (!(st & IEEE_ST_RX_AVAIL)) break;

        bool is_atn = (st & IEEE_ST_RX_ATN) != 0;
        uint8_t byte = spi_read_at(IEEE_REG_RX);
        spi_write_at(IEEE_REG_RX, 0);   // explicit pop (reads are side-effect-free)

        if (is_atn) {
            handle_command(byte);
        } else {
            dbg_data++;
            if (collecting_name && open_name_len < sizeof(open_name) - 1) {
                open_name[open_name_len++] = (char) byte;
            } else if (collecting_ch15 && ch15_cmd_len < sizeof(ch15_cmd)) {
                ch15_cmd[ch15_cmd_len++] = byte;
            } else if (listen_chan != 0xFF) {
                rel_channel_t* wrc = rel_find(listen_unit, listen_chan);
                if (wrc != NULL && wrc->wr_active) {
                    rel_write_byte(wrc, byte);
                    hud_write[wrc->drive & 3]++;   // REL write on this channel's slot
                }
            }
        }
    }

    if (streaming) service_tx();
}
