/* SPDX-License-Identifier: CC0-1.0 */
/* https://github.com/dlehenbauer/econopet */

/*
 * drive-test.c -- Destructive table-driven qualification suite for all eight virtual drive slots.
 *
 * Formats every mounted image, runs per-slot directory, BAM, sequential/typed
 * file, command, counted-block, and relative-file smoke tests, then exercises
 * unit/drive isolation, cross-drive copy and concatenation, multi-sector and
 * boundary transfers, wildcard scratch, block allocation, initialize/validate,
 * quick format, geometry boundaries, and disk duplicate.
 */

#include <conio.h>
#include <peekpoke.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "drive_test_catalog.h"
#include "target_diag.h"

#define COMMAND_SECONDARY 0x6fu
#define FILE_CHANNEL 2u
#define DIRECTORY_CHANNEL 0u
#define MAX_STATUS_LENGTH 32u
#define EMPTY_DIRECTORY_SIZE 64u
#define POPULATED_DIRECTORY_SIZE 96u
#define DIRECTORY_ENTRY_SIZE 32u
#define RAW_SECTOR_SIZE 256u
#define DISK_LABEL_LENGTH 16u

#define DOS4040_BAM_TRACK 18u
#define DOS4040_FIRST_FILE_TRACK 17u
#define DOS4040_BAM_NAME_OFFSET 0x90u
#define DOS4040_BAM_NAME_SIZE 27u
#define DOS4040_BAM_NAMBUF_OFFSET 180u

#define DOS8050_BAM_TRACK 38u
#define DOS8050_FIRST_FILE_SECTOR 1u
#define DOS8050_BAM_TRACKS_PER_SECTOR 50u
#define DOS8050_BAM_ENTRY_OFFSET 6u
#define DOS8050_DIRECTORY_TRACK 39u
#define DOS8250_LAST_TRACK (DT_DOS8250_TRACK_LIMIT - 1u)
#define DOS8250_LAST_SECTOR 22u
#define DOS8050_TRACKS_PER_SIDE 77u
#define DOS8050_BAM_INTERLEAVE 3u
#define DOS8050_ZONE_29_LAST_TRACK 39u
#define DOS8050_ZONE_27_LAST_TRACK 53u
#define DOS8050_ZONE_25_LAST_TRACK 64u

#define SMOKE_PAYLOAD_SIZE 16u
#define LONG_PAYLOAD_SIZE 512u
#define BLOCK_SMOKE_SIZE 4u
#define REL_MIN_TEST_SIZE 2u
#define REL_RECORD_SIZE 127u
#define REL_ODD_BOUNDARY_SIZE 129u
#define REL_SECTOR_DATA_SIZE 254u
#define REL_WRAP_BOUNDARY_SIZE 255u
#define REL_CROSS_FIRST_BYTES 3u
#define REL_CROSS_OFFSET (REL_SECTOR_DATA_SIZE - REL_ODD_BOUNDARY_SIZE - REL_CROSS_FIRST_BYTES + 1u)
#define REL_SIDE_DATA_SECTORS 120u

#define PAYLOAD_STEP 13u
#define REPLACE_STEP 17u
#define TRACK_WEIGHT 29u
#define SECTOR_WEIGHT 53u

#define STATUS_EOI 0x40u
#define PET_DEVICE 0x00d4u
#define PET_STATUS 0x0096u
#define PETSCII_UPPER_FIRST 0xc1u
#define PETSCII_UPPER_LAST 0xdau
#define PETSCII_CR 0x0du
#define PET_PNT_LO 0x00c4u
#define PET_PNT_HI 0x00c5u
#define PET_PNTR 0x00c6u
#define PET_TBLX 0x00d8u
#define PET_LNMX 0x00d5u
#define PROGRESS_ROW 5u
#define DETAIL_ROW 6u
#define PROGRESS_CLEAR_WIDTH 40u

#define DT_PHASE_NONE 0xffu

/** Indices into status_templates[] for exact DOS status string comparisons. */
typedef enum {
    ST_OK = 0,
    ST_SCRATCHED_1,
    ST_SCRATCHED_8,
    ST_SYNTAX_31,
    ST_RECORD_MISSING_50,
    ST_OVERFLOW_51,
    ST_NOT_FOUND_62,
    ST_FILE_EXISTS_63,
    ST_TYPE_MISMATCH_64,
    ST_NO_BLOCK_65,
} status_id_t;

/** Post-command verification actions executed by run_cmd_table() after checking status. */
typedef enum {
    POST_NONE = 0,
    POST_CONSUMED_OK,
    POST_READ_COPIED,
    POST_READ_COLLISION,
    POST_CONCAT_2,
    POST_CONCAT_3,
    POST_MIXED_SEQ_PRG,
    POST_MIXED_PRG_SEQ,
    POST_KEEP_TYPED_P,
    POST_KEEP_TYPED_U,
} cmd_post_t;

/** Table-driven DOS command step pairing a phase, format template, expected status, and follow-up check. */
typedef struct {
    /** Phase ordinal published to the mailbox, or DT_PHASE_NONE for sub-steps inside an active check. */
    unsigned char phase;
    /** Compact format string expanded by format_command() into cmd_buf. */
    const char* cmd_fmt;
    /** Expected status_templates[] index checked immediately after sending cmd_fmt. */
    unsigned char status_id;
    /** Optional post-command file or status verification action (cmd_post_t). */
    unsigned char post;
} cmd_step_t;

/** Descriptor for one relative-file record-length check in run_rel_case(). */
typedef struct {
    /** Phase ordinal published at the start of the record-length check. */
    unsigned char phase;
    /** Leading filename character distinguishing this REL file on the slot. */
    char prefix;
    /** Binary record length byte passed in OPEN "<drive>:<name>,L,<record_size>". */
    unsigned char record_size;
    /** Bitmask of REL_FLAG_* modifiers controlling overflow or boundary sub-checks. */
    unsigned char flags;
} rel_case_t;

#define REL_FLAG_OVERFLOW_WRITE 0x01u
#define REL_FLAG_BOUNDARY_EXTRAS 0x02u

/** Initialize the PET PIA/VIA IEEE-488 controller ports via ROM 4 CINT ($E000). */
void drive_bus_init(void);
/** Release all PET IEEE-488 controller output lines to idle without sending UNLISTEN or UNTALK. */
void drive_bus_release(void);
/** Address PET_DEVICE ($D4) as an IEEE-488 listener under ATN. */
void drive_listen(void);
/** Address PET_DEVICE ($D4) as an IEEE-488 talker under ATN. */
void drive_talk(void);
/** Send a listener secondary address byte under ATN and release ATN. */
void drive_secondary(unsigned char secondary);
/** Send a talker secondary address byte under ATN, arm NRFD/NDAC low, and release ATN. */
void drive_talk_secondary(unsigned char secondary);
/** Buffer one output byte so the final byte before UNLISTEN or role change carries EOI. */
void drive_ciout(unsigned char value);
/** Receive one byte from an active talker, setting STATUS_EOI ($40) in PET_STATUS ($96) on EOI. */
unsigned char drive_acptr(void);
/** Flush any buffered byte with EOI and transmit UNLISTEN ($3F) under ATN. */
void drive_unlisten(void);
/** Preempt any pending talker byte with ATN and transmit UNTALK ($5F). */
void drive_untalk(void);
/** Select the extended format/duplicate handshake timeout when enabled != 0, or restore default. */
void drive_long_timeout(unsigned char enabled);
/** Stream length bytes from payload[] through drive_ciout(). */
void drive_send_bytes(unsigned int length);
/**
 * Receive drive_bad_offset bytes from an active talker, comparing the first
 * compare_length bytes against payload[] and requiring EOI only on the last byte.
 */
unsigned char drive_recv_check(unsigned int compare_length);
/** Spin forever after failure output so hardware or VICE breakpoints stop cleanly. */
void drive_fail_halt(void);

/** Total receive byte count on entry to drive_recv_check(), or failing 0-based byte offset on return. */
extern unsigned int drive_bad_offset;
/** Expected payload byte or EOI flag recorded by drive_recv_check() on mismatch. */
extern unsigned char drive_bad_expected;
/** Actual received byte or EOI flag recorded by drive_recv_check() on mismatch. */
extern unsigned char drive_bad_actual;
/** Non-zero when drive_recv_check() failed on EOI placement rather than payload data. */
extern unsigned char drive_bad_eoi;
/** Raw VIA Port B byte captured when wait_via exhausted its handshake timeout budget. */
extern unsigned char drive_timeout_bus;

/** Canonical DOS status message prefixes (before optional DOS 2.7 ',<drive>' suffix and CR). */
static const char* const status_templates[] = {
    "00, OK,00,00",
    "01, FILES SCRATCHED,01,00",
    "01, FILES SCRATCHED,08,00",
    "31,SYNTAX ERROR,00,00",
    "50, RECORD NOT PRESENT,00,00",
    "51,OVERFLOW IN RECORD,00,00",
    "62, FILE NOT FOUND,00,00",
    "63, FILE EXISTS,00,00",
    "64, FILE TYPE MISMATCH,00,00",
    "65,NO BLOCK,01,01",
};

/** Single-drive command checks (rename, copy, collisions, scratch, missing file, syntax error). */
static const cmd_step_t smoke_cmd_steps[] = {
    {DT_PHASE_RENAME, "R%d:%r=%n", ST_OK, POST_NONE},
    {DT_PHASE_COPY, "C%d:%c=%r", ST_OK, POST_READ_COPIED},
    {DT_PHASE_RENAME_COLL, "R%d:%c=%r", ST_FILE_EXISTS_63, POST_NONE},
    {DT_PHASE_COPY_COLL, "C%d:%c=%r", ST_FILE_EXISTS_63, POST_READ_COLLISION},
    {DT_PHASE_SCRATCH, "S%d:%c", ST_SCRATCHED_1, POST_NONE},
    {DT_PHASE_MISSING_FILE, "R%d:%c=%c", ST_NOT_FOUND_62, POST_CONSUMED_OK},
    {DT_PHASE_UNKNOWN_CMD, "X", ST_SYNTAX_31, POST_NONE},
};

/** Cross-drive concatenation, mixed-type concatenation, and precedence error steps. */
static const cmd_step_t concat_cmd_steps[] = {
    {DT_PHASE_CROSS_CONCAT, "C1:JOIN=0:%0,1:%1", ST_OK, POST_CONCAT_2},
    {DT_PHASE_CONCAT_APPEND, "C1:JOIN=1:JOIN,0:%0", ST_OK, POST_CONCAT_3},
    {DT_PHASE_SEQ_PRG_CONCAT, "C1:MIX=0:%0,1:%p", ST_OK, POST_MIXED_SEQ_PRG},
    {DT_PHASE_PRG_SEQ_CONCAT, "C1:PMIX=1:%p,0:%0", ST_OK, POST_MIXED_PRG_SEQ},
    {DT_PHASE_CONCAT_MISSING, "C1:JOIN=1:%1,1:MISSING", ST_NOT_FOUND_62, POST_CONCAT_3},
    {DT_PHASE_CONCAT_EXISTS, "C1:JOIN=1:%1,1:%1", ST_FILE_EXISTS_63, POST_CONCAT_3},
    {DT_PHASE_PRG_SELF_APPEND, "C1:%p=1:%p,1:%1", ST_TYPE_MISMATCH_64, POST_KEEP_TYPED_P},
};

/** Wildcard scratch steps verifying '?' length-sensitivity and '*' prefix matching. */
static const cmd_step_t wildcard_cmd_steps[] = {
    {DT_PHASE_SCRATCH_Q, "S%d:P?", ST_SCRATCHED_8, POST_KEEP_TYPED_P},
    {DT_PHASE_SCRATCH_STAR, "S%d:P*", ST_SCRATCHED_1, POST_KEEP_TYPED_U},
};

/** Direct BAM block-free (B-F) and block-allocate (B-A) sequence on track 1, sectors 0..1. */
static const cmd_step_t alloc_cmd_steps[] = {
    {DT_PHASE_NONE, "B-F:%d,1,0", ST_OK, POST_NONE},          {DT_PHASE_NONE, "B-A:%d,1,0", ST_OK, POST_NONE},
    {DT_PHASE_NONE, "B-A:%d,1,0", ST_NO_BLOCK_65, POST_NONE}, {DT_PHASE_NONE, "B-A:%d,1,1", ST_OK, POST_NONE},
    {DT_PHASE_NONE, "B-F:%d,1,1", ST_OK, POST_NONE},          {DT_PHASE_NONE, "B-F:%d,1,0", ST_OK, POST_NONE},
    {DT_PHASE_NONE, "B-F:%d,1,0", ST_OK, POST_NONE},          {DT_PHASE_NONE, "B-A:%d,1,0", ST_OK, POST_NONE},
    {DT_PHASE_NONE, "B-F:%d,1,0", ST_OK, POST_NONE},
};

/** Representative relative-file record sizes exercising min, odd, full-sector, and wrap overflow. */
static const rel_case_t rel_length_cases[] = {
    {DT_PHASE_REL_2B, 'B', REL_MIN_TEST_SIZE, 0u},
    {DT_PHASE_REL_129B, 'M', REL_ODD_BOUNDARY_SIZE, 0u},
    {DT_PHASE_REL_254B, 'T', REL_SECTOR_DATA_SIZE, REL_FLAG_BOUNDARY_EXTRAS},
    {DT_PHASE_REL_255_OVF, 'W', REL_WRAP_BOUNDARY_SIZE, REL_FLAG_OVERFLOW_WRITE},
};

/** Shared transmit and expected-readback buffer read directly by drive_vectors.s. */
unsigned char payload[LONG_PAYLOAD_SIZE];
/** Assembled expected status bytes (including optional DOS 2.7 drive suffix and CR). */
static unsigned char expected_status[MAX_STATUS_LENGTH];
/** Raw status bytes received from secondary address 15. */
static unsigned char actual_status[MAX_STATUS_LENGTH];
/** Formatted DOS command buffer populated by format_command(). */
static char cmd_buf[48];
/** Formatted OPEN filename/type/mode buffer populated by open_typed_file(). */
static char name_buf[24];

/** Active byte offset reported on status or payload failure. */
static unsigned int byte_index;
/** Latest PET KERNAL status byte ($96) sampled after drive_acptr(). */
static unsigned char status;
/** Non-zero wait_via stage ID when a handshake timed out. */
static unsigned char timeout_stage;
/** Count of checks completed in the current pass. */
static unsigned int success_count;
/** 1-based ordinal of the check currently in progress. */
static unsigned int test_number;
/** Active dt_phase_t ordinal for screen and mailbox reporting. */
static unsigned char current_phase;
/** Active drive_slot_t pointer for screen and mailbox reporting. */
static const drive_slot_t* current_slot;
/** Pointer to drive 0 of the active dual-drive unit during cross-drive operations. */
static const drive_slot_t* pair_base_slot;
/** Active byte-length parameter displayed on failure when non-zero. */
static unsigned int current_length;
/** Active 1-based physical track reported on failure when non-zero. */
static unsigned char current_track;
/** Active 0-based physical sector reported on failure when current_track != 0. */
static unsigned char current_sector;
/** Last IEEE device whose command channel 15 was opened during the format sweep. */
static unsigned char previous_device;
/** Per-unit tracked DOS LSTDRV (0 or 1) used when commands omit an explicit drive number. */
static unsigned char last_drive_by_unit[DT_UNIT_COUNT];
/** Odd/even seqlock counter mirrored into TARGET_DIAG_OFF_SEQ around multi-byte mailbox updates. */
static unsigned char mailbox_seq;
/** Single-character substitution parameter expanded by '%k' in format_command(). */
static char fmt_char;

/** Emit an unsigned 16-bit integer in decimal without pulling in cprintf/ultoa. */
static void put_u16(unsigned int value) {
    /* Subtract powers of ten from 10000 down to 10, suppressing leading zeros. */
    static const unsigned int tens[] = {10000u, 1000u, 100u, 10u};
    unsigned char idx;
    unsigned char digit;
    bool started = false;

    for (idx = 0; idx < 4u; ++idx) {
        const unsigned int place = tens[idx];
        digit = 0;
        while (value >= place) {
            value -= place;
            ++digit;
        }
        if (digit != 0 || started) {
            cputc((char)('0' + digit));
            started = true;
        }
    }

    /* Emit the ones digit unconditionally so zero prints as '0'. */
    cputc((char)('0' + (unsigned char)value));
}

/** Emit an unsigned byte as two uppercase hexadecimal digits. */
static void put_hex8(unsigned char value) {
    /* Print high nibble followed by low nibble using a static hex lookup table. */
    static const char hex[] = "0123456789ABCDEF";
    cputc(hex[value >> 4]);
    cputc(hex[value & 0x0fu]);
}

/** Position the PET screen cursor without calling CC65's plot/JSR $FFF0 (invalid on PET KERNAL). */
static void set_cursor_row(unsigned char row) {
    /* Infer 40- or 80-column video pitch from LNMX ($D5) and update KERNAL TBLX/PNT/PNTR directly. */
    const unsigned char width = PEEK(PET_LNMX) >= 40u ? 80u : 40u;
    const unsigned int addr = 0x8000u + (unsigned int)row * width;
    POKE(PET_TBLX, row);
    POKE(PET_PNT_LO, (unsigned char)addr);
    POKE(PET_PNT_HI, (unsigned char)(addr >> 8));
    POKE(PET_PNTR, 0u);
}

/** Overwrite row tail with spaces directly in video RAM to erase stale characters cleanly. */
static void clear_line_tail(void) {
    /* Fill from the current cursor column PNTR ($C6) to PROGRESS_CLEAR_WIDTH with screen spaces. */
    const unsigned int line = (unsigned int)PEEK(PET_PNT_LO) | ((unsigned int)PEEK(PET_PNT_HI) << 8);
    unsigned char col = PEEK(PET_PNTR);
    while (col < PROGRESS_CLEAR_WIDTH) {
        POKE(line + col, ' ');
        ++col;
    }
}

/** Publish one completed check and its coordinates atomically to the $7FF0 mailbox. */
static void complete_test(void) {
    /* Advance seq to odd while writing phase, slot, and completed count, then even to commit. */
    ++mailbox_seq;
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SEQ), mailbox_seq);
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_PHASE), current_phase);
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SLOT),
         current_slot != 0 ? (unsigned char)(current_slot - dt_slots) : TARGET_DIAG_SLOT_NONE);
    ++success_count;
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_LO), (unsigned char)success_count);
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_HI), (unsigned char)(success_count >> 8));
    ++mailbox_seq;
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SEQ), mailbox_seq);
}

/** Stop on the first mismatch, rendering the PET screen before publishing FAILED. */
static void fail(unsigned char detail_id, const drive_slot_t* slot, unsigned char expected, unsigned char actual) {
    /* Release IEEE outputs without sending cleanup commands so the failing drive state stays intact. */
    drive_bus_release();

    /* Render a standalone failure summary on the PET video screen. */
    clrscr();
    cputs("DRIVE TEST FAILED\r\n");
    if (current_phase < DT_PHASE_COUNT) {
        cputs(dt_phase_pet_names[current_phase]);
        cputs("\r\n");
    }
    cputs(dt_detail_pet_names[detail_id]);
    cputs("\r\n");
    if (slot != 0) {
        cputs("UNIT ");
        put_u16(slot->device);
        cputs(" DRIVE ");
        put_u16(slot->drive);
        cputs("\r\n");
    }
    cputs("TEST ");
    put_u16(test_number);
    cputs(" OFFSET ");
    put_u16(byte_index);
    cputs("\r\n");
    if (current_length != 0) {
        cputs("LEN ");
        put_u16(current_length);
        cputs("\r\n");
    }
    if (current_track != 0) {
        cputs("TRK ");
        put_u16(current_track);
        cputs(" SEC ");
        put_u16(current_sector);
        cputs("\r\n");
    }
    cputs("EXP ");
    put_hex8(expected);
    cputs(" GOT ");
    put_hex8(actual);
    cputs("\r\n");
    if (timeout_stage != 0) {
        cputs("VIA ");
        put_hex8(drive_timeout_bus);
        cputs("\r\n");
    }
    cputs("HALTED\r\n");

    /* Publish the failure coordinates under an odd/even seqlock update and spin in _drive_fail_halt. */
    ++mailbox_seq;
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SEQ), mailbox_seq);
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_MAGIC0), TARGET_DIAG_MAGIC_0);
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_MAGIC1), TARGET_DIAG_MAGIC_1);
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_DETAIL_ID), detail_id);
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_EXPECTED), expected);
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_ACTUAL), actual);
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_AUX0), timeout_stage != 0 ? timeout_stage : current_track);
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_AUX1), timeout_stage != 0 ? drive_timeout_bus : current_sector);
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_OFFSET_LO), (unsigned char)byte_index);
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_OFFSET_HI), (unsigned char)(byte_index >> 8));
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_PHASE), current_phase);
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SLOT), slot != 0 ? (unsigned char)(slot - dt_slots) : TARGET_DIAG_SLOT_NONE);
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_STATE), TARGET_DIAG_STATE_FAILED);
    ++mailbox_seq;
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SEQ), mailbox_seq);
    drive_fail_halt();
}

/** Report a stalled handshake from assembly wait_via after controller outputs were released. */
void drive_timeout(unsigned char stage) {
    /* Latch the handshake stage and delegate to fail() with DT_ERR_TIMEOUT. */
    timeout_stage = stage;
    fail(DT_ERR_TIMEOUT, current_slot, 0, stage);
}

/** Update the in-place progress row and reset diagnostic coordinates before I/O. */
static void begin_test(unsigned char phase, const drive_slot_t* slot) {
    /* Advance the 1-based check counter and clear per-check diagnostic coordinates. */
    ++test_number;
    byte_index = 0;
    current_phase = phase;
    current_slot = slot;
    current_length = 0;
    current_track = 0;
    current_sector = 0;

    /* Overwrite the fixed progress and detail rows in place to avoid costly screen scrolling. */
    set_cursor_row(PROGRESS_ROW);
    put_u16(test_number);
    cputs(": ");
    cputs(dt_phase_pet_names[phase]);
    if (slot != 0) {
        cputc(' ');
        put_u16(slot->device);
        cputc('/');
        put_u16(slot->drive);
    }
    clear_line_tail();
    set_cursor_row(DETAIL_ROW);
    clear_line_tail();
}

/** Show a secondary parameter such as boundary length without scrolling the screen. */
static void show_length(unsigned int length) {
    /* Record length for failure reporting and render it on DETAIL_ROW. */
    current_length = length;
    set_cursor_row(DETAIL_ROW);
    cputs("LEN ");
    put_u16(length);
    clear_line_tail();
}

/** Set the PET KERNAL's current IEEE device address ($D4). */
static void select_device(unsigned char device) { POKE(PET_DEVICE, device); }

/** Convert CC65 shifted-PETSCII uppercase literals ($C1..$DA) into unshifted DOS ASCII ($41..$5A). */
static unsigned char dos_text(unsigned char value) {
    if (value >= PETSCII_UPPER_FIRST && value <= PETSCII_UPPER_LAST) {
        return value & 0x7fu;
    }
    return value;
}

/** Append an unsigned decimal byte (0..255) without 16-bit division helpers. */
static char* append_u8(char* dest, unsigned char value) {
    /* Subtract hundreds and tens places iteratively and append the resulting ASCII digits. */
    unsigned char digit = 0;
    if (value >= 100u) {
        while (value >= 100u) {
            value -= 100u;
            ++digit;
        }
        *dest++ = (char)('0' + digit);
        digit = 0;
        while (value >= 10u) {
            value -= 10u;
            ++digit;
        }
        *dest++ = (char)('0' + digit);
    } else if (value >= 10u) {
        while (value >= 10u) {
            value -= 10u;
            ++digit;
        }
        *dest++ = (char)('0' + digit);
    }
    *dest++ = (char)('0' + value);
    return dest;
}

/** Copy a slot name while replacing its leading character when prefix != 0. */
static char* append_prefixed_name(char* dest, const char* name, char prefix) {
    /* Substitute prefix for name[0] when requested, then copy the remaining characters. */
    if (prefix != 0) {
        *dest++ = prefix;
        ++name;
    }
    while (*name != '\0') {
        *dest++ = *name++;
    }
    return dest;
}

/** Expand a compact DOS command template into cmd_buf for slot and pair_base_slot. */
static void format_command(const drive_slot_t* slot, const char* fmt) {
    char* dest = cmd_buf;
    char token;

    /* Copy literal characters and expand '%'-prefixed slot, pair, track, sector, or char tokens. */
    while ((token = *fmt++) != '\0') {
        if (token != '%') {
            *dest++ = token;
            continue;
        }
        token = *fmt++;
        switch (token) {
        case 'd':
            *dest++ = (char)('0' + slot->drive);
            break;
        case 'n':
            dest = append_prefixed_name(dest, slot->name, 0);
            break;
        case 'r':
            dest = append_prefixed_name(dest, slot->name, 'R');
            break;
        case 'c':
            dest = append_prefixed_name(dest, slot->name, 'C');
            break;
        case 'i':
            *dest++ = slot->disk_id[0];
            *dest++ = slot->disk_id[1];
            break;
        case '0':
            dest = append_prefixed_name(dest, pair_base_slot[0].name, 'R');
            break;
        case '1':
            dest = append_prefixed_name(dest, pair_base_slot[1].name, 'R');
            break;
        case 'p':
            dest = append_prefixed_name(dest, pair_base_slot[1].name, 'P');
            break;
        case 't':
            dest = append_u8(dest, current_track);
            break;
        case 's':
            dest = append_u8(dest, current_sector);
            break;
        case 'k':
            *dest++ = fmt_char;
            break;
        default:
            *dest++ = token;
            break;
        }
    }
    *dest = '\0';
}

/** Open the device's command channel (secondary 15, $FF) using the PET ROM 4 IEEE sequence. */
static void open_command_channel(unsigned char device) {
    /* Issue LISTEN, OPEN 15 ($FF), and UNLISTEN with an empty filename. */
    select_device(device);
    drive_listen();
    drive_secondary(0xffu);
    drive_unlisten();
}

/** Send one null-terminated DOS command string on secondary 15 ($6F). */
static void send_command(unsigned char device, const char* command) {
    /* Stream unshifted DOS ASCII bytes on channel 15 and assert EOI on the last byte via UNLISTEN. */
    select_device(device);
    drive_listen();
    drive_secondary(COMMAND_SECONDARY);
    while (*command != '\0') {
        drive_ciout(dos_text((unsigned char)*command++));
    }
    drive_unlisten();
}

/** Expand fmt into cmd_buf and transmit it on slot->device's command channel. */
static void send_formatted_command(const drive_slot_t* slot, const char* fmt) {
    format_command(slot, fmt);
    send_command(slot->device, cmd_buf);
}

/** Close an open data or directory channel ($E0 | channel) on the selected device. */
static void close_secondary(unsigned char channel) {
    drive_listen();
    drive_secondary((unsigned char)(0xe0u | channel));
    drive_unlisten();
}

/** Open bare '#' on channel 2 for direct-access sector operations. */
static void open_direct_channel(const drive_slot_t* slot) {
    /* Send LISTEN, OPEN 2 ($F2), '#', and UNLISTEN to allocate an unnumbered direct buffer. */
    select_device(slot->device);
    drive_listen();
    drive_secondary((unsigned char)(0xf0u | FILE_CHANNEL));
    drive_ciout('#');
    drive_unlisten();
}

/** Read one complete status record from channel 15, stopping only when EOI is observed. */
static unsigned char read_status(unsigned char device, unsigned char* output) {
    unsigned char length = 0;
    bool eoi = false;

    /* Address channel 15 to talk and collect bytes until EOI or MAX_STATUS_LENGTH. */
    select_device(device);
    drive_talk();
    drive_talk_secondary(COMMAND_SECONDARY);
    while (length < MAX_STATUS_LENGTH) {
        POKE(PET_STATUS, 0);
        output[length] = drive_acptr();
        status = PEEK(PET_STATUS);
        ++length;
        if ((status & STATUS_EOI) != 0) {
            eoi = true;
            break;
        }
    }
    drive_untalk();
    return eoi ? length : 0;
}

/** Compare an entire status record by template ID, using explicit expected_drive for DOS 2.7. */
static void check_status_drive(const drive_slot_t* slot, unsigned char id, unsigned char expected_drive) {
    const char* const text = status_templates[id];
    unsigned char expected_length = 0;
    unsigned char actual_length;

    /* Assemble the expected status bytes, appending ',<drive>' on 8050/8250 DOS 2.7 and trailing CR. */
    while (text[expected_length] != '\0') {
        expected_status[expected_length] = dos_text((unsigned char)text[expected_length]);
        ++expected_length;
    }
    if (dt_geoms[slot->geom].d80_status) {
        expected_status[expected_length++] = ',';
        expected_status[expected_length++] = (unsigned char)('0' + expected_drive);
    }
    expected_status[expected_length++] = PETSCII_CR;

    /* Read channel 15 and verify both exact length (with EOI) and every status byte. */
    byte_index = 0;
    actual_length = read_status(slot->device, actual_status);
    if (actual_length != expected_length) {
        fail(DT_ERR_STATUS_LEN, slot, expected_length, actual_length);
    }
    for (byte_index = 0; byte_index < expected_length; ++byte_index) {
        if (actual_status[byte_index] != expected_status[byte_index]) {
            fail(DT_ERR_STATUS_BYTE, slot, expected_status[byte_index], actual_status[byte_index]);
        }
    }
}

/** Compare an entire status record by template ID and record slot->drive as the unit's LSTDRV. */
static void check_status_id(const drive_slot_t* slot, unsigned char id) {
    check_status_drive(slot, id, slot->drive);
    last_drive_by_unit[slot->device - DT_DEVICE_FIRST] = slot->drive;
}

/** Require the complete expected 00, OK status record for slot. */
static void check_status(const drive_slot_t* slot) { check_status_id(slot, ST_OK); }

/** Open bare '#' on channel 2 and verify OPNBLK's LSTDRV-attributed status. */
static void open_direct_checked(const drive_slot_t* slot) {
    /* Bare '#' omits a drive prefix, so DOS 2.7 reports the unit's retained LSTDRV in status. */
    open_direct_channel(slot);
    check_status_drive(slot, ST_OK, last_drive_by_unit[slot->device - DT_DEVICE_FIRST]);
}

/** Fill a stream segment with an offset-sensitive progression and optional leading binary bytes. */
void drive_fill_payload(unsigned char* dest, unsigned char seed, unsigned int length, bool binary_header) {
    /* Mix in the 256-byte region so replaying an earlier region cannot pass a multi-sector comparison. */
    unsigned char value = seed;
    for (byte_index = 0; byte_index < length; ++byte_index) {
        dest[byte_index] = value ^ (unsigned char)(byte_index >> 8);
        value = (unsigned char)(value + PAYLOAD_STEP);
    }

    /* Inject NUL, CR, and $FF in the leading bytes when testing binary transparency. */
    if (binary_header && length != 0) {
        dest[0] = 0;
        if (length >= 3u) {
            dest[1] = PETSCII_CR;
            dest[2] = 0xffu;
        } else if (length == 2u) {
            dest[1] = (unsigned char)(seed ^ PETSCII_CR);
        }
    }
    byte_index = 0;
}

/** Receive total_length bytes on channel, verifying compare_length bytes against payload[] and exact EOI. */
static void recv_and_verify(const drive_slot_t* slot, unsigned char channel, unsigned int total_length,
                            unsigned int compare_length, unsigned char byte_err, unsigned char eoi_err) {
    /* Address channel to talk, run the assembly stream verifier, and issue UNTALK on success. */
    select_device(slot->device);
    drive_talk();
    drive_talk_secondary((unsigned char)(0x60u | channel));
    drive_bad_offset = total_length;
    if (!drive_recv_check(compare_length)) {
        byte_index = drive_bad_offset;
        fail(drive_bad_eoi ? eoi_err : byte_err, slot, drive_bad_expected, drive_bad_actual);
    }
    drive_untalk();
}

/** Build "[@]<drive>:<name>,<type>,<mode>" in name_buf and send OPEN on channel 2. */
static void open_typed_file(const drive_slot_t* slot, const char* name, char type, char mode, bool replace) {
    char* dest = name_buf;
    char* cursor;

    /* Format the DOS file specification with optional '@' save-with-replace prefix. */
    if (replace) {
        *dest++ = '@';
    }
    *dest++ = (char)('0' + slot->drive);
    *dest++ = ':';
    while (*name != '\0') {
        *dest++ = *name++;
    }
    *dest++ = ',';
    *dest++ = type;
    *dest++ = ',';
    *dest++ = mode;
    *dest = '\0';

    /* Transmit the OPEN specification on channel 2 ($F2) and require 00, OK status. */
    select_device(slot->device);
    drive_listen();
    drive_secondary((unsigned char)(0xf0u | FILE_CHANNEL));
    for (cursor = name_buf; *cursor != '\0'; ++cursor) {
        drive_ciout(dos_text((unsigned char)*cursor));
    }
    drive_unlisten();
    check_status(slot);
}

/** Open a typed ordinary file, stream payload[0..length-1], and verify close status. */
static void write_typed_payload_mode(const drive_slot_t* slot, const char* name, char type, char mode,
                                     unsigned int length, bool replace) {
    /* Open for write/append, stream length bytes on channel 2, close, and check status. */
    open_typed_file(slot, name, type, mode, replace);
    drive_listen();
    drive_secondary((unsigned char)(0x60u | FILE_CHANNEL));
    drive_send_bytes(length);
    drive_unlisten();
    close_secondary(FILE_CHANNEL);
    check_status(slot);
}

/** Read a typed ordinary file, verifying compare_length bytes against payload[] and EOI. */
static void read_and_verify_typed(const drive_slot_t* slot, const char* name, char type, unsigned int length,
                                  unsigned int compare_length, unsigned char byte_err) {
    /* Open for read, verify the received byte stream and final EOI, then close and check status. */
    open_typed_file(slot, name, type, 'R', false);
    recv_and_verify(slot, FILE_CHANNEL, length, compare_length, byte_err, DT_ERR_READ_EOI);
    select_device(slot->device);
    close_secondary(FILE_CHANNEL);
    check_status(slot);
}

/** Full-format one drive with a slot-specific label and two-character disk ID. */
void drive_format_slot(const drive_slot_t* slot) {
    /* Open channel 15 once per device, then run NEW under the extended format timeout budget. */
    begin_test(DT_PHASE_FULL_FORMAT, slot);
    if (slot->device != previous_device) {
        open_command_channel(slot->device);
        previous_device = slot->device;
    }
    drive_long_timeout(1);
    send_formatted_command(slot, "N%d:%n,%i");
    check_status(slot);
    drive_long_timeout(0);
    complete_test();
}

/** Verify a slot's 16-byte SEQ or binary-header smoke file from its deterministic signature. */
static void verify_slot_smoke_file(const drive_slot_t* slot, char prefix, char type, unsigned char byte_err) {
    /* Reconstruct the expected 16-byte payload from slot->signature and read back the named file. */
    char target_name[8];
    *append_prefixed_name(target_name, slot->name, prefix) = '\0';
    drive_fill_payload(payload, slot->signature, SMOKE_PAYLOAD_SIZE, type != 'S');
    read_and_verify_typed(slot, target_name, type, SMOKE_PAYLOAD_SIZE, SMOKE_PAYLOAD_SIZE, byte_err);
}

/** Read one slot's retained 'R...' smoke file and verify its unique signature. */
static void check_isolation_slot(const drive_slot_t* slot, unsigned char phase) {
    begin_test(phase, slot);
    verify_slot_smoke_file(slot, 'R', 'S', DT_ERR_ISO_READBACK);
    complete_test();
}

/** Compare alternating source signatures in a concatenated sequential file. */
static void check_concat_payload(const drive_slot_t* first_slot, unsigned char segments) {
    const drive_slot_t* const target = &first_slot[1];
    const unsigned int length = (unsigned int)segments * SMOKE_PAYLOAD_SIZE;
    unsigned char seg;

    /* Concatenate 16-byte progressions alternating between drive 0 and drive 1 signatures, then read JOIN. */
    for (seg = 0; seg < segments; ++seg) {
        drive_fill_payload(&payload[seg * SMOKE_PAYLOAD_SIZE], first_slot[seg & 1u].signature, SMOKE_PAYLOAD_SIZE,
                           false);
    }
    read_and_verify_typed(target, "JOIN", 'S', length, length, DT_ERR_CONCAT_READBACK);
}

/** Compare mixed PRG/SEQ inputs while requiring the first source's output type. */
static void check_mixed_concat(const drive_slot_t* first_slot, const char* name, bool program_first) {
    const drive_slot_t* const target = &first_slot[1];
    unsigned char half;

    /* Assemble both source streams independently, then verify the first source's output file type. */
    for (half = 0; half < 2u; ++half) {
        /* Explicit normalization avoids CC65 reusing the loop-bound comparison's flags for half == 0. */
        const bool program = (half == 0 ? 1u : 0u) == (program_first ? 1u : 0u);
        const drive_slot_t* const source = program ? target : first_slot;
        unsigned char* const dst = &payload[half * SMOKE_PAYLOAD_SIZE];
        drive_fill_payload(dst, source->signature, SMOKE_PAYLOAD_SIZE, program);
    }
    read_and_verify_typed(target, name, program_first ? 'P' : 'S', 2u * SMOKE_PAYLOAD_SIZE, 2u * SMOKE_PAYLOAD_SIZE,
                          DT_ERR_MIXED_READBACK);
}

/** Execute a table of formatted DOS commands, status checks, and post-condition verifications. */
static void run_cmd_table(const drive_slot_t* slot, const cmd_step_t* steps, unsigned char step_count) {
    unsigned char idx;

    /* Run each table entry's command, status assertion, and optional post-condition readback. */
    for (idx = 0; idx < step_count; ++idx) {
        const cmd_step_t* const step = &steps[idx];
        if (step->phase != DT_PHASE_NONE) {
            begin_test(step->phase, slot);
        }
        send_formatted_command(slot, step->cmd_fmt);
        check_status_id(slot, step->status_id);

        switch (step->post) {
        case POST_CONSUMED_OK:
            check_status(slot);
            break;
        case POST_READ_COPIED:
            verify_slot_smoke_file(slot, 'C', 'S', DT_ERR_COPY_READBACK);
            break;
        case POST_READ_COLLISION:
            verify_slot_smoke_file(slot, 'R', 'S', DT_ERR_COLL_SOURCE);
            verify_slot_smoke_file(slot, 'C', 'S', DT_ERR_COLL_TARGET);
            break;
        case POST_CONCAT_2:
            check_concat_payload(pair_base_slot, 2u);
            break;
        case POST_CONCAT_3:
            check_concat_payload(pair_base_slot, 3u);
            break;
        case POST_MIXED_SEQ_PRG:
            check_mixed_concat(pair_base_slot, "MIX", false);
            break;
        case POST_MIXED_PRG_SEQ:
            check_mixed_concat(pair_base_slot, "PMIX", true);
            break;
        case POST_KEEP_TYPED_P:
            verify_slot_smoke_file(slot, 'P', 'P', DT_ERR_TYPED_READBACK);
            break;
        case POST_KEEP_TYPED_U:
            verify_slot_smoke_file(slot, 'U', 'U', DT_ERR_TYPED_READBACK);
            break;
        default:
            break;
        }
        if (step->phase != DT_PHASE_NONE) {
            complete_test();
        }
    }
}

/** Check PRG and USR file streams without invoking the PET's LOAD parser. */
static void test_file_types(const drive_slot_t* slot) {
    static const char types[] = {'P', 'U'};
    char target_name[8];
    unsigned char idx;

    /* Create and read back 16-byte binary-header PRG and USR files on channel 2. */
    for (idx = 0; idx < count_of(types); ++idx) {
        const char type = types[idx];
        begin_test(type == 'P' ? DT_PHASE_PRG_STREAM : DT_PHASE_USR_STREAM, slot);
        *append_prefixed_name(target_name, slot->name, type) = '\0';
        drive_fill_payload(payload, slot->signature, SMOKE_PAYLOAD_SIZE, true);
        write_typed_payload_mode(slot, target_name, type, 'W', SMOKE_PAYLOAD_SIZE, false);
        read_and_verify_typed(slot, target_name, type, SMOKE_PAYLOAD_SIZE, SMOKE_PAYLOAD_SIZE, DT_ERR_FILE_READBACK);
        complete_test();
    }
}

/** Write and read a counted direct block and exercise B-P cursor repositioning. */
static void smoke_block(const drive_slot_t* slot) {
    /* Open a direct '#' buffer on channel 2 and stage a 4-byte binary payload. */
    begin_test(DT_PHASE_DIRECT_BLOCK, slot);
    open_direct_checked(slot);

    payload[0] = 0x00;
    payload[1] = PETSCII_CR;
    payload[2] = 0xffu;
    payload[3] = slot->signature;
    drive_listen();
    drive_secondary((unsigned char)(0x60u | FILE_CHANNEL));
    drive_send_bytes(BLOCK_SMOKE_SIZE);
    drive_unlisten();

    /* Commit the counted block to track 1, sector 0 with B-W and reload it with B-R. */
    current_track = 1u;
    current_sector = 0u;
    send_formatted_command(slot, "B-W:2,%d,1,0");
    check_status(slot);
    send_formatted_command(slot, "B-R:2,%d,1,0");
    check_status(slot);

    /* Reposition to buffer index 2 via B-P while preserving B-R's counted end pointer. */
    send_command(slot->device, "B-P:2,2");
    check_status(slot);
    payload[0] = PETSCII_CR;
    payload[1] = 0xffu;
    payload[2] = slot->signature;
    recv_and_verify(slot, FILE_CHANNEL, BLOCK_SMOKE_SIZE - 1u, BLOCK_SMOKE_SIZE - 1u, DT_ERR_BP_BYTE, DT_ERR_BP_EOI);

    /* Issue a fresh B-R to reset the buffer cursor and verify all 4 counted bytes. */
    payload[0] = 0x00;
    payload[1] = PETSCII_CR;
    payload[2] = 0xffu;
    payload[3] = slot->signature;
    send_formatted_command(slot, "B-R:2,%d,1,0");
    check_status(slot);
    recv_and_verify(slot, FILE_CHANNEL, BLOCK_SMOKE_SIZE, BLOCK_SMOKE_SIZE, DT_ERR_BLK_BYTE, DT_ERR_BLK_EOI);

    /* Close the direct buffer channel and complete the check. */
    select_device(slot->device);
    close_secondary(FILE_CHANNEL);
    check_status(slot);
    complete_test();
}

/** Open a slot's named relative file, optionally specifying its binary record size on creation. */
static void open_rel(const drive_slot_t* slot, char prefix, unsigned char record_size, bool create) {
    char target_name[8];
    char* cursor;

    /* Send "<drive>:<name>[,L,<record_size>]" on channel 2 ($F2) and verify 00, OK status. */
    *append_prefixed_name(target_name, slot->name, prefix) = '\0';
    select_device(slot->device);
    drive_listen();
    drive_secondary((unsigned char)(0xf0u | FILE_CHANNEL));
    drive_ciout((unsigned char)('0' + slot->drive));
    drive_ciout(':');
    for (cursor = target_name; *cursor != '\0'; ++cursor) {
        drive_ciout(dos_text((unsigned char)*cursor));
    }
    if (create) {
        drive_ciout(',');
        drive_ciout(dos_text('L'));
        drive_ciout(',');
        drive_ciout(record_size);
    }
    drive_unlisten();
    check_status(slot);
}

/** Close the active relative channel and verify 00, OK status. */
static void close_rel(const drive_slot_t* slot) {
    select_device(slot->device);
    close_secondary(FILE_CHANNEL);
    check_status(slot);
}

/** Position a relative channel with the 5-byte binary RECORD ('P') command. */
static void position_rel(const drive_slot_t* slot, unsigned int record_number, unsigned char offset) {
    /* Transmit 'P', channel number, 16-bit little-endian record number, and 1-based byte offset. */
    select_device(slot->device);
    drive_listen();
    drive_secondary(COMMAND_SECONDARY);
    drive_ciout(dos_text('P'));
    drive_ciout(FILE_CHANNEL);
    drive_ciout((unsigned char)record_number);
    drive_ciout((unsigned char)(record_number >> 8));
    drive_ciout(offset);
    drive_unlisten();
}

/** Send payload[start..start+length-1] as one relative record write with final EOI. */
static void write_rel_slice(const drive_slot_t* slot, unsigned char start, unsigned char length) {
    /* Stream the slice on channel 2 so UNLISTEN marks the record boundary with EOI. */
    unsigned char i;
    select_device(slot->device);
    drive_listen();
    drive_secondary((unsigned char)(0x60u | FILE_CHANNEL));
    for (i = 0; i < length; ++i) {
        drive_ciout(payload[start + i]);
    }
    drive_unlisten();
}

/** Position to record_number offset 1 and compare length bytes against payload[]. */
static void compare_rel(const drive_slot_t* slot, unsigned int record_number, unsigned int length) {
    /* Verify status 00 both after positioning and after reading the record through EOI. */
    position_rel(slot, record_number, 1u);
    check_status(slot);
    recv_and_verify(slot, FILE_CHANNEL, length, length, DT_ERR_REL_READBACK, DT_ERR_REL_EOI);
    check_status(slot);
}

/** Verify native missing-record status 50 at both POSITION and actual GET. */
static void check_rel_missing_record(const drive_slot_t* slot, unsigned char phase, unsigned int record_number) {
    /* Position to an unallocated record (status 50), then read its synthetic CR+EOI and re-verify status 50. */
    begin_test(phase, slot);
    position_rel(slot, record_number, 1u);
    check_status_id(slot, ST_RECORD_MISSING_50);
    payload[0] = PETSCII_CR;
    recv_and_verify(slot, FILE_CHANNEL, 1u, 1u, DT_ERR_REL_MISS_BYTE, DT_ERR_REL_MISS_EOI);
    check_status_id(slot, ST_RECORD_MISSING_50);
    complete_test();
}

/** Position and compare a single byte and its EOI state on an open REL file. */
static void check_rel_byte_at(const drive_slot_t* slot, unsigned int record_number, unsigned char offset,
                              unsigned char expected, bool expect_eoi) {
    unsigned char actual;

    /* Position to (record_number, offset) and read a single byte via drive_acptr() before UNTALK. */
    position_rel(slot, record_number, offset);
    check_status(slot);

    byte_index = 0;
    select_device(slot->device);
    drive_talk();
    drive_talk_secondary((unsigned char)(0x60u | FILE_CHANNEL));
    POKE(PET_STATUS, 0);
    actual = drive_acptr();
    status = PEEK(PET_STATUS);
    if (actual != expected) {
        fail(DT_ERR_REL_POS_BYTE, slot, expected, actual);
    }
    if (((status & STATUS_EOI) != 0 ? 1u : 0u) != (expect_eoi ? 1u : 0u)) {
        fail(DT_ERR_REL_POS_EOI, slot, expect_eoi ? 1u : 0u, (status & STATUS_EOI) != 0 ? 1u : 0u);
    }
    drive_untalk();
}

/** Execute one relative record creation/readback descriptor and optional boundary sub-checks. */
static void run_rel_case(const drive_slot_t* slot, const rel_case_t* spec) {
    /* Create the REL file and write one full record of spec->record_size bytes. */
    begin_test(spec->phase, slot);
    current_length = spec->record_size;
    open_rel(slot, spec->prefix, spec->record_size, true);
    drive_fill_payload(payload, slot->signature, spec->record_size, true);
    write_rel_slice(slot, 0, spec->record_size);

    /* Verify native 255-byte NULBUF:ADDNR pointer-wrap overflow (status 51) when expected. */
    if ((spec->flags & REL_FLAG_OVERFLOW_WRITE) != 0) {
        check_status_id(slot, ST_OVERFLOW_51);
        close_rel(slot);
        complete_test();
        return;
    }

    /* Reopen the REL file and verify full-record readback at record 1, offset 1. */
    check_status(slot);
    close_rel(slot);
    open_rel(slot, spec->prefix, spec->record_size, false);
    compare_rel(slot, 1u, spec->record_size);
    if ((spec->flags & REL_FLAG_BOUNDARY_EXTRAS) == 0) {
        close_rel(slot);
        complete_test();
        return;
    }
    complete_test();

    /* Exercise final-offset EOI, zero-record/zero-offset aliases, missing records, and offset overflow. */
    begin_test(DT_PHASE_REL_LAST_OFF, slot);
    check_rel_byte_at(slot, 1u, spec->record_size, payload[spec->record_size - 1u], true);
    complete_test();

    begin_test(DT_PHASE_REL_ZERO_ALIAS, slot);
    check_rel_byte_at(slot, 0u, 1u, payload[0], false);
    check_rel_byte_at(slot, 1u, 0u, payload[0], false);
    complete_test();

    check_rel_missing_record(slot, DT_PHASE_REL_MISSING, 2u);
    check_rel_missing_record(slot, DT_PHASE_REL_HIGH_BYTE, 257u);

    begin_test(DT_PHASE_REL_POS_OVF, slot);
    position_rel(slot, 1u, (unsigned char)(spec->record_size + 1u));
    check_status_id(slot, ST_OVERFLOW_51);
    close_rel(slot);
    complete_test();
}

/** Grow a short record across sectors, then verify padding, empty records, and persistence. */
static void test_rel_growth(const drive_slot_t* slot) {
    static const unsigned char leading[] = {0, 0, PETSCII_CR, 0xffu, 0x55u, 0x7eu};
    static const unsigned char tail[] = {0, PETSCII_CR, 0xffu, 0x55u, 0xaau, 0x5au, 0x7eu};

    /* Create a 129-byte-record REL file and write a short 3-byte first record. */
    begin_test(DT_PHASE_REL_GROW_SETUP, slot);
    open_rel(slot, 'G', REL_ODD_BOUNDARY_SIZE, true);
    payload[0] = slot->signature;
    payload[1] = 0;
    payload[2] = PETSCII_CR;
    write_rel_slice(slot, 0, 3u);
    check_status(slot);
    complete_test();

    /* Confirm record 2 is initially missing (status 50). */
    check_rel_missing_record(slot, DT_PHASE_REL_GROW_MISS, 2u);

    /* Position to missing record 2 at a non-unit offset and verify ADDREL resets RECPTR to offset 1 on write. */
    begin_test(DT_PHASE_REL_OFF_GROW, slot);
    position_rel(slot, 2u, REL_CROSS_OFFSET);
    check_status_id(slot, ST_RECORD_MISSING_50);
    memcpy(payload, leading, sizeof(leading));
    payload[0] = slot->signature;
    write_rel_slice(slot, 0, sizeof(leading));
    check_status(slot);
    compare_rel(slot, 2u, sizeof(leading));
    complete_test();

    /* Write across the 254-byte data-sector boundary at REL_CROSS_OFFSET, reopen, and verify padding and neighbors. */
    begin_test(DT_PHASE_REL_SEC_CROSS, slot);
    position_rel(slot, 2u, REL_CROSS_OFFSET);
    check_status(slot);
    memset(&payload[sizeof(leading)], 0, REL_ODD_BOUNDARY_SIZE - sizeof(leading));
    memcpy(&payload[REL_CROSS_OFFSET - 1u], tail, sizeof(tail));
    write_rel_slice(slot, REL_CROSS_OFFSET - 1u, sizeof(tail));
    check_status(slot);
    close_rel(slot);
    open_rel(slot, 'G', REL_ODD_BOUNDARY_SIZE, false);
    compare_rel(slot, 2u, REL_ODD_BOUNDARY_SIZE);
    payload[0] = slot->signature;
    payload[1] = 0;
    payload[2] = PETSCII_CR;
    compare_rel(slot, 1u, 3u);
    payload[0] = 0xffu;
    compare_rel(slot, 3u, 1u);
    complete_test();

    /* Confirm record 4 beyond the newly formatted sector remains missing. */
    check_rel_missing_record(slot, DT_PHASE_REL_NEXT_MISS, 4u);
    close_rel(slot);
}

/** Extend the 254-byte 'T...' REL file past the first side-sector pointer table (120 sectors). */
static void test_rel_side_boundary(const drive_slot_t* slot) {
    /* Reopen the 254-byte-record file created during rel_length_cases[] and confirm record 121 is missing. */
    begin_test(DT_PHASE_REL_SIDE_OPEN, slot);
    open_rel(slot, 'T', REL_SECTOR_DATA_SIZE, false);
    complete_test();

    check_rel_missing_record(slot, DT_PHASE_REL_SIDE_MISS, REL_SIDE_DATA_SECTORS + 1u);

    /* Position to record 121 and write 2 bytes to force allocation of the second side sector. */
    begin_test(DT_PHASE_REL_SIDE_GROW, slot);
    position_rel(slot, REL_SIDE_DATA_SECTORS + 1u, 1u);
    check_status_id(slot, ST_RECORD_MISSING_50);
    payload[0] = slot->signature;
    payload[1] = 0x7eu;
    drive_long_timeout(1);
    write_rel_slice(slot, 0, 2u);
    check_status(slot);
    close_rel(slot);
    drive_long_timeout(0);
    complete_test();

    /* Reopen and verify record 121, the initialized empty record 120 ($FF), and record 1. */
    begin_test(DT_PHASE_REL_SIDE_RET, slot);
    open_rel(slot, 'T', REL_SECTOR_DATA_SIZE, false);
    compare_rel(slot, REL_SIDE_DATA_SECTORS + 1u, 2u);
    payload[0] = 0xffu;
    compare_rel(slot, REL_SIDE_DATA_SECTORS, 1u);
    drive_fill_payload(payload, slot->signature, REL_SECTOR_DATA_SIZE, true);
    compare_rel(slot, 1u, REL_SECTOR_DATA_SIZE);
    complete_test();

    /* Confirm record 122 remains missing and close the file. */
    check_rel_missing_record(slot, DT_PHASE_REL_SIDE_NEXT, REL_SIDE_DATA_SECTORS + 2u);
    close_rel(slot);
}

/** Check the complete BASIC directory stream before or after creating one smoke file. */
static void check_smoke_directory(const drive_slot_t* slot, bool populated) {
    static const char footer[] = "BLOCKS FREE.";
    const drive_geom_t* const geom = &dt_geoms[slot->geom];
    unsigned int free_blocks = geom->free_blocks - (populated ? 1u : 0u);
    unsigned char footer_offset = populated ? 2u * DIRECTORY_ENTRY_SIZE : DIRECTORY_ENTRY_SIZE;
    unsigned char directory_size = populated ? POPULATED_DIRECTORY_SIZE : EMPTY_DIRECTORY_SIZE;
    unsigned char i;

    /* Construct the expected $0401 BASIC header line with reverse-video disk name, ID, and DOS version. */
    begin_test(populated ? DT_PHASE_POP_DIR : DT_PHASE_EMPTY_DIR, slot);
    memset(payload, 0, POPULATED_DIRECTORY_SIZE);
    payload[0] = 1u;
    payload[1] = 4u;
    payload[2] = payload[3] = 1u;
    payload[4] = slot->drive;
    payload[6] = 0x12u;
    payload[7] = '"';
    memset(&payload[8], ' ', DISK_LABEL_LENGTH);
    for (i = 0; slot->name[i] != '\0'; ++i) {
        payload[8u + i] = dos_text((unsigned char)slot->name[i]);
    }
    payload[24] = '"';
    payload[25] = payload[28] = ' ';
    payload[26] = dos_text((unsigned char)slot->disk_id[0]);
    payload[27] = dos_text((unsigned char)slot->disk_id[1]);
    payload[29] = '2';
    payload[30] = dos_text(geom->d80_status ? 'C' : 'A');

    /* Insert the 1-block SEQ directory line when checking a populated disk. */
    if (populated) {
        payload[32] = payload[33] = 1u;
        payload[34] = 1u;
        memset(&payload[36], ' ', 27u);
        payload[39] = '"';
        for (i = 0; slot->name[i] != '\0'; ++i) {
            payload[40u + i] = dos_text((unsigned char)slot->name[i]);
        }
        payload[40u + i] = '"';
        payload[58] = dos_text('S');
        payload[59] = dos_text('E');
        payload[60] = dos_text('Q');
    }

    /* Append the 'BLOCKS FREE.' BASIC footer line and two terminating zero link bytes. */
    payload[footer_offset] = payload[footer_offset + 1u] = 1u;
    payload[footer_offset + 2u] = (unsigned char)free_blocks;
    payload[footer_offset + 3u] = (unsigned char)(free_blocks >> 8);
    memset(&payload[footer_offset + 4u], ' ', 25u);
    for (i = 0; footer[i] != '\0'; ++i) {
        payload[footer_offset + 4u + i] = dos_text((unsigned char)footer[i]);
    }

    /*
     * Open "$0" or "$1" on SA 0. DOS 2.7 LOADIR returns via RTS without ENDCMD,
     * so OPEN status still carries PARSXQ's prior-DRVNUM OKERR until reading status
     * refreshes OKERR with the newly selected drive.
     */
    select_device(slot->device);
    drive_listen();
    drive_secondary((unsigned char)(0xf0u | DIRECTORY_CHANNEL));
    drive_ciout('$');
    drive_ciout((unsigned char)('0' + slot->drive));
    drive_unlisten();
    check_status_drive(slot, ST_OK, last_drive_by_unit[slot->device - DT_DEVICE_FIRST]);

    /* Read and verify the complete directory byte stream, then close SA 0 and confirm status. */
    recv_and_verify(slot, DIRECTORY_CHANNEL, directory_size, directory_size, DT_ERR_DIR_BYTE, DT_ERR_DIR_EOI);
    select_device(slot->device);
    close_secondary(DIRECTORY_CHANNEL);
    check_status(slot);
    complete_test();
}

/** Read a 256-byte raw sector via U1 and verify expected_length prefix bytes against payload[]. */
static void check_raw_sector(const drive_slot_t* slot, unsigned char track, unsigned char sector,
                             unsigned int expected_length) {
    /* Open a direct buffer, execute U1 on (track, sector), verify prefix bytes and 256th-byte EOI, and close. */
    current_track = track;
    current_sector = sector;
    open_direct_checked(slot);
    send_formatted_command(slot, "U1:2,%d,%t,%s");
    check_status(slot);
    recv_and_verify(slot, FILE_CHANNEL, RAW_SECTOR_SIZE, expected_length, DT_ERR_SEC_BYTE, DT_ERR_SEC_EOI);
    select_device(slot->device);
    close_secondary(FILE_CHANNEL);
    check_status(slot);
}

/** Compare the 4040 BAM at 18/0 after format or after closing the first smoke file. */
static void check_4040_bam(const drive_slot_t* slot, bool populated) {
    unsigned char track;
    unsigned char sectors;
    unsigned char offset = 4u;
    unsigned char i;
    /*
     * 4040 DOS 2.0 places NAMBUF at $41B4 (inside drive 0's BAM0 buffer $4100-$41FF).
     * Listing drive 0's directory before closing a file populates $41B4..$41D7 in RAM,
     * which MAPOUT writes to 18/0. Bytes 0..179 hold the entire 35-track BAM and label/ID.
     */
    const unsigned int check_len = (populated && slot->drive == 0) ? DOS4040_BAM_NAMBUF_OFFSET : RAW_SECTOR_SIZE;

    /* Build the expected 35-track 4040 BAM bitmaps, reserving 18/0..1 and (when populated) 17/0. */
    begin_test(populated ? DT_PHASE_FILE_BAM : DT_PHASE_FORMAT_BAM, slot);
    memset(payload, 0, RAW_SECTOR_SIZE);
    payload[0] = DOS4040_BAM_TRACK;
    payload[1] = 1u;
    payload[2] = dos_text('A');
    for (track = 1u; track <= DT_DOS4040_TRACKS; ++track) {
        sectors = track <= 17u ? 21u : track <= 24u ? 19u : track <= 30u ? 18u : 17u;
        payload[offset] = sectors;
        payload[offset + 1u] = 0xffu;
        payload[offset + 2u] = 0xffu;
        payload[offset + 3u] = (unsigned char)((1u << (sectors - 16u)) - 1u);
        offset = (unsigned char)(offset + 4u);
    }
    payload[DOS4040_BAM_TRACK * 4u] -= 2u;
    payload[DOS4040_BAM_TRACK * 4u + 1u] &= 0xfcu;
    if (populated) {
        --payload[DOS4040_FIRST_FILE_TRACK * 4u];
        payload[DOS4040_FIRST_FILE_TRACK * 4u + 1u] &= 0xfeu;
    }

    /* Populate the $90..$AA shifted-space-padded disk name, ID, and '2A' format marker, then compare 18/0. */
    memset(&payload[DOS4040_BAM_NAME_OFFSET], 0xa0u, DOS4040_BAM_NAME_SIZE);
    for (i = 0; slot->name[i] != '\0'; ++i) {
        payload[DOS4040_BAM_NAME_OFFSET + i] = dos_text((unsigned char)slot->name[i]);
    }
    payload[DOS4040_BAM_NAME_OFFSET + 18u] = dos_text((unsigned char)slot->disk_id[0]);
    payload[DOS4040_BAM_NAME_OFFSET + 19u] = dos_text((unsigned char)slot->disk_id[1]);
    payload[DOS4040_BAM_NAME_OFFSET + 21u] = '2';
    payload[DOS4040_BAM_NAME_OFFSET + 22u] = dos_text('A');

    check_raw_sector(slot, DOS4040_BAM_TRACK, 0u, check_len);
    complete_test();
}

/** Check formatted 8050/8250 BAM parts or the affected BAM part after the first file. */
static void check_8050_bam(const drive_slot_t* slot, bool populated) {
    const drive_geom_t* const geom = &dt_geoms[slot->geom];
    const unsigned char parts = geom->bam_parts;
    const unsigned char pages = populated ? 1u : parts;
    unsigned char page;

    /* Verify each 50-track BAM sector on track 38 (sector interleave 3: 0, 3, 6, 9). */
    begin_test(populated ? DT_PHASE_FILE_BAM : DT_PHASE_FORMAT_BAM, slot);
    for (page = 0; page < pages; ++page) {
        unsigned char first = (unsigned char)(1u + page * DOS8050_BAM_TRACKS_PER_SECTOR);
        unsigned char limit = (unsigned char)(first + DOS8050_BAM_TRACKS_PER_SECTOR);
        unsigned char track;
        unsigned char offset = DOS8050_BAM_ENTRY_OFFSET;

        if (limit > geom->track_limit) {
            limit = geom->track_limit;
        }

        /* Build the BAM sector chain link, 'C' format byte, track range, and 5-byte per-track free bitmaps. */
        memset(payload, 0, RAW_SECTOR_SIZE);
        payload[0] = page + 1u == parts ? DOS8050_DIRECTORY_TRACK : DOS8050_BAM_TRACK;
        payload[1] = page + 1u == parts ? 1u : (unsigned char)((page + 1u) * DOS8050_BAM_INTERLEAVE);
        payload[2] = dos_text('C');
        payload[4] = first;
        payload[5] = limit;

        for (track = first; track < limit; ++track) {
            const unsigned char side_track =
                track > DOS8050_TRACKS_PER_SIDE ? (unsigned char)(track - DOS8050_TRACKS_PER_SIDE) : track;
            unsigned char sectors;
            if (side_track <= DOS8050_ZONE_29_LAST_TRACK) {
                sectors = 29u;
                payload[offset + 4u] = 0x1fu;
            } else if (side_track <= DOS8050_ZONE_27_LAST_TRACK) {
                sectors = 27u;
                payload[offset + 4u] = 0x07u;
            } else if (side_track <= DOS8050_ZONE_25_LAST_TRACK) {
                sectors = 25u;
                payload[offset + 4u] = 0x01u;
            } else {
                sectors = 23u;
            }
            payload[offset] = sectors;
            payload[offset + 1u] = 0xffu;
            payload[offset + 2u] = 0xffu;
            payload[offset + 3u] = sectors == 23u ? 0x7fu : 0xffu;
            offset = (unsigned char)(offset + 5u);
        }

        /* Mark BAM sectors on track 38, directory header/first block on track 39, and first data sector 38/1. */
        if (page == 0) {
            const bool d82 = slot->geom == DT_GEOM_D82;
            offset = DOS8050_BAM_ENTRY_OFFSET + (DOS8050_BAM_TRACK - 1u) * 5u;
            payload[offset] -= d82 ? 4u : 2u;
            payload[offset + 1u] &= d82 ? 0xb6u : 0xf6u;
            if (d82) {
                payload[offset + 2u] &= 0xfdu;
            }
            if (populated) {
                --payload[offset];
                payload[offset + 1u] &= (unsigned char)~(1u << DOS8050_FIRST_FILE_SECTOR);
            }
            offset = DOS8050_BAM_ENTRY_OFFSET + (DOS8050_DIRECTORY_TRACK - 1u) * 5u;
            payload[offset] -= 2u;
            payload[offset + 1u] &= 0xfcu;
        }
        check_raw_sector(slot, DOS8050_BAM_TRACK, (unsigned char)(page * DOS8050_BAM_INTERLEAVE), RAW_SECTOR_SIZE);
    }
    complete_test();
}

/** Dispatch format or populated BAM verification for the slot's drive family. */
static void check_slot_bam(const drive_slot_t* slot, bool populated) {
    if (dt_geoms[slot->geom].d80_status) {
        check_8050_bam(slot, populated);
    } else {
        check_4040_bam(slot, populated);
    }
}

/** Check the native link bytes (0, last_index) and payload of the first file's allocated sector. */
static void smoke_file_sector(const drive_slot_t* slot) {
    const bool d80 = dt_geoms[slot->geom].d80_status != 0;

    /* Read 17/0 (4040) or 38/1 (8050/8250) via U1 and verify its EOF link and 16 payload bytes. */
    begin_test(DT_PHASE_FIRST_SECTOR, slot);
    payload[0] = 0;
    payload[1] = SMOKE_PAYLOAD_SIZE + 1u;
    drive_fill_payload(payload + 2u, slot->signature, SMOKE_PAYLOAD_SIZE, false);
    check_raw_sector(slot, d80 ? DOS8050_BAM_TRACK : DOS4040_FIRST_FILE_TRACK, d80 ? DOS8050_FIRST_FILE_SECTOR : 0u,
                     SMOKE_PAYLOAD_SIZE + 2u);
    complete_test();
}

/** Create and verify the single 16-byte SEQ file used by the populated directory check. */
static void smoke_file(const drive_slot_t* slot) {
    begin_test(DT_PHASE_SMOKE_FILE, slot);
    drive_fill_payload(payload, slot->signature, SMOKE_PAYLOAD_SIZE, false);
    write_typed_payload_mode(slot, slot->name, 'S', 'W', SMOKE_PAYLOAD_SIZE, false);
    read_and_verify_typed(slot, slot->name, 'S', SMOKE_PAYLOAD_SIZE, SMOKE_PAYLOAD_SIZE, DT_ERR_READBACK);
    check_status(slot);
    complete_test();
}

/** Run all 17 single-drive smoke checks against one freshly formatted slot. */
static void smoke_slot(const drive_slot_t* slot) {
    static const rel_case_t smoke_rel = {DT_PHASE_REL_SMOKE, 'Q', REL_RECORD_SIZE, 0u};

    /* Verify empty and single-file directory/BAM states, file commands, direct block, REL, and PRG/USR streams. */
    check_smoke_directory(slot, false);
    check_slot_bam(slot, false);
    smoke_file(slot);
    check_smoke_directory(slot, true);
    check_slot_bam(slot, true);
    smoke_file_sector(slot);
    run_cmd_table(slot, smoke_cmd_steps, count_of(smoke_cmd_steps));
    smoke_block(slot);
    run_rel_case(slot, &smoke_rel);
    test_file_types(slot);
}

/** Generate a 256-byte sector signature with hoisted track/sector weights (no per-byte multiply). */
static void fill_raw_block_payload(const drive_slot_t* slot, unsigned char track, unsigned char sector) {
    unsigned char t_acc = 0;
    unsigned char s_acc = 0;
    unsigned char i;

    /* Accumulate distinct track and sector offsets by repeated addition, then stamp track/sector at +3..+4. */
    for (i = 0; i < track; ++i) {
        t_acc = (unsigned char)(t_acc + TRACK_WEIGHT);
    }
    for (i = 0; i < sector; ++i) {
        s_acc = (unsigned char)(s_acc + SECTOR_WEIGHT);
    }
    drive_fill_payload(payload, (unsigned char)(slot->signature + t_acc + s_acc), RAW_SECTOR_SIZE, true);
    payload[3] = track;
    payload[4] = sector;
}

/** Round-trip an unused sector through either numeric (U2/U1) or alphabetic (UB/UA) commands. */
static void raw_block_at(const drive_slot_t* slot, bool alias, unsigned char track, unsigned char sector) {
    /* Open a direct buffer at index 0 via B-P:2,0 and fill all 256 bytes. */
    current_track = track;
    current_sector = sector;
    fill_raw_block_payload(slot, track, sector);
    open_direct_checked(slot);
    send_command(slot->device, "B-P:2,0");
    check_status_drive(slot, ST_OK, last_drive_by_unit[slot->device - DT_DEVICE_FIRST]);

    drive_listen();
    drive_secondary((unsigned char)(0x60u | FILE_CHANNEL));
    drive_send_bytes(RAW_SECTOR_SIZE);
    drive_unlisten();

    /* Write the raw sector with U2/UB, read it back with U1/UA, and verify all 256 bytes and EOI. */
    fmt_char = alias ? 'B' : '2';
    send_formatted_command(slot, "U%k:2,%d,%t,%s");
    check_status(slot);
    fmt_char = alias ? 'A' : '1';
    send_formatted_command(slot, "U%k:2,%d,%t,%s");
    check_status(slot);
    recv_and_verify(slot, FILE_CHANNEL, RAW_SECTOR_SIZE, RAW_SECTOR_SIZE, DT_ERR_RAW_BYTE, DT_ERR_RAW_EOI);

    select_device(slot->device);
    close_secondary(FILE_CHANNEL);
    check_status(slot);
}

/** Copy a retained smoke file in both directions between one unit's drives. */
static void test_cross_copy(const drive_slot_t* first_slot) {
    unsigned char direction;
    char target_name[] = "CPY0";

    /* Copy D0->D1 and D1->D0, verifying both the destination copy and source retention each time. */
    pair_base_slot = first_slot;
    for (direction = 0; direction < DT_DRIVES_PER_DEVICE; ++direction) {
        const drive_slot_t* const source = &first_slot[direction];
        const drive_slot_t* const target = &first_slot[direction ^ 1u];

        begin_test(DT_PHASE_CROSS_COPY, target);
        target_name[3] = (char)('0' + target->drive);
        fmt_char = (char)('0' + source->drive);
        send_formatted_command(target, direction == 0 ? "C1:CPY1=0:%0" : "C0:CPY0=1:%1");
        check_status(target);

        drive_fill_payload(payload, source->signature, SMOKE_PAYLOAD_SIZE, false);
        read_and_verify_typed(target, target_name, 'S', SMOKE_PAYLOAD_SIZE, SMOKE_PAYLOAD_SIZE, DT_ERR_COPY_READBACK);
        verify_slot_smoke_file(source, 'R', 'S', DT_ERR_COPY_SOURCE);
        complete_test();
    }
}

/** Check one ordinary-file type at the selected sector boundary, including 0/1-byte EOF quirk. */
static void test_file_boundary(const drive_slot_t* slot, unsigned char length_index, char type) {
    const unsigned int length = dt_boundary_lengths[length_index];
    const unsigned int read_length = length <= 1u ? 4u : length;
    char target_name[3];

    /* Build a short unique filename ('B0'..'B7', 'P0'..'P7', 'U0'..'U7') and write length bytes. */
    target_name[0] = type == 'S' ? 'B' : type;
    target_name[1] = (char)('0' + length_index);
    target_name[2] = '\0';
    begin_test(type == 'S' ? DT_PHASE_SEQ_BOUNDARY : type == 'P' ? DT_PHASE_PRG_BOUNDARY : DT_PHASE_USR_BOUNDARY, slot);
    show_length(length);

    drive_fill_payload(payload, slot->signature, length, false);
    write_typed_payload_mode(slot, target_name, type, 'W', length, false);

    /* Model CLSWRT inserting CR for 0-byte files and OR30 dropping the initial EOI into a 4-byte stream. */
    if (length <= 1u) {
        if (length == 0) {
            payload[0] = PETSCII_CR;
        }
        payload[1] = 0;
        payload[2] = 2u;
        payload[3] = payload[0];
    }
    read_and_verify_typed(slot, target_name, type, read_length, read_length, DT_ERR_BND_READBACK);
    complete_test();
}

/** Append two bytes across a sector boundary and replace the file on a representative slot. */
static void test_append_and_replace(const drive_slot_t* slot) {
    static const unsigned int prefix_length = 253u;
    unsigned char step_acc = 0;
    unsigned char i;

    /* Append 2 bytes to 253-byte 'B0' so the combined 255-byte stream crosses into a second sector. */
    begin_test(DT_PHASE_SEQ_APPEND, slot);
    current_length = prefix_length + 2u;
    payload[0] = 0x00;
    payload[1] = 0xffu;
    write_typed_payload_mode(slot, "B0", 'S', 'A', 2u, false);
    drive_fill_payload(payload, slot->signature, prefix_length, false);
    payload[prefix_length] = 0x00;
    payload[prefix_length + 1u] = 0xffu;
    read_and_verify_typed(slot, "B0", 'S', current_length, current_length, DT_ERR_APP_READBACK);
    complete_test();

    /* Overwrite 'B0' using '@0:B0,S,W' save-with-replace and verify the new 16-byte payload. */
    begin_test(DT_PHASE_SEQ_REPLACE, slot);
    current_length = SMOKE_PAYLOAD_SIZE;
    for (i = 0; i < SMOKE_PAYLOAD_SIZE; ++i) {
        payload[i] = (unsigned char)(slot->signature ^ step_acc);
        step_acc = (unsigned char)(step_acc + REPLACE_STEP);
    }
    payload[0] = 0;
    payload[1] = PETSCII_CR;
    payload[2] = 0xffu;
    write_typed_payload_mode(slot, "B0", 'S', 'W', SMOKE_PAYLOAD_SIZE, true);
    read_and_verify_typed(slot, "B0", 'S', SMOKE_PAYLOAD_SIZE, SMOKE_PAYLOAD_SIZE, DT_ERR_RPL_READBACK);
    complete_test();
}

/** Initialize, then validate away an unreferenced allocation while retaining a file. */
static void test_initialize_validate(const drive_slot_t* slot) {
    /* Initialize BAM, claim an orphan block via B-A, run VALIDATE to reclaim it, and verify file survival. */
    begin_test(DT_PHASE_INIT_VALIDATE, slot);
    send_formatted_command(slot, "I%d");
    check_status(slot);
    send_formatted_command(slot, "B-A:%d,1,0");
    check_status(slot);
    drive_long_timeout(1);
    send_formatted_command(slot, "V%d");
    check_status(slot);
    drive_long_timeout(0);
    send_formatted_command(slot, "B-A:%d,1,0");
    check_status(slot);
    send_formatted_command(slot, "B-F:%d,1,0");
    check_status(slot);
    complete_test();
    check_isolation_slot(slot, DT_PHASE_VALIDATE_FILE);
}

/** Quick NEW keeps the disk ID and free-sector data while replacing directory and BAM metadata. */
static void test_quick_format(const drive_slot_t* slot) {
    /* Seed free sector 1/3, run ID-less quick NEW, and verify empty directory/BAM plus sector 1/3 retention. */
    begin_test(DT_PHASE_QUICK_FORMAT, slot);
    raw_block_at(slot, true, 1u, 3u);
    drive_long_timeout(1);
    send_formatted_command(slot, "N%d:%n");
    check_status(slot);
    drive_long_timeout(0);
    complete_test();

    check_smoke_directory(slot, false);
    check_slot_bam(slot, false);

    begin_test(DT_PHASE_QUICK_RET, slot);
    fill_raw_block_payload(slot, 1u, 3u);
    check_raw_sector(slot, 1u, 3u, RAW_SECTOR_SIZE);
    complete_test();
}

/** Seed every ROM-defined geometry zone boundary, then reread all seeded sectors. */
static void test_geometry_boundaries(const drive_slot_t* slot) {
    const drive_geom_t* const geom = &dt_geoms[slot->geom];
    unsigned char phase;
    unsigned char idx;

    /* Pass 0 writes and verifies each boundary sector, then pass 1 rereads all of them to catch aliasing. */
    for (phase = 0; phase < 2u; ++phase) {
        for (idx = 0; idx < geom->zone_count; ++idx) {
            const unsigned char track = geom->zones[idx][0];
            const unsigned char sector = geom->zones[idx][1];
            if (phase == 0) {
                begin_test(DT_PHASE_GEOM_WRITE, slot);
                raw_block_at(slot, false, track, sector);
            } else {
                begin_test(DT_PHASE_GEOM_RET, slot);
                fill_raw_block_payload(slot, track, sector);
                check_raw_sector(slot, track, sector, RAW_SECTOR_SIZE);
            }
            complete_test();
        }
    }
}

/** Duplicate in both directions and verify free-sector copy plus source retention. */
static void test_duplicate_pair(const drive_slot_t* first_slot) {
    unsigned char direction;

    /* Seed 1/2 on source, run D1=0 or D0=1, and verify 1/2 on both destination and source. */
    for (direction = 0; direction < DT_DRIVES_PER_DEVICE; ++direction) {
        const drive_slot_t* const source = &first_slot[direction];
        const drive_slot_t* const target = &first_slot[direction ^ 1u];

        begin_test(DT_PHASE_DUP_SEED, source);
        raw_block_at(source, false, 1u, 2u);
        complete_test();

        begin_test(DT_PHASE_DUP_DISK, target);
        drive_long_timeout(1);
        send_command(source->device, direction == 0 ? "D1=0" : "D0=1");
        check_status(target);
        drive_long_timeout(0);
        complete_test();

        fill_raw_block_payload(source, 1u, 2u);
        begin_test(DT_PHASE_DUP_TARGET, target);
        check_raw_sector(target, 1u, 2u, RAW_SECTOR_SIZE);
        complete_test();

        begin_test(DT_PHASE_DUP_SOURCE, source);
        check_raw_sector(source, 1u, 2u, RAW_SECTOR_SIZE);
        complete_test();
    }
}

/** Execute one complete 468-check pass across all eight virtual drive slots. */
static void run_pass(void) {
    static const char boundary_types[DT_BOUNDARY_TYPE_COUNT] = {'S', 'P', 'U'};
    unsigned char i;
    unsigned char j;
    unsigned char k;

    /* Reset per-pass counters and print the PET screen banner. */
    success_count = 0;
    test_number = 0;
    timeout_stage = 0;
    previous_device = 0;
    memset(last_drive_by_unit, 0, sizeof(last_drive_by_unit));

    clrscr();
    cputs("DRIVE TEST (DESTRUCTIVE)\r\n");
    cputs("DEVICES 8-11, DRIVES 0-1\r\n");

    /* Format all eight slots, then run single-drive smoke checks on each slot. */
    for (i = 0; i < DT_SLOT_COUNT; ++i) {
        drive_format_slot(&dt_slots[i]);
    }
    for (i = 0; i < DT_SLOT_COUNT; ++i) {
        smoke_slot(&dt_slots[i]);
    }

    /* Unit and dual-drive isolation across mounted slots. */
    for (i = 0; i < DT_UNIT_ISO_COUNT; ++i) {
        check_isolation_slot(&dt_slots[dt_unit_iso_slots[i]], DT_PHASE_UNIT_ISO);
    }
    for (i = 0; i < DT_SLOT_COUNT; i += DT_DRIVES_PER_DEVICE) {
        for (j = 0; j < DT_DRIVE_ISO_PASSES; ++j) {
            check_isolation_slot(&dt_slots[i + (j & 1u)], DT_PHASE_DRIVE_ISO);
        }
    }

    /* Cross-drive copies and concatenation on every dual-drive unit. */
    for (i = 0; i < DT_SLOT_COUNT; i += DT_DRIVES_PER_DEVICE) {
        test_cross_copy(&dt_slots[i]);
        pair_base_slot = &dt_slots[i];
        run_cmd_table(&dt_slots[i + 1u], concat_cmd_steps, count_of(concat_cmd_steps));
        check_isolation_slot(&dt_slots[i], DT_PHASE_CONCAT_SOURCE);
        check_isolation_slot(&dt_slots[i + 1u], DT_PHASE_CONCAT_PEER);
    }

    /* 512-byte multi-sector sequential transfers on every slot. */
    for (i = 0; i < DT_SLOT_COUNT; ++i) {
        const drive_slot_t* const slot = &dt_slots[i];
        begin_test(DT_PHASE_MULTI_SEQ, slot);
        drive_fill_payload(payload, slot->signature, LONG_PAYLOAD_SIZE, false);
        write_typed_payload_mode(slot, slot->long_name, 'S', 'W', LONG_PAYLOAD_SIZE, false);
        read_and_verify_typed(slot, slot->long_name, 'S', LONG_PAYLOAD_SIZE, LONG_PAYLOAD_SIZE, DT_ERR_LONG_READBACK);
        complete_test();
    }

    /* Representative per-geometry REL record lengths, sector crossing, and side-sector boundary. */
    for (i = 0; i < DT_REP_SLOT_COUNT; ++i) {
        const drive_slot_t* const slot = &dt_slots[dt_rep_slots[i]];
        for (j = 0; j < count_of(rel_length_cases); ++j) {
            run_rel_case(slot, &rel_length_cases[j]);
        }
        test_rel_growth(slot);
        test_rel_side_boundary(slot);
    }

    /* Raw block round-trips on every slot, UB/UA per geometry, and D82's final sector. */
    for (i = 0; i < DT_SLOT_COUNT; ++i) {
        const drive_slot_t* const slot = &dt_slots[i];
        begin_test(DT_PHASE_RAW_BLOCK, slot);
        raw_block_at(slot, false, 1u, 2u);
        if (slot->drive == 0 && slot->device != DT_DEVICE_LAST) {
            raw_block_at(slot, true, 1u, 3u);
        }
        if (slot->geom == DT_GEOM_D82) {
            raw_block_at(slot, false, DOS8250_LAST_TRACK, DOS8250_LAST_SECTOR);
        }
        complete_test();
    }

    /* Ordinary-file sector boundaries, append, replace, wildcard scratch, B-A/B-F, I/V, quick format, and zones. */
    for (i = 0; i < DT_REP_SLOT_COUNT; ++i) {
        const drive_slot_t* const slot = &dt_slots[dt_rep_slots[i]];
        for (j = 0; j < DT_BOUNDARY_LEN_COUNT; ++j) {
            for (k = 0; k < DT_BOUNDARY_TYPE_COUNT; ++k) {
                test_file_boundary(slot, j, boundary_types[k]);
            }
            if (j == 0) {
                test_append_and_replace(slot);
            }
        }
        run_cmd_table(slot, wildcard_cmd_steps, count_of(wildcard_cmd_steps));

        begin_test(DT_PHASE_BLOCK_ALLOC, slot);
        run_cmd_table(slot, alloc_cmd_steps, count_of(alloc_cmd_steps));
        complete_test();

        test_initialize_validate(slot);
        test_quick_format(slot);
        test_geometry_boundaries(slot);
    }

    /* Disk DUPLICATE in both directions on representative 4040, 8050, and 8250 units. */
    for (i = 0; i < DT_SLOT_COUNT - DT_DRIVES_PER_DEVICE; i += DT_DRIVES_PER_DEVICE) {
        test_duplicate_pair(&dt_slots[i]);
    }

    /* Render the pass summary on the PET screen and publish STATE_PASSED to the $7FF0 mailbox. */
    set_cursor_row(PROGRESS_ROW);
    clear_line_tail();
    set_cursor_row(DETAIL_ROW);
    clear_line_tail();
    set_cursor_row(PROGRESS_ROW);
    cputs("DRIVE TEST PASSED\r\n");
    put_u16(success_count);
    cputs(" CHECKS PASSED\r\n");

    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_STATE), TARGET_DIAG_STATE_PASSED);
}

/** Initialize the $7FF0 diagnostic mailbox, configure IEEE I/O, and run standalone or MCU-triggered passes. */
int main(void) {
    /* Publish RUNNING state and magic bytes in $7FF0 while preserving HOST_CONTROL set by the MCU. */
    mailbox_seq = 0;
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SEQ), 0);
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_LO), 0);
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_HI), 0);
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_PHASE), TARGET_DIAG_PHASE_NONE);
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SLOT), TARGET_DIAG_SLOT_NONE);
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_STATE), TARGET_DIAG_STATE_RUNNING);
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_MAGIC0), TARGET_DIAG_MAGIC_0);
    POKE(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_MAGIC1), TARGET_DIAG_MAGIC_1);
    drive_bus_init();

    /* Run passes: return to BASIC READY in standalone mode, or wait for MCU STATE_RUNNING re-triggers. */
    for (;;) {
        run_pass();
        if (PEEK(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_HOST_CONTROL)) == TARGET_DIAG_HOST_STANDALONE) {
            return 0;
        }
        while (PEEK(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_STATE)) == TARGET_DIAG_STATE_PASSED) {
        }
    }
}
