// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "tape.h"

#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#include "breakpoint.h"
#include "cbm/filename.h"
#include "cbm/petscii.h"
#include "diag/log/log.h"
#include "driver.h"
#include "global.h"
#include "sd/sd.h"
#include "system_state.h"
#include "tape_dir.h"

#define PRGS_DIR "/prgs"

// BASIC program start address (universal across all PET ROM versions). The
// synthesized directory listing loads here, just like LOAD "$" on a disk drive.
#define BASIC_START 0x0401

// Maximum number of directory entries to list (matches the classic 1541).
#define MAX_DIR_ENTRIES 144

// Disk name shown in the synthesized directory header. The FAT volume label is
// not yet available (f_getlabel is disabled in the FatFs build), so use a fixed
// name for now.
#define DIR_DISK_NAME "ECONOPET"

// Capacity of the synthesized directory program image. Each rendered line is
// at most ~32 bytes (link + line number + text + terminator).
#define DIR_IMAGE_CAPACITY (MAX_DIR_ENTRIES * 32 + 64)

// Fixed 6502 print loop: LDX #0 / LDA msg,X / BEQ done / JSR CHROUT / INX / BNE loop
static const uint8_t print_loop[] = {
    0xA2, 0x00,                     // LDX #$00
    0xBD, 0x00, 0x00,               // LDA $xxxx,X  (operand patched at runtime)
    0xF0, 0x06,                     // BEQ done
    0x20, 0xD2, 0xFF,               // JSR $FFD2 (CHROUT)
    0xE8,                           // INX
    0xD0, 0xF5                      // BNE loop
};

#define PRINT_LOOP_SIZE  sizeof(print_loop)     // 13 bytes
#define JMP_SIZE         3                      // JMP ld210
#define TAPE_BUFFER_CAPACITY 192

typedef struct {
    tape_config_t cfg;      // Parsed from config.yaml hex blob
    bool enabled;           // Virtual tape enabled (config had 'tape' key)
    uint8_t saved_buf[TAPE_BUFFER_CAPACITY]; // Original tape buffer contents
} tape_state_t;

static tape_state_t state;

// Search PRGS_DIR for a .prg file matching 'pattern'. On success, writes the
// full path (e.g. "/prgs/game.prg") to 'path_out' and returns true.
static bool find_prg_file(const uint8_t* pattern, uint8_t pattern_len,
                          char* path_out, size_t path_out_size) {
    DIR* dir = opendir(PRGS_DIR);
    if (dir == NULL) {
        log_warn("tape: cannot open " PRGS_DIR);
        return false;
    }

    bool found = false;
    struct dirent* ent;

    while ((ent = readdir(dir)) != NULL) {
        const char* name = ent->d_name;

        // Skip entries that do not end with ".prg" (case-insensitive)
        size_t len = strlen(name);
        if (len < 5) continue;
        const char* ext = name + len - 4;
        if (tolower((unsigned char)ext[0]) != '.'
            || tolower((unsigned char)ext[1]) != 'p'
            || tolower((unsigned char)ext[2]) != 'r'
            || tolower((unsigned char)ext[3]) != 'g') {
            continue;
        }

        if (cbm_filename_match(pattern, pattern_len, name)) {
            int n = snprintf(path_out, path_out_size, "%s/%s", PRGS_DIR, name);
            if (n < 0 || (size_t)n >= path_out_size) {
                log_warn("tape: path too long for %s", name);
                continue;
            }
            found = true;
            break;
        }
    }

    closedir(dir);
    return found;
}

// "loading" message printed after 'line1' (encoded for the active PET charset).
static const char stub_loading[] = "loading";

// Fixed overhead written around 'line1' in tape_build_stub: print loop, JMP,
// the CR separator, the trailing "loading", and the null terminator.
#define STUB_FIXED_OVERHEAD \
    (PRINT_LOOP_SIZE + JMP_SIZE + 1 + (sizeof(stub_loading) - 1) + 1)

// The stub overhead must leave room for at least one message byte. This catches
// any future growth of the print loop, JMP, or "loading" string at compile time.
static_assert(STUB_FIXED_OVERHEAD < TAPE_BUFFER_CAPACITY,
              "tape stub overhead must leave room for message text");

// Maximum number of 'line1' bytes that fit in the tape buffer after the fixed
// overhead. Messages longer than this are truncated so the stub always fits.
#define STUB_MAX_LINE1 (TAPE_BUFFER_CAPACITY - STUB_FIXED_OVERHEAD)

// Build the tape buffer stub: print loop + JMP ld210 + message string.
// 'line1' is printed (encoded for the active PET charset) followed by a newline
// and "LOADING". 'graphics_charset' selects the same case handling the directory
// listing uses. 'line1' is truncated to STUB_MAX_LINE1 so the stub (written to a
// TAPE_BUFFER_CAPACITY buffer) never overflows. Returns the bytes written.
static size_t tape_build_stub(uint8_t* buf, const char* line1, bool graphics_charset) {
    // Print loop (13 bytes)
    memcpy(buf, print_loop, PRINT_LOOP_SIZE);

    // Patch LDA operand with the message address
    uint16_t msg_addr = TAPE_BUFFER + PRINT_LOOP_SIZE + JMP_SIZE;
    buf[3] = (uint8_t)(msg_addr & 0xFF);
    buf[4] = (uint8_t)(msg_addr >> 8);

    // JMP ld210 (3 bytes) - jump to KERNAL post-load fixup
    buf[PRINT_LOOP_SIZE]     = 0x4C;    // JMP
    buf[PRINT_LOOP_SIZE + 1] = (uint8_t)(state.cfg.ld210 & 0xFF);
    buf[PRINT_LOOP_SIZE + 2] = (uint8_t)(state.cfg.ld210 >> 8);

    // Message string: <LINE1>\rLOADING\0
    size_t off = PRINT_LOOP_SIZE + JMP_SIZE;

    // First line encoded for the PET display, matching the directory listing's
    // charset handling so letters display correctly and relocated punctuation
    // (such as '_' -> $A4) renders as the proper glyph instead of a graphics
    // symbol. Truncate so the fixed trailing bytes always fit.
    for (size_t i = 0; i < STUB_MAX_LINE1 && line1[i] != '\0'; i++) {
        buf[off++] = ascii_to_petscii((uint8_t)line1[i], graphics_charset);
    }

    buf[off++] = '\r';      // CR (newline on PET)

    for (size_t i = 0; i < sizeof(stub_loading) - 1; i++) {
        buf[off++] = ascii_to_petscii((uint8_t)stub_loading[i], graphics_charset);
    }
    buf[off++] = '\0';      // Null terminator for print loop

    return off;
}

// One-shot breakpoint callback at LD210. Restores the tape buffer contents
// that were saved before the stub was written, then resumes LD210 normally.
static bp_result_t tape_ld210_callback(uint16_t pc, void* context) {
    (void)context;

    spi_write(TAPE_BUFFER, state.saved_buf, TAPE_BUFFER_CAPACITY);

    return (bp_result_t){ .pc = pc, .rearm = false };
}

// Case-insensitive comparator for sorting directory entries by name.
static int dir_entry_cmp(const void* a, const void* b) {
    const char* sa = ((const tape_dir_entry_t*)a)->name;
    const char* sb = ((const tape_dir_entry_t*)b)->name;

    while (*sa != '\0' && *sb != '\0') {
        int ca = tolower((unsigned char)*sa);
        int cb = tolower((unsigned char)*sb);
        if (ca != cb) return ca - cb;
        sa++;
        sb++;
    }
    return (int)(unsigned char)*sa - (int)(unsigned char)*sb;
}

// Scan PRGS_DIR for loadable .prg files. For each, record the display name
// (with the ".prg" extension suppressed) and the block count derived from the
// file size. Returns the number of entries collected (up to 'max').
static int gather_dir_entries(tape_dir_entry_t* out, int max) {
    DIR* dir = opendir(PRGS_DIR);
    if (dir == NULL) {
        log_warn("tape: cannot open " PRGS_DIR);
        return 0;
    }

    int count = 0;
    struct dirent* ent;

    while (count < max && (ent = readdir(dir)) != NULL) {
        const char* name = ent->d_name;

        // Skip entries that do not end with ".prg" (case-insensitive).
        size_t len = strlen(name);
        if (len < 5) continue;
        const char* ext = name + len - 4;
        if (tolower((unsigned char)ext[0]) != '.'
            || tolower((unsigned char)ext[1]) != 'p'
            || tolower((unsigned char)ext[2]) != 'r'
            || tolower((unsigned char)ext[3]) != 'g') {
            continue;
        }

        // Display name with the ".prg" extension suppressed.
        size_t base_len = len - 4;
        if (base_len > TAPE_DIR_MAX_NAME) base_len = TAPE_DIR_MAX_NAME;
        memcpy(out[count].name, name, base_len);
        out[count].name[base_len] = '\0';

        // Block count from the file size (rounded up, at least one block).
        uint16_t blocks = 1;
        char path[PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%s", PRGS_DIR, name);
        struct stat st;
        if (n > 0 && (size_t)n < sizeof(path) && stat(path, &st) == 0) {
            blocks = tape_dir_blocks_from_bytes((uint64_t)st.st_size, true);
        }
        out[count].blocks = blocks;

        count++;
    }

    closedir(dir);
    return count;
}

// Handle LOAD "$" by synthesizing a Commodore-style directory listing of the
// loadable .prg files in PRGS_DIR. The listing is a fake BASIC program written
// to SRAM at the BASIC start. Reusing the LD210 fixup path relinks the lines
// and returns to READY, so the user can LIST the directory.
static bp_result_t tape_load_directory(uint16_t pc) {
    static tape_dir_entry_t entries[MAX_DIR_ENTRIES];
    int count = gather_dir_entries(entries, MAX_DIR_ENTRIES);

    qsort(entries, (size_t)count, sizeof(entries[0]), dir_entry_cmp);

    uint64_t free_bytes = sd_free_bytes();

    // system_state.video_graphics mirrors the PET's CA2/char-ROM A10 line, where
    // 0 selects the graphics charset and 1 selects the text/business charset. So
    // the graphics charset is active when video_graphics is false.
    bool graphics_charset = !system_state.video_graphics;

    static uint8_t image[DIR_IMAGE_CAPACITY];
    size_t image_len = tape_dir_render(image, sizeof(image), BASIC_START,
                                       DIR_DISK_NAME, entries, (size_t)count,
                                       free_bytes, graphics_charset);
    if (image_len == 0) {
        log_warn("tape: directory image too large");
        return (bp_result_t){ .pc = pc, .rearm = true };
    }

    // Copy the synthesized program into SRAM at the BASIC start.
    uint16_t dest = BASIC_START;
    size_t written = 0;
    while (written < image_len) {
        size_t chunk = image_len - written;
        if (chunk > TEMP_BUFFER_SIZE) chunk = TEMP_BUFFER_SIZE;
        spi_write(dest, image + written, chunk);
        dest += (uint16_t)chunk;
        written += chunk;
    }

    uint16_t end_addr = (uint16_t)(BASIC_START + image_len);
    log_info("tape: directory listing, %d entries, $%04X-$%04X",
             count, (unsigned)BASIC_START, end_addr);

    // Set EAL/EAH so LD210 updates VARTAB and relinks the (fake) BASIC lines.
    spi_write_at(state.cfg.eal, (uint8_t)(end_addr & 0xFF));
    spi_write_at(state.cfg.eah, (uint8_t)(end_addr >> 8));

    // Save the tape buffer, arm the one-shot LD210 restore, write the stub.
    spi_read(TAPE_BUFFER, TAPE_BUFFER_CAPACITY, state.saved_buf);
    bp_set(state.cfg.ld210, tape_ld210_callback, NULL);

    // Mirror the regular SD load path's "FOUND <path>" message so the user sees
    // the same clue that the virtual tape drive intercepted the command.
    uint8_t stub_buf[TAPE_BUFFER_CAPACITY];
    size_t stub_len = tape_build_stub(stub_buf, "FOUND " PRGS_DIR "/$", graphics_charset);
    spi_write(TAPE_BUFFER, stub_buf, stub_len);

    return (bp_result_t){ .pc = TAPE_BUFFER, .rearm = true };
}

static bp_result_t tape_load_callback(uint16_t pc, void* context) {
    (void)context;

    // The breakpoint fires for every LOAD (tape, disk, etc.), so check
    // the device number first. Only intercept tape devices (1 or 2).
    uint8_t devnum = spi_read_at(state.cfg.devnum);
    if (devnum != 1 && devnum != 2) {
        return (bp_result_t){ .pc = pc, .rearm = true };
    }

    // Read the requested filename from PET memory.
    uint8_t fnlen = spi_read_at(state.cfg.fnlen);
    if (fnlen > 16) fnlen = 16;

    uint8_t fnadr_lo = spi_read_at(state.cfg.fnadr);
    uint8_t fnadr_hi = spi_read_at(state.cfg.fnadr + 1);
    uint16_t fnadr = (uint16_t)(fnadr_lo | (fnadr_hi << 8));

    // Read the filename from PET memory as raw PETSCII. Matching is done in
    // PETSCII space, so keep the typed bytes as-is.
    uint8_t pattern[17];
    for (uint8_t i = 0; i < fnlen; i++) {
        pattern[i] = spi_read_at(fnadr + i);
    }
    pattern[fnlen] = '\0';

    // Decode an ASCII copy for human-readable logging only.
    char log_name[17];
    for (uint8_t i = 0; i < fnlen; i++) {
        log_name[i] = petscii_to_ascii(pattern[i]);
    }
    log_name[fnlen] = '\0';

    log_info("tape: LOAD \"%s\" (len=%d)", log_name, fnlen);

    // Check for empty name or lone "*" (fall through to datasette).
    if (fnlen == 0 || (fnlen == 1 && pattern[0] == '*')) {
        log_info("tape: fall through to datasette");
        return (bp_result_t){ .pc = pc, .rearm = true };
    }

    // LOAD "$" generates a directory listing of PRGS_DIR.
    if (fnlen == 1 && pattern[0] == '$') {
        log_info("tape: directory listing requested");
        return tape_load_directory(pc);
    }

    // Search SD card for a matching .prg file.
    char path[PATH_MAX];
    if (!find_prg_file(pattern, fnlen, path, sizeof(path))) {
        log_info("tape: no match for \"%s\"", log_name);
        return (bp_result_t){ .pc = pc, .rearm = true };
    }

    log_info("tape: found %s", path);

    // Open the PRG file and read the 2-byte load address.
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        log_warn("tape: cannot open %s", path);
        return (bp_result_t){ .pc = pc, .rearm = true };
    }

    uint8_t hdr[2];
    if (fread(hdr, 1, 2, file) != 2) {
        log_warn("tape: short read on header of %s", path);
        fclose(file);
        return (bp_result_t){ .pc = pc, .rearm = true };
    }
    uint16_t load_addr = (uint16_t)(hdr[0] | (hdr[1] << 8));
    uint16_t dest = load_addr;

    // Step 5: Copy program data into SRAM via SPI.
    uint8_t* temp = acquire_temp_buffer();
    size_t bytes_read;

    while ((bytes_read = fread(temp, 1, TEMP_BUFFER_SIZE, file)) > 0) {
        spi_write(dest, temp, bytes_read);
        dest += bytes_read;
    }

    release_temp_buffer(&temp);
    fclose(file);

    uint16_t end_addr = dest;
    log_info("tape: loaded $%04X-$%04X from %s", load_addr, end_addr, path);

    // Set EAL/EAH to the end address. The KERNAL's LD210 routine
    // copies these into VARTAB before calling FINI to relink and clear.
    spi_write_at(state.cfg.eal, (uint8_t)(end_addr & 0xFF));
    spi_write_at(state.cfg.eah, (uint8_t)(end_addr >> 8));

    // Save the tape buffer contents before overwriting with the stub.
    spi_read(TAPE_BUFFER, TAPE_BUFFER_CAPACITY, state.saved_buf);

    // Arm a one-shot breakpoint at LD210 to restore the tape buffer
    // after the stub has finished executing.
    bp_set(state.cfg.ld210, tape_ld210_callback, NULL);

    // Build the stub in the tape buffer and write it to SRAM. The message is
    // bounded to STUB_MAX_LINE1 so it always fits (tape_build_stub also truncates
    // as a backstop). Cap the path so the prefix plus path cannot exceed it.
    static const char found_prefix[] = "found ";
    char msg[STUB_MAX_LINE1 + 1];
    snprintf(msg, sizeof(msg), "%s%.*s", found_prefix,
             (int)(STUB_MAX_LINE1 - (sizeof(found_prefix) - 1)), path);
    bool graphics_charset = !system_state.video_graphics;
    uint8_t stub_buf[TAPE_BUFFER_CAPACITY];
    size_t stub_len = tape_build_stub(stub_buf, msg, graphics_charset);
    spi_write(TAPE_BUFFER, stub_buf, stub_len);

    // Redirect execution to the tape buffer.
    return (bp_result_t){ .pc = TAPE_BUFFER, .rearm = true };
}

void tape_init(const tape_config_t* cfg) {
    memset(&state, 0, sizeof(state));

    if (cfg == NULL) {
        log_info("tape: disabled (no config blob)");
        return;
    }

    state.cfg = *cfg;
    state.enabled = true;
    bp_set(state.cfg.bp_addr, tape_load_callback, NULL);
}

void tape_deinit(void) {
    if (state.enabled) {
        bp_remove(state.cfg.bp_addr);
        state.enabled = false;
        log_info("tape: disabled");
    }
}
