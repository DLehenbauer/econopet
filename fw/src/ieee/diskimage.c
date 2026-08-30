// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "diskimage.h"

#include <ctype.h>
#include <string.h>

// Sectors per track by zone. Track numbers are 1-based.
static unsigned int d64_sectors(unsigned int track) {
    if (track <= 17) return 21;
    if (track <= 24) return 19;
    if (track <= 30) return 18;
    return 17;
}

static unsigned int d80_sectors(unsigned int track) {
    if (track <= 39) return 29;
    if (track <= 53) return 27;
    if (track <= 64) return 25;
    return 23;
}

static uint32_t track_offset(const diskimage_t* img, unsigned int track) {
    uint32_t sectors = 0;
    for (unsigned int t = 1; t < track; t++) {
        sectors += (img->type == diskimage_type_d80) ? d80_sectors(t) : d64_sectors(t);
    }
    return sectors * 256u;
}

static bool read_sector(const diskimage_t* img, uint8_t track, uint8_t sector, uint8_t buf[256]) {
    if (track == 0) return false;
    unsigned int max_track = (img->type == diskimage_type_d80) ? 77 : 35;
    if (track > max_track) return false;
    unsigned int spt = (img->type == diskimage_type_d80) ? d80_sectors(track) : d64_sectors(track);
    if (sector >= spt) return false;

    uint32_t offset = track_offset(img, track) + (uint32_t) sector * 256u;
    return img->read(img->ctx, offset, buf, 256);
}

bool diskimage_open(diskimage_t* img, diskimage_read_fn read, void* ctx, uint32_t size) {
    img->read = read;
    img->write = NULL;
    img->ctx = ctx;
    img->size = size;

    switch (size) {
        case DISKIMAGE_D64_SIZE: img->type = diskimage_type_d64; return true;
        case DISKIMAGE_D80_SIZE: img->type = diskimage_type_d80; return true;
        default: img->type = diskimage_type_none; return false;
    }
}

bool diskimage_open_hdd(diskimage_t* img, diskimage_read_fn read, void* ctx, uint32_t size) {
    img->read = read;
    img->write = NULL;
    img->ctx = ctx;
    img->size = size;

    // One 258-byte record pair per 256-byte RBF sector; at least one sector.
    if (size == 0 || size % 258u != 0) {
        img->type = diskimage_type_none;
        return false;
    }
    img->type = diskimage_type_hdd;
    return true;
}

static void dir_start(const diskimage_t* img, uint8_t* track, uint8_t* sector) {
    if (img->type == diskimage_type_d80) {
        *track = 39;
        *sector = 1;
    } else {
        *track = 18;
        *sector = 1;
    }
}

bool diskimage_entry(const diskimage_t* img, unsigned int index, diskimage_entry_t* out) {
    // One synthetic entry: the REL file the Super-OS/9 d8d9 descriptors
    // OPEN ("0:OS9 DRIVE A,L").
    if (img->type == diskimage_type_hdd) {
        if (index != 0) return false;
        strcpy(out->name, "OS9 DRIVE A");
        out->file_type = DISKIMAGE_FTYPE_REL;
        out->start_track = 0;
        out->start_sector = 0;
        out->record_len = 129;
        return true;
    }

    uint8_t track, sector;
    dir_start(img, &track, &sector);

    unsigned int seen = 0;
    unsigned int guard = 0;             // malformed-chain protection

    while (track != 0 && guard++ < 256) {
        uint8_t buf[256];
        if (!read_sector(img, track, sector, buf)) return false;

        for (unsigned int i = 0; i < 8; i++) {
            const uint8_t* e = &buf[i * 32];
            uint8_t ftype = e[2];
            if ((ftype & 0x80) == 0 || (ftype & 0x07) == DISKIMAGE_FTYPE_DEL) continue;

            if (seen++ == index) {
                unsigned int n = 0;
                for (unsigned int j = 0; j < 16; j++) {
                    uint8_t c = e[5 + j];
                    if (c == 0xA0 || c == 0x00) break;
                    out->name[n++] = (char) c;
                }
                out->name[n] = '\0';
                out->file_type = ftype & 0x07;
                out->start_track = e[3];
                out->start_sector = e[4];
                out->record_len = e[23];
                return true;
            }
        }

        track = buf[0];
        sector = buf[1];
    }
    return false;
}

// Strips an optional drive prefix ("0:", "1:", "0.", "1.") and returns the
// bare name.
static const char* strip_drive_prefix(const char* name) {
    if ((name[0] == '0' || name[0] == '1') && (name[1] == ':' || name[1] == '.')) {
        return name + 2;
    }
    return name;
}

static bool name_matches(const char* want, const char* have) {
    while (*want && *want != '*') {
        if (toupper((unsigned char) *want) != toupper((unsigned char) *have)) return false;
        want++;
        have++;
    }
    if (*want == '*') return true;
    return *have == '\0';
}

bool diskimage_find(const diskimage_t* img, const char* name, diskimage_entry_t* out) {
    name = strip_drive_prefix(name);

    // Scan into a local: leaving a half-matched entry in *out on a miss would
    // hand the caller some other file's start track/sector.
    diskimage_entry_t candidate;
    for (unsigned int i = 0;; i++) {
        if (!diskimage_entry(img, i, &candidate)) return false;
        if (name_matches(name, candidate.name)) {
            *out = candidate;
            return true;
        }
    }
}

static bool load_sector(diskstream_t* st, uint8_t track, uint8_t sector) {
    // A chain cannot have more links than the image has sectors; a longer walk
    // means a circular T/S chain in a corrupt image and must not stream forever.
    if (st->sectors >= st->img->size / 256) return false;
    st->sectors++;
    if (!read_sector(st->img, track, sector, st->buf)) return false;

    st->pos = 2;
    if (st->buf[0] == 0) {
        // Final sector: byte 1 is the offset of the last valid byte.
        st->last_sector = true;
        st->end = (uint16_t) (st->buf[1] + 1);
        if (st->end < 2) st->end = 2;   // empty/degenerate
    } else {
        st->last_sector = false;
        st->end = 256;
    }
    return true;
}

bool diskstream_open(diskstream_t* st, const diskimage_t* img, uint8_t track, uint8_t sector) {
    st->img = img;
    st->eof = false;
    st->sectors = 0;
    return load_sector(st, track, sector);
}

bool diskstream_next(diskstream_t* st, uint8_t* out, bool* last) {
    if (st->eof) return false;

    if (st->pos >= st->end) {
        if (st->last_sector) {
            st->eof = true;
            return false;
        }
        if (!load_sector(st, st->buf[0], st->buf[1])) {
            st->eof = true;
            return false;
        }
        if (st->pos >= st->end) {
            st->eof = true;
            return false;
        }
    }

    *out = st->buf[st->pos++];
    *last = st->last_sector && st->pos >= st->end;
    return true;
}

// ---------------------------------------------------------------------------
// Random-access chain (REL files)
// ---------------------------------------------------------------------------

bool diskchain_build(diskchain_t* ch, const diskimage_t* img, uint8_t track, uint8_t sector) {
    // No chain: the record stream is the file, so offsets are
    // identity-mapped.
    if (img->type == diskimage_type_hdd) {
        ch->img = img;
        ch->flat = true;
        ch->flat_size = img->size;
        ch->count = 0;
        ch->last_used = 0;
        return true;
    }
    ch->flat = false;

    // Read whole tracks, not per-link 2-byte headers: links mostly hop
    // within a track, and each small SD read is a full transaction --
    // walking a large REL file per-link takes seconds.
    static uint8_t trkbuf[29 * 256];    // largest track (8050 tracks 1-39)
    unsigned int buf_track = 0;         // 0 = nothing buffered

    ch->img = img;
    ch->count = 0;
    ch->last_used = 0;

    unsigned int max_track = (img->type == diskimage_type_d80) ? 77 : 35;

    while (track != 0) {
        if (ch->count >= DISKCHAIN_MAX_SECTORS) return false;
        if (track > max_track) return false;
        unsigned int spt = (img->type == diskimage_type_d80) ? d80_sectors(track)
                                                             : d64_sectors(track);
        if (sector >= spt) return false;
        if (track != buf_track) {
            if (!img->read(img->ctx, track_offset(img, track), trkbuf, spt * 256u))
                return false;
            buf_track = track;
        }
        const uint8_t* hdr = &trkbuf[(unsigned int) sector * 256u];
        ch->ts[ch->count][0] = track;
        ch->ts[ch->count][1] = sector;
        ch->count++;
        if (hdr[0] == 0) {
            // Final sector: byte 1 = index of last used byte.
            ch->last_used = (hdr[1] >= 2) ? (uint16_t) (hdr[1] - 1) : 0;
            return ch->count > 0;
        }
        track = hdr[0];
        sector = hdr[1];
    }
    return false;
}

uint32_t diskchain_size(const diskchain_t* ch) {
    if (ch->flat) return ch->flat_size;
    if (ch->count == 0) return 0;
    return (uint32_t) (ch->count - 1) * 254u + ch->last_used;
}

bool diskchain_read(const diskchain_t* ch, uint32_t off, uint8_t* buf, uint16_t len) {
    if (ch->flat) {
        if (off + len > ch->flat_size) return false;
        return len == 0 || ch->img->read(ch->img->ctx, off, buf, len);
    }
    while (len > 0) {
        uint16_t sec = (uint16_t) (off / 254u);
        uint16_t within = (uint16_t) (off % 254u);
        if (sec >= ch->count) return false;
        uint16_t avail = (sec == ch->count - 1) ? ch->last_used : 254u;
        if (within >= avail) return false;
        uint16_t take = (uint16_t) (avail - within);
        if (take > len) take = len;
        uint32_t soff = track_offset(ch->img, ch->ts[sec][0])
                        + (uint32_t) ch->ts[sec][1] * 256u;
        if (soff + 256u > ch->img->size) return false;
        if (!ch->img->read(ch->img->ctx, soff + 2 + within, buf, take)) return false;
        buf += take;
        off += take;
        len = (uint16_t) (len - take);
    }
    return true;
}

bool diskchain_write(const diskchain_t* ch, uint32_t off, const uint8_t* buf, uint16_t len) {
    if (ch->img->write == NULL) return false;
    if (ch->flat) {
        if (off + len > ch->flat_size) return false;
        return len == 0 || ch->img->write(ch->img->ctx, off, buf, len);
    }
    while (len > 0) {
        uint16_t sec = (uint16_t) (off / 254u);
        uint16_t within = (uint16_t) (off % 254u);
        if (sec >= ch->count) return false;
        uint16_t avail = (sec == ch->count - 1) ? ch->last_used : 254u;
        if (within >= avail) return false;
        uint16_t take = (uint16_t) (avail - within);
        if (take > len) take = len;
        uint32_t soff = track_offset(ch->img, ch->ts[sec][0])
                        + (uint32_t) ch->ts[sec][1] * 256u;
        if (soff + 256u > ch->img->size) return false;
        if (!ch->img->write(ch->img->ctx, soff + 2 + within, buf, take)) return false;
        buf += take;
        off += take;
        len = (uint16_t) (len - take);
    }
    return true;
}
